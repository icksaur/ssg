#pragma once

// The authoritative cell renderer.  ssg::Renderer{}.render turns a
// SessionSnapshot into a
// deterministic monospace CellGrid: the single place where shell geometry and
// content become cells.  Clients (terminal, browser) only translate the grid to
// their medium; they add no layout, content, or color.

#include <ssg/GridPresenter.h>
#include <ssg/Style.h>
#include <ssg/Theme.h>
#include <ssg/ShellState.h>
#include <ssg/LineLayoutCache.h>

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

// A diagnostic underline on a cell.  Separate from foreground/background so a
// squiggle can sit under syntax-coloured text without taking its colour, and
// separate from DiffTint for the same reason: a diagnostic and a diff can both
// apply to the same cell and neither should erase the other.
enum class CellUnderline : std::uint8_t {
    None,
    Error,    // A red curly underline.
    Warning,  // A yellow curly underline.
    Info,     // Information and hint diagnostics, drawn plainer.
};

struct CellGridCell {
    std::string text{" "};
    std::uint8_t foreground{0};
    std::uint8_t background{0};
    SemanticRole role{SemanticRole::Canvas};
    bool continuation{false};
    DiffTint tint{DiffTint::None};
    CellUnderline underline{CellUnderline::None};

    bool operator==(CellGridCell const&) const = default;
};

// The primary caret's cell, for the client to place its terminal/hardware cursor.
struct GridPosition {
    int column = 0;
    int row = 0;

    bool operator==(GridPosition const&) const = default;
};

// A run of cells the terminal should make clickable (OSC 8).
//
// Kept as grid-level ranges rather than a string on every cell: a URI per cell
// would pay for a string on all of them to serve the few that are links, and the
// terminal needs the run's extent anyway to open and close the hyperlink.
struct CellHyperlink {
    int row = 0;
    int column = 0;
    int width = 0;
    std::string uri;

    bool operator==(CellHyperlink const&) const = default;
};

struct CellGrid {
    GridSize size;
    // The render color table: the theme's role colors (slots
    // 0..kSemanticRoleCount-1) followed by its scope colors. A cell's
    // foreground/background index into this; the client resolves colors[index]
    // to the terminal's depth. Replaces the former 16-color palette.
    std::array<SrgbColor, kThemeColorSlotCount> colors{};
    std::vector<CellGridCell> cells;
    std::optional<GridPosition> caret;
    std::vector<CellHyperlink> hyperlinks;
    DiffTints diffTints;
    SrgbColor selectionFill{};

    [[nodiscard]] CellGridCell const& at(int column, int row) const;
    [[nodiscard]] std::string canonical() const;
};

class Renderer {
public:
    // `lineCache`, when supplied, is a caller-owned bounded cache of shaped
    // document lines that survives across frames. render() borrows it to skip
    // re-segmenting on-screen lines that a previous frame already shaped; a
    // nullptr cache reproduces the exact pre-cache behaviour (a fresh shape per
    // visible line). The cache holds only stable document-line text.
    [[nodiscard]] CellGrid render(GridFrame const& snapshot,
                                  LineLayoutCache* lineCache = nullptr) const;

    // Style is no longer a renderer member: it is read from the snapshot's
    // published Style section, so the runtime and the
    // renderer share the one instance the runtime owns rather than two that can
    // drift.

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
