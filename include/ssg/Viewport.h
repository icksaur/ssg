#pragma once

#include <ssg/GraphemeLayout.h>
#include <ssg/types.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ssg {

struct ViewportDimensions {
    uint32_t columns;
    uint32_t rows;

    ViewportDimensions(uint32_t columns, uint32_t rows);
    bool operator==(const ViewportDimensions&) const noexcept = default;
};

struct VisualRow {
    uint32_t logicalLine;
    uint32_t firstSpan;
    uint32_t spanCount;
    CellIndex startCell;
    uint32_t contentCells;
    uint32_t visibleCells;
    // Document-absolute byte offset of this VISUAL row's end — the position a
    // click at/past the row's last content cell takes (M8 click-past-EOL).  For a
    // final/unwrapped visual row this is the logical end-of-line (the newline byte,
    // or text.size() for the last line without a trailing newline); for an interior
    // wrapped row it is the wrap boundary; for an empty line it is the line start.
    // Always a valid document position.
    uint32_t endByteOffset;

    bool operator==(const VisualRow&) const noexcept = default;
};

struct CellHitTarget {
    uint32_t viewportRow;
    uint32_t viewportColumn;
    uint32_t logicalLine;
    CellIndex cell;
    uint32_t byteOffset;
    uint32_t byteLen;

    bool operator==(const CellHitTarget&) const noexcept = default;
};

struct ScrollbarMetrics {
    uint32_t totalRows;
    uint32_t viewportRows;
    uint32_t firstRow;
    uint32_t maximumFirstRow;
    uint32_t thumbStart;
    uint32_t thumbSize;

    bool operator==(const ScrollbarMetrics&) const noexcept = default;
};

// A resolved scroll view for a simple list region: the clamped first visible
// item, how many items are visible, and the scrollbar geometry.  This is the
// generalized primitive the tree and palette use, mirroring what
// `Viewport::compute` produces for the editor.
struct ListScrollView {
    uint32_t firstVisible;
    uint32_t visibleCount;
    ScrollbarMetrics scrollbar;

    bool operator==(const ListScrollView&) const noexcept = default;
};

struct ViewportViewState {
    ViewportDimensions dimensions;
    uint32_t firstVisualRow;
    // The horizontal scroll offset in cells (word wrap OFF only; always 0 when
    // word wrap is on, since wrapped lines never scroll horizontally).  All
    // visible rows share this single per-pane offset.  It snaps to a grapheme
    // boundary: the leftmost visible cell is the first span start >= the requested
    // offset, so a wide cluster is never split (M12 VP-H / Decision A / H0).
    uint32_t firstVisualColumn;
    uint32_t totalVisualRows;
    std::vector<VisualRow> visibleRows;
    std::vector<CellHitTarget> hitTargets;
    ScrollbarMetrics scrollbar;

    bool operator==(const ViewportViewState&) const noexcept = default;
};

struct ViewportDelta {
    bool changed;
    std::optional<ViewportViewState> replacement;

    bool operator==(const ViewportDelta&) const noexcept = default;
};

class Viewport {
public:
    // The scrollbar thumb geometry for a list of `total_rows` items shown in a
    // `viewport_rows`-tall window scrolled to `first_row`.  When the content fits
    // (`total_rows <= viewport_rows`) the thumb is hidden: `maximum_first_row`,
    // `thumb_start`, and `thumb_size` collapse to a no-thumb sentinel.  Shared by
    // every scrollable region (editor, tree, palette) so thumb math lives in one
    // place (see doc/spec-scroll.md).
    [[nodiscard]] ScrollbarMetrics scrollbarMetrics(
        uint32_t totalRows,
        uint32_t viewportRows,
        uint32_t firstRow) const;

    // Resolve a list scroll view.  `first_visible` is always clamped to
    // `[0, maximum_first_row]`.  Keep-visible is an explicit input, never inferred:
    // only when `keep_selection_visible` is true AND `selected` holds an item index
    // does the window shift minimally so `selected` lies within
    // `[first_visible, first_visible + visible_count)`.  With
    // `keep_selection_visible == false` the (clamped) `first_visible` is honored
    // verbatim and the selection may fall outside the window, exactly as the editor
    // caret can.  `selected` is an absolute item index.
    [[nodiscard]] ListScrollView listScrollView(
        uint32_t totalItems,
        uint32_t viewportRows,
        uint32_t firstVisible,
        std::optional<uint32_t> selected,
        bool keepSelectionVisible) const;

    [[nodiscard]] ViewportViewState compute(
        std::span<const CellRun> logicalLines,
        ViewportDimensions dimensions,
        uint32_t requestedFirstVisualRow = 0) const;

    // Word-wrap-OFF viewport projection.  Builds the SAME ViewportViewState shape as
    // `compute` for a NON-wrapping document, but in O(visible rows) grapheme
    // segmentation instead of O(document): the total visual row count is the logical
    // line count (a byte scan for '\n'), and compute_cell_run runs only for the
    // visible lines.  Long lines are clipped at `dimensions.columns` (cells beyond
    // the width are not emitted).  For documents whose lines all fit the width, the
    // result is field-for-field equal to
    // `compute(active_cell_runs(document_text), dimensions, first_row)` — the
    // reference oracle (INV-projection-equivalence).  `tab_width` must match the full
    // path's (4 today).  Hit-target byte offsets are document-absolute.
    [[nodiscard]] ViewportViewState computeUnwrapped(
        std::string_view documentText,
        ViewportDimensions dimensions,
        uint32_t requestedFirstVisualRow,
        uint32_t requestedFirstVisualColumn,
        int tabWidth) const;

    [[nodiscard]] ViewportViewState scrollBy(
        std::span<const CellRun> logicalLines,
        ViewportDimensions dimensions,
        uint32_t currentFirstVisualRow,
        int64_t rowDelta) const;

    [[nodiscard]] ViewportDelta deriveDelta(
        const ViewportViewState& previous,
        const ViewportViewState& current) const;
};

}  // namespace ssg
