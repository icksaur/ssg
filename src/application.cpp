#include <ssg/application.h>

#include <ssg/pointer_routing.h>
#include <ssg/ssg_terminal.h>

#include <ssg/EditorSession.h>
#include <ssg/GraphemeLayout.h>
#include <ssg/TreeSitterGrammars.h>
#include <ssg/HitTester.h>
#include <ssg/ScriptHost.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/Picker.h>
#include <ssg/platform_files.h>
#include <ssg/TextInputCommands.h>

#include <ssg/InitScriptWatcher.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>

#include <csignal>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace ssg::app {

namespace fs = std::filesystem;

#ifdef SSG_STARTUP_TRACE_ENABLED
void recordStartupMark(char const* phase) {
    static char const* const path = std::getenv("SSG_STARTUP_TRACE");
    if (path == nullptr) return;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long const ns =
        static_cast<long long>(now.tv_sec) * 1'000'000'000LL + now.tv_nsec;
    // seam-exempt: append-only diagnostic trace, not file content access
    if (std::FILE* file = std::fopen(path, "a"); file != nullptr) {
        std::fprintf(file, "%s %lld\n", phase, ns);
        std::fclose(file);
    }
}
#else
void recordStartupMark(char const*) {}
#endif

namespace {

constexpr int kEscapeTimeoutMs = 30;
constexpr int kEdgeScrollIntervalMs = 40;
constexpr int kAutosaveTickMs = 1000;
constexpr int kDragFrameIntervalMs = 16;
volatile std::sig_atomic_t gSignalPipeWrite = -1;

struct FdReadiness {
    bool input = false;
    bool signal = false;
    bool gitDiff = false;
    bool initScript = false;
};

FdReadiness waitReadiness(int timeoutMs, int signalFd, int gitDiffFd,
                          int initScriptFd = -1) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    if (signalFd >= 0) FD_SET(signalFd, &set);
    int maxFd = std::max(STDIN_FILENO, signalFd);
    if (gitDiffFd >= 0) {
        FD_SET(gitDiffFd, &set);
        maxFd = std::max(maxFd, gitDiffFd);
    }
    if (initScriptFd >= 0) {
        FD_SET(initScriptFd, &set);
        maxFd = std::max(maxFd, initScriptFd);
    }
    timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int const ready =
        ::select(maxFd + 1, &set, nullptr, nullptr,
                 timeoutMs < 0 ? nullptr : &timeout);
    if (ready <= 0) return {};
    return {
        FD_ISSET(STDIN_FILENO, &set) != 0,
        signalFd >= 0 ? FD_ISSET(signalFd, &set) != 0 : false,
        gitDiffFd >= 0 ? FD_ISSET(gitDiffFd, &set) != 0 : false,
        initScriptFd >= 0 ? FD_ISSET(initScriptFd, &set) != 0 : false};
}

extern "C" void signalTagHandler(int signo) {
    int const fd = gSignalPipeWrite;
    if (fd < 0) return;
    unsigned char const tag = static_cast<unsigned char>(signo);
    ssize_t const written = ::write(fd, &tag, 1);
    (void)written;
}

void installSignalTagHandler(int signo) {
    struct sigaction action{};
    action.sa_handler = signalTagHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(signo, &action, nullptr);
}

void popGrapheme(std::string& text) {
    if (text.empty()) return;
    const auto run = ssg::GraphemeLayout{}.computeRun(text);
    if (!run.spans.empty()) text.resize(run.spans.back().byteOffset);
}

void popWord(std::string& text) {
    while (!text.empty() &&
           !ssg::isWordByte(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    while (!text.empty() &&
           ssg::isWordByte(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
}

}  // namespace

LaunchArguments parseArguments(int argc, char** argv) {
    LaunchArguments arguments;
    int firstOperand = 1;
    if (argc > 1 && std::string_view{argv[1]} == "--") {
        firstOperand = 2;
    }
    if (argc > firstOperand) arguments.argument = argv[firstOperand];
    return arguments;
}

Application::Application(LaunchArguments arguments) {
    auto target = resolve_launch(arguments.argument);

    fs::path stateBase;
    if (const char* stateOverride = std::getenv("SSG_STATE_DIR");
        stateOverride != nullptr && *stateOverride != '\0' &&
        fs::path{stateOverride}.is_absolute()) {
        stateBase = stateOverride;
    } else {
        stateBase = ssg::userStateRoot("ssg");
    }
    std::error_code code;
    fs::create_directories(stateBase / "scratch", code);
    fs::create_directories(stateBase / "archive", code);
    for (const auto& dir : {stateBase, stateBase / "scratch", stateBase / "archive"}) {
        std::error_code permissionError;
        if (fs::is_directory(dir, permissionError)) {
            try {
                ssg::setOwnerOnlyPermissions(dir);
            } catch (const std::exception&) {
            }
        }
    }

    auto recoveryBase =
        fs::temp_directory_path() / ("ssg-" + std::to_string(::getpid()));
    fs::create_directories(recoveryBase / "recovery", code);

    ssg::EditorSessionConfig config;
    config.cwd = target.cwd;
    config.scratchRoot = stateBase / "scratch";
    config.recoveryRoot = recoveryBase / "recovery";
    config.archiveRoot = stateBase / "archive";
    config.deferEnrichment = true;
    config.syntaxParser = ssg::TreeSitterParserFactory::createDefault();
    auto created = ssg::EditorSession::create(config);
    if (!created.accepted()) {
        std::fprintf(stderr, "ssg: %s\n", created.message.c_str());
        return;
    }
    runtime = std::move(created.session);
    gitDiffWakeFd = runtime->gitDiffWakeDescriptor();
    recordStartupMark("post_create");

    gridPresenter.emplace();
    recordStartupMark("post_presenter_init");
    scripts.emplace(*runtime, [this](ssg::ViewAction const& request) {
        auto frame = gridPresenter->project(*runtime, {terminalSize(), {}});
        if (!frame) {
            return ssg::ViewActionResult{
                ssg::ViewActionStatus::Rejected, std::nullopt,
                "view action has no current grid frame"};
        }
        return gridPresenter->apply(request, *frame);
    });
    auto const appliedInitScript = loadInitScript(*scripts, *runtime);
    if (auto scriptPath = resolveInitScriptPath()) {
        initScriptWatcher.emplace(*scriptPath, appliedInitScript);
    }

    if (target.file) {
        if (fs::exists(target.cwd / *target.file)) {
            auto const openResult = runtime->dispatch({"file.open", *target.file});
            startsWithAnEditableDocument = openResult.accepted();
            openedNamedFile = openResult.accepted();
        }
    }
    if (!startsWithAnEditableDocument) {
        startsWithAnEditableDocument = runtime->dispatch({"file.new", {}}).accepted();
    }
    recordStartupMark("post_open");

    terminal.emplace();
    if (!terminal->active()) {
        std::fprintf(stderr, "ssg: stdin/stdout is not an interactive terminal\n");
        return;
    }
    ready = true;
}

int Application::runEventLoop() {
    if (!ready) return 1;
    auto& runtime = *this->runtime;
    auto& gridPresenter = *this->gridPresenter;
    auto& scripts = *this->scripts;
    auto& mode = *this->terminal;
    auto& initScriptWatcher = this->initScriptWatcher;
    int const gitDiffWakeFd = this->gitDiffWakeFd;
    bool const startsWithAnEditableDocument = this->startsWithAnEditableDocument;
    bool const openedNamedFile = this->openedNamedFile;
    int signalPipe[2] = {-1, -1};
    if (::pipe(signalPipe) != 0) {
        std::fprintf(stderr, "ssg: failed to create signal pipe\n");
        return 1;
    }
    ::fcntl(signalPipe[0], F_SETFL,
            ::fcntl(signalPipe[0], F_GETFL, 0) | O_NONBLOCK);
    ::fcntl(signalPipe[1], F_SETFL,
            ::fcntl(signalPipe[1], F_GETFL, 0) | O_NONBLOCK);
    gSignalPipeWrite = signalPipe[1];
    installSignalTagHandler(SIGWINCH);
    installSignalTagHandler(SIGTERM);
    installSignalTagHandler(SIGHUP);

    ssg::app::TerminalCapabilities capabilities{
        [](std::string_view name) { return std::getenv(std::string{name}.c_str()); }};
    ssg::ColorDepth const colorDepth = capabilities.colorDepth();
    writeAll(capabilities.beginProbe());

    auto drainSignals = [&]() -> void {
        char scratch[64];
        std::string tags;
        for (;;) {
            auto const n = ::read(signalPipe[0], scratch, sizeof scratch);
            if (n <= 0) break;
            tags.append(scratch, static_cast<std::size_t>(n));
        }
        auto const events = ssg::app::classify_signal_tags(tags);
        if (events.terminate) {
            int const signo = *events.terminate;
            mode.restore();
            ::signal(signo, SIG_DFL);
            ::raise(signo);
        }
    };

    std::string buffer;             // Raw bytes read but not yet decoded.
    bool quit = false;
    auto focus = ssg::FocusTarget::Editor;
    // Client-owned palette state: query and selection are local (reported for
    // library presentation), ranked against the server's published candidates.
    bool pickerOpen = false;
    // Which picker the open prompt is, adopted from the snapshot rather than
    // invented locally, so submit routes to that picker's command.
    ssg::SearchMode pickerMode = ssg::SearchMode::Command;
    std::optional<ssg::PickerActivation> pickerActivation;
    // One value, not four parallel locals: the library's own window type carries
    // query, selection, scroll offset and pane height together, so a second
    // picker cannot introduce a drifting copy of half of them.
    // The pane height is cached from the last snapshot (the palette pane == the
    // editor pane, so it is populated before any picker opens). The window is
    // resolved with the shared list-scroll
    // primitive.
    ssg::PaletteWindowState picker{};
    // Native drag tracking retains only the device gesture anchor. Selection
    // policy and authoritative gesture state live in EditorSession::input.
    bool dragging = false;
    std::optional<ssg::DocumentPosition> dragAnchor;
    // Alt-drag state is fixed at press. The library owns the baseline selection
    // set and all multi-cursor policy for the gesture.
    bool altDrag = false;
    // Which scrollbar gutter a press landed on, held until release.  A drag is
    // routed to THIS gutter regardless of where the pointer has since moved, and
    // the cursor is hidden while it is set: during a drag the caret is not what
    // the user is looking at, and the terminal cursor otherwise flickers across
    // the screen chasing the frame's caret position.
    // A live scrollbar-thumb drag: the region, the gutter's top row, the thumb's
    // travel (viewportRows - thumbSize) and the offset from the thumb top to the
    // grabbed point, all captured at press.  Held so each drag motion moves the
    // grabbed point of the thumb to the cursor, rather than mapping the absolute
    // pointer row to the scroll position.
    struct GutterDrag {
        ssg::HitRegion region;
        int gutterY;
        int travel;
        int grabOffset;
    };
    std::optional<GutterDrag> draggingGutter;
    // Double-click tracking (client-side: the terminal reports no click count).
    // A second left editor press on the same cell within the window selects the
    // word.
    ssg::app::ClickTracker clickTracker;
    static constexpr auto kDoubleClickWindow = std::chrono::milliseconds{400};
    // The last pointer cell (0-based) from a press/drag, so a drag held still at
    // the editor edge can auto-scroll on a timer without a fresh pointer event
    // (M8-S2).
    int lastPointerColumn = 0;
    int lastPointerRow = 0;
    // The last clipboard write served to the terminal, so one copy produces one
    // OSC 52 rather than one per frame for as long as it stays published.
    ssg::app::SystemClipboardWriter clipboardWriter;
    std::vector<ssg::PaletteCandidate> candidates;

    // Re-center the client-owned palette window on the current selection
    // (keep-visible). Called ONLY when the selection changes (arrow navigation,
    // open, type, backspace); the per-frame build_report otherwise honors the
    // free offset so a wheel scroll persists. Mirrors
    // the tree's reveal_tree_selection.
    auto revealPaletteSelection = [&] {
        auto order = ssg::PaletteSearcher{}.rank(candidates, picker.query);
        if (picker.selected >= order.size()) {
            picker.selected = order.empty() ? 0 : order.size() - 1;
        }
        std::optional<std::uint32_t> selected =
            order.empty() ? std::nullopt
                          : std::optional<std::uint32_t>{
                                static_cast<std::uint32_t>(picker.selected)};
        if (!selected) return;
        ssg::ScrollOffset offset{picker.firstVisible};
        offset.revealSelection(*selected,
                               static_cast<std::uint32_t>(order.size()),
                               picker.paneRows);
        picker.firstVisible = offset.firstVisible();
    };
    // Scroll the client-owned palette window by `delta` rows WITHOUT moving the
    // selection (a wheel over the open palette). The clamp-and-shift is the same
    // ScrollOffset the editor and tree use; only the ownership differs.
    auto scrollPalette = [&](std::int64_t delta) {
        if (!pickerOpen) return;
        auto order = ssg::PaletteSearcher{}.rank(candidates, picker.query);
        ssg::ScrollOffset offset{picker.firstVisible};
        offset.byLines(delta, static_cast<std::uint32_t>(order.size()),
                       picker.paneRows);
        picker.firstVisible = offset.firstVisible();
    };
    // Position the client-owned palette window along its track, as from a gutter
    // click or thumb drag. The picker's counterpart to view.scroll_to_fraction,
    // kept client-local for the same latency reason its ranking is.
    auto scrollPaletteToFraction = [&](std::uint32_t numerator,
                                       std::uint32_t denominator) {
        if (!pickerOpen) return;
        auto order = ssg::PaletteSearcher{}.rank(candidates, picker.query);
        ssg::ScrollOffset offset{picker.firstVisible};
        offset.toFraction(numerator, denominator,
                          static_cast<std::uint32_t>(order.size()),
                          picker.paneRows);
        picker.firstVisible = offset.firstVisible();
    };
    auto submitSelectedCandidate = [&] {
        auto order = ssg::PaletteSearcher{}.rank(candidates, picker.query);
        if (order.empty() || picker.selected >= order.size()) return;
        auto const& id = candidates[order[picker.selected]].id;
        if (pickerActivation) {
            auto result = runtime.input(
                ssg::PickerPointerInput{*pickerActivation, id});
        }
    };
    auto applyClientOwnedInput = [&](ssg::ClientOwnedInput const& input) {
        switch (input.kind) {
        case ssg::ClientOwnedInputKind::AppendText:
            picker.query += input.text;
            picker.selected = 0;
            revealPaletteSelection();
            break;
        case ssg::ClientOwnedInputKind::DeleteGraphemeBackward:
            popGrapheme(picker.query);
            picker.selected = 0;
            revealPaletteSelection();
            break;
        case ssg::ClientOwnedInputKind::DeleteWordBackward:
            popWord(picker.query);
            picker.selected = 0;
            revealPaletteSelection();
            break;
        case ssg::ClientOwnedInputKind::SelectNext:
            ++picker.selected;
            revealPaletteSelection();
            break;
        case ssg::ClientOwnedInputKind::SelectPrevious:
            if (picker.selected > 0) --picker.selected;
            revealPaletteSelection();
            break;
        case ssg::ClientOwnedInputKind::Submit:
            submitSelectedCandidate();
            break;
        }
    };
    auto buildReport = [&] {
        ssg::PaletteReport report;
        if (pickerOpen) {
            // The library owns the palette projection: rank the published
            // candidates, window them, and assemble the bounded report. The app
            // supplies only the client-owned window (query/selection/scroll) and
            // adopts back the clamped selection and resolved offset, inventing no
            // product data (INV-derived-view-bounded).
            report = ssg::PaletteSearcher{}.report(candidates, picker);
        }
        return report;
    };
    // Take a fresh snapshot and adopt its authoritative view state.
    auto refresh = [&]() -> std::optional<ssg::GridPresentation> {
        auto snapshot = gridPresenter.project(
            runtime, {terminalSize(), buildReport()});
        if (snapshot) {
            focus = ssg::effectiveUiFocus(snapshot->uiTree);
            pickerActivation = snapshot->paletteView.activePicker;
            pickerMode = pickerActivation
                             ? pickerActivation->mode
                             : ssg::SearchMode::Command;
            if (auto const* published =
                    snapshot->paletteView.candidatesFor(pickerMode)) {
                candidates = *published;
            } else {
                candidates.clear();
            }
            // Cache the palette pane height for the next window computation: the
            // palette pane is the editor pane, so this is populated every frame,
            // including before the palette opens (no cold start). The window is
            // computed from the PREVIOUS frame's height, so a terminal resize
            // lags one frame before keep-visible re-settles — the same one-frame
            // clamp the editor's server-side scroll offset already has, and it
            // self-corrects on the next snapshot.
            if (snapshot->document) {
                picker.paneRows = static_cast<std::uint32_t>(
                    std::max(snapshot->document->content.height, 1));
            }
            // A copy or cut offers its text for the SYSTEM clipboard.  Serve it
            // with OSC 52, which over SSH is the only way the remote editor can
            // reach the local clipboard at all.  Fire and forget: keyed by id so
            // one copy is written once, with nothing reported back, and skipped
            // entirely when the terminal did not advertise the capability --
            // where it would be an unrecognised sequence rather than a copy.
            if (auto const bytes = clipboardWriter.bytesFor(
                    snapshot->clipboardWrite,
                    capabilities.has(ssg::app::Capability::ClipboardWrite))) {
                writeAll(*bytes);
            }
            // Derive find fulfillment from the ACTIVE prompt kind, not merely the
            // controller being open under prompt focus: a palette/settings prompt
            // may be active while the find controller is still open, and find
            // fulfillment must not hijack that unrelated prompt's keys.
            auto const& findView = snapshot->findReplace;
            // The AUTHORITATIVE active-prompt kind: present even for a
            // header-hosted prompt (palette / file
            // finder) whose query renders in the header input line and so
            // produces no footer semantic prompt view. Deriving the open-flags
            // from this -- rather than from `promptStatus.prompt->kind`, which is
            // nullopt for a header-hosted prompt -- is what lets typed text reach
            // the picker query.
            auto const activeKind = snapshot->promptStatus.activeKind;
            bool const wasPickerOpen = pickerOpen;
            pickerOpen = activeKind == ssg::PromptKind::Palette;
            if (pickerOpen && !wasPickerOpen) {
                picker.query.clear();
                picker.selected = 0;
                picker.firstVisible = 0;
            }
        }
        return snapshot;
    };
    std::optional<ssg::GridPresentation> activeSnapshot;
    auto handleInputResult = [&](ssg::ClientInputResult result) {
        if (result.command) {
            activeSnapshot.reset();
        }
        if (result.clientOwned) {
            applyClientOwnedInput(*result.clientOwned);
        }
        if (result.outcome != ssg::ClientInputOutcome::ViewOwned ||
            !result.command || !result.command->viewAction) {
            return result.outcome;
        }
        if (!activeSnapshot) activeSnapshot = refresh();
        if (!activeSnapshot) return ssg::ClientInputOutcome::Rejected;
        auto applied = gridPresenter.apply(*result.command->viewAction,
                                           *activeSnapshot);
        if (!applied.accepted()) return ssg::ClientInputOutcome::Rejected;
        if (applied.transition) {
            auto transition = runtime.input(*applied.transition);
            if (transition.outcome == ssg::ClientInputOutcome::Rejected) {
                return transition.outcome;
            }
        }
        activeSnapshot.reset();
        return result.outcome;
    };
    auto routeInput = [&](ssg::KeyStroke stroke, std::string text) {
        return handleInputResult(runtime.input(
            ssg::ClientKeyInput{stroke, std::move(text)}));
    };

    // Top-level boundary (M9-X): an exception escaping the loop is not portably
    // guaranteed to unwind `mode` once past main, so restore the terminal here
    // before it propagates.
    bool firstFrameMarked = false;
    auto lastFrameAt = std::chrono::steady_clock::time_point{};
    // Borrowed by Renderer::render to reuse shaped on-screen lines across frames.
    ssg::LineLayoutCache renderLineCache;
    try {
        while (!quit) {
            // Render-rate cap while a drag is held: a scrollbar or selection drag
            // floods motion events, and rendering a full frame per event pins a
            // core. When a drag is active and the previous frame was less than one
            // drag-frame interval ago, wait out the remainder so the queued motion
            // events accumulate between rendered frames.
            if (draggingGutter.has_value() || dragging) {
                const auto sinceFrame =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - lastFrameAt)
                        .count();
                if (sinceFrame < kDragFrameIntervalMs) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{
                        kDragFrameIntervalMs - sinceFrame});
                }
            }
            lastFrameAt = std::chrono::steady_clock::now();
            activeSnapshot = refresh();
            auto& snapshot = activeSnapshot;
            if (snapshot) {
                // The library renders every screen branch, including the declined-
                // layout "too small" placeholder (M11-L); the app only encodes.
                auto grid = ssg::Renderer{}.render(*snapshot, &renderLineCache);
                std::string frame = ssg::app::encode_frame(
                    grid, colorDepth, !draggingGutter.has_value());
                if (!firstFrameMarked) {
                    // Mark the first rendered payload, not terminal-setup bytes.
                    recordStartupMark("first_content_frame");
                    firstFrameMarked = true;
                    writeAll(frame);
                    // Run deferred enrichment after the first frame is visible.
                    // It publishes on the next snapshot at the loop top.
                    runtime.primeDeferred();
                    // Open the Files sidebar only when startup did NOT open a
                    // named file: with nothing but an empty buffer the sidebar is
                    // the only thing worth showing, but over a file the user
                    // asked for by name it just covers the text they came to
                    // edit.  Uses the SAME panel.show_files command Escape-B and
                    // Escape-O reach interactively; this is not a new mechanism,
                    // only sequenced AFTER primeDeferred() rather than before it:
                    // dispatching it earlier (pre-loop, before the deferred tree
                    // scan has run) would fail with "files tree provider is
                    // unavailable" because the fast-startup path defers
                    // registering that provider until exactly this point.
                    if (!openedNamedFile) {
                        if (auto const panelResult = runtime.dispatch(
                                {"panel.show_files", {}});
                            !panelResult.accepted()) {
                            std::fprintf(stderr,
                                         "ssg: could not open Files sidebar: %s\n",
                                         panelResult.message.c_str());
                        }
                    }
                    // panel.show_files moves focus to the panel as a side effect
                    // (showing a panel focuses it), so editor
                    // focus is re-asserted here, AFTER it, to be the final word.
                    if (startsWithAnEditableDocument) {
                        runtime.focusEditor();
                    }
                    continue;
                }
                writeAll(frame);
            }

            // A read buffer large enough to pull a whole burst of terminal input
            // (a mouse-drag motion flood, or a paste) in one read, so the inner
            // decode loop can drop intermediate drag events from one read instead
            // of rendering once per 64-byte chunk.
            char bytes[4096];
            // Edge auto-scroll (M8-S2): if a drag is held past the top/bottom of the
            // editor content, don't block indefinitely on input — wake on a timer to
            // scroll one line and re-extend the selection to the new edge cell, so a
            // drag held still at the edge keeps scrolling and selecting.
            std::optional<int> dragEdge;
            if (dragging && snapshot && snapshot->document) {
                dragEdge = ssg::app::edge_scroll(
                    dragging, lastPointerRow,
                    snapshot->document->content);
            }
            if (dragEdge) {
                auto const ready = waitReadiness(kEdgeScrollIntervalMs, signalPipe[0], -1);
                if (ready.signal) {
                    // A resize/terminate signal arrived mid-drag: drain it now rather
                    // than deferring until the drag releases.  Terminate does not
                    // return (restore + re-raise); resize re-snapshots at the loop
                    // top with the drag preserved.
                    drainSignals();
                    continue;
                }
                if (!ready.input) {
                    (void)handleInputResult(runtime.input(
                        ssg::DocumentPointerInput{
                                    std::nullopt, false, false,
                                    ssg::InputPointerButton::Primary,
                                    ssg::InputPointerPhase::Move,
                                    *dragEdge < 0
                                        ? ssg::DocumentPointerEdge::Before
                                        : ssg::DocumentPointerEdge::After}));
                    continue;  // re-render with the scrolled viewport, then re-evaluate
                }
                // Keyboard input arrived during the drag: fall through and read it.
            }
            // Block until keyboard input OR a signal-driven self-pipe wake (M9-W)
            // OR a runtime git-diff wake OR an init-script reload wake. A bare
            // read() could not be interrupted reliably by resize/terminate or
            // background diff/config updates.
            // Block until a real event (input / signal / background wake), but
            // wake on the autosave cadence to flush due drafts. A pure-timeout
            // tick that writes nothing must NOT repaint (encode_frame is a full
            // frame, so an idle repaint every second is wasteful) — keep waiting
            // without re-rendering. Only a flush that actually wrote something
            // falls through to re-render, since it may change a recovery badge.
            FdReadiness wait;
            while (true) {
                wait = waitReadiness(
                    kAutosaveTickMs, signalPipe[0], gitDiffWakeFd,
                    initScriptWatcher ? initScriptWatcher->wakeDescriptor() : -1);
                if (wait.input || wait.signal || wait.gitDiff || wait.initScript) {
                    break;
                }
                if (runtime.flushDueAutosaveDrafts() > 0) break;
            }
            if (wait.signal) {
                // Terminate does not return (restore + re-raise); a resize just
                // re-snapshots at the loop top.
                drainSignals();
                if (!wait.input) continue;
            }
            if (wait.initScript) {
                // Worker completion is accepted only by EditorSession::pump();
                // nothing else drains this -- evaluate the reloaded script here,
                // on the main thread, exactly like startup's loadInitScript.
                initScriptWatcher->drainAndEvaluate(scripts, runtime);
                if (!wait.input) continue;
            }
            if (wait.gitDiff) {
                (void)runtime.pump();
                if (!wait.input) continue;
            }
            if (!wait.input) continue;  // Wake-only cycle: re-render and retry.
            auto readBytes = ::read(STDIN_FILENO, bytes, sizeof bytes);
            if (readBytes <= 0) break;
            buffer.append(bytes, static_cast<std::size_t>(readBytes));

        while (!buffer.empty() && !quit) {
            std::size_t consumed = 0;
            auto decoded = ssg::app::decode_input(buffer, false, consumed);
            if (decoded.status == ssg::app::DecodeStatus::incomplete) {
                // A partial sequence (lone ESC or truncated CSI) remains.  Wait
                // briefly for the disambiguating bytes; if none arrive, force the
                // bounded-Escape resolution.  The wait also watches the signal
                // pipe so a resize/terminate is not deferred by a lone ESC.
                auto const ready = waitReadiness(kEscapeTimeoutMs, signalPipe[0], -1);
                if (ready.signal) drainSignals();
                if (ready.input) {
                    auto more = ::read(STDIN_FILENO, bytes, sizeof bytes);
                    if (more > 0) {
                        buffer.append(bytes, static_cast<std::size_t>(more));
                        continue;
                    }
                }
                decoded = ssg::app::decode_input(buffer, true, consumed);
                if (decoded.status == ssg::app::DecodeStatus::incomplete) break;
            }
            buffer.erase(0, consumed);

            // A terminal report answering a startup query, not something the user
            // typed.  It is consumed either way; whether it is believed is the
            // capability object's decision, not this loop's.
            if (decoded.status == ssg::app::DecodeStatus::reply) {
                capabilities.observeReply(decoded.reply);
                // The moment the terminal confirms the keyboard protocol, enable
                // it (latched).  Gated solely on the capability -- a terminal that
                // never answers never gets the enable, which is the whole escape
                // hatch (INV-capability-single-source).
                if (capabilities.has(ssg::app::Capability::KeyboardProtocol)) {
                    mode.enableKeyboardProtocol();
                }
                continue;
            }

            // Coalesce a burst of pointer drag events: only the final position
            // matters for a scrollbar drag or a drag-select, and taking a full
            // snapshot and encoding a full frame for every motion event the
            // terminal emits at motion rate spikes CPU. If this is a drag and
            // another pointer event is already buffered behind it, drop this one;
            // the last drag in the run is processed normally, so the end position
            // is honoured without the intermediate work.
            if (decoded.status == ssg::app::DecodeStatus::pointer &&
                decoded.pointer.kind == ssg::app::PointerKind::drag) {
                std::size_t peekConsumed = 0;
                const auto next =
                    ssg::app::decode_input(buffer, false, peekConsumed);
                if (next.status == ssg::app::DecodeStatus::pointer &&
                    next.pointer.kind == ssg::app::PointerKind::drag) {
                    continue;
                }
            }

            snapshot = refresh();

            if (decoded.status == ssg::app::DecodeStatus::pointer) {
                lastPointerColumn = decoded.pointer.column;
                lastPointerRow = decoded.pointer.row;
                // Classify the cell via the library hit_test, resolve the target
                // the hit needs, then let the pure route_pointer decide the
                // command sequence and drag-state change.
                ssg::RegionHit hit;
                ssg::app::PointerTargets targets;
                std::optional<ssg::DocumentPosition> doubleClickPosition;
                if (snapshot) {
                    ssg::HitTester tester{*snapshot};
                    if (draggingGutter &&
                        decoded.pointer.kind == ssg::app::PointerKind::drag) {
                        // Continue the drag using the offset captured at press:
                        // the grabbed point of the thumb tracks the cursor.  The
                        // pointer may wander off the one-column gutter
                        // horizontally without dropping the drag, so this keys off
                        // the held region, never a fresh column hit-test.
                        hit = ssg::RegionHit{};
                        hit.region = draggingGutter->region;
                        int const rel =
                            decoded.pointer.row - draggingGutter->gutterY;
                        auto const fraction = ssg::app::gutter_fraction(
                            rel, draggingGutter->grabOffset,
                            draggingGutter->travel);
                        hit.scrollNumerator = fraction.numerator;
                        hit.scrollDenominator = fraction.denominator;
                    } else {
                        hit = tester.at(decoded.pointer.column,
                                        decoded.pointer.row);
                        // A left press on a gutter grabs the thumb: capture the
                        // grab offset and replace the raw absolute-row fraction
                        // with the grab fraction, so the press and the drag that
                        // follows compute the position the same way.
                        const bool leftPress =
                            decoded.pointer.kind == ssg::app::PointerKind::press &&
                            decoded.pointer.button == ssg::app::PointerButton::left;
                        if (leftPress &&
                            ssg::app::is_scrollbar_region(hit.region)) {
                            if (auto const thumb = tester.gutterThumb(hit.region)) {
                                int const rel = decoded.pointer.row - thumb->gutterY;
                                int const travel =
                                    static_cast<int>(thumb->viewportRows) -
                                    static_cast<int>(thumb->thumbSize);
                                int const grabOffset =
                                    ssg::app::scrollbar_grab_offset(
                                        rel, static_cast<int>(thumb->thumbStart),
                                        static_cast<int>(thumb->thumbSize));
                                draggingGutter = GutterDrag{hit.region,
                                                            thumb->gutterY, travel,
                                                            grabOffset};
                                auto const fraction = ssg::app::gutter_fraction(
                                    rel, grabOffset, travel);
                                hit.scrollNumerator = fraction.numerator;
                                hit.scrollDenominator = fraction.denominator;
                            }
                        } else if (leftPress) {
                            draggingGutter.reset();
                        }
                    }
                    if (hit.region == ssg::HitRegion::Editor) {
                        targets.document_position = ssg::SelectionNavigator::resolvePosition(
                            snapshot->documentText,
                            ssg::ByteOffset{hit.byteOffset});
                    } else if (hit.region == ssg::HitRegion::Tab) {
                        auto const& tabs = snapshot->tabs.tabs;
                        if (hit.tabIndex < tabs.size()) {
                            targets.tab_id = tabs[hit.tabIndex].id;
                        }
                    } else if (hit.region == ssg::HitRegion::Palette) {
                        // Map the absolute rank index to its candidate id using
                        // the same ranked order the client renders.
                        auto order = ssg::PaletteSearcher{}.rank(candidates, picker.query);
                        if (hit.itemIndex < order.size()) {
                            targets.picker_candidate_id =
                                candidates[order[hit.itemIndex]].id;
                            targets.picker_activation = pickerActivation;
                        }
                    } else if (hit.region == ssg::HitRegion::HeaderField ||
                               hit.region == ssg::HitRegion::FooterField) {
                        if (hit.fieldId) {
                            targets.ui_node_id = ssg::UiNodeId{*hit.fieldId};
                        }
                    } else if (hit.region == ssg::HitRegion::NoticeAction) {
                        targets.notice_action_id = hit.fieldId;
                    } else if (hit.region == ssg::HitRegion::ExternalAction &&
                               hit.externalFileId && hit.commandId) {
                        const auto fileId = ssg::DiffFileId{*hit.externalFileId};
                        for (auto const& file :
                             snapshot->externalModification.files) {
                            if (file.id != fileId) continue;
                            for (auto const& action : file.actions) {
                                if (action.command == *hit.commandId) {
                                    targets.external_invocation =
                                        ssg::ExternalActionInvocation{
                                            file.id, action.action};
                                    break;
                                }
                            }
                        }
                    }
                    // A second left click on the same editor cell within the
                    // window selects the word there instead of just placing the
                    // caret. Detected here so the
                    // word geometry stays server-side; the client only recognizes
                    // the gesture. The active tab id keys the tracker so a fast
                    // click on the same cell of a DIFFERENT document does not pair.
                    const bool leftEditorPress =
                        decoded.pointer.kind == ssg::app::PointerKind::press &&
                        decoded.pointer.button == ssg::app::PointerButton::left &&
                        hit.region == ssg::HitRegion::Editor;
                    if (leftEditorPress && targets.document_position &&
                        ssg::app::register_click_is_double(
                            clickTracker, std::chrono::steady_clock::now(),
                            snapshot->tabs.active
                                ? snapshot->tabs.active->value()
                                : 0,
                            decoded.pointer.row, decoded.pointer.column,
                            kDoubleClickWindow)) {
                        doubleClickPosition = targets.document_position;
                    }
                }
                // A recognized double-click selects the word (no drag); every
                // other press goes through the normal router. Both yield a
                // PointerDispatch handled uniformly below. The router decides the
                // command from the EFFECTIVE Alt: a press establishes the gesture
                // from its own Alt bit; a drag/release reuses the established
                // altDrag so a dropped modifier mid-drag cannot flip it.
                bool const effectiveAlt =
                    decoded.pointer.kind == ssg::app::PointerKind::press
                        ? decoded.pointer.alt
                        : altDrag;
                auto plan =
                    doubleClickPosition
                        ? ssg::app::double_click_dispatch(*doubleClickPosition)
                        : ssg::app::route_pointer(
                              hit, decoded.pointer.button, decoded.pointer.kind,
                              effectiveAlt, dragging, dragAnchor, targets);
                if (plan.semantic_input) {
                    (void)handleInputResult(
                        runtime.input(*plan.semantic_input));
                }
                if (plan.command) {
                    (void)runtime.dispatch(*plan.command);
                }
                // A gutter gesture on a client-owned surface has no command to
                // dispatch (the picker's offset must not round-trip), so the
                // loop applies it here. The switch is exhaustive over the
                // targets a client-owned descriptor can name, so adding one
                // without handling it is a visible gap rather than a silently
                // dropped gesture.
                if (plan.client_scroll) {
                    switch (plan.client_scroll->target) {
                        case ssg::app::WheelTarget::palette:
                            scrollPaletteToFraction(
                                plan.client_scroll->numerator,
                                plan.client_scroll->denominator);
                            break;
                        case ssg::app::WheelTarget::editor:
                        case ssg::app::WheelTarget::tree:
                        case ssg::app::WheelTarget::none:
                            // Server-owned surfaces dispatch a command instead
                            // and never reach here; `none` is not scrollable.
                            break;
                    }
                }
                if (plan.begins_drag) {
                    dragging = true;
                    dragAnchor = targets.document_position;
                    altDrag = effectiveAlt;
                }
                // A gutter thumb drag ends on any release; the press that starts
                // it, and the grab offset it captures, are handled where `hit` is
                // classified above.
                if (decoded.pointer.kind == ssg::app::PointerKind::release) {
                    draggingGutter.reset();
                }
                if (plan.ends_drag) {
                    dragging = false;
                    dragAnchor.reset();
                    altDrag = false;
                }
                continue;
            }

            if (decoded.status == ssg::app::DecodeStatus::paste) {
                // Pasted bytes are CONTENT.  They go straight to the text sink
                // without touching the keymap, so a newline in the paste cannot
                // fire whatever Enter is bound to and an escape sequence in it
                // cannot be obeyed.
                if (!decoded.text.empty()) {
                    (void)routeInput(ssg::KeyStroke{}, decoded.text);
                }
                continue;
            }
            if (decoded.status == ssg::app::DecodeStatus::scroll) {
                // Route the wheel to the region under the pointer: the side panel
                // scrolls its tree, the open palette scrolls its client-owned
                // window, everything else scrolls the editor document.
                ssg::HitRegion region = ssg::HitRegion::None;
                if (snapshot) {
                    region = ssg::HitTester{*snapshot}.at( decoded.pointer.column,
                                           decoded.pointer.row).region;
                }
                switch (ssg::app::route_wheel(region)) {
                    case ssg::app::WheelTarget::editor:
                        (void)handleInputResult(runtime.input(
                            ssg::ScrollLinesInput{
                                {ssg::ScrollTarget::Document,
                                 decoded.scroll}}));
                        break;
                    case ssg::app::WheelTarget::tree:
                        (void)handleInputResult(runtime.input(
                            ssg::ScrollLinesInput{
                                {ssg::ScrollTarget::Tree,
                                 decoded.scroll}}));
                        break;
                    case ssg::app::WheelTarget::palette:
                        scrollPalette(decoded.scroll);
                        break;
                    case ssg::app::WheelTarget::none:
                        break;
                }
                continue;
            }
            if (decoded.status != ssg::app::DecodeStatus::key) {
                // A recognized but unhandled byte (unknown CSI, stray control).
                continue;
            }

            // Quit is process lifecycle, not editor behavior. Consume it before
            // committed text routing, which can otherwise accept the same `q`.
            if (ssg::app::application_quit_requested(decoded.stroke)) {
                quit = true;
                continue;
            }
            (void)routeInput(decoded.stroke, decoded.text);
        }
    }
    } catch (std::exception const& error) {
        mode.restore();
        std::fprintf(stderr, "ssg: %s\n", error.what());
        return 1;
    } catch (...) {
        mode.restore();
        std::fprintf(stderr, "ssg: terminated by an unknown error\n");
        return 1;
    }

    // Clean exit (the loop quit without an exception): best-effort final flush of
    // every dirty draft, capturing edits newer than the last autosave tick. This
    // does NOT run on SIGKILL or a crash — those rely on the last debounced tick
    runtime.flushAllAutosaveDrafts();

    return 0;
}

}  // namespace ssg::app
