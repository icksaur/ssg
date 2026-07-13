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
#include <ssg/input.h>
#include <ssg/session_snapshot.h>
#include <ssg/text_input_commands.h>

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

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

    std::string pending;
    bool quit = false;
    auto focus = ssg::FocusTarget::editor;
    bool panel_open = false;
    while (!quit) {
        auto snapshot = runtime.snapshot(client, terminal_size(),
                                         ssg::app::pending_leader(pending));
        if (snapshot) {
            focus = snapshot->sections().shell.focus;
            panel_open = snapshot->sections().shell.panel.has_value();
            auto grid = ssg::render(*snapshot);
            std::string frame = "\x1b[?25l";  // Hide the cursor while redrawing.
            frame += ssg::app::encode_ansi_frame(grid);
            if (grid.caret) {
                frame += "\x1b[" + std::to_string(grid.caret->row + 1) + ";" +
                         std::to_string(grid.caret->column + 1) + "H\x1b[?25h";
            }
            write_all(frame);
        }

        char buffer[64];
        auto read_bytes = ::read(STDIN_FILENO, buffer, sizeof buffer);
        if (read_bytes <= 0) break;
        pending.append(buffer, static_cast<std::size_t>(read_bytes));

        for (;;) {
            std::size_t consumed = 0;
            auto event = ssg::app::parse_input(pending, consumed);
            if (consumed == 0) break;  // Incomplete sequence; read more.
            pending.erase(0, consumed);
            auto const editor = focus == ssg::FocusTarget::editor;
            auto const on_panel = focus == ssg::FocusTarget::panel;
            auto scroll = [&](std::int64_t lines) {
                (void)runtime.dispatch(
                    client, {"view.scroll_lines", runtime.revision(),
                             ssg::ScrollLinesArguments{lines}});
            };
            auto command = [&](char const* id) {
                (void)runtime.dispatch(client, {id, runtime.revision(), {}});
            };
            switch (event.action) {
            case ssg::app::InputAction::chord:
                if (event.key == 'Q') {
                    quit = true;
                } else if (event.key == 'b') {
                    // Cycle focus: reveal+focus the bar, focus it, then hide it.
                    if (!panel_open) {
                        command("panel.toggle");
                        command("panel.focus");
                    } else if (!on_panel) {
                        command("panel.focus");
                    } else {
                        command("panel.toggle");
                    }
                } else if (event.key == 's') {
                    command("file.save");
                } else if (event.key == 'z') {
                    command("edit.undo");
                } else if (event.key == 'Z') {
                    command("edit.redo");
                } else if (event.key == ']') {
                    command("tab.next");
                } else if (event.key == 'p') {
                    command("tab.previous");
                } else if (event.key == 'w') {
                    command("tab.close");
                }
                break;
            case ssg::app::InputAction::text:
                if (editor) {
                    (void)runtime.dispatch(
                        client, {"text.insert", runtime.revision(),
                                 ssg::TextInputArguments{event.text}});
                }
                break;
            case ssg::app::InputAction::delete_backward:
                if (editor) command("text.delete_backward");
                break;
            case ssg::app::InputAction::line_up:
                command(on_panel ? "tree.select_previous" : "cursor.line_up");
                break;
            case ssg::app::InputAction::line_down:
                command(on_panel ? "tree.select_next" : "cursor.line_down");
                break;
            case ssg::app::InputAction::caret_left:
                if (editor) command("cursor.left");
                break;
            case ssg::app::InputAction::caret_right:
                if (editor) command("cursor.right");
                break;
            case ssg::app::InputAction::activate:
                command(on_panel ? "tree.activate" : "text.newline");
                break;
            case ssg::app::InputAction::scroll_lines:
                scroll(event.amount);
                break;
            case ssg::app::InputAction::scroll_pages:
                (void)runtime.dispatch(
                    client, {"view.scroll_pages", runtime.revision(),
                             ssg::ScrollPagesArguments{event.amount}});
                break;
            case ssg::app::InputAction::none:
                break;
            }
            if (quit || pending.empty()) break;
        }
    }

    return 0;
}
