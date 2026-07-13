#include "ssg_terminal.h"

#include "test_helpers.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

TEST(resolve_launch_no_argument_opens_cwd) {
    auto target = ssg::app::resolve_launch({});
    ASSERT_EQ(target.cwd, fs::current_path());
    ASSERT_FALSE(target.file.has_value());
}

TEST(resolve_launch_directory_opens_that_directory) {
    auto dir = fs::temp_directory_path() / "ssg-app-dir-case";
    fs::create_directories(dir);
    auto target = ssg::app::resolve_launch(dir);
    ASSERT_EQ(target.cwd, fs::absolute(dir));
    ASSERT_FALSE(target.file.has_value());
}

TEST(resolve_launch_file_opens_parent_directory_and_file) {
    auto dir = fs::temp_directory_path() / "ssg-app-file-case";
    fs::create_directories(dir);
    auto file = dir / "hello.txt";
    std::ofstream{file} << "hi";
    auto target = ssg::app::resolve_launch(file);
    ASSERT_EQ(target.cwd, fs::absolute(dir));
    ASSERT_TRUE(target.file.has_value());
    ASSERT_EQ(*target.file, std::string{"hello.txt"});
}

TEST(encode_ansi_frame_addresses_rows_and_emits_palette_colors) {
    ssg::CellGrid screen;
    screen.size = {2, 1};
    screen.palette[0] = {10, 20, 30};
    screen.palette[1] = {200, 100, 50};
    ssg::CellGridCell left;
    left.text = "X";
    left.foreground = 1;
    left.background = 0;
    ssg::CellGridCell right;
    right.text = "Y";
    right.foreground = 1;
    right.background = 0;
    screen.cells = {left, right};

    auto frame = ssg::app::encode_ansi_frame(screen);
    ASSERT_TRUE(frame.find("\x1b[1;1H") != std::string::npos);
    ASSERT_TRUE(frame.find("\x1b[38;2;200;100;50m") != std::string::npos);
    ASSERT_TRUE(frame.find("\x1b[48;2;10;20;30m") != std::string::npos);
    ASSERT_TRUE(frame.find('X') != std::string::npos);
    ASSERT_TRUE(frame.find('Y') != std::string::npos);
    // The two cells share fg/bg, so the color escape is emitted once.
    auto first = frame.find("\x1b[38;2;200;100;50m");
    ASSERT_TRUE(frame.find("\x1b[38;2;200;100;50m", first + 1) ==
                std::string::npos);
}

TEST(encode_ansi_frame_skips_wide_glyph_continuation) {
    ssg::CellGrid screen;
    screen.size = {2, 1};
    screen.palette[0] = {0, 0, 0};
    ssg::CellGridCell wide;
    wide.text = "\xe4\xb8\xad";  // U+4E2D, a double-width glyph.
    ssg::CellGridCell continuation;
    continuation.continuation = true;
    continuation.text = " ";
    screen.cells = {wide, continuation};

    auto frame = ssg::app::encode_ansi_frame(screen);
    ASSERT_TRUE(frame.find("\xe4\xb8\xad") != std::string::npos);
    // The wide glyph occupies both columns; the continuation adds no glyph.
    auto row_start = frame.find("\x1b[1;1H");
    ASSERT_TRUE(row_start != std::string::npos);
    auto glyph = frame.find("\xe4\xb8\xad", row_start);
    ASSERT_TRUE(frame.find(' ', glyph + 3) == std::string::npos ||
                frame.find("\x1b[0m", glyph) < frame.find(' ', glyph + 3));
}

TEST(parse_input_chord_keys) {
    std::size_t consumed = 0;
    auto quit = ssg::app::parse_input(std::string_view{"\x1b" "Q"}, consumed);
    ASSERT_TRUE(quit.action == ssg::app::InputAction::chord);
    ASSERT_EQ(quit.key, 'Q');
    ASSERT_EQ(consumed, std::size_t{2});
    auto toggle = ssg::app::parse_input(std::string_view{"\x1b" "b"}, consumed);
    ASSERT_TRUE(toggle.action == ssg::app::InputAction::chord);
    ASSERT_EQ(toggle.key, 'b');
    ASSERT_EQ(consumed, std::size_t{2});
}

TEST(parse_input_arrows_are_line_events) {
    std::size_t consumed = 0;
    auto up = ssg::app::parse_input(std::string_view{"\x1b[A"}, consumed);
    ASSERT_TRUE(up.action == ssg::app::InputAction::line_up);
    ASSERT_EQ(consumed, std::size_t{3});
    auto down = ssg::app::parse_input(std::string_view{"\x1b[B"}, consumed);
    ASSERT_TRUE(down.action == ssg::app::InputAction::line_down);
}

TEST(parse_input_enter_activates) {
    std::size_t consumed = 0;
    auto cr = ssg::app::parse_input(std::string_view{"\r"}, consumed);
    ASSERT_TRUE(cr.action == ssg::app::InputAction::activate);
    ASSERT_EQ(consumed, std::size_t{1});
    auto lf = ssg::app::parse_input(std::string_view{"\n"}, consumed);
    ASSERT_TRUE(lf.action == ssg::app::InputAction::activate);
}

TEST(parse_input_page_keys_scroll_pages) {
    std::size_t consumed = 0;
    auto up = ssg::app::parse_input(std::string_view{"\x1b[5~"}, consumed);
    ASSERT_TRUE(up.action == ssg::app::InputAction::scroll_pages);
    ASSERT_EQ(up.amount, std::int64_t{-1});
    ASSERT_EQ(consumed, std::size_t{4});
    auto down = ssg::app::parse_input(std::string_view{"\x1b[6~"}, consumed);
    ASSERT_TRUE(down.action == ssg::app::InputAction::scroll_pages);
    ASSERT_EQ(down.amount, std::int64_t{1});
}

TEST(parse_input_sgr_wheel) {
    std::size_t consumed = 0;
    auto up = ssg::app::parse_input(std::string_view{"\x1b[<64;10;5M"}, consumed);
    ASSERT_TRUE(up.action == ssg::app::InputAction::scroll_lines);
    ASSERT_EQ(up.amount, std::int64_t{-3});
    ASSERT_EQ(consumed, std::size_t{11});
    auto down =
        ssg::app::parse_input(std::string_view{"\x1b[<65;10;5M"}, consumed);
    ASSERT_TRUE(down.action == ssg::app::InputAction::scroll_lines);
    ASSERT_EQ(down.amount, std::int64_t{3});
}

TEST(parse_input_incomplete_waits) {
    std::size_t consumed = 99;
    auto partial = ssg::app::parse_input(std::string_view{"\x1b["}, consumed);
    ASSERT_TRUE(partial.action == ssg::app::InputAction::none);
    ASSERT_EQ(consumed, std::size_t{0});
    auto partial_mouse =
        ssg::app::parse_input(std::string_view{"\x1b[<64;10"}, consumed);
    ASSERT_TRUE(partial_mouse.action == ssg::app::InputAction::none);
    ASSERT_EQ(consumed, std::size_t{0});
}

TEST(parse_input_printable_is_text) {
    std::size_t consumed = 0;
    auto ascii = ssg::app::parse_input(std::string_view{"a"}, consumed);
    ASSERT_TRUE(ascii.action == ssg::app::InputAction::text);
    ASSERT_EQ(ascii.text, std::string{"a"});
    ASSERT_EQ(consumed, std::size_t{1});
    // A two-byte UTF-8 character is emitted whole.
    auto utf8 = ssg::app::parse_input(std::string_view{"\xc3\xa9"}, consumed);
    ASSERT_TRUE(utf8.action == ssg::app::InputAction::text);
    ASSERT_EQ(utf8.text, std::string{"\xc3\xa9"});
    ASSERT_EQ(consumed, std::size_t{2});
    // A truncated UTF-8 lead byte waits for the rest.
    auto partial = ssg::app::parse_input(std::string_view{"\xc3"}, consumed);
    ASSERT_TRUE(partial.action == ssg::app::InputAction::none);
    ASSERT_EQ(consumed, std::size_t{0});
}

TEST(parse_input_backspace_and_caret) {
    std::size_t consumed = 0;
    auto del = ssg::app::parse_input(std::string_view{"\x7f"}, consumed);
    ASSERT_TRUE(del.action == ssg::app::InputAction::delete_backward);
    ASSERT_EQ(consumed, std::size_t{1});
    auto right = ssg::app::parse_input(std::string_view{"\x1b[C"}, consumed);
    ASSERT_TRUE(right.action == ssg::app::InputAction::caret_right);
    auto left = ssg::app::parse_input(std::string_view{"\x1b[D"}, consumed);
    ASSERT_TRUE(left.action == ssg::app::InputAction::caret_left);
}

TEST(pending_leader_reports_escape_prefix_only) {
    // No pending buffer, or ordinary bytes: not in leader mode.
    ASSERT_TRUE(ssg::app::pending_leader("").empty());
    ASSERT_TRUE(ssg::app::pending_leader("a").empty());
    // A lone Escape means a chord is being collected.
    auto lone = ssg::app::pending_leader("\x1b");
    ASSERT_EQ(lone.size(), std::size_t{1});
    if (!lone.empty()) ASSERT_EQ(lone.front().code, std::string{"Escape"});
    // Escape + a non-CSI key is still a leader chord in progress.
    ASSERT_EQ(ssg::app::pending_leader("\x1b" "b").size(), std::size_t{1});
    // Escape introducing a CSI/SS3 sequence (arrow) is not leader mode.
    ASSERT_TRUE(ssg::app::pending_leader("\x1b[").empty());
    ASSERT_TRUE(ssg::app::pending_leader("\x1bO").empty());
}

int main() {
    RUN(resolve_launch_no_argument_opens_cwd);
    RUN(resolve_launch_directory_opens_that_directory);
    RUN(resolve_launch_file_opens_parent_directory_and_file);
    RUN(encode_ansi_frame_addresses_rows_and_emits_palette_colors);
    RUN(encode_ansi_frame_skips_wide_glyph_continuation);
    RUN(parse_input_chord_keys);
    RUN(parse_input_arrows_are_line_events);
    RUN(parse_input_enter_activates);
    RUN(parse_input_page_keys_scroll_pages);
    RUN(parse_input_sgr_wheel);
    RUN(parse_input_incomplete_waits);
    RUN(parse_input_printable_is_text);
    RUN(parse_input_backspace_and_caret);
    RUN(pending_leader_reports_escape_prefix_only);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
