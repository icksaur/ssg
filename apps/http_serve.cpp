#include "http_serve.h"

#include <ssg/EditorRuntime.h>
#include <ssg/CommandCatalog.h>
#include <ssg/CompiledKeymap.h>
#include <ssg/FindReplace.h>
#include <ssg/HttpEditorServer.h>
#include <ssg/KeyCode.h>
#include <ssg/Keymap.h>
#include <ssg/Protocol.h>
#include <ssg/PromptRouting.h>
#include <ssg/PromptSurface.h>
#include <ssg/StatusQueue.h>
#include <ssg/focus.h>
#include <ssg/session_snapshot.h>

#include <http.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ssg::app {

// The served web client lives in apps/web/{index.html,client.mjs,reconcile.mjs}
// and is embedded into the binary at build (see cmake embed_text). Keeping it as
// real files rather than a C++ string constant lets a node test exercise the
// pure client logic in reconcile.mjs, and keeps the markup/JS readable.
std::string_view embeddedWebAsset(std::string_view key);

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop.store(true); }

struct WebAsset {
    std::string_view route;
    std::string_view key;
    std::string_view contentType;
};

// One table drives both the embed keys and the served routes.
constexpr std::array<WebAsset, 3> kWebAssets{{
    {"/", "index.html", "text/html"},
    {"/client.mjs", "client.mjs", "text/javascript"},
    {"/reconcile.mjs", "reconcile.mjs", "text/javascript"},
}};

// Map the runtime's live focus and active prompt onto the shared routing seam,
// so the web host makes the exact text-routing decision the TUI does. The
// palette query is the one client-owned derived view: the browser edits it
// locally, so an AppendPaletteQuery result dispatches nothing here.
void routeText(EditorRuntime& runtime, ClientId client, std::string const& text) {
    auto snapshot = runtime.snapshot(client);
    if (!snapshot) return;
    auto const& sections = snapshot->sections();
    PromptRoutingState state;
    state.focus = sections.focus;
    if (sections.promptStatus.activeKind) {
        switch (*sections.promptStatus.activeKind) {
        case PromptKind::Palette:
            state.prompt = ActivePrompt::Palette;
            break;
        case PromptKind::Find:
            state.prompt = ActivePrompt::Find;
            state.currentValue = sections.findReplace.query;
            break;
        case PromptKind::Replace:
            state.prompt = ActivePrompt::Replace;
            state.currentValue = sections.findReplace.replacement;
            break;
        case PromptKind::Path:
        case PromptKind::Settings:
        case PromptKind::CommandArgument:
            // The generic text prompts do not publish their current value in the
            // semantic snapshot, so routing prompt.update_value here with only
            // the new text would OVERWRITE the existing value, not append. Leave
            // the prompt unset (the seam then ignores the text) until that value
            // is in the snapshot and the DOM renderer shows these prompts; the
            // TUI, which holds the value app-side, is unaffected.
            break;
        }
    }
    auto const route = PromptTextRouter{}.route(state, text);
    switch (route.kind) {
    case PromptTextRoute::Kind::Dispatch:
        (void)runtime.dispatch(
            client, {route.command, runtime.revision(), route.payload});
        break;
    case PromptTextRoute::Kind::AppendPaletteQuery:
    case PromptTextRoute::Kind::Ignore:
        break;
    }
}

// Drive a KEY:<event.code>:<mods>:<text> frame exactly as the TUI's keystroke
// path: a printable without a keycode routes as text; a keycode resolves against
// the keymap for the current focus and dispatches when bound, otherwise falls
// back to inserting its text. Single-stroke resolution mirrors the TUI, which
// also passes one stroke per resolve; a per-connection pending buffer would give
// the web host multi-stroke behavior the TUI does not have, so it is deferred
// until the keymap grows a multi-stroke binding and both clients adopt it.
void handleKey(EditorRuntime& runtime, ClientId client,
               CompiledKeymap const& keymap, std::string_view body) {
    auto const firstColon = body.find(':');
    if (firstColon == std::string_view::npos) return;
    auto const secondColon = body.find(':', firstColon + 1);
    if (secondColon == std::string_view::npos) return;
    std::string_view const codeName = body.substr(0, firstColon);
    std::string_view const mods =
        body.substr(firstColon + 1, secondColon - firstColon - 1);
    std::string const text{body.substr(secondColon + 1)};

    KeyStroke stroke;
    stroke.code = keyCodeFromName(codeName);
    stroke.control = mods.find('c') != std::string_view::npos;
    stroke.alt = mods.find('a') != std::string_view::npos;
    stroke.meta = mods.find('m') != std::string_view::npos;
    stroke.shift = mods.find('s') != std::string_view::npos;

    if (stroke.code == KeyCode::None) {
        if (!text.empty()) routeText(runtime, client, text);
        return;
    }

    FocusTarget focus = FocusTarget::Editor;
    if (auto const snapshot = runtime.snapshot(client)) {
        focus = snapshot->sections().focus;
    }
    auto const resolution =
        keymap.resolve(std::array{CompiledKeymap::compile(stroke)}, focus);
    if (resolution.kind == KeymapMatchKind::Resolved) {
        (void)runtime.dispatch(client,
                               {resolution.command, runtime.revision(), {}});
    } else if (!text.empty()) {
        routeText(runtime, client, text);
    }
}

}  // namespace

int run_http_server(EditorRuntime& runtime, unsigned short port) {
    // Attach the single browser client through the runtime's own API so
    // follow-model registration and every per-client setup runs, exactly as the
    // TUI's InProcess attach does.
    ClientId const client{1};
    if (!runtime.attach({client, InvocationOrigin::Websocket, {}}, ViewId{1})
             .accepted()) {
        std::fprintf(stderr, "ssg: --http attach failed\n");
        return 1;
    }
    // An always-editable buffer to type into, mirroring the TUI's startup.
    if (!runtime.dispatch(client, {"file.new", runtime.revision(), {}})
             .accepted()) {
        std::fprintf(stderr, "ssg: --http failed to open a buffer\n");
        return 1;
    }

    Http::Server server{port, Http::BindAddress::loopback};

    // M1 serves exactly one browser, sharing the single attached ClientId; a
    // second concurrent attach is refused explicitly rather than silently
    // mutating the first client's state. Multi-client is a later milestone.
    auto attached = std::make_shared<std::atomic<bool>>(false);

    // Compile the runtime's keymap once for keystroke resolution. The --http
    // path runs no init script, so the bindings are the defaults; rebuilding on
    // a keymap change is deferred until the web path can load one.
    std::shared_ptr<CompiledKeymap> compiledKeymap;
    if (auto const initial = runtime.snapshot(client)) {
        compiledKeymap = std::make_shared<CompiledKeymap>(
            initial->sections().keymap, *runtime.commandCatalog());
    }

    // The browser lays out natively, so it consumes the dimensionless semantic
    // snapshot -- no grid projection is sent.
    auto sendSnapshot = [&runtime, &server, client](Http::WebSocketHandle handle) {
        auto snapshot = runtime.snapshot(client);
        if (!snapshot) {
            (void)server.send(handle, std::string{"no snapshot for client"});
            return;
        }
        auto const bytes = ProtocolCodec{}.encodeSessionSnapshot(*snapshot);
        (void)server.send(
            handle, std::vector<std::uint8_t>{bytes.begin(), bytes.end()});
    };

    for (auto const& asset : kWebAssets) {
        server.get(std::string{asset.route},
                   [asset](Http::Context&) {
                       return Http::Ok(std::string{embeddedWebAsset(asset.key)},
                                       std::string{asset.contentType});
                   });
    }
    server.ws(
        "/session",
        Http::WebSocketHandler{
            .onOpen = {},
            .onMessage =
                [&runtime, &server, client, sendSnapshot, attached,
                 compiledKeymap](Http::WebSocketHandle handle,
                                 Http::WebSocketMessage message) {
                    std::string_view const payload{message.data};
                    if (payload.rfind("KEY:", 0) == 0) {
                        if (compiledKeymap) {
                            handleKey(runtime, client, *compiledKeymap,
                                      payload.substr(4));
                        }
                        sendSnapshot(handle);
                        return;
                    }
                    auto const attach = decodeSessionAttachRequest(message.data);
                    if (!attach.accepted()) {
                        return;
                    }
                    bool expected = false;
                    if (!attached->compare_exchange_strong(expected, true)) {
                        (void)server.send(
                            handle,
                            std::string{"a client is already attached"});
                        return;
                    }
                    sendSnapshot(handle);
                },
            .onClose =
                [attached](Http::WebSocketHandle) {
                    attached->store(false);
                },
        });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    try {
        server.start();
    } catch (std::exception const& error) {
        std::fprintf(stderr, "ssg: --http failed: %s\n", error.what());
        return 1;
    }
    std::fprintf(stderr,
                 "ssg: serving http://127.0.0.1:%u/  (Ctrl-C to stop)\n",
                 static_cast<unsigned>(port));
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    server.stop();
    return 0;
}

}  // namespace ssg::app
