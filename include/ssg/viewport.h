#pragma once

#include <ssg/layout.h>
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
    uint32_t logical_line;
    uint32_t first_span;
    uint32_t span_count;
    CellIndex start_cell;
    uint32_t content_cells;
    uint32_t visible_cells;
    // Document-absolute byte offset of this VISUAL row's end — the position a
    // click at/past the row's last content cell takes (M8 click-past-EOL).  For a
    // final/unwrapped visual row this is the logical end-of-line (the newline byte,
    // or text.size() for the last line without a trailing newline); for an interior
    // wrapped row it is the wrap boundary; for an empty line it is the line start.
    // Always a valid document position.
    uint32_t end_byte_offset;

    bool operator==(const VisualRow&) const noexcept = default;
};

struct CellHitTarget {
    uint32_t viewport_row;
    uint32_t viewport_column;
    uint32_t logical_line;
    CellIndex cell;
    uint32_t byte_offset;
    uint32_t byte_len;

    bool operator==(const CellHitTarget&) const noexcept = default;
};

struct ScrollbarMetrics {
    uint32_t total_rows;
    uint32_t viewport_rows;
    uint32_t first_row;
    uint32_t maximum_first_row;
    uint32_t thumb_start;
    uint32_t thumb_size;

    bool operator==(const ScrollbarMetrics&) const noexcept = default;
};

// The scrollbar thumb geometry for a list of `total_rows` items shown in a
// `viewport_rows`-tall window scrolled to `first_row`.  When the content fits
// (`total_rows <= viewport_rows`) the thumb is hidden: `maximum_first_row`,
// `thumb_start`, and `thumb_size` collapse to a no-thumb sentinel.  Shared by
// every scrollable region (editor, tree, palette) so thumb math lives in one
// place (see doc/spec-scroll.md).
ScrollbarMetrics scrollbarMetrics(uint32_t totalRows, uint32_t viewportRows,
                                   uint32_t firstRow);

// A resolved scroll view for a simple list region: the clamped first visible
// item, how many items are visible, and the scrollbar geometry.  This is the
// generalized primitive the tree and palette use, mirroring what
// `compute_viewport` produces for the editor.
struct ListScrollView {
    uint32_t first_visible;
    uint32_t visible_count;
    ScrollbarMetrics scrollbar;

    bool operator==(const ListScrollView&) const noexcept = default;
};

// Resolve a list scroll view.  `first_visible` is always clamped to
// `[0, maximum_first_row]`.  Keep-visible is an explicit input, never inferred:
// only when `keep_selection_visible` is true AND `selected` holds an item index
// does the window shift minimally so `selected` lies within
// `[first_visible, first_visible + visible_count)`.  With
// `keep_selection_visible == false` the (clamped) `first_visible` is honored
// verbatim and the selection may fall outside the window, exactly as the editor
// caret can.  `selected` is an absolute item index.
ListScrollView computeListScrollView(uint32_t totalItems,
                                        uint32_t viewportRows,
                                        uint32_t firstVisible,
                                        std::optional<uint32_t> selected,
                                        bool keepSelectionVisible);


struct ViewportViewState {
    ViewportDimensions dimensions;
    uint32_t first_visual_row;
    // The horizontal scroll offset in cells (word wrap OFF only; always 0 when
    // word wrap is on, since wrapped lines never scroll horizontally).  All
    // visible rows share this single per-pane offset.  It snaps to a grapheme
    // boundary: the leftmost visible cell is the first span start >= the requested
    // offset, so a wide cluster is never split (M12 VP-H / Decision A / H0).
    uint32_t first_visual_column;
    uint32_t total_visual_rows;
    std::vector<VisualRow> visible_rows;
    std::vector<CellHitTarget> hit_targets;
    ScrollbarMetrics scrollbar;

    bool operator==(const ViewportViewState&) const noexcept = default;
};

struct ViewportDelta {
    bool changed;
    std::optional<ViewportViewState> replacement;

    bool operator==(const ViewportDelta&) const noexcept = default;
};

ViewportViewState computeViewport(
    std::span<const CellRun> logicalLines,
    ViewportDimensions dimensions,
    uint32_t requestedFirstVisualRow = 0);

// Word-wrap-OFF viewport projection.  Builds the SAME ViewportViewState shape as
// compute_viewport for a NON-wrapping document, but in O(visible rows) grapheme
// segmentation instead of O(document): the total visual row count is the logical
// line count (a byte scan for '\n'), and compute_cell_run runs only for the
// visible lines.  Long lines are clipped at `dimensions.columns` (cells beyond
// the width are not emitted).  For documents whose lines all fit the width, the
// result is field-for-field equal to
// compute_viewport(active_cell_runs(document_text), dimensions, first_row) — the
// reference oracle (INV-projection-equivalence).  `tab_width` must match the full
// path's (4 today).  Hit-target byte offsets are document-absolute.
ViewportViewState computeViewportUnwrapped(
    std::string_view documentText,
    ViewportDimensions dimensions,
    uint32_t requestedFirstVisualRow,
    uint32_t requestedFirstVisualColumn,
    int tabWidth);

ViewportViewState scrollViewportBy(
    std::span<const CellRun> logicalLines,
    ViewportDimensions dimensions,
    uint32_t currentFirstVisualRow,
    int64_t rowDelta);

ViewportDelta deriveViewportDelta(const ViewportViewState& previous,
                                    const ViewportViewState& current);

}  // namespace ssg
