// The `ssg` terminal editor entry point.  This file owns terminal I/O only:
// raw mode, size queries, byte reads, frame writes, and clean restoration.  All
// editor, workspace, and layout behavior is the ssg library's; the app attaches
// an in-process client to an EditorRuntime, renders the library's snapshot, and
// forwards input.
//
// Milestone 1 scope: launch over a path argument, draw the shell grid, and quit
// on the `ESC Q` chord.  Input translation through the library keymap and
// editing arrive in later milestones; quitting is an application lifecycle
// concern owned here.

#include "pointer_routing.h"
#include "ssg_terminal.h"

#include <ssg/editor_runtime.h>
#include <ssg/hit_test.h>
#include <ssg/find_replace.h>
#include <ssg/input.h>
#include <ssg/palette.h>
#include <ssg/session_snapshot.h>
#include <ssg/text_input_commands.h>

#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <any>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

// M10-1 startup instrumentation.  Records a CLOCK_MONOTONIC timestamp per cold-
// start phase to the file named by the SSG_STARTUP_TRACE env var, so the startup
// benchmark can attribute exec->first-frame time to phases.  The call sites are
// preprocessor-gated (STARTUP_MARK), so unless SSG_STARTUP_TRACE_ENABLED is
// defined (the shipped `ssg` binary) there is no function, symbol, or call at
// any optimization level.
#ifdef SSG_STARTUP_TRACE_ENABLED
void startup_mark_impl(char const* phase) {
    static char const* const path = std::getenv("SSG_STARTUP_TRACE");
    if (path == nullptr) return;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long const ns =
        static_cast<long long>(now.tv_sec) * 1'000'000'000LL + now.tv_nsec;
    if (std::FILE* file = std::fopen(path, "a"); file != nullptr) {
        std::fprintf(file, "%s %lld\n", phase, ns);
        std::fclose(file);
    }
}
#define STARTUP_MARK(phase) startup_mark_impl(phase)
#else
#define STARTUP_MARK(phase) ((void)0)
#endif

void write_all(std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto written =
            ::write(STDOUT_FILENO, bytes.data() + offset, bytes.size() - offset);
        if (written <= 0) break;
        offset += static_cast<std::size_t>(written);
    }
}

// Puts the terminal in raw mode on the alternate screen and restores the
// original mode, cursor, and primary screen on destruction (RAII).  restore()
// performs the same teardown eagerly and idempotently so a terminating-signal
// path (M9-X) can restore the terminal before re-raising, without the
// destructor undoing or repeating it.
class TerminalMode {
public:
    TerminalMode() {
        if (tcgetattr(STDIN_FILENO, &original_) != 0) return;
        termios raw = original_;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        raw.c_oflag &= ~(OPOST);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return;
        active_ = true;
        write_all(ssg::app::terminal_setup_sequence());
    }

    ~TerminalMode() { restore(); }

    // Restore the terminal to its pre-launch state.  Safe to call more than once
    // (the destructor calls it again); only the first call does the work.
    void restore() noexcept {
        if (!active_) return;
        active_ = false;
        write_all(ssg::app::terminal_restore_sequence());
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
    }

    TerminalMode(TerminalMode const&) = delete;
    TerminalMode& operator=(TerminalMode const&) = delete;

    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    termios original_{};
    bool active_ = false;
};

ssg::ViewportDimensions terminal_size() {
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 &&
        size.ws_row > 0) {
        return {size.ws_col, size.ws_row};
    }
    return {80, 24};
}

// Which descriptors became ready within `timeout_ms` (negative blocks): the
// keyboard, the signal self-pipe, or both.  Every wait in the loop watches the
// signal pipe so a resize/terminate signal is observed promptly even mid-drag or
// mid-Escape (M9-W); the callers drain and act on `signal`.
struct FdReadiness {
    bool input = false;   // STDIN has bytes.
    bool signal = false;  // The signal self-pipe has pending tags.
};

FdReadiness wait_readiness(int timeout_ms, int signal_fd) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    FD_SET(signal_fd, &set);
    int const max_fd = std::max(STDIN_FILENO, signal_fd);
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int const ready =
        ::select(max_fd + 1, &set, nullptr, nullptr,
                 timeout_ms < 0 ? nullptr : &timeout);
    if (ready <= 0) return {};
    return {FD_ISSET(STDIN_FILENO, &set) != 0, FD_ISSET(signal_fd, &set) != 0};
}

constexpr int kEscapeTimeoutMs = 30;

// While a drag is held at the editor edge, wake this often to auto-scroll one
// line and re-extend the selection, even with no new pointer event (M8-S2).
constexpr int kEdgeScrollIntervalMs = 40;

// M9-W signal-event wakeup: the write end of a non-blocking self-pipe.  Signal
// handlers are the only writers and touch nothing else, so a raw fd in a
// sig_atomic-safe int is the whole async-signal-safe surface.  -1 until the pipe
// is created; a handler firing before then is a no-op.
volatile std::sig_atomic_t g_signal_pipe_write = -1;

// The sole action taken in async-signal context: write this signal's number as
// one tag byte to the self-pipe so the event loop wakes and handles it in normal
// context.  A full pipe (EAGAIN) already means "wake pending", so the result is
// ignored; write() is async-signal-safe.
extern "C" void signal_tag_handler(int signo) {
    int const fd = g_signal_pipe_write;
    if (fd < 0) return;
    unsigned char const tag = static_cast<unsigned char>(signo);
    ssize_t const written = ::write(fd, &tag, 1);
    (void)written;
}

// Install the tag-writing handler for a signal, restarting interrupted syscalls
// (the self-pipe select() is the reliable wake, independent of SA_RESTART).
void install_signal_tag_handler(int signo) {
    struct sigaction action{};
    action.sa_handler = signal_tag_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(signo, &action, nullptr);
}

// Delete one UTF-8 code point from the end of a client-local query string.
void pop_code_point(std::string& text) {
    while (!text.empty() &&
           (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) {
        text.pop_back();
    }
    if (!text.empty()) text.pop_back();
}

}  // namespace

int main(int argc, char** argv) {
    STARTUP_MARK("main_entry");
    fs::path argument = argc > 1 ? fs::path{argv[1]} : fs::path{};
    auto target = ssg::app::resolve_launch(argument);

    auto base = fs::temp_directory_path() / ("ssg-" + std::to_string(::getpid()));
    std::error_code code;
    fs::create_directories(base / "scratch", code);
    fs::create_directories(base / "recovery", code);

    ssg::EditorRuntimeConfig config;
    config.cwd = target.cwd;
    config.scratch_root = base / "scratch";
    config.recovery_root = base / "recovery";
    // M10 fast startup: defer the workspace tree scan and syntax highlighting off
    // the first-frame path; prime_deferred() runs them once the first frame is
    // drawn.
    config.defer_enrichment = true;
    auto created = ssg::EditorRuntime::create(config);
    if (!created.accepted()) {
        std::fprintf(stderr, "ssg: %s\n", created.message.c_str());
        return 1;
    }
    auto& runtime = *created.runtime;
    STARTUP_MARK("post_create");

    ssg::ClientId client{1};
    if (!runtime.attach({client, ssg::InvocationOrigin::in_process}, ssg::ViewId{1})
             .accepted()) {
        std::fprintf(stderr, "ssg: failed to attach client\n");
        return 1;
    }
    STARTUP_MARK("post_attach");

    if (target.file) {
        if (fs::exists(target.cwd / *target.file)) {
            (void)runtime.dispatch(
                client, {"file.open", runtime.revision(), *target.file});
        }
    }
    STARTUP_MARK("post_open");

    TerminalMode mode;
    if (!mode.active()) {
        std::fprintf(stderr, "ssg: stdin/stdout is not an interactive terminal\n");
        return 1;
    }

    // M9-W: a non-blocking self-pipe the event loop selects on, woken by signal
    // handlers that write one tag byte each.  Both ends are non-blocking so the
    // handler never blocks and a drain never stalls.
    int signal_pipe[2] = {-1, -1};
    if (::pipe(signal_pipe) != 0) {
        std::fprintf(stderr, "ssg: failed to create signal pipe\n");
        return 1;
    }
    ::fcntl(signal_pipe[0], F_SETFL,
            ::fcntl(signal_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    ::fcntl(signal_pipe[1], F_SETFL,
            ::fcntl(signal_pipe[1], F_GETFL, 0) | O_NONBLOCK);
    g_signal_pipe_write = signal_pipe[1];
    install_signal_tag_handler(SIGWINCH);
    install_signal_tag_handler(SIGTERM);
    install_signal_tag_handler(SIGHUP);

    // Detect the terminal color depth once at startup (M9-C2); the frame encoder
    // adapts the theme's 16 colors to it via the library's resolve_color.
    ssg::ColorDepth const color_depth =
        ssg::app::detect_color_depth(std::getenv("COLORTERM"), std::getenv("TERM"));

    // Drain and classify any pending signal tags.  Returns false to keep looping;
    // a terminating signal does not return — it restores the terminal in normal
    // context (tcsetattr is not async-signal-safe) and re-raises with the default
    // disposition so the exit status reflects the signal (M9-X).  A resize needs
    // no work here: the next snapshot at the loop top re-queries terminal_size().
    auto drain_signals = [&]() -> void {
        char scratch[64];
        std::string tags;
        for (;;) {
            auto const n = ::read(signal_pipe[0], scratch, sizeof scratch);
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
    ssg::KeySequence chord;         // The pending (mid-entry) key chord.
    bool quit = false;
    auto focus = ssg::FocusTarget::editor;
    // Client-owned palette state: query and selection are local (reported for
    // library presentation), ranked against the server's published candidates.
    bool palette_open = false;
    std::string palette_query;
    std::size_t palette_selected = 0;
    // Client-owned palette scroll: the offset into the ranked order and the pane
    // height cached from the last snapshot (the palette pane == the editor pane,
    // so this is populated before the palette ever opens; see doc/spec-scroll.md
    // R3). The window is resolved with the shared list-scroll primitive.
    std::uint32_t palette_first_visible = 0;
    std::uint32_t palette_pane_rows = 1;
    // Mouse drag state (M8): a left press on the editor records the anchor and
    // enters dragging; subsequent motion extends the selection. Client-local and
    // transient — the server only ever sees cursor.set_position / select.set_range.
    bool dragging = false;
    std::optional<ssg::DocumentPosition> drag_anchor;
    // The last pointer cell (0-based) from a press/drag, so a drag held still at
    // the editor edge can auto-scroll on a timer without a fresh pointer event
    // (M8-S2).
    int last_pointer_column = 0;
    int last_pointer_row = 0;
    // Find prompt: the client holds no authoritative query.  It reads the
    // published controller query (adopted in refresh), edits it, and reports the
    // full next string via find.update_query.  find_open mirrors the controller.
    bool find_open = false;
    std::string find_query;
    // Replace prompt: the query row is display-only; the client edits the
    // replacement the same no-copy way as the find query (reads the published
    // replacement, mutates it, dispatches replace.update_replacement).
    bool replace_open = false;
    std::string replace_replacement;
    ssg::KeymapViewState keymap;
    std::vector<ssg::PaletteCandidate> candidates;

    auto dispatch = [&](std::string_view id, std::any payload = {}) {
        (void)runtime.dispatch(client, {std::string{id}, runtime.revision(),
                                        std::move(payload)});
    };
    // Re-center the client-owned palette window on the current selection
    // (keep-visible). Called ONLY when the selection changes (arrow navigation,
    // open, type, backspace); the per-frame build_report otherwise honors the
    // free offset so a wheel scroll persists (see doc/spec-m8.md M8-P). Mirrors
    // the tree's reveal_tree_selection.
    auto reveal_palette_selection = [&] {
        auto order = ssg::palette_rank(candidates, palette_query);
        if (palette_selected >= order.size()) {
            palette_selected = order.empty() ? 0 : order.size() - 1;
        }
        std::optional<std::uint32_t> selected =
            order.empty() ? std::nullopt
                          : std::optional<std::uint32_t>{
                                static_cast<std::uint32_t>(palette_selected)};
        auto scroll = ssg::compute_list_scroll_view(
            static_cast<std::uint32_t>(order.size()), palette_pane_rows,
            palette_first_visible, selected, /*keep_selection_visible=*/true);
        palette_first_visible = scroll.first_visible;
    };
    // Scroll the client-owned palette window by `delta` rows WITHOUT moving the
    // selection (a wheel over the open palette), clamped to [0, maximum_first_row].
    // Saturating: `delta` is a decoded int64, so guard the extremes before adding.
    auto scroll_palette = [&](std::int64_t delta) {
        if (!palette_open) return;
        auto order = ssg::palette_rank(candidates, palette_query);
        auto probe = ssg::compute_list_scroll_view(
            static_cast<std::uint32_t>(order.size()), palette_pane_rows,
            palette_first_visible, std::nullopt, /*keep_selection_visible=*/false);
        auto const maximum =
            static_cast<std::int64_t>(probe.scrollbar.maximum_first_row);
        auto const current = static_cast<std::int64_t>(palette_first_visible);
        std::int64_t next;
        if (delta >= maximum) {
            next = maximum;
        } else if (delta <= -maximum) {
            next = 0;
        } else {
            next = std::clamp<std::int64_t>(current + delta, 0, maximum);
        }
        palette_first_visible = static_cast<std::uint32_t>(next);
    };
    auto execute_selected_candidate = [&] {
        auto order = ssg::palette_rank(candidates, palette_query);
        if (!order.empty() && palette_selected < order.size()) {
            dispatch("palette.execute",
                     ssg::PaletteExecuteArguments{candidates[order[palette_selected]].id});
        }
    };
    // Dispatch a resolved command, fulfilling prompt-context commands against the
    // client-local palette view when a palette prompt is open (doc/spec-keymap.md
    // Prompt-focus fulfillment).  Palette open/closed is reconciled from the
    // server focus on the next snapshot, not forced here, so a failed submit (no
    // candidate / rejected execute) leaves the prompt open rather than
    // desynchronizing the client.
    auto dispatch_resolved = [&](std::string const& id) {
        if (find_open && focus == ssg::FocusTarget::prompt) {
            if (id == "prompt.submit" || id == "palette.next") { dispatch("find.next"); return; }
            if (id == "palette.previous") { dispatch("find.previous"); return; }
            if (id == "prompt.cancel") { dispatch("find.close"); return; }
        }
        if (replace_open && focus == ssg::FocusTarget::prompt) {
            if (id == "prompt.submit") { dispatch("replace.current"); return; }
            if (id == "palette.next") { dispatch("find.next"); return; }
            if (id == "palette.previous") { dispatch("find.previous"); return; }
            if (id == "prompt.cancel") { dispatch("find.close"); return; }
        }
        if (palette_open && focus == ssg::FocusTarget::prompt) {
            if (id == "prompt.submit") { execute_selected_candidate(); return; }
            if (id == "prompt.cancel") { dispatch("palette.close"); return; }
            if (id == "palette.next") { ++palette_selected; reveal_palette_selection(); return; }
            if (id == "palette.previous") { if (palette_selected > 0) --palette_selected; reveal_palette_selection(); return; }
        }
        dispatch(id);
        if (id == "palette.open") {
            palette_open = true;
            palette_query.clear();
            palette_selected = 0;
            palette_first_visible = 0;
            reveal_palette_selection();
        }
    };
    auto route_text = [&](std::string const& text) {
        switch (ssg::text_routing(ssg::focus_target_name(focus))) {
        case ssg::TextRouting::insert:
            dispatch("text.insert", ssg::TextInputArguments{text});
            break;
        case ssg::TextRouting::prompt_query:
            if (palette_open) { palette_query += text; palette_selected = 0; reveal_palette_selection(); }
            else if (replace_open) { dispatch("replace.update_replacement", ssg::FindQueryArguments{replace_replacement + text}); }
            else if (find_open) { dispatch("find.update_query", ssg::FindQueryArguments{find_query + text}); }
            break;
        case ssg::TextRouting::ignore:
            break;
        }
    };

    auto build_report = [&] {
        ssg::PaletteReport report;
        if (palette_open) {
            // The library owns the palette projection: rank the published
            // candidates, window them, and assemble the bounded report. The app
            // supplies only the client-owned window (query/selection/scroll) and
            // adopts back the clamped selection and resolved offset, inventing no
            // product data (INV-derived-view-bounded).
            ssg::PaletteWindowState window{palette_query, palette_selected,
                                           palette_first_visible,
                                           palette_pane_rows};
            report = ssg::derive_palette_report(candidates, window);
            palette_selected = window.selected;
            palette_first_visible = window.first_visible;
        }
        return report;
    };
    // Take a fresh snapshot and adopt its authoritative client state (focus,
    // keymap, published candidates).  Called before every input event so that
    // coalesced input after a focus-changing command routes against the new
    // focus rather than a stale one.
    auto refresh = [&]() -> std::optional<ssg::SessionSnapshot> {
        auto snapshot = runtime.snapshot(client, terminal_size(), chord, build_report());
        if (snapshot) {
            focus = snapshot->sections().shell.focus;
            keymap = snapshot->sections().keymap;
            candidates = snapshot->sections().palette.candidates;
            if (focus != ssg::FocusTarget::prompt) palette_open = false;
            // Cache the palette pane height for the next window computation: the
            // palette pane is the editor pane, so this is populated every frame,
            // including before the palette opens (no cold start). The window is
            // computed from the PREVIOUS frame's height, so a terminal resize
            // lags one frame before keep-visible re-settles — the same one-frame
            // clamp the editor's server-side scroll offset already has, and it
            // self-corrects on the next snapshot.
            auto const& shell = snapshot->sections().shell;
            if (!shell.panes.empty()) {
                palette_pane_rows = static_cast<std::uint32_t>(
                    std::max(shell.panes.front().content.height, 1));
            }
            // Derive find fulfillment from the ACTIVE prompt kind, not merely the
            // controller being open under prompt focus: a palette/settings prompt
            // may be active while the find controller is still open, and find
            // fulfillment must not hijack that unrelated prompt's keys.
            auto const& find_view = snapshot->sections().find_replace;
            auto const& active_prompt = snapshot->sections().prompt_status.prompt;
            bool const find_prompt_active =
                active_prompt && active_prompt->kind == ssg::PromptKind::find;
            find_open = find_view.open && find_prompt_active;
            find_query = find_view.query;
            // The replace prompt edits the replacement, not the query.
            bool const replace_prompt_active =
                active_prompt && active_prompt->kind == ssg::PromptKind::replace;
            replace_open = find_view.open && replace_prompt_active;
            replace_replacement = find_view.replacement;
        }
        return snapshot;
    };

    // Top-level boundary (M9-X): an exception escaping the loop is not portably
    // guaranteed to unwind `mode` once past main, so restore the terminal here
    // before it propagates.
    bool first_frame_marked = false;
    try {
        while (!quit) {
            auto snapshot = refresh();
        if (snapshot) {
            // The library renders every screen branch, including the declined-
            // layout "too small" placeholder (M11-L); the app only encodes.
            auto grid = ssg::render(*snapshot);
            std::string frame = "\x1b[?25l";  // Hide the cursor while redrawing.
            frame += ssg::app::encode_ansi_frame(grid, color_depth);
            if (grid.caret) {
                frame += "\x1b[" + std::to_string(grid.caret->row + 1) + ";" +
                         std::to_string(grid.caret->column + 1) + "H\x1b[?25h";
            }
            if (!first_frame_marked) {
                // M10-1 stop mark: the first content frame (an actual rendered
                // payload), not the earlier terminal-setup bytes.
                STARTUP_MARK("first_content_frame");
                first_frame_marked = true;
                write_all(frame);
                // M10-3/M10-4: the first frame is on screen; now run the
                // enrichment (tree scan, syntax) deferred off the startup
                // path.  It publishes on the next snapshot at the loop top.
                runtime.prime_deferred();
                continue;
            }
            write_all(frame);
        }

        char bytes[64];
        // Edge auto-scroll (M8-S2): if a drag is held past the top/bottom of the
        // editor content, don't block indefinitely on input — wake on a timer to
        // scroll one line and re-extend the selection to the new edge cell, so a
        // drag held still at the edge keeps scrolling and selecting.
        std::optional<int> drag_edge;
        if (dragging && snapshot && !snapshot->sections().shell.panes.empty()) {
            drag_edge = ssg::app::edge_scroll(
                dragging, last_pointer_row,
                snapshot->sections().shell.panes.front().content);
        }
        if (drag_edge) {
            auto const ready = wait_readiness(kEdgeScrollIntervalMs, signal_pipe[0]);
            if (ready.signal) {
                // A resize/terminate signal arrived mid-drag: drain it now rather
                // than deferring until the drag releases.  Terminate does not
                // return (restore + re-raise); resize re-snapshots at the loop
                // top with the drag preserved.
                drain_signals();
                continue;
            }
            if (!ready.input) {
                dispatch("view.scroll_lines", ssg::ScrollLinesArguments{*drag_edge});
                auto scrolled = refresh();
                if (scrolled && drag_anchor &&
                    !scrolled->sections().shell.panes.empty()) {
                    auto const content = scrolled->sections().shell.panes.front().content;
                    int const edge_row =
                        *drag_edge < 0 ? content.y : content.bottom() - 1;
                    int const column = std::clamp(last_pointer_column, content.x,
                                                  content.right() - 1);
                    auto hit = ssg::hit_test(*scrolled, column, edge_row);
                    if (hit.region == ssg::HitRegion::editor) {
                        auto active = ssg::resolve_document_position(
                            scrolled->sections().document.text,
                            ssg::ByteOffset{hit.byte_offset});
                        if (active) {
                            dispatch("select.set_range",
                                     ssg::SelectionCommandArguments{
                                         std::nullopt,
                                         ssg::Selection{*drag_anchor, *active}});
                        }
                    }
                }
                continue;  // re-render with the scrolled viewport, then re-evaluate
            }
            // Keyboard input arrived during the drag: fall through and read it.
        }
        // Block until keyboard input OR a signal-driven self-pipe wake (M9-W).
        // A bare read() could not be interrupted reliably by a resize/terminate
        // signal; selecting on both fds makes the wake deterministic.
        auto const wait = wait_readiness(-1, signal_pipe[0]);
        if (wait.signal) {
            // Terminate does not return (restore + re-raise); a resize just
            // re-snapshots at the loop top.
            drain_signals();
            if (!wait.input) continue;
        }
        if (!wait.input) continue;  // EINTR or spurious wake: re-render and retry.
        auto read_bytes = ::read(STDIN_FILENO, bytes, sizeof bytes);
        if (read_bytes <= 0) break;
        buffer.append(bytes, static_cast<std::size_t>(read_bytes));

        bool first_event = true;
        while (!buffer.empty() && !quit) {
            std::size_t consumed = 0;
            auto decoded = ssg::app::decode_input(buffer, false, consumed);
            if (decoded.status == ssg::app::DecodeStatus::incomplete) {
                // A partial sequence (lone ESC or truncated CSI) remains.  Wait
                // briefly for the disambiguating bytes; if none arrive, force the
                // bounded-Escape resolution.  The wait also watches the signal
                // pipe so a resize/terminate is not deferred by a lone ESC.
                auto const ready = wait_readiness(kEscapeTimeoutMs, signal_pipe[0]);
                if (ready.signal) drain_signals();
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

            // Adopt fresh authoritative focus before every event after the first
            // (the first uses the snapshot already taken at the top of the loop).
            // Capture the refreshed snapshot so pointer hit-testing sees the
            // current frame's layout.
            if (!first_event) snapshot = refresh();
            first_event = false;

            if (decoded.status == ssg::app::DecodeStatus::pointer) {
                last_pointer_column = decoded.pointer.column;
                last_pointer_row = decoded.pointer.row;
                // Classify the cell via the library hit_test, resolve the target
                // the hit needs, then let the pure route_pointer decide the
                // command sequence and drag-state change.
                ssg::RegionHit hit;
                ssg::app::PointerTargets targets;
                if (snapshot) {
                    hit = ssg::hit_test(*snapshot, decoded.pointer.column,
                                        decoded.pointer.row);
                    if (hit.region == ssg::HitRegion::editor) {
                        targets.document_position = ssg::resolve_document_position(
                            snapshot->sections().document.text,
                            ssg::ByteOffset{hit.byte_offset});
                    } else if (hit.region == ssg::HitRegion::tab) {
                        auto const& tabs = snapshot->sections().tabs.tabs;
                        if (hit.tab_index < tabs.size()) {
                            targets.tab_id = tabs[hit.tab_index].id;
                        }
                    } else if (hit.region == ssg::HitRegion::palette) {
                        // Map the absolute rank index to its candidate id using
                        // the same ranked order the client renders.
                        auto order = ssg::palette_rank(candidates, palette_query);
                        if (hit.item_index < order.size()) {
                            targets.palette_command_id =
                                candidates[order[hit.item_index]].id;
                        }
                    }
                }
                auto plan =
                    ssg::app::route_pointer(hit, decoded.pointer.button,
                                            decoded.pointer.kind, dragging,
                                            drag_anchor, targets);
                for (auto const& command : plan.commands) {
                    dispatch(command.command_id, command.payload);
                }
                if (plan.begins_drag) {
                    dragging = true;
                    drag_anchor = targets.document_position;
                }
                if (plan.ends_drag) {
                    dragging = false;
                    drag_anchor.reset();
                }
                chord.clear();
                continue;
            }

            if (decoded.status == ssg::app::DecodeStatus::scroll) {
                // Route the wheel to the region under the pointer: the side panel
                // scrolls its tree, the open palette scrolls its client-owned
                // window, everything else scrolls the editor document.
                ssg::HitRegion region = ssg::HitRegion::none;
                if (snapshot) {
                    region = ssg::hit_test(*snapshot, decoded.pointer.column,
                                           decoded.pointer.row).region;
                }
                switch (ssg::app::route_wheel(region)) {
                    case ssg::app::WheelTarget::editor:
                        dispatch("view.scroll_lines",
                                 ssg::ScrollLinesArguments{decoded.scroll});
                        break;
                    case ssg::app::WheelTarget::tree:
                        dispatch("tree.scroll",
                                 ssg::ScrollLinesArguments{decoded.scroll});
                        break;
                    case ssg::app::WheelTarget::palette:
                        scroll_palette(decoded.scroll);
                        break;
                    case ssg::app::WheelTarget::none:
                        break;
                }
                chord.clear();
                continue;
            }
            if (decoded.status != ssg::app::DecodeStatus::key) {
                // A recognized but unhandled byte (unknown CSI, stray control):
                // a non-matching continuation that clears any pending chord.
                chord.clear();
                continue;
            }

            // A printable without a keycode (e.g. multibyte text) cannot be a
            // chord; route it straight to the text sink.
            if (decoded.stroke.code.empty()) {
                route_text(decoded.text);
                chord.clear();
                continue;
            }

            chord.push_back(decoded.stroke);
            auto resolution = ssg::resolve_key_sequence(
                keymap, chord, ssg::focus_target_name(focus));
            if (resolution.kind == ssg::KeymapMatchKind::resolved) {
                dispatch_resolved(resolution.command_id);
                chord.clear();
            } else if (resolution.kind == ssg::KeymapMatchKind::pending) {
                // Keep collecting; the leader hint renders next frame.
            } else {
                // No binding.  Quit is the sole app-local chord (process
                // lifecycle); everything else clears the chord and, for a
                // printable, still routes as text.
                const bool quit_chord =
                    chord.size() == 2 && chord[0].code == "Escape" &&
                    chord[1].code == "KeyQ";
                const auto stroke = decoded.stroke;
                chord.clear();
                if (quit_chord) {
                    quit = true;
                } else if (palette_open && focus == ssg::FocusTarget::prompt &&
                           stroke.code == "Backspace") {
                    pop_code_point(palette_query);
                    palette_selected = 0;
                    reveal_palette_selection();
                } else if (find_open && focus == ssg::FocusTarget::prompt &&
                           stroke.code == "Backspace") {
                    auto next = find_query;
                    pop_code_point(next);
                    dispatch("find.update_query", ssg::FindQueryArguments{next});
                } else if (replace_open && focus == ssg::FocusTarget::prompt &&
                           stroke.code == "Backspace") {
                    auto next = replace_replacement;
                    pop_code_point(next);
                    dispatch("replace.update_replacement", ssg::FindQueryArguments{next});
                } else if (!decoded.text.empty()) {
                    route_text(decoded.text);
                }
            }
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

    return 0;
}
