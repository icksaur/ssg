#include "http_serve.h"

#include "pointer_routing.h"

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
#include <ssg/TreeModel.h>
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

// Reconstruct the shared routing state from the runtime's live focus, active
// prompt, and the published semantic PromptView -- its active input and that
// input's current value -- so append and deletion edit exactly the input the
// library considers active, for every footer prompt kind (find, replace, and the
// generic goto/save/settings prompts alike). The palette is the one client-owned
// prompt: its query lives only in the browser, so it stays a bare Palette with no
// server-held value and the seam appends client-side.
PromptRoutingState buildPromptRouting(SessionSnapshot const& snapshot) {
    auto const& sections = snapshot.sections();
    PromptRoutingState state;
    state.focus = sections.focus;
    if (sections.promptStatus.activeKind &&
        *sections.promptStatus.activeKind == PromptKind::Palette) {
        state.prompt = ActivePrompt::Palette;
        return state;
    }
    if (!sections.promptView) return state;
    auto const& view = *sections.promptView;
    switch (view.kind) {
    case PromptKind::Find:
        state.prompt = ActivePrompt::Find;
        break;
    case PromptKind::Replace:
        state.prompt = ActivePrompt::Replace;
        break;
    case PromptKind::Path:
    case PromptKind::Settings:
    case PromptKind::CommandArgument:
        state.prompt = ActivePrompt::TextPrompt;
        break;
    case PromptKind::Palette:
        return state;
    }
    state.activeInput = view.activeInput;
    std::size_t inputIndex = 0;
    for (auto const& control : view.controls) {
        if (control.kind != PromptControlKind::Input) continue;
        if (inputIndex == view.activeInput) {
            state.currentValue = control.value;
            break;
        }
        ++inputIndex;
    }
    return state;
}

// Apply one edit (append or backward delete) to the active prompt input through
// the shared seam, so the web host makes the exact routing and delete decision
// the TUI does. The palette query is the one client-owned derived view: the
// browser edits it locally, so an AppendPaletteQuery/Ignore result dispatches
// nothing here. Returns true when a library command was actually dispatched, so
// the caller settles a predicted edit only once it has genuinely been resolved.
bool routeEdit(EditorRuntime& runtime, ClientId client,
               PromptTextEdit const& change) {
    auto snapshot = runtime.snapshot(client);
    if (!snapshot) return false;
    auto const route = PromptTextRouter{}.edit(buildPromptRouting(*snapshot), change);
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

bool routeText(EditorRuntime& runtime, ClientId client, std::string const& text) {
    return routeEdit(runtime, client,
                     PromptTextEdit{PromptTextEdit::Kind::Append, text});
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
        focus = effectiveFocusFromSections(snapshot->sections());
    }
    auto const resolution =
        keymap.resolve(std::array{CompiledKeymap::compile(stroke)}, focus);
    if (resolution.kind == KeymapMatchKind::Resolved) {
        (void)runtime.dispatch(client,
                               {resolution.command, runtime.revision(), {}});
        return true;
    }
    // Backspace / Alt+Backspace reach a footer prompt as keycodes, not text; the
    // editor binds them in its own keymap context (resolved above), so an
    // unresolved Backspace here means a prompt owns the keyboard. Route it through
    // the same deletion seam the append path uses so grapheme/word semantics are
    // identical across hosts.
    if (stroke.code == KeyCode::Backspace) {
        return routeEdit(runtime, client,
                         PromptTextEdit{stroke.alt
                                            ? PromptTextEdit::Kind::DeleteWordBack
                                            : PromptTextEdit::Kind::DeleteGraphemeBack,
                                        {}});
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
        if (prevSnapshot->has_value() &&
            (*prevSnapshot)->revision() != current->revision()) {
            std::string body;
            try {
                auto delta =
                    SessionSnapshotCodec{}.deriveDelta(**prevSnapshot, *current);
                body = ProtocolCodec{}.encodeSessionDelta(delta);
            } catch (std::exception const&) {
                // Some authoritative transitions cannot be expressed as a delta
                // (a presentation-mode change, or a document appearing where the
                // previous snapshot had none). The delta is only an optimization:
                // re-sync the client with a full snapshot -- which every client
                // already accepts as a fresh base -- rather than dropping the
                // update or letting the exception escape and terminate the host.
                body = ProtocolCodec{}.encodeSessionSnapshot(*current);
            }
            *prevSnapshot = std::move(current);
            (void)server.send(
                handle, makeEnvelope(settledId->load(),
                                     {{kSectionLibraryBody, std::move(body)}}));
            return;
        }
        (void)server.send(handle, makeEnvelope(settledId->load(), {}));
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
                    // A single connection's malformed or unexpected message must
                    // never terminate the server process: an exception escaping this
                    // callback would propagate out of the http library's connection
                    // thread and std::terminate the host. Contain it here, log it,
                    // and keep serving.
                    try {
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
                    // A footer-prompt input was clicked: focus it so the next
                    // keystroke edits it. PFOC:<inputIndex>. An index not
                    // addressing an input is rejected by the command with no side
                    // effect, so no separate validation is needed.
                    if (payload.rfind("PFOC:", 0) == 0) {
                        auto const indexText = payload.substr(5);
                        std::lock_guard lock{*runtimeMutex};
                        if (*attachedHandle != handle) return;
                        std::size_t index = 0;
                        auto const parsed = std::from_chars(
                            indexText.data(), indexText.data() + indexText.size(),
                            index);
                        if (parsed.ec == std::errc{} &&
                            parsed.ptr == indexText.data() + indexText.size()) {
                            (void)runtime.dispatch(
                                client, {"prompt.focus_control", runtime.revision(),
                                         PromptFocusArguments{index}});
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
                    // Select and activate a tree node by its authoritative id, the
                    // same command pair a TUI pointer press on a tree row dispatches
                    // (see route_pointer's Panel branch): tree.select carries the
                    // node id, tree.activate then opens a file or toggles a
                    // directory. One behavior path -- a click and a keyboard
                    // select-then-Enter mean the same thing.
                    if (payload.rfind("TSEL:", 0) == 0) {
                        std::string const id{payload.substr(5)};
                        std::lock_guard lock{*runtimeMutex};
                        if (*attachedHandle != handle) return;
                        if (!id.empty()) {
                            // Activate only when the selection was accepted: an
                            // arbitrary/unknown client id must be a no-op, never
                            // activate whatever node was previously selected.
                            auto const selected = runtime.dispatch(
                                client,
                                {"tree.select", runtime.revision(),
                                 TreeSelectArguments{TreeNodeId{id}}});
                            if (selected.accepted()) {
                                (void)runtime.dispatch(
                                    client,
                                    {"tree.activate", runtime.revision(), {}});
                            }
                        }
                        sendUpdateLocked(handle);
                        return;
                    }
                    // A pointer click on an external-modification action: select
                    // its file then run the action (select-then-act, the same one
                    // behavior path the keyboard and the TUI pointer drive).
                    // EXMD:<action>\t<diffFileId>. The action token is fixed
                    // ({reload,keep_buffer,open_diff}); a TAB delimits it from the
                    // id, whose ENTIRE remainder is opaque and never split on ':'
                    // (the id is "external:"+path and contains colons). The (id,
                    // action) PAIR is validated against the published section --
                    // the id must be present AND the action must be one that file
                    // offers -- so an unknown id or an unoffered action is a no-op.
                    if (payload.rfind("EXMD:", 0) == 0) {
                        std::lock_guard lock{*runtimeMutex};
                        if (*attachedHandle != handle) return;
                        auto const frame =
                            ssg::app::parse_external_pointer_frame(payload);
                        auto snapshot = runtime.snapshot(client);
                        if (frame && snapshot &&
                            ssg::app::external_pointer_frame_is_offered(
                                *frame, snapshot->sections()
                                            .externalModification.files)) {
                            auto const selected = runtime.dispatch(
                                client, {"external.select", runtime.revision(),
                                         frame->id});
                            if (selected.accepted()) {
                                (void)runtime.dispatch(
                                    client, {frame->command, runtime.revision(),
                                             {}});
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
                    } catch (std::exception const& error) {
                        std::fprintf(stderr,
                                     "ssg: --http message handler error: %s\n",
                                     error.what());
                    }
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
    auto flushDueDrafts = [&runtime, runtimeMutex]() {
        std::lock_guard lock{*runtimeMutex};
        return runtime.flushDueAutosaveDrafts();
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
        // Persist due autosave drafts so single-file draft recovery (M15) works in
        // a web session exactly as in the terminal: a draft written now is what a
        // later reopen against changed disk classifies as a conflict.
        (void)flushDueDrafts();
        broadcastGitUpdate();
    }
    {
        // A clean shutdown flushes every dirty draft, capturing edits newer than
        // the last periodic flush, so no in-flight recovery draft is lost.
        std::lock_guard lock{*runtimeMutex};
        (void)runtime.flushAllAutosaveDrafts();
    }
    server.stop();
    return 0;
}

}  // namespace ssg::app
