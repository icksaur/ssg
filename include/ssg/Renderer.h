#pragma once

// The authoritative cell renderer.  ssg::Renderer{}.render turns a
// SessionSnapshot into a
// deterministic monospace CellGrid: the single place where shell geometry and
// content become cells.  Clients (terminal, browser) only translate the grid to
// their medium; they add no layout, content, or color.

#include <ssg/session_snapshot.h>
#include <ssg/Style.h>
#include <ssg/Theme.h>
#include <ssg/ShellState.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ssg {

enum class DiffTint : std::uint8_t {
    None,
    AddedRow,
    RemovedRow,
    ModifiedRow,
    AddedWord,
    RemovedWord,
    ModifiedWord,
};

struct CellGridCell {
    std::string text{" "};
    std::uint8_t foreground{0};
    std::uint8_t background{0};
    SemanticRole role{SemanticRole::Background};
    bool continuation{false};
    DiffTint tint{DiffTint::None};

    bool operator==(CellGridCell const&) const = default;
};

// The primary caret's cell, for the client to place its terminal/hardware cursor.
struct GridPosition {
    int column = 0;
    int row = 0;

    bool operator==(GridPosition const&) const = default;
};

struct CellGrid {
    GridSize size;
    std::array<SrgbColor, kThemePaletteSize> palette{};
    std::vector<CellGridCell> cells;
    std::optional<GridPosition> caret;
    DiffTints diffTints;
    SrgbColor selectionFill{};

    [[nodiscard]] CellGridCell const& at(int column, int row) const;
    [[nodiscard]] std::string canonical() const;
};

class Renderer {
public:
    // The chrome glyphs and dimensions this renderer draws with.  Public and
    // assignable so a client can restyle without a new render entry point;
    // defaults reproduce the shipped appearance.  Style will move onto the
    // snapshot when it becomes a published, wire-visible section
    // (doc/spec-style.md Y4).
    Style style{};

    [[nodiscard]] CellGrid render(SessionSnapshot const& snapshot) const;

    // Test instrumentation (M12 INV-render-projection).  Counts the
    // compute_cell_run (grapheme-segmentation) calls render() has made since the
    // last reset.  This is a diagnostic counter, not production state; it lets a
    // test assert that render segments only the logical lines the viewport shows
    // (<= viewport rows), never the whole document — so a regression to
    // whole-document segmentation fails the count oracle.  Not thread-safe across
    // concurrent render() calls (per-thread counter).
    [[nodiscard]] static std::uint64_t renderSegmentationCalls();
    static void resetRenderSegmentationCalls();
};

}  // namespace ssg
