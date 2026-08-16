#include "http_serve.h"

#include <ssg/EditorRuntime.h>
#include <ssg/CommandCatalog.h>
#include <ssg/CompiledKeymap.h>
#include <ssg/FindReplace.h>
#include <ssg/HttpEditorServer.h>
#include <ssg/KeyCode.h>
#include <ssg/Keymap.h>
#include <ssg/PaletteSubmit.h>
#include <ssg/Protocol.h>
#include <ssg/PromptRouting.h>
#include <ssg/PromptSurface.h>
#include <ssg/Search.h>
#include <ssg/StatusQueue.h>
#include <ssg/Viewport.h>
#include <ssg/focus.h>
#include <ssg/session_snapshot.h>

#include <http.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>

namespace ssg::app {

// The served web client lives in apps/web/{index.html,client.mjs,reconcile.mjs,fuzzy.mjs}
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
constexpr std::array<WebAsset, 4> kWebAssets{{
    {"/", "index.html", "text/html"},
    {"/client.mjs", "client.mjs", "text/javascript"},
    {"/reconcile.mjs", "reconcile.mjs", "text/javascript"},
    {"/fuzzy.mjs", "fuzzy.mjs", "text/javascript"},
}};

// Map the runtime's live focus and active prompt onto the shared routing seam,
// so the web host makes the exact text-routing decision the TUI does. The
// palette query is the one client-owned derived view: the browser edits it
// locally, so an AppendPaletteQuery result dispatches nothing here. Returns true
// when a library command was actually dispatched, so the caller settles a
// predicted edit only once it has genuinely been resolved by the runtime.
bool routeText(EditorRuntime& runtime, ClientId client, std::string const& text) {
    auto snapshot = runtime.snapshot(client);
    if (!snapshot) return false;
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
        return true;
    case PromptTextRoute::Kind::AppendPaletteQuery:
    case PromptTextRoute::Kind::Ignore:
        break;
    }
    return false;
}

// A parsed KEY:<event.code>:<mods>:<editId>:<text> frame. editId is present when
// the client predicted this input locally (caret-anchored text insertion) and
// wants it settled; it comes before text so text may itself contain ':'.
struct KeyFrame {
    std::string_view code;
    std::string_view mods;
    std::optional<std::uint64_t> editId;
    std::string text;
};

std::optional<KeyFrame> parseKeyFrame(std::string_view body) {
    auto const c1 = body.find(':');
    if (c1 == std::string_view::npos) return std::nullopt;
    auto const c2 = body.find(':', c1 + 1);
    if (c2 == std::string_view::npos) return std::nullopt;
    auto const c3 = body.find(':', c2 + 1);
    if (c3 == std::string_view::npos) return std::nullopt;
    KeyFrame frame;
    frame.code = body.substr(0, c1);
    frame.mods = body.substr(c1 + 1, c2 - c1 - 1);
    auto const idText = body.substr(c2 + 1, c3 - c2 - 1);
    frame.text = std::string{body.substr(c3 + 1)};
    if (!idText.empty()) {
        std::uint64_t value = 0;
        auto const parsed = std::from_chars(
            idText.data(), idText.data() + idText.size(), value);
        if (parsed.ec == std::errc{} &&
            parsed.ptr == idText.data() + idText.size()) {
            frame.editId = value;
        }
    }
    return frame;
}

// Drive a parsed KEY frame exactly as the TUI's keystroke path: a printable
// without a keycode routes as text; a keycode resolves against the keymap for the
// current focus and dispatches when bound, otherwise falls back to inserting its
// text. Single-stroke resolution mirrors the TUI, which also passes one stroke
// per resolve; a per-connection pending buffer would give the web host
// multi-stroke behavior the TUI does not have, so it is deferred until the keymap
// grows a multi-stroke binding and both clients adopt it. Returns true when a
// command was dispatched, so a predicted edit is settled only once resolved.
bool handleKey(EditorRuntime& runtime, ClientId client,
               CompiledKeymap const& keymap, KeyFrame const& frame) {
    KeyStroke stroke;
    stroke.code = keyCodeFromName(frame.code);
    stroke.control = frame.mods.find('c') != std::string_view::npos;
    stroke.alt = frame.mods.find('a') != std::string_view::npos;
    stroke.meta = frame.mods.find('m') != std::string_view::npos;
    stroke.shift = frame.mods.find('s') != std::string_view::npos;

    if (stroke.code == KeyCode::None) {
        if (!frame.text.empty()) return routeText(runtime, client, frame.text);
        return false;
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
        return true;
    }
    if (!frame.text.empty()) return routeText(runtime, client, frame.text);
    return false;
}

// Sentinel settled-id meaning "nothing settled yet". Client edit ids start at 1,
// so 0 is unambiguous.
constexpr std::uint64_t kNoSettlement = 0;

// Envelope section tags. tag 0 is the optional library message (a snapshot or a
// delta, version+kind+value).
constexpr std::uint8_t kSectionLibraryBody = 0;

struct EnvelopeSection {
    std::uint8_t tag;
    std::string bytes;
};

void pushU32LE(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

// Frame the browser reads: an 8-byte little-endian settledClientEditId, a section
// count, then that many [tag][u32 length][bytes] sections. Zero sections is a
// pure settlement of a command that advanced no revision.
std::vector<std::uint8_t> makeEnvelope(std::uint64_t settledId,
                                       std::vector<EnvelopeSection> const& sections) {
    std::vector<std::uint8_t> frame;
    for (int shift = 0; shift < 64; shift += 8) {
        frame.push_back(static_cast<std::uint8_t>((settledId >> shift) & 0xFFU));
    }
    frame.push_back(static_cast<std::uint8_t>(sections.size()));
    for (auto const& section : sections) {
        frame.push_back(section.tag);
        pushU32LE(frame, static_cast<std::uint32_t>(section.bytes.size()));
        frame.insert(frame.end(), section.bytes.begin(), section.bytes.end());
    }
    return frame;
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

    // Compile the runtime's keymap once for keystroke resolution. The --http
    // path runs no init script, so the bindings are the defaults; rebuilding on
    // a keymap change is deferred until the web path can load one.
    std::shared_ptr<CompiledKeymap> compiledKeymap;
    if (auto const initial = runtime.snapshot(client)) {
        compiledKeymap = std::make_shared<CompiledKeymap>(
            initial->sections().keymap, *runtime.commandCatalog());
    }

    // Per-connection reconciliation state: the last semantic snapshot sent (to
    // derive the next delta against) and the highest client edit id settled.
    // The host serves one client, so a single cell suffices.
    auto prevSnapshot = std::make_shared<std::optional<SessionSnapshot>>();
    auto settledId = std::make_shared<std::atomic<std::uint64_t>>(kNoSettlement);
    auto runtimeMutex = std::make_shared<std::mutex>();
    // M1 serves exactly one browser, sharing the single attached ClientId; a
    // second concurrent attach is refused explicitly rather than silently
    // mutating the first client's state. Multi-client is a later milestone.
    // Guarded by runtimeMutex; 0 means no attached connection.
    auto attachedHandle = std::make_shared<Http::WebSocketHandle>(0);

    // Send the whole semantic snapshot as the envelope body -- the first frame
    // after attach, and the base every later delta re-bases onto. The browser
    // lays out natively, so it consumes the dimensionless snapshot (no grid).
    auto sendSnapshotLocked = [&runtime, &server, client, prevSnapshot,
                               settledId](Http::WebSocketHandle handle) {
        auto snapshot = runtime.snapshot(client);
        if (!snapshot) {
            (void)server.send(handle, std::string{"no snapshot for client"});
            return;
        }
        auto const bytes = ProtocolCodec{}.encodeSessionSnapshot(*snapshot);
        (void)server.send(
            handle, makeEnvelope(settledId->load(),
                                 {{kSectionLibraryBody, bytes}}));
        *prevSnapshot = std::move(snapshot);
    };

    // After a dispatched command, ship what changed: a delta from the previous
    // snapshot when the revision advanced, or a header-only settlement when it
    // did not, always stamping the highest settled client edit id.
    auto sendUpdateLocked = [&runtime, &server, client, prevSnapshot,
                             settledId](Http::WebSocketHandle handle) {
        auto current = runtime.snapshot(client);
        if (!current) return;
        std::vector<EnvelopeSection> sections;
        if (prevSnapshot->has_value() &&
            (*prevSnapshot)->revision() != current->revision()) {
            auto delta =
                SessionSnapshotCodec{}.deriveDelta(**prevSnapshot, *current);
            sections.push_back(
                {kSectionLibraryBody, ProtocolCodec{}.encodeSessionDelta(delta)});
            *prevSnapshot = std::move(current);
        }
        (void)server.send(handle, makeEnvelope(settledId->load(), sections));
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
                [&runtime, &server, client, sendSnapshotLocked, sendUpdateLocked,
                 compiledKeymap, settledId,
                 runtimeMutex, attachedHandle](Http::WebSocketHandle handle,
                            Http::WebSocketMessage message) {
                    std::string_view const payload{message.data};
                    if (payload.rfind("KEY:", 0) == 0) {
                        auto const frame = parseKeyFrame(payload.substr(4));
                        std::lock_guard lock{*runtimeMutex};
                        if (*attachedHandle != handle) return;
                        if (frame && compiledKeymap) {
                            bool dispatched = false;
                            dispatched = handleKey(
                                runtime, client, *compiledKeymap, *frame);
                            // Settle the predicted edit only once it is actually
                            // resolved (applied or rejected) by a real dispatch;
                            // a malformed or no-op frame must not falsely
                            // acknowledge and drop the client's prediction. Ids
                            // are monotonic and settle in message order.
                            if (dispatched && frame->editId) {
                                settledId->store(*frame->editId);
                            }
                        }
                        sendUpdateLocked(handle);
                        return;
                    }
                    // A chrome widget was clicked: dispatch its library-resolved
                    // command. CMD:<command_id>. The id comes from the published
                    // dynamic node state; an unknown id is rejected by dispatch with
                    // no side effect, so no separate validation is needed.
                    if (payload.rfind("CMD:", 0) == 0) {
                        std::string const command{payload.substr(4)};
                        std::lock_guard lock{*runtimeMutex};
                        if (*attachedHandle != handle) return;
                        if (!command.empty()) {
                            (void)runtime.dispatch(
                                client, {command, runtime.revision(), {}});
                        }
                        sendUpdateLocked(handle);
                        return;
                    }
                    auto const status = ProtocolCodec{}.decodeStatusActionInvocation(
                        message.data);
                    if (status.accepted()) {
                        std::lock_guard lock{*runtimeMutex};
                        if (*attachedHandle != handle) return;
                        (void)runtime.dispatch(
                            client, {"status.invoke_action", runtime.revision(),
                                     *status.invocation});
                        sendUpdateLocked(handle);
                        return;
                    }
                    // Submit the selected candidate by authoritative candidate id.
                    if (payload.rfind("PSUB:", 0) == 0) {
                        std::string const id{payload.substr(5)};
                        std::lock_guard lock{*runtimeMutex};
                        if (*attachedHandle != handle) return;
                        auto snapshot = runtime.snapshot(client);
                        if (snapshot && !id.empty()) {
                            auto const& palette = snapshot->sections().palette;
                            auto const found = std::find_if(
                                palette.candidates.begin(), palette.candidates.end(),
                                [&id](auto const& candidate) {
                                    return candidate.id == id;
                                });
                            if (found != palette.candidates.end()) {
                                if (auto const submit = paletteSubmitCommand(
                                        palette.mode, id)) {
                                    (void)runtime.dispatch(
                                        client, {submit->command,
                                                 runtime.revision(),
                                                 submit->payload});
                                }
                            }
                        }
                        sendUpdateLocked(handle);
                        return;
                    }
                    if (payload == "SNAP") {
                        std::lock_guard lock{*runtimeMutex};
                        if (*attachedHandle != handle) return;
                        sendSnapshotLocked(handle);
                        return;
                    }
                    auto const attach = decodeSessionAttachRequest(message.data);
                    if (!attach.accepted()) {
                        return;
                    }
                    std::lock_guard lock{*runtimeMutex};
                    if (*attachedHandle != 0) {
                        (void)server.send(
                            handle,
                            std::string{"a client is already attached"});
                        return;
                    }
                    *attachedHandle = handle;
                    sendSnapshotLocked(handle);
                },
            .onClose =
                [runtimeMutex, attachedHandle](Http::WebSocketHandle handle) {
                    // Only the attached connection closing detaches the client; an
                    // unattached or attach-refused socket closing must not.
                    std::lock_guard lock{*runtimeMutex};
                    if (*attachedHandle == handle) *attachedHandle = 0;
                },
        });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    runtime.primeDeferred();
    try {
        server.start();
    } catch (std::exception const& error) {
        std::fprintf(stderr, "ssg: --http failed: %s\n", error.what());
        return 1;
    }
    std::fprintf(stderr,
                 "ssg: serving http://127.0.0.1:%u/  (Ctrl-C to stop)\n",
                 static_cast<unsigned>(port));
    auto gitWakeDescriptor = [&runtime, runtimeMutex]() {
        std::lock_guard lock{*runtimeMutex};
        return runtime.gitDiffWakeDescriptor();
    };
    auto consumeGitWake = [&runtime, runtimeMutex]() {
        std::lock_guard lock{*runtimeMutex};
        int const fd = runtime.gitDiffWakeDescriptor();
        if (fd == -1) return;
        char scratch[64];
        while (true) {
            auto const count = ::read(fd, scratch, sizeof scratch);
            if (count <= 0) {
                if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }
                break;
            }
        }
    };
    auto broadcastGitUpdate = [&runtime, &server, client, prevSnapshot,
                               runtimeMutex, settledId, attachedHandle]() {
        std::lock_guard lock{*runtimeMutex};
        auto current = runtime.snapshot(client);
        if (!current) return;
        // Send under the lock so delta derivation, prevSnapshot advance, and the
        // write are one ordered step: a concurrent per-connection update cannot
        // interleave and deliver a later delta before an earlier one on the
        // ordered connection. Target only the attached connection -- an
        // authoritative delta must never reach an unattached or attach-refused
        // socket.
        const Http::WebSocketHandle target = *attachedHandle;
        if (target != 0 && prevSnapshot->has_value() &&
            (*prevSnapshot)->revision() != current->revision()) {
            auto delta =
                SessionSnapshotCodec{}.deriveDelta(**prevSnapshot, *current);
            auto frame = makeEnvelope(
                settledId->load(),
                {{kSectionLibraryBody,
                  ProtocolCodec{}.encodeSessionDelta(delta)}});
            (void)server.send(target, frame);
        }
        *prevSnapshot = std::move(current);
    };
    while (!g_stop.load()) {
        int const fd = gitWakeDescriptor();
        if (fd == -1) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        } else {
            pollfd gitWake{fd, POLLIN, 0};
            (void)::poll(&gitWake, 1, 100);
            consumeGitWake();
        }
        broadcastGitUpdate();
    }
    server.stop();
    return 0;
}

}  // namespace ssg::app
