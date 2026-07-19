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
            // Empty line: the row end is the line start (line-relative offset 0);
            // compute_viewport adds the line's document start.
            rows.push_back(
                VisualRow{logical_line, 0, 0, CellIndex{0}, 0, 0, 0});
            continue;
        }

        uint32_t first_span = 0;
        uint32_t span_count = 0;
        uint32_t start_cell = 0;
        uint32_t content_cells = 0;

        const auto finish_row = [&] {
            // The row's end is its LAST span's line-relative end byte
            // (byte_offset + byte_len); compute_viewport adds the line's document
            // start to make it document-absolute.
            const auto& last = line.spans[first_span + span_count - 1];
            rows.push_back(VisualRow{
                logical_line,
                first_span,
                span_count,
                CellIndex{start_cell},
                content_cells,
                std::min(content_cells, columns),
                last.byte_offset + last.byte_len,
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

    // A CellSpan's byte_offset is relative to its logical line, but a hit target
    // must carry a DOCUMENT-absolute byte offset (so resolve_document_position
    // maps it to the right line, not always line 0). Reconstruct each logical
    // line's document start: lines are separated by exactly one '\n', and
    // compute_cell_run accounts for every content byte, so a line's document
    // start is the running sum of prior lines' content bytes plus one separator
    // byte each -- the same accounting TextModel uses (start = newline + 1).
    std::vector<uint32_t> line_document_start(logical_lines.size(), 0);
    {
        uint64_t document_byte = 0;
        for (std::size_t line = 0; line < logical_lines.size(); ++line) {
            line_document_start[line] =
                checked_u32(document_byte, "viewport byte offset exceeds uint32");
            uint64_t content_bytes = 0;
            for (const auto& span : logical_lines[line].spans) {
                content_bytes += span.byte_len;
            }
            document_byte += content_bytes + 1;  // +1 for the '\n' separator
        }
    }

    std::vector<VisualRow> visible_rows;
    std::vector<CellHitTarget> hit_targets;
    const auto visible_count =
        std::min<uint32_t>(dimensions.rows, total_rows - first_row);
    visible_rows.reserve(visible_count);

    for (uint32_t viewport_row = 0; viewport_row < visible_count;
         ++viewport_row) {
        auto row = all_rows[first_row + viewport_row];
        // wrap_rows stored a LINE-RELATIVE end offset; make it document-absolute.
        row.end_byte_offset += line_document_start[row.logical_line];
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
                    line_document_start[row.logical_line] + span.byte_offset,
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
        0,  // word-wrap-ON never scrolls horizontally (first_visual_column)
        total_rows,
        std::move(visible_rows),
        std::move(hit_targets),
        scrollbar_metrics(total_rows, dimensions.rows, first_row),
    };
}

ViewportViewState compute_viewport_unwrapped(
    std::string_view document_text, ViewportDimensions dimensions,
    uint32_t requested_first_visual_row, uint32_t requested_first_visual_column,
    int tab_width) {
    // Word wrap OFF: one logical line renders as exactly one visual row, clipped
    // to the pane width.  The total visual row count is the logical line count, a
    // cheap byte scan for '\n' — NO compute_cell_run over the whole document.
    // compute_cell_run runs only for the visible lines.  For documents whose lines
    // all fit `dimensions.columns`, the result is field-for-field equal to
    // compute_viewport(active_cell_runs(document_text), ...) (INV-projection-
    // equivalence): the line count matches (active_cell_runs splits on '\n' the
    // same way), and because compute_cell_run accounts for every content byte, the
    // '\n' offsets recorded here equal the running-sum offsets compute_viewport
    // derives — so hit-target byte offsets are identical and document-absolute.
    std::vector<std::size_t> line_start{0};
    for (std::size_t newline = document_text.find('\n');
         newline != std::string_view::npos;
         newline = document_text.find('\n', newline + 1)) {
        line_start.push_back(newline + 1);
    }
    const auto total_rows =
        checked_u32(line_start.size(), "viewport line count exceeds uint32");
    const uint32_t maximum_first =
        total_rows > dimensions.rows ? total_rows - dimensions.rows : 0;
    const uint32_t first_row =
        std::min(requested_first_visual_row, maximum_first);
    const uint32_t visible_count =
        std::min<uint32_t>(dimensions.rows, total_rows - first_row);

    std::vector<VisualRow> visible_rows;
    std::vector<CellHitTarget> hit_targets;
    visible_rows.reserve(visible_count);

    // Horizontal scroll (VP-H / H0): every visible row windows from the SAME
    // requested offset `requested_first_visual_column` (the shared per-pane left
    // origin, which is the reported first_visual_column).  Each row snaps that
    // offset to its OWN grapheme boundary (a wide cluster straddling the offset
    // scrolls fully off rather than splitting), recorded per row in
    // VisualRow.start_cell / first_span — so a short row and a long row can have
    // different per-row origins while sharing one pane offset.  Render paints each
    // row's spans from the pane's left edge using VisualRow.first_span, so it needs
    // no change; at offset 0 this is exactly the pre-VP-H behavior, so
    // INV-projection-equivalence for fitting lines holds.
    for (uint32_t viewport_row = 0; viewport_row < visible_count; ++viewport_row) {
        const uint32_t logical_line = first_row + viewport_row;
        const std::size_t start = line_start[logical_line];
        const std::size_t end = logical_line + 1 < line_start.size()
                                    ? line_start[logical_line + 1] - 1
                                    : document_text.size();
        const auto run = compute_cell_run(
            document_text.substr(start, end - start), tab_width);
        const auto document_start =
            checked_u32(start, "viewport byte offset exceeds uint32");
        // The row's end is the line's TRUE end (newline byte, or text.size() for
        // the last line) — the FULL line, independent of the horizontal clip.
        const auto end_byte_offset =
            checked_u32(end, "viewport byte offset exceeds uint32");

        // Locate the first span at or past the requested horizontal offset; its
        // start cell is this row's visible origin.  A row shorter than the offset
        // contributes no visible spans.
        uint32_t first_span = 0;
        uint32_t start_cell = 0;
        for (; first_span < run.spans.size(); ++first_span) {
            if (start_cell >= requested_first_visual_column) break;
            start_cell += run.spans[first_span].cell_width;
        }

        if (first_span >= run.spans.size()) {
            // Empty line, or the whole line scrolled off to the left.
            visible_rows.push_back(
                VisualRow{logical_line, first_span, 0, CellIndex{start_cell},
                          run.total_cells, 0, end_byte_offset});
            continue;
        }

        uint32_t viewport_column = 0;
        uint32_t logical_cell = start_cell;
        uint32_t span_count = 0;
        for (uint32_t span_index = first_span; span_index < run.spans.size();
             ++span_index) {
            const auto& span = run.spans[span_index];
            const auto available = dimensions.columns - viewport_column;
            if (available == 0) break;
            const auto visible_width = std::min(span.cell_width, available);
            for (uint32_t cell = 0; cell < visible_width; ++cell) {
                hit_targets.push_back(CellHitTarget{
                    viewport_row,
                    viewport_column + cell,
                    logical_line,
                    CellIndex{logical_cell},
                    document_start + span.byte_offset,
                    span.byte_len,
                });
            }
            viewport_column += visible_width;
            logical_cell += span.cell_width;
            ++span_count;
        }
        const uint32_t content_from_offset =
            run.total_cells > start_cell ? run.total_cells - start_cell : 0;
        visible_rows.push_back(
            VisualRow{logical_line, first_span, span_count, CellIndex{start_cell},
                      run.total_cells,
                      std::min(content_from_offset, dimensions.columns),
                      end_byte_offset});
    }

    return ViewportViewState{
        dimensions,
        first_row,
        requested_first_visual_column,
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
