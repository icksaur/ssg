#pragma once

// The authoritative cell renderer.  ssg::render turns a SessionSnapshot into a
// deterministic monospace CellGrid: the single place where shell geometry and
// content become cells.  Clients (terminal, browser) only translate the grid to
// their medium; they add no layout, content, or color.

#include <ssg/session_snapshot.h>
#include <ssg/theme.h>
#include <ssg/ui_layout.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ssg {

struct CellGridCell {
    std::string text{" "};
    std::uint8_t foreground{0};
    std::uint8_t background{0};
    SemanticRole role{SemanticRole::background};
    bool continuation{false};

    bool operator==(CellGridCell const&) const = default;
};

struct CellGrid {
    GridSize size;
    std::array<SrgbColor, theme_palette_size> palette{};
    std::vector<CellGridCell> cells;

    [[nodiscard]] CellGridCell const& at(int column, int row) const;
    [[nodiscard]] std::string canonical() const;
};

[[nodiscard]] CellGrid render(SessionSnapshot const& snapshot);

}  // namespace ssg
