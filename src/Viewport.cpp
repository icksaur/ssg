#include <ssg/Viewport.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ssg {
namespace {

uint32_t checkedU32(std::size_t value, const char* what) {
    if (value > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error(what);
    }
    return static_cast<uint32_t>(value);
}

std::vector<VisualRow> wrapRows(std::span<const CellRun> lines,
                                 uint32_t columns) {
    std::vector<VisualRow> rows;
    for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const auto logicalLine =
            checkedU32(lineIndex, "viewport logical line count exceeds uint32");
        const auto& line = lines[lineIndex];
        if (line.spans.empty()) {
            // Empty line: the row end is the line start (line-relative offset 0);
            // compute_viewport adds the line's document start.
            rows.push_back(
                VisualRow{logicalLine, 0, 0, CellIndex{0}, 0, 0, 0});
            continue;
        }

        uint32_t firstSpan = 0;
        uint32_t spanCount = 0;
        uint32_t startCell = 0;
        uint32_t contentCells = 0;

        const auto finishRow = [&] {
            // The row's end is its LAST span's line-relative end byte
            // (byte_offset + byte_len); compute_viewport adds the line's document
            // start to make it document-absolute.
            const auto& last = line.spans[firstSpan + spanCount - 1];
            rows.push_back(VisualRow{
                logicalLine,
                firstSpan,
                spanCount,
                CellIndex{startCell},
                contentCells,
                std::min(contentCells, columns),
                last.byteOffset + last.byteLen,
            });
        };

        for (std::size_t spanIndex = 0; spanIndex < line.spans.size();
             ++spanIndex) {
            const auto width = line.spans[spanIndex].cellWidth;
            if (width > 0 && contentCells > 0 &&
                width > columns - std::min(contentCells, columns)) {
                finishRow();
                firstSpan = checkedU32(
                    spanIndex, "viewport span count exceeds uint32");
                spanCount = 0;
                startCell += contentCells;
                contentCells = 0;
            }
            ++spanCount;
            contentCells += width;
        }
        finishRow();
    }
    if (rows.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error("viewport visual row count exceeds uint32");
    }
    return rows;
}

ScrollbarMetrics scrollbarMetricsImpl(uint32_t totalRows,
                                        uint32_t viewportRows,
                                        uint32_t firstRow) {
    const uint32_t maximumFirst =
        totalRows > viewportRows ? totalRows - viewportRows : 0;
    if (maximumFirst == 0) {
        return ScrollbarMetrics{
            totalRows, viewportRows, 0, 0, 0, viewportRows};
    }

    const auto scaledSize =
        (static_cast<uint64_t>(viewportRows) * viewportRows) / totalRows;
    const uint32_t thumbSize =
        std::max<uint32_t>(1, static_cast<uint32_t>(scaledSize));
    const uint32_t travel = viewportRows - thumbSize;
    const uint32_t thumbStart = static_cast<uint32_t>(
        (static_cast<uint64_t>(firstRow) * travel) / maximumFirst);
    return ScrollbarMetrics{totalRows,
                            viewportRows,
                            firstRow,
                            maximumFirst,
                            thumbStart,
                            thumbSize};
}

}  // namespace

ScrollbarMetrics Viewport::scrollbarMetrics(uint32_t totalRows,
                                            uint32_t viewportRows,
                                            uint32_t firstRow) const {
    return scrollbarMetricsImpl(totalRows, viewportRows, firstRow);
}

ListScrollView Viewport::listScrollView(
    uint32_t totalItems,
    uint32_t viewportRows,
    uint32_t firstVisible,
    std::optional<uint32_t> selected,
    bool keepSelectionVisible) const {
    if (viewportRows == 0) {
        return ListScrollView{0, 0, scrollbarMetricsImpl(totalItems, 0, 0)};
    }
    const uint32_t maximumFirst =
        totalItems > viewportRows ? totalItems - viewportRows : 0;
    uint32_t first = std::min(firstVisible, maximumFirst);

    // Keep-visible only shifts the window when scrolling is possible and the
    // caller opted in with a selection.  It is never inferred from `selected`
    // alone: explicit scroll (keep_selection_visible == false) leaves `first`
    // at the clamped request even if the selection falls outside the window.
    if (keepSelectionVisible && selected && maximumFirst > 0) {
        const uint32_t target = std::min(*selected, totalItems - 1);
        if (target < first) {
            first = target;
        } else if (target >= first + viewportRows) {
            first = target - viewportRows + 1;
        }
        first = std::min(first, maximumFirst);
    }

    const uint32_t visibleCount =
        std::min(viewportRows, totalItems - first);
    return ListScrollView{first, visibleCount,
                          scrollbarMetricsImpl(totalItems, viewportRows,
                                                 first)};
}

ViewportDimensions::ViewportDimensions(uint32_t columnCount,
                                       uint32_t rowCount)
    : columns(columnCount), rows(rowCount) {
    if (columns == 0) {
        throw std::invalid_argument(
            "viewport columns must be greater than zero");
    }
    if (rows == 0) {
        throw std::invalid_argument("viewport rows must be greater than zero");
    }
}

ViewportViewState Viewport::compute(std::span<const CellRun> logicalLines,
                                    ViewportDimensions dimensions,
                                    uint32_t requestedFirstVisualRow) const {
    const auto allRows = wrapRows(logicalLines, dimensions.columns);
    const auto totalRows =
        checkedU32(allRows.size(), "viewport visual row count exceeds uint32");
    const uint32_t maximumFirst =
        totalRows > dimensions.rows ? totalRows - dimensions.rows : 0;
    const uint32_t firstRow =
        std::min(requestedFirstVisualRow, maximumFirst);

    // A CellSpan's byte_offset is relative to its logical line, but a hit target
    // must carry a DOCUMENT-absolute byte offset (so resolve_document_position
    // maps it to the right line, not always line 0). Reconstruct each logical
    // line's document start: lines are separated by exactly one '\n', and
    // compute_cell_run accounts for every content byte, so a line's document
    // start is the running sum of prior lines' content bytes plus one separator
    // byte each -- the same accounting TextModel uses (start = newline + 1).
    std::vector<uint32_t> lineDocumentStart(logicalLines.size(), 0);
    {
        uint64_t documentByte = 0;
        for (std::size_t line = 0; line < logicalLines.size(); ++line) {
            lineDocumentStart[line] =
                checkedU32(documentByte, "viewport byte offset exceeds uint32");
            uint64_t contentBytes = 0;
            for (const auto& span : logicalLines[line].spans) {
                contentBytes += span.byteLen;
            }
            documentByte += contentBytes + 1;  // +1 for the '\n' separator
        }
    }

    std::vector<VisualRow> visibleRows;
    std::vector<CellHitTarget> hitTargets;
    const auto visibleCount =
        std::min<uint32_t>(dimensions.rows, totalRows - firstRow);
    visibleRows.reserve(visibleCount);

    for (uint32_t viewportRow = 0; viewportRow < visibleCount;
         ++viewportRow) {
        auto row = allRows[firstRow + viewportRow];
        // wrap_rows stored a LINE-RELATIVE end offset; make it document-absolute.
        row.endByteOffset += lineDocumentStart[row.logicalLine];
        visibleRows.push_back(row);
        const auto& line = logicalLines[row.logicalLine];
        uint32_t viewportColumn = 0;
        uint64_t logicalCell = row.startCell.value();
        const uint32_t endSpan = row.firstSpan + row.spanCount;
        for (uint32_t spanIndex = row.firstSpan; spanIndex < endSpan;
             ++spanIndex) {
            const auto& span = line.spans[spanIndex];
            const auto available = dimensions.columns - viewportColumn;
            const auto visibleWidth = std::min(span.cellWidth, available);
            for (uint32_t cell = 0; cell < visibleWidth; ++cell) {
                hitTargets.push_back(CellHitTarget{
                    viewportRow,
                    viewportColumn + cell,
                    row.logicalLine,
                    CellIndex{logicalCell},
                    lineDocumentStart[row.logicalLine] + span.byteOffset,
                    span.byteLen,
                });
            }
            viewportColumn += visibleWidth;
            logicalCell += span.cellWidth;
        }
    }

    return ViewportViewState{
        dimensions,
        firstRow,
        0,  // word-wrap-ON never scrolls horizontally (first_visual_column)
        totalRows,
        std::move(visibleRows),
        std::move(hitTargets),
        scrollbarMetrics(totalRows, dimensions.rows, firstRow),
    };
}

ViewportViewState Viewport::computeUnwrapped(
    std::string_view documentText, ViewportDimensions dimensions,
    uint32_t requestedFirstVisualRow, uint32_t requestedFirstVisualColumn, int tabWidth) const {
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
    std::vector<std::size_t> lineStart{0};
    for (std::size_t newline = documentText.find('\n');
         newline != std::string_view::npos;
         newline = documentText.find('\n', newline + 1)) {
        lineStart.push_back(newline + 1);
    }
    const auto totalRows =
        checkedU32(lineStart.size(), "viewport line count exceeds uint32");
    const uint32_t maximumFirst =
        totalRows > dimensions.rows ? totalRows - dimensions.rows : 0;
    const uint32_t firstRow =
        std::min(requestedFirstVisualRow, maximumFirst);
    const uint32_t visibleCount =
        std::min<uint32_t>(dimensions.rows, totalRows - firstRow);

    std::vector<VisualRow> visibleRows;
    std::vector<CellHitTarget> hitTargets;
    visibleRows.reserve(visibleCount);

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
    for (uint32_t viewportRow = 0; viewportRow < visibleCount; ++viewportRow) {
        const uint32_t logicalLine = firstRow + viewportRow;
        const std::size_t start = lineStart[logicalLine];
        const std::size_t end = logicalLine + 1 < lineStart.size()
                                    ? lineStart[logicalLine + 1] - 1
                                    : documentText.size();
        const auto run = GraphemeLayout{}.computeRun(
            documentText.substr(start, end - start), tabWidth);
        const auto documentStart =
            checkedU32(start, "viewport byte offset exceeds uint32");
        // The row's end is the line's TRUE end (newline byte, or text.size() for
        // the last line) — the FULL line, independent of the horizontal clip.
        const auto endByteOffset =
            checkedU32(end, "viewport byte offset exceeds uint32");

        // Locate the first span at or past the requested horizontal offset; its
        // start cell is this row's visible origin.  A row shorter than the offset
        // contributes no visible spans.
        uint32_t firstSpan = 0;
        uint32_t startCell = 0;
        for (; firstSpan < run.spans.size(); ++firstSpan) {
            if (startCell >= requestedFirstVisualColumn) break;
            startCell += run.spans[firstSpan].cellWidth;
        }

        if (firstSpan >= run.spans.size()) {
            // Empty line, or the whole line scrolled off to the left.
            visibleRows.push_back(
                VisualRow{logicalLine, firstSpan, 0, CellIndex{startCell},
                          run.totalCells, 0, endByteOffset});
            continue;
        }

        uint32_t viewportColumn = 0;
        uint32_t logicalCell = startCell;
        uint32_t spanCount = 0;
        for (uint32_t spanIndex = firstSpan; spanIndex < run.spans.size();
             ++spanIndex) {
            const auto& span = run.spans[spanIndex];
            const auto available = dimensions.columns - viewportColumn;
            if (available == 0) break;
            const auto visibleWidth = std::min(span.cellWidth, available);
            for (uint32_t cell = 0; cell < visibleWidth; ++cell) {
                hitTargets.push_back(CellHitTarget{
                    viewportRow,
                    viewportColumn + cell,
                    logicalLine,
                    CellIndex{logicalCell},
                    documentStart + span.byteOffset,
                    span.byteLen,
                });
            }
            viewportColumn += visibleWidth;
            logicalCell += span.cellWidth;
            ++spanCount;
        }
        const uint32_t contentFromOffset =
            run.totalCells > startCell ? run.totalCells - startCell : 0;
        visibleRows.push_back(
            VisualRow{logicalLine, firstSpan, spanCount, CellIndex{startCell},
                      run.totalCells,
                      std::min(contentFromOffset, dimensions.columns),
                      endByteOffset});
    }

    return ViewportViewState{
        dimensions,
        firstRow,
        requestedFirstVisualColumn,
        totalRows,
        std::move(visibleRows),
        std::move(hitTargets),
        scrollbarMetrics(totalRows, dimensions.rows, firstRow),
    };
}

ViewportViewState Viewport::scrollBy(
    std::span<const CellRun> logicalLines,
    ViewportDimensions dimensions,
    uint32_t currentFirstVisualRow,
    int64_t rowDelta) const {
    uint32_t requested = currentFirstVisualRow;
    if (rowDelta >= 0) {
        const auto delta = static_cast<uint64_t>(rowDelta);
        requested = delta > std::numeric_limits<uint32_t>::max() - requested
                        ? std::numeric_limits<uint32_t>::max()
                        : requested + static_cast<uint32_t>(delta);
    } else {
        const uint64_t magnitude =
            static_cast<uint64_t>(-(rowDelta + 1)) + 1;
        requested = magnitude > requested
                        ? 0
                        : requested - static_cast<uint32_t>(magnitude);
    }
    return compute(logicalLines, dimensions, requested);
}

ViewportDelta Viewport::deriveDelta(const ViewportViewState& previous,
                                    const ViewportViewState& current) const {
    if (previous == current) {
        return ViewportDelta{false, std::nullopt};
    }
    return ViewportDelta{true, current};
}

}  // namespace ssg
