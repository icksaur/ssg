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
    ssg::tui::ScreenSnapshot screen;
    screen.size = {2, 1};
    screen.palette[0] = {10, 20, 30};
    screen.palette[1] = {200, 100, 50};
    ssg::tui::ScreenCell left;
    left.text = "X";
    left.foreground = 1;
    left.background = 0;
    ssg::tui::ScreenCell right;
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
    ssg::tui::ScreenSnapshot screen;
    screen.size = {2, 1};
    screen.palette[0] = {0, 0, 0};
    ssg::tui::ScreenCell wide;
    wide.text = "\xe4\xb8\xad";  // U+4E2D, a double-width glyph.
    ssg::tui::ScreenCell continuation;
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

int main() {
    RUN(resolve_launch_no_argument_opens_cwd);
    RUN(resolve_launch_directory_opens_that_directory);
    RUN(resolve_launch_file_opens_parent_directory_and_file);
    RUN(encode_ansi_frame_addresses_rows_and_emits_palette_colors);
    RUN(encode_ansi_frame_skips_wide_glyph_continuation);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
