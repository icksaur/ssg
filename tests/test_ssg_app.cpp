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

TEST(decode_input_maps_printables_and_named_keys) {
    std::size_t consumed = 0;
    // Lowercase letter: KeyA stroke (no shift) plus committed text.
    auto a = ssg::app::decode_input("a", true, consumed);
    ASSERT_EQ(consumed, std::size_t{1});
    ASSERT_TRUE(a.status == ssg::app::DecodeStatus::key);
    ASSERT_EQ(a.stroke.code, std::string{"KeyA"});
    ASSERT_FALSE(a.stroke.shift);
    ASSERT_EQ(a.text, std::string{"a"});

    // Uppercase: Shift+KeyZ plus committed text "Z".
    auto z = ssg::app::decode_input("Z", true, consumed);
    ASSERT_EQ(z.stroke.code, std::string{"KeyZ"});
    ASSERT_TRUE(z.stroke.shift);
    ASSERT_EQ(z.text, std::string{"Z"});

    // Bracket punctuation used by tab chords.
    auto bracket = ssg::app::decode_input("]", true, consumed);
    ASSERT_EQ(bracket.stroke.code, std::string{"BracketRight"});
    ASSERT_EQ(bracket.text, std::string{"]"});

    // Enter and Backspace are strokes without committed text.
    auto enter = ssg::app::decode_input("\r", true, consumed);
    ASSERT_EQ(enter.stroke.code, std::string{"Enter"});
    ASSERT_TRUE(enter.text.empty());
    auto back = ssg::app::decode_input("\x7f", true, consumed);
    ASSERT_EQ(back.stroke.code, std::string{"Backspace"});
}

TEST(decode_input_arrows_and_mouse) {
    std::size_t consumed = 0;
    auto up = ssg::app::decode_input("\x1b[A", true, consumed);
    ASSERT_EQ(consumed, std::size_t{3});
    ASSERT_EQ(up.stroke.code, std::string{"ArrowUp"});
    auto down = ssg::app::decode_input("\x1b[B", true, consumed);
    ASSERT_EQ(down.stroke.code, std::string{"ArrowDown"});

    auto wheel = ssg::app::decode_input("\x1b[<65;10;5M", true, consumed);
    ASSERT_TRUE(wheel.status == ssg::app::DecodeStatus::scroll);
    ASSERT_EQ(wheel.scroll, std::int64_t{3});
}

TEST(decode_input_escape_boundary_is_bounded) {
    std::size_t consumed = 0;
    // A buffered CSI introducer disambiguates to an arrow, not an Escape stroke.
    auto arrow = ssg::app::decode_input("\x1b[A", false, consumed);
    ASSERT_EQ(arrow.stroke.code, std::string{"ArrowUp"});

    // ESC followed by a non-CSI byte: ESC is a standalone Escape stroke consuming
    // only itself, so the next byte (the chord continuation) decodes separately.
    auto esc_then = ssg::app::decode_input("\x1bs", false, consumed);
    ASSERT_EQ(consumed, std::size_t{1});
    ASSERT_EQ(esc_then.stroke.code, std::string{"Escape"});

    // A lone ESC with more input possibly coming: incomplete, consume nothing.
    auto pending = ssg::app::decode_input("\x1b", false, consumed);
    ASSERT_TRUE(pending.status == ssg::app::DecodeStatus::incomplete);
    ASSERT_EQ(consumed, std::size_t{0});

    // A lone ESC with input exhausted (bounded read returned nothing): the
    // Escape stroke.
    auto exhausted = ssg::app::decode_input("\x1b", true, consumed);
    ASSERT_EQ(consumed, std::size_t{1});
    ASSERT_EQ(exhausted.stroke.code, std::string{"Escape"});

    // A truncated CSI is always incomplete regardless of exhaustion (the final
    // byte has not arrived).
    auto partial = ssg::app::decode_input("\x1b[", true, consumed);
    ASSERT_TRUE(partial.status == ssg::app::DecodeStatus::incomplete);
}

int main() {
    RUN(resolve_launch_no_argument_opens_cwd);
    RUN(resolve_launch_directory_opens_that_directory);
    RUN(resolve_launch_file_opens_parent_directory_and_file);
    RUN(encode_ansi_frame_addresses_rows_and_emits_palette_colors);
    RUN(encode_ansi_frame_skips_wide_glyph_continuation);
    RUN(decode_input_maps_printables_and_named_keys);
    RUN(decode_input_arrows_and_mouse);
    RUN(decode_input_escape_boundary_is_bounded);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
