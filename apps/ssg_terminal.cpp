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

std::string encode_ansi_frame(ssg::tui::ScreenSnapshot const& screen) {
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

}  // namespace ssg::app
