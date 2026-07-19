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
ScrollbarMetrics scrollbar_metrics(uint32_t total_rows, uint32_t viewport_rows,
                                   uint32_t first_row);

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
ListScrollView compute_list_scroll_view(uint32_t total_items,
                                        uint32_t viewport_rows,
                                        uint32_t first_visible,
                                        std::optional<uint32_t> selected,
                                        bool keep_selection_visible);


struct ViewportViewState {
    ViewportDimensions dimensions;
    uint32_t first_visual_row;
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

ViewportViewState compute_viewport(
    std::span<const CellRun> logical_lines,
    ViewportDimensions dimensions,
    uint32_t requested_first_visual_row = 0);

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
ViewportViewState compute_viewport_unwrapped(
    std::string_view document_text,
    ViewportDimensions dimensions,
    uint32_t requested_first_visual_row,
    int tab_width);

ViewportViewState scroll_viewport_by(
    std::span<const CellRun> logical_lines,
    ViewportDimensions dimensions,
    uint32_t current_first_visual_row,
    int64_t row_delta);

ViewportDelta derive_viewport_delta(const ViewportViewState& previous,
                                    const ViewportViewState& current);

}  // namespace ssg
