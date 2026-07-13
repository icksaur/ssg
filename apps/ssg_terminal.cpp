#include "ssg_terminal.h"

#include <ssg/theme.h>

#include <algorithm>
#include <system_error>

namespace ssg::app {

namespace fs = std::filesystem;

LaunchTarget resolve_launch(fs::path const& argument) {
    if (argument.empty()) {
        return {fs::current_path(), std::nullopt};
    }
    std::error_code code;
    if (fs::is_directory(argument, code)) {
        return {fs::absolute(argument), std::nullopt};
    }
    auto absolute = fs::absolute(argument);
    auto parent =
        absolute.has_parent_path() ? absolute.parent_path() : fs::current_path();
    return {parent, absolute.filename().string()};
}

std::string encode_ansi_frame(ssg::CellGrid const& screen) {
    constexpr std::size_t max_index = ssg::theme_palette_size - 1;
    auto color = [&](std::uint8_t index, char kind) {
        auto const& c = screen.palette[std::min<std::size_t>(index, max_index)];
        return "\x1b[" + std::string{kind} + "8;2;" +
               std::to_string(c.red) + ";" + std::to_string(c.green) + ";" +
               std::to_string(c.blue) + "m";
    };

    std::string out = "\x1b[H";
    int const columns = screen.size.columns;
    int const rows = screen.size.rows;
    for (int y = 0; y < rows; ++y) {
        out += "\x1b[" + std::to_string(y + 1) + ";1H\x1b[0m";
        int foreground = -1;
        int background = -1;
        for (int x = 0; x < columns; ++x) {
            auto const& cell =
                screen.cells[static_cast<std::size_t>(y * columns + x)];
            if (cell.continuation) continue;
            if (cell.foreground != foreground || cell.background != background) {
                out += color(cell.foreground, '3');
                out += color(cell.background, '4');
                foreground = cell.foreground;
                background = cell.background;
            }
            out += cell.text.empty() ? std::string{" "} : cell.text;
        }
    }
    out += "\x1b[0m";
    return out;
}

namespace {

// Parse a leading run of decimal digits; returns the value and advances `pos`.
std::int64_t parse_decimal(std::string_view text, std::size_t& pos) {
    std::int64_t value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        value = value * 10 + (text[pos] - '0');
        ++pos;
    }
    return value;
}

}  // namespace

InputEvent parse_input(std::string_view bytes, std::size_t& consumed) {
    consumed = 0;
    if (bytes.empty()) return {};

    auto const first = static_cast<unsigned char>(bytes[0]);
    if (first != 0x1b) {
        consumed = 1;  // Milestone 2 has no text entry; skip ordinary bytes.
        return {};
    }
    if (bytes.size() < 2) return {};  // Lone ESC: wait for the rest.

    auto const second = static_cast<unsigned char>(bytes[1]);
    if (second != '[' && second != 'O') {
        // ESC followed by any non-CSI byte is a two-key chord (ESC b, ESC Q).
        consumed = 2;
        return {InputAction::chord, 0, static_cast<char>(second)};
    }
    if (bytes.size() < 3) return {};

    auto const third = static_cast<unsigned char>(bytes[2]);
    switch (third) {
    case 'A':
        consumed = 3;
        return {InputAction::scroll_lines, -1};
    case 'B':
        consumed = 3;
        return {InputAction::scroll_lines, 1};
    case 'C':
    case 'D':
    case 'H':
    case 'F':
        consumed = 3;  // Horizontal/home/end: unhandled in milestone 2.
        return {};
    case '5':
    case '6': {
        if (bytes.size() < 4) return {};
        consumed = 4;
        if (bytes[3] == '~') {
            return {InputAction::scroll_pages, third == '5' ? -1 : 1};
        }
        return {};
    }
    case '<': {
        // SGR mouse: ESC [ < Cb ; Cx ; Cy (M|m).  Wheel up is 64, down is 65.
        std::size_t end = 3;
        while (end < bytes.size() && bytes[end] != 'M' && bytes[end] != 'm') {
            ++end;
        }
        if (end >= bytes.size()) return {};  // Incomplete report.
        std::size_t pos = 3;
        auto const button = parse_decimal(bytes, pos);
        consumed = end + 1;
        if (button == 64) return {InputAction::scroll_lines, -3};
        if (button == 65) return {InputAction::scroll_lines, 3};
        return {};
    }
    case 'M': {
        // Legacy X10 mouse: ESC [ M b x y.  Wheel up is 0x60, down is 0x61.
        if (bytes.size() < 6) return {};
        auto const button = static_cast<unsigned char>(bytes[3]);
        consumed = 6;
        if (button == 0x60) return {InputAction::scroll_lines, -3};
        if (button == 0x61) return {InputAction::scroll_lines, 3};
        return {};
    }
    default:
        consumed = 3;  // Unknown CSI: skip its introducer conservatively.
        return {};
    }
}

}  // namespace ssg::app
