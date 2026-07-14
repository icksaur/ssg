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

#include "ssg_terminal.h"

#include <ssg/editor_runtime.h>
#include <ssg/find_replace.h>
#include <ssg/input.h>
#include <ssg/palette.h>
#include <ssg/session_snapshot.h>
#include <ssg/text_input_commands.h>

#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <any>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

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
// original mode, cursor, and primary screen on destruction (RAII; the only exit
// path in milestone 1 is `ESC Q`, which returns normally).
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
        // Alternate screen, blinking bar cursor, SGR mouse reporting for wheel.
        write_all("\x1b[?1049h\x1b[5 q\x1b[?1000h\x1b[?1006h");
    }

    ~TerminalMode() {
        if (!active_) return;
        write_all("\x1b[?1006l\x1b[?1000l\x1b[0 q\x1b[?25h\x1b[?1049l");
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

// Whether more input is available within `timeout_ms`.  Used only to bound the
// terminal Escape ambiguity: a lone trailing ESC waits briefly for a follow-up
// byte before being decoded as a standalone Escape stroke.
bool input_ready(int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return ::select(STDIN_FILENO + 1, &set, nullptr, nullptr, &timeout) > 0;
}

constexpr int kEscapeTimeoutMs = 30;

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
    fs::path argument = argc > 1 ? fs::path{argv[1]} : fs::path{};
    auto target = ssg::app::resolve_launch(argument);

    auto base = fs::temp_directory_path() / ("ssg-" + std::to_string(::getpid()));
    std::error_code code;
    fs::create_directories(base / "scratch", code);
    fs::create_directories(base / "recovery", code);

    auto created = ssg::EditorRuntime::create(
        {target.cwd, base / "scratch", base / "recovery"});
    if (!created.accepted()) {
        std::fprintf(stderr, "ssg: %s\n", created.message.c_str());
        return 1;
    }
    auto& runtime = *created.runtime;

    ssg::ClientId client{1};
    if (!runtime.attach({client, ssg::InvocationOrigin::in_process}, ssg::ViewId{1})
             .accepted()) {
        std::fprintf(stderr, "ssg: failed to attach client\n");
        return 1;
    }

    if (target.file) {
        if (fs::exists(target.cwd / *target.file)) {
            (void)runtime.dispatch(
                client, {"file.open", runtime.revision(), *target.file});
        }
    }

    TerminalMode mode;
    if (!mode.active()) {
        std::fprintf(stderr, "ssg: stdin/stdout is not an interactive terminal\n");
        return 1;
    }

    std::string buffer;             // Raw bytes read but not yet decoded.
    ssg::KeySequence chord;         // The pending (mid-entry) key chord.
    bool quit = false;
    auto focus = ssg::FocusTarget::editor;
    // Client-owned palette state: query and selection are local (reported for
    // library presentation), ranked against the server's published candidates.
    bool palette_open = false;
    std::string palette_query;
    std::size_t palette_selected = 0;
    // Find prompt: the client holds no authoritative query.  It reads the
    // published controller query (adopted in refresh), edits it, and reports the
    // full next string via find.update_query.  find_open mirrors the controller.
    bool find_open = false;
    std::string find_query;
    ssg::KeymapViewState keymap;
    std::vector<ssg::PaletteCandidate> candidates;

    auto dispatch = [&](std::string_view id, std::any payload = {}) {
        (void)runtime.dispatch(client, {std::string{id}, runtime.revision(),
                                        std::move(payload)});
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
        if (palette_open && focus == ssg::FocusTarget::prompt) {
            if (id == "prompt.submit") { execute_selected_candidate(); return; }
            if (id == "prompt.cancel") { dispatch("palette.close"); return; }
            if (id == "palette.next") { ++palette_selected; return; }
            if (id == "palette.previous") { if (palette_selected > 0) --palette_selected; return; }
        }
        dispatch(id);
        if (id == "palette.open") {
            palette_open = true;
            palette_query.clear();
            palette_selected = 0;
        }
    };
    auto route_text = [&](std::string const& text) {
        switch (ssg::text_routing(ssg::focus_target_name(focus))) {
        case ssg::TextRouting::insert:
            dispatch("text.insert", ssg::TextInputArguments{text});
            break;
        case ssg::TextRouting::prompt_query:
            if (palette_open) { palette_query += text; palette_selected = 0; }
            else if (find_open) { dispatch("find.update_query", ssg::FindQueryArguments{find_query + text}); }
            break;
        case ssg::TextRouting::ignore:
            break;
        }
    };

    auto build_report = [&] {
        ssg::PaletteReport report;
        if (palette_open) {
            auto order = ssg::palette_rank(candidates, palette_query);
            report.query = palette_query;
            if (!order.empty()) {
                report.ghost =
                    ssg::palette_ghost(candidates[order.front()].label, palette_query);
            }
            if (palette_selected >= order.size()) {
                palette_selected = order.empty() ? 0 : order.size() - 1;
            }
            for (auto index : order) report.rows.push_back(candidates[index]);
            if (!order.empty()) {
                report.selected = static_cast<std::uint32_t>(palette_selected);
            }
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
        }
        return snapshot;
    };

    while (!quit) {
        auto snapshot = refresh();
        if (snapshot) {
            auto grid = ssg::render(*snapshot);
            std::string frame = "\x1b[?25l";  // Hide the cursor while redrawing.
            frame += ssg::app::encode_ansi_frame(grid);
            if (grid.caret) {
                frame += "\x1b[" + std::to_string(grid.caret->row + 1) + ";" +
                         std::to_string(grid.caret->column + 1) + "H\x1b[?25h";
            }
            write_all(frame);
        }

        char bytes[64];
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
                // bounded-Escape resolution.
                if (input_ready(kEscapeTimeoutMs)) {
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
            if (!first_event) refresh();
            first_event = false;

            if (decoded.status == ssg::app::DecodeStatus::scroll) {
                dispatch("view.scroll_lines", ssg::ScrollLinesArguments{decoded.scroll});
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
                } else if (find_open && focus == ssg::FocusTarget::prompt &&
                           stroke.code == "Backspace") {
                    auto next = find_query;
                    pop_code_point(next);
                    dispatch("find.update_query", ssg::FindQueryArguments{next});
                } else if (!decoded.text.empty()) {
                    route_text(decoded.text);
                }
            }
        }
    }

    return 0;
}
