#pragma once

#include <ssg/layout.h>
#include <ssg/types.h>

#include <cstdint>
#include <optional>
#include <span>
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

ViewportViewState scroll_viewport_by(
    std::span<const CellRun> logical_lines,
    ViewportDimensions dimensions,
    uint32_t current_first_visual_row,
    int64_t row_delta);

ViewportDelta derive_viewport_delta(const ViewportViewState& previous,
                                    const ViewportViewState& current);

}  // namespace ssg
