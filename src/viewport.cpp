#include <ssg/viewport.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ssg {
namespace {

uint32_t checked_u32(std::size_t value, const char* what) {
    if (value > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error(what);
    }
    return static_cast<uint32_t>(value);
}

std::vector<VisualRow> wrap_rows(std::span<const CellRun> lines,
                                 uint32_t columns) {
    std::vector<VisualRow> rows;
    for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
        const auto logical_line =
            checked_u32(line_index, "viewport logical line count exceeds uint32");
        const auto& line = lines[line_index];
        if (line.spans.empty()) {
            rows.push_back(
                VisualRow{logical_line, 0, 0, CellIndex{0}, 0, 0});
            continue;
        }

        uint32_t first_span = 0;
        uint32_t span_count = 0;
        uint32_t start_cell = 0;
        uint32_t content_cells = 0;

        const auto finish_row = [&] {
            rows.push_back(VisualRow{
                logical_line,
                first_span,
                span_count,
                CellIndex{start_cell},
                content_cells,
                std::min(content_cells, columns),
            });
        };

        for (std::size_t span_index = 0; span_index < line.spans.size();
             ++span_index) {
            const auto width = line.spans[span_index].cell_width;
            if (width > 0 && content_cells > 0 &&
                width > columns - std::min(content_cells, columns)) {
                finish_row();
                first_span = checked_u32(
                    span_index, "viewport span count exceeds uint32");
                span_count = 0;
                start_cell += content_cells;
                content_cells = 0;
            }
            ++span_count;
            content_cells += width;
        }
        finish_row();
    }
    if (rows.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error("viewport visual row count exceeds uint32");
    }
    return rows;
}

ScrollbarMetrics scrollbar_metrics_impl(uint32_t total_rows,
                                        uint32_t viewport_rows,
                                        uint32_t first_row) {
    const uint32_t maximum_first =
        total_rows > viewport_rows ? total_rows - viewport_rows : 0;
    if (maximum_first == 0) {
        return ScrollbarMetrics{
            total_rows, viewport_rows, 0, 0, 0, viewport_rows};
    }

    const auto scaled_size =
        (static_cast<uint64_t>(viewport_rows) * viewport_rows) / total_rows;
    const uint32_t thumb_size =
        std::max<uint32_t>(1, static_cast<uint32_t>(scaled_size));
    const uint32_t travel = viewport_rows - thumb_size;
    const uint32_t thumb_start = static_cast<uint32_t>(
        (static_cast<uint64_t>(first_row) * travel) / maximum_first);
    return ScrollbarMetrics{total_rows,
                            viewport_rows,
                            first_row,
                            maximum_first,
                            thumb_start,
                            thumb_size};
}

}  // namespace

ScrollbarMetrics scrollbar_metrics(uint32_t total_rows, uint32_t viewport_rows,
                                   uint32_t first_row) {
    return scrollbar_metrics_impl(total_rows, viewport_rows, first_row);
}

ListScrollView compute_list_scroll_view(uint32_t total_items,
                                        uint32_t viewport_rows,
                                        uint32_t first_visible,
                                        std::optional<uint32_t> selected,
                                        bool keep_selection_visible) {
    if (viewport_rows == 0) {
        return ListScrollView{0, 0, scrollbar_metrics_impl(total_items, 0, 0)};
    }
    const uint32_t maximum_first =
        total_items > viewport_rows ? total_items - viewport_rows : 0;
    uint32_t first = std::min(first_visible, maximum_first);

    // Keep-visible only shifts the window when scrolling is possible and the
    // caller opted in with a selection.  It is never inferred from `selected`
    // alone: explicit scroll (keep_selection_visible == false) leaves `first`
    // at the clamped request even if the selection falls outside the window.
    if (keep_selection_visible && selected && maximum_first > 0) {
        const uint32_t target = std::min(*selected, total_items - 1);
        if (target < first) {
            first = target;
        } else if (target >= first + viewport_rows) {
            first = target - viewport_rows + 1;
        }
        first = std::min(first, maximum_first);
    }

    const uint32_t visible_count =
        std::min(viewport_rows, total_items - first);
    return ListScrollView{first, visible_count,
                          scrollbar_metrics_impl(total_items, viewport_rows,
                                                 first)};
}

ViewportDimensions::ViewportDimensions(uint32_t column_count,
                                       uint32_t row_count)
    : columns(column_count), rows(row_count) {
    if (columns == 0) {
        throw std::invalid_argument(
            "viewport columns must be greater than zero");
    }
    if (rows == 0) {
        throw std::invalid_argument("viewport rows must be greater than zero");
    }
}

ViewportViewState compute_viewport(std::span<const CellRun> logical_lines,
                                   ViewportDimensions dimensions,
                                   uint32_t requested_first_visual_row) {
    const auto all_rows = wrap_rows(logical_lines, dimensions.columns);
    const auto total_rows =
        checked_u32(all_rows.size(), "viewport visual row count exceeds uint32");
    const uint32_t maximum_first =
        total_rows > dimensions.rows ? total_rows - dimensions.rows : 0;
    const uint32_t first_row =
        std::min(requested_first_visual_row, maximum_first);

    std::vector<VisualRow> visible_rows;
    std::vector<CellHitTarget> hit_targets;
    const auto visible_count =
        std::min<uint32_t>(dimensions.rows, total_rows - first_row);
    visible_rows.reserve(visible_count);

    for (uint32_t viewport_row = 0; viewport_row < visible_count;
         ++viewport_row) {
        const auto& row = all_rows[first_row + viewport_row];
        visible_rows.push_back(row);
        const auto& line = logical_lines[row.logical_line];
        uint32_t viewport_column = 0;
        uint64_t logical_cell = row.start_cell.value();
        const uint32_t end_span = row.first_span + row.span_count;
        for (uint32_t span_index = row.first_span; span_index < end_span;
             ++span_index) {
            const auto& span = line.spans[span_index];
            const auto available = dimensions.columns - viewport_column;
            const auto visible_width = std::min(span.cell_width, available);
            for (uint32_t cell = 0; cell < visible_width; ++cell) {
                hit_targets.push_back(CellHitTarget{
                    viewport_row,
                    viewport_column + cell,
                    row.logical_line,
                    CellIndex{logical_cell},
                    span.byte_offset,
                    span.byte_len,
                });
            }
            viewport_column += visible_width;
            logical_cell += span.cell_width;
        }
    }

    return ViewportViewState{
        dimensions,
        first_row,
        total_rows,
        std::move(visible_rows),
        std::move(hit_targets),
        scrollbar_metrics(total_rows, dimensions.rows, first_row),
    };
}

ViewportViewState scroll_viewport_by(
    std::span<const CellRun> logical_lines,
    ViewportDimensions dimensions,
    uint32_t current_first_visual_row,
    int64_t row_delta) {
    uint32_t requested = current_first_visual_row;
    if (row_delta >= 0) {
        const auto delta = static_cast<uint64_t>(row_delta);
        requested = delta > std::numeric_limits<uint32_t>::max() - requested
                        ? std::numeric_limits<uint32_t>::max()
                        : requested + static_cast<uint32_t>(delta);
    } else {
        const uint64_t magnitude =
            static_cast<uint64_t>(-(row_delta + 1)) + 1;
        requested = magnitude > requested
                        ? 0
                        : requested - static_cast<uint32_t>(magnitude);
    }
    return compute_viewport(logical_lines, dimensions, requested);
}

ViewportDelta derive_viewport_delta(const ViewportViewState& previous,
                                    const ViewportViewState& current) {
    if (previous == current) {
        return ViewportDelta{false, std::nullopt};
    }
    return ViewportDelta{true, current};
}

}  // namespace ssg
