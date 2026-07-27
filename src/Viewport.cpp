#include <ssg/Viewport.h>

#include <ssg/DiffModel.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace ssg {
namespace {

uint32_t checkedU32(std::size_t value, const char* what) {
    if (value > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error(what);
    }
    return static_cast<uint32_t>(value);
}

struct RemovedLine {
    uint32_t baselineLine;
    std::string text;
    std::vector<DiffWordRange> removedWordRanges;
};

using RemovedBlocks = std::map<uint32_t, std::vector<RemovedLine>>;

std::string lineText(std::string text) {
    if (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    return text;
}

// A Modified pair's baseline text is never shown as a real row (only its
// target/edited text is -- see DiffModel.cpp's flush()), so its removed
// words would otherwise be invisible; find the changedLines entry for this
// baseline line to recover which specific words were removed. Pure-Removed
// lines have no such entry and fall back to no marks (the whole line is
// already RemovedRow-tinted, nothing more specific to mark).
std::vector<DiffWordRange> removedWordRangesFor(const DiffFileView& diff,
                                                uint32_t baselineLine) {
    for (const auto& change : diff.changedLines) {
        if (change.kind == DiffLineKind::Modified && change.baselineLine &&
            *change.baselineLine == baselineLine) {
            return change.baselineRemovedWordRanges;
        }
    }
    return {};
}

RemovedBlocks removedBlocks(const DiffFileView& diff, uint32_t lineCount,
                             bool skipMergedPairs) {
    RemovedBlocks result;
    for (const auto& hunk : diff.hunks) {
        // A clean single-line 1:1 Modified pair (see DiffModel.cpp's
        // flush()/alignHunkLines) renders as ONE merged inline row
        // (RealRow::mergedSegments, populated below in
        // projectedUnwrappedRows) instead of a separate phantom row above
        // the real target row -- skip it here so it isn't shown twice.
        // Ghost spans are UNWRAPPED-ONLY (see RealRow::mergedSegments), so
        // the WRAPPED projection never skips: `skipMergedPairs` is false
        // there, and every hunk shape keeps the phantom-row-above split.
        if (skipMergedPairs && hunk.baselineLines.size() == 1 &&
            hunk.targetLines.size() == 1) {
            continue;
        }
        const auto insertion = checkedU32(
            std::min<std::size_t>(hunk.targetStart, lineCount),
            "phantom insertion row exceeds uint32");
        auto& block = result[insertion];
        for (std::size_t index = 0; index < hunk.baselineLines.size();
             ++index) {
            const auto baselineLine = checkedU32(
                hunk.baselineStart + index, "phantom baseline row exceeds uint32");
            block.push_back(RemovedLine{
                baselineLine,
                lineText(hunk.baselineLines[index]),
                removedWordRangesFor(diff, baselineLine),
            });
        }
    }
    return result;
}

// The inline merged-row segments for the Modified change targeting `line`,
// or empty if there is none (only a clean single-line 1:1 Modified pair --
// see DiffModel.cpp's flush() -- ever populates DiffLineChange::
// inlineWordSegments, so this and removedBlocks' skip condition above always
// agree on which lines merge).
std::vector<InlineWordSegment> mergedSegmentsFor(const DiffFileView& diff,
                                                  uint32_t line) {
    for (const auto& change : diff.changedLines) {
        if (change.kind == DiffLineKind::Modified && change.targetLine &&
            *change.targetLine == line && !change.inlineWordSegments.empty()) {
            return change.inlineWordSegments;
        }
    }
    return {};
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

std::vector<uint32_t> documentLineStarts(
    std::span<const CellRun> logicalLines) {
    std::vector<uint32_t> starts(logicalLines.size(), 0);
    uint64_t documentByte = 0;
    for (std::size_t line = 0; line < logicalLines.size(); ++line) {
        starts[line] =
            checkedU32(documentByte, "viewport byte offset exceeds uint32");
        uint64_t contentBytes = 0;
        for (const auto& span : logicalLines[line].spans) {
            contentBytes += span.byteLen;
        }
        documentByte += contentBytes + 1;
    }
    return starts;
}

std::vector<std::size_t> documentLineStarts(std::string_view text) {
    std::vector<std::size_t> starts{0};
    for (std::size_t newline = text.find('\n');
         newline != std::string_view::npos;
         newline = text.find('\n', newline + 1)) {
        starts.push_back(newline + 1);
    }
    return starts;
}

template <typename LineStart>
uint32_t followingOffset(uint32_t insertion,
                         std::span<const LineStart> lineStarts,
                         uint32_t documentSize) {
    return insertion < lineStarts.size()
               ? checkedU32(lineStarts[insertion],
                            "viewport byte offset exceeds uint32")
               : documentSize;
}

std::vector<std::pair<VisualRow, ProjectedRow>> projectedWrappedRows(
    std::span<const CellRun> logicalLines,
    uint32_t columns,
    const DiffFileView& diff) {
    auto realRows = wrapRows(logicalLines, columns);
    auto starts = documentLineStarts(logicalLines);
    uint64_t documentSize = 0;
    if (!logicalLines.empty()) {
        documentSize = starts.back();
        for (const auto& span : logicalLines.back().spans) {
            documentSize += span.byteLen;
        }
    }
    const auto checkedDocumentSize =
        checkedU32(documentSize, "viewport byte offset exceeds uint32");
    const auto lineCount = checkedU32(
        logicalLines.size(), "viewport logical line count exceeds uint32");
    auto blocks = removedBlocks(diff, lineCount, /*skipMergedPairs=*/false);
    std::vector<std::pair<VisualRow, ProjectedRow>> result;

    const auto appendBlock = [&](uint32_t insertion) {
        const auto found = blocks.find(insertion);
        if (found == blocks.end()) return;
        const auto offset =
            followingOffset(insertion, std::span<const uint32_t>{starts},
                            checkedDocumentSize);
        const auto logicalLine =
            lineCount == 0 ? 0 : std::min(insertion, lineCount - 1);
        for (const auto& removed : found->second) {
            const auto run = GraphemeLayout{}.computeRun(removed.text);
            const std::array<CellRun, 1> line{run};
            const auto wrapped = wrapRows(line, columns);
            for (const auto& segment : wrapped) {
                std::string text;
                std::size_t segmentStart = 0;
                std::size_t segmentEnd = 0;
                if (segment.spanCount != 0) {
                    const auto& first = run.spans[segment.firstSpan];
                    const auto& last =
                        run.spans[segment.firstSpan + segment.spanCount - 1];
                    segmentStart = first.byteOffset;
                    segmentEnd = last.byteOffset + last.byteLen;
                    text = removed.text.substr(segmentStart,
                                               segmentEnd - segmentStart);
                }
                std::vector<DiffWordRange> segmentRanges;
                for (const auto& range : removed.removedWordRanges) {
                    const auto rangeEnd = range.byteStart + range.byteLength;
                    const auto overlapStart = std::max(range.byteStart, segmentStart);
                    const auto overlapEnd = std::min(rangeEnd, segmentEnd);
                    if (overlapStart < overlapEnd) {
                        segmentRanges.push_back(
                            {overlapStart - segmentStart, overlapEnd - overlapStart});
                    }
                }
                result.emplace_back(
                    VisualRow{logicalLine, 0, 0, CellIndex{0},
                              segment.contentCells, segment.visibleCells,
                              offset},
                    PhantomRow{removed.baselineLine, std::move(text), offset,
                              std::move(segmentRanges)});
            }
        }
    };

    std::size_t realVisualRow = 0;
    for (uint32_t line = 0; line < lineCount; ++line) {
        appendBlock(line);
        while (realVisualRow < realRows.size() &&
               realRows[realVisualRow].logicalLine == line) {
            auto geometry = realRows[realVisualRow];
            geometry.endByteOffset += starts[line];
            result.emplace_back(
                geometry,
                RealRow{line, checkedU32(
                                  realVisualRow,
                                  "viewport visual row count exceeds uint32"),
                        geometry.spanCount == 0
                            ? starts[line]
                            : starts[line] +
                                  logicalLines[line]
                                      .spans[geometry.firstSpan]
                                      .byteOffset,
                        geometry.endByteOffset,
                        checkedU32(
                            geometry.startCell.value(),
                            "viewport cell offset exceeds uint32"),
                        checkedU32(
                            geometry.startCell.value() +
                                geometry.contentCells,
                            "viewport cell offset exceeds uint32")});
            ++realVisualRow;
        }
    }
    appendBlock(lineCount);
    return result;
}

RowProjection projectedUnwrappedRows(std::string_view documentText,
                                     const DiffFileView& diff) {
    const auto starts = documentLineStarts(documentText);
    const auto lineCount =
        checkedU32(starts.size(), "viewport line count exceeds uint32");
    const auto documentSize =
        checkedU32(documentText.size(), "viewport byte offset exceeds uint32");
    const auto blocks = removedBlocks(diff, lineCount, /*skipMergedPairs=*/true);
    std::vector<ProjectedRow> rows;
    for (uint32_t line = 0; line <= lineCount; ++line) {
        if (const auto found = blocks.find(line); found != blocks.end()) {
            const auto offset =
                followingOffset(line, std::span<const std::size_t>{starts},
                                documentSize);
            for (const auto& removed : found->second) {
                rows.emplace_back(PhantomRow{
                    removed.baselineLine, removed.text, offset,
                    removed.removedWordRanges});
            }
        }
        if (line < lineCount) {
            const auto start = checkedU32(
                starts[line], "viewport byte offset exceeds uint32");
            const auto end = checkedU32(
                line + 1 < lineCount ? starts[line + 1] - 1
                                     : documentText.size(),
                "viewport byte offset exceeds uint32");
            rows.emplace_back(
                RealRow{line, line, start, end, 0, 0, mergedSegmentsFor(diff, line)});
        }
    }
    return RowProjection{std::move(rows)};
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

RowProjection::RowProjection(std::vector<ProjectedRow> rows)
    : rows_(std::move(rows)) {
    if (rows_.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error("viewport row projection exceeds uint32");
    }
}

std::span<const ProjectedRow> RowProjection::rows() const noexcept {
    return rows_;
}

const ProjectedRow& RowProjection::row(uint32_t visualRow) const {
    return rows_.at(visualRow);
}

uint32_t RowProjection::totalRows() const noexcept {
    return static_cast<uint32_t>(rows_.size());
}

uint32_t RowProjection::visualRowForReal(uint32_t bufferVisualRow) const {
    for (uint32_t visual = 0; visual < rows_.size(); ++visual) {
        if (const auto* real = std::get_if<RealRow>(&rows_[visual]);
            real != nullptr && real->bufferVisualRow == bufferVisualRow) {
            return visual;
        }
    }
    return totalRows();
}

uint32_t RowProjection::visualRowForBufferLine(uint32_t bufferLine) const {
    for (uint32_t visual = 0; visual < rows_.size(); ++visual) {
        if (const auto* real = std::get_if<RealRow>(&rows_[visual]);
            real != nullptr && real->bufferLine == bufferLine) {
            return visual;
        }
    }
    return totalRows();
}

uint32_t RowProjection::visualRowForPosition(
    const DocumentPosition& position) const {
    const auto bufferLine = static_cast<uint32_t>(position.line.value());
    const auto byteOffset =
        static_cast<uint32_t>(position.byteOffset.value());
    uint32_t fallback = totalRows();
    for (uint32_t visual = 0; visual < rows_.size(); ++visual) {
        const auto* real = std::get_if<RealRow>(&rows_[visual]);
        if (real == nullptr || real->bufferLine != bufferLine) continue;
        fallback = visual;
        const auto next = std::find_if(
            rows_.begin() + visual + 1, rows_.end(),
            [](const ProjectedRow& row) {
                return std::holds_alternative<RealRow>(row);
            });
        const auto finalRow =
            next == rows_.end() ||
            std::get<RealRow>(*next).bufferLine != bufferLine;
        if (byteOffset >= real->startByteOffset &&
            (byteOffset < real->endByteOffset ||
             (finalRow && byteOffset == real->endByteOffset))) {
            return visual;
        }
    }
    return fallback;
}

uint32_t RowProjection::movedRealRow(uint32_t visualRow,
                                    int64_t visualDistance) const {
    if (rows_.empty()) return 0;
    const auto last = static_cast<int64_t>(rows_.size() - 1);
    auto target = std::clamp<int64_t>(
        static_cast<int64_t>(visualRow) + visualDistance, 0, last);
    const auto direction = visualDistance < 0 ? -1 : 1;
    while (target >= 0 && target <= last &&
           std::holds_alternative<PhantomRow>(
               rows_[static_cast<std::size_t>(target)])) {
        target += direction;
    }
    if (target < 0 || target > last) {
        target = std::clamp<int64_t>(
            static_cast<int64_t>(visualRow), 0, last);
        while (target >= 0 && target <= last &&
               std::holds_alternative<PhantomRow>(
                   rows_[static_cast<std::size_t>(target)])) {
            target -= direction;
        }
    }
    return static_cast<uint32_t>(std::clamp<int64_t>(target, 0, last));
}

ProjectedRow ViewportViewState::projectedRow(uint32_t viewportRow) const {
    if (!rowProjection.empty()) {
        return rowProjection.at(viewportRow);
    }
    const auto& row = visibleRows.at(viewportRow);
    return RealRow{row.logicalLine, firstVisualRow + viewportRow};
}

uint32_t ViewportViewState::editableOffset(uint32_t viewportRow) const {
    const auto projected = projectedRow(viewportRow);
    if (const auto* phantom = std::get_if<PhantomRow>(&projected)) {
        return phantom->followingByteOffset;
    }
    return visibleRows.at(viewportRow).endByteOffset;
}

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
// Saturating clamped shift, shared by byLines and byPages. `delta` arrives from
// the wire and may be any int64, so the extremes are handled before the add
// rather than relying on it not overflowing.
uint32_t shiftedOffset(uint32_t current, std::int64_t delta,
                       uint32_t maximumFirst) {
    const auto maximum = static_cast<std::int64_t>(maximumFirst);
    if (delta >= maximum) return maximumFirst;
    if (delta <= -maximum) return 0;
    const auto from =
        static_cast<std::int64_t>(std::min(current, maximumFirst));
    return static_cast<uint32_t>(
        std::clamp<std::int64_t>(from + delta, 0, maximum));
}

void ScrollOffset::byLines(std::int64_t delta, uint32_t totalItems,
                           uint32_t viewportRows) {
    // A surface with no rows shows nothing, so there is nowhere to scroll to.
    // Checked here rather than trusting the resolved metrics: with zero rows
    // those report a maximum of `totalItems`, which would let a shift land
    // outside the range every other operation guarantees.
    if (viewportRows == 0) {
        firstVisible_ = 0;
        return;
    }
    const auto view = resolve(totalItems, viewportRows);
    firstVisible_ =
        shiftedOffset(firstVisible_, delta, view.scrollbar.maximumFirstRow);
}

void ScrollOffset::byPages(std::int64_t pages, uint32_t totalItems,
                           uint32_t viewportRows) {
    // A page is the window height, so paging and line-scrolling cannot drift
    // apart into different units.
    const auto rows = static_cast<std::int64_t>(viewportRows);
    if (rows == 0) {
        firstVisible_ = 0;
        return;
    }
    // Saturate the multiply before it can overflow; byLines saturates the rest.
    constexpr auto kLimit = std::numeric_limits<std::int64_t>::max();
    const std::int64_t delta =
        pages > kLimit / rows    ? kLimit
        : pages < -kLimit / rows ? -kLimit
                                 : pages * rows;
    byLines(delta, totalItems, viewportRows);
}

void ScrollOffset::toFraction(uint32_t numerator, uint32_t denominator,
                              uint32_t totalItems, uint32_t viewportRows) {
    if (denominator == 0 || viewportRows == 0) {
        firstVisible_ = 0;
        return;
    }
    const auto maximum = resolve(totalItems, viewportRows).scrollbar.maximumFirstRow;
    const auto scaled =
        (static_cast<std::uint64_t>(maximum) * numerator) / denominator;
    firstVisible_ = static_cast<uint32_t>(std::min<std::uint64_t>(scaled, maximum));
}

void ScrollOffset::revealSelection(uint32_t selected, uint32_t totalItems,
                                   uint32_t viewportRows) {
    firstVisible_ = Viewport{}
                        .listScrollView(totalItems, viewportRows, firstVisible_,
                                        selected, true)
                        .firstVisible;
}

ListScrollView ScrollOffset::resolve(uint32_t totalItems,
                                     uint32_t viewportRows) const {
    return Viewport{}.listScrollView(totalItems, viewportRows, firstVisible_,
                                     std::nullopt, false);
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
                                    uint32_t requestedFirstVisualRow,
                                    const DiffFileView* diff) const {
    if (diff != nullptr) {
        auto projected =
            projectedWrappedRows(logicalLines, dimensions.columns, *diff);
        const auto totalRows = checkedU32(
            projected.size(), "viewport visual row count exceeds uint32");
        // The shared clamp, not a local one: the editor used to compute its own
        // maximumFirst here, which made it a second implementation of the rule
        // listScrollView exists to own (doc/spec-scroll.md S-I3).
        const auto scroll =
            ScrollOffset{requestedFirstVisualRow}.resolve(totalRows,
                                                          dimensions.rows);
        const auto firstRow = scroll.firstVisible;
        const auto visibleCount = scroll.visibleCount;
        std::vector<VisualRow> visibleRows;
        std::vector<ProjectedRow> rowProjection;
        std::vector<CellHitTarget> hitTargets;
        const auto lineStarts = documentLineStarts(logicalLines);
        visibleRows.reserve(visibleCount);
        rowProjection.reserve(visibleCount);
        for (uint32_t viewportRow = 0; viewportRow < visibleCount;
             ++viewportRow) {
            auto& [geometry, source] = projected[firstRow + viewportRow];
            visibleRows.push_back(geometry);
            rowProjection.push_back(source);
            const auto* real = std::get_if<RealRow>(&source);
            if (real == nullptr) continue;
            const auto& line = logicalLines[real->bufferLine];
            uint32_t viewportColumn = 0;
            uint64_t logicalCell = geometry.startCell.value();
            for (uint32_t spanIndex = geometry.firstSpan;
                 spanIndex < geometry.firstSpan + geometry.spanCount;
                 ++spanIndex) {
                const auto& span = line.spans[spanIndex];
                const auto available = dimensions.columns - viewportColumn;
                const auto visibleWidth = std::min(span.cellWidth, available);
                for (uint32_t cell = 0; cell < visibleWidth; ++cell) {
                    hitTargets.push_back(CellHitTarget{
                        viewportRow, viewportColumn + cell, real->bufferLine,
                        CellIndex{logicalCell},
                        lineStarts[real->bufferLine] + span.byteOffset,
                        span.byteLen});
                }
                viewportColumn += visibleWidth;
                logicalCell += span.cellWidth;
            }
        }
        return ViewportViewState{
            dimensions,
            firstRow,
            0,
            totalRows,
            std::move(visibleRows),
            std::move(rowProjection),
            std::move(hitTargets),
            scrollbarMetrics(totalRows, dimensions.rows, firstRow),
        };
    }
    const auto allRows = wrapRows(logicalLines, dimensions.columns);
    const auto totalRows =
        checkedU32(allRows.size(), "viewport visual row count exceeds uint32");
    const uint32_t firstRow =
        ScrollOffset{requestedFirstVisualRow}
            .resolve(totalRows, dimensions.rows)
            .firstVisible;

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
        {},
        std::move(hitTargets),
        scrollbarMetrics(totalRows, dimensions.rows, firstRow),
    };
}

ViewportViewState Viewport::computeUnwrapped(
    std::string_view documentText, ViewportDimensions dimensions,
    uint32_t requestedFirstVisualRow, uint32_t requestedFirstVisualColumn,
    int tabWidth, const DiffFileView* diff) const {
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
    const auto lineStart = documentLineStarts(documentText);
    const auto projection =
        diff == nullptr ? std::optional<RowProjection>{}
                        : std::optional<RowProjection>{
                              projectedUnwrappedRows(documentText, *diff)};
    const auto totalRows =
        projection ? projection->totalRows()
                   : checkedU32(lineStart.size(),
                                "viewport line count exceeds uint32");
    const auto scroll =
        ScrollOffset{requestedFirstVisualRow}.resolve(totalRows,
                                                      dimensions.rows);
    const uint32_t firstRow = scroll.firstVisible;
    const uint32_t visibleCount = scroll.visibleCount;

    std::vector<VisualRow> visibleRows;
    std::vector<ProjectedRow> rowProjection;
    std::vector<CellHitTarget> hitTargets;
    visibleRows.reserve(visibleCount);
    if (projection) rowProjection.reserve(visibleCount);

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
        const auto projected =
            projection ? projection->row(firstRow + viewportRow)
                       : ProjectedRow{RealRow{firstRow + viewportRow,
                                              firstRow + viewportRow}};
        if (const auto* phantom = std::get_if<PhantomRow>(&projected)) {
            const auto run =
                GraphemeLayout{}.computeRun(phantom->text, tabWidth);
            const auto following = std::lower_bound(
                lineStart.begin(), lineStart.end(),
                static_cast<std::size_t>(phantom->followingByteOffset));
            const auto logicalLine = checkedU32(
                std::min<std::size_t>(
                    std::distance(lineStart.begin(), following),
                    lineStart.size() - 1),
                "viewport logical line count exceeds uint32");
            visibleRows.push_back(VisualRow{
                logicalLine, 0, 0, CellIndex{0}, run.totalCells,
                std::min(run.totalCells, dimensions.columns),
                phantom->followingByteOffset});
            rowProjection.push_back(projected);
            continue;
        }
        const auto& real = std::get<RealRow>(projected);
        const uint32_t logicalLine = real.bufferLine;
        const std::size_t start = lineStart[logicalLine];
        const std::size_t end = logicalLine + 1 < lineStart.size()
                                    ? lineStart[logicalLine + 1] - 1
                                    : documentText.size();
        const auto documentStart =
            checkedU32(start, "viewport byte offset exceeds uint32");
        // The row's end is the line's TRUE end (newline byte, or text.size() for
        // the last line) — the FULL line, independent of the horizontal clip.
        const auto endByteOffset =
            checkedU32(end, "viewport byte offset exceeds uint32");

        if (!real.mergedSegments.empty()) {
            // A Modified line rendered as ONE merged inline row (git
            // --word-diff style). This is the SOLE place the merged text is
            // built and turned into cells/hit-targets -- Renderer paints
            // RealRow::mergedSegments verbatim and does not recompute this.
            // Unchanged/Added segments are REAL: concatenated in order they
            // reconstruct the target line's bytes exactly, so their byte
            // offsets accumulate from `documentStart` as the merged text is
            // assembled. Removed/Separator segments are GHOST: every cell
            // inside one resolves to the real byte offset immediately
            // before it with byteLen 0 -- the same "phantom content has no
            // bytes of its own" convention already used for whole
            // PhantomRows, generalized to span granularity.
            struct SegmentBound {
                uint32_t mergedStart;
                uint32_t mergedEnd;
                bool ghost;
                uint32_t byteOffsetBase;
            };
            std::string mergedText;
            std::vector<SegmentBound> bounds;
            bounds.reserve(real.mergedSegments.size());
            uint32_t realCursor = documentStart;
            for (const auto& segment : real.mergedSegments) {
                const auto mergedStart = checkedU32(
                    mergedText.size(), "viewport byte offset exceeds uint32");
                mergedText += segment.text;
                const auto ghost =
                    segment.kind == InlineWordSegment::Kind::Removed ||
                    segment.kind == InlineWordSegment::Kind::Separator;
                bounds.push_back(
                    {mergedStart,
                     checkedU32(mergedText.size(),
                                "viewport byte offset exceeds uint32"),
                     ghost, realCursor});
                if (!ghost) {
                    realCursor += checkedU32(
                        segment.text.size(), "viewport byte offset exceeds uint32");
                }
            }
            const auto boundFor = [&](uint32_t mergedByteOffset) {
                for (const auto& bound : bounds) {
                    if (mergedByteOffset >= bound.mergedStart &&
                        mergedByteOffset < bound.mergedEnd) {
                        return bound;
                    }
                }
                return bounds.back();
            };
            const auto run = GraphemeLayout{}.computeRun(mergedText, tabWidth);

            uint32_t firstSpan = 0;
            uint32_t startCell = 0;
            for (; firstSpan < run.spans.size(); ++firstSpan) {
                if (startCell >= requestedFirstVisualColumn) break;
                startCell += run.spans[firstSpan].cellWidth;
            }
            if (firstSpan >= run.spans.size()) {
                visibleRows.push_back(
                    VisualRow{logicalLine, firstSpan, 0, CellIndex{startCell},
                              run.totalCells, 0, endByteOffset});
                if (projection) rowProjection.push_back(projected);
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
                const auto bound = boundFor(span.byteOffset);
                const auto byteOffset =
                    bound.ghost ? bound.byteOffsetBase
                                : bound.byteOffsetBase +
                                      (span.byteOffset - bound.mergedStart);
                const auto byteLen = bound.ghost ? 0u : span.byteLen;
                for (uint32_t cell = 0; cell < visibleWidth; ++cell) {
                    hitTargets.push_back(CellHitTarget{
                        viewportRow,
                        viewportColumn + cell,
                        logicalLine,
                        CellIndex{logicalCell},
                        byteOffset,
                        byteLen,
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
            if (projection) rowProjection.push_back(projected);
            continue;
        }

        const auto run = GraphemeLayout{}.computeRun(
            documentText.substr(start, end - start), tabWidth);

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
            if (projection) rowProjection.push_back(projected);
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
        if (projection) rowProjection.push_back(projected);
    }

    return ViewportViewState{
        dimensions,
        firstRow,
        requestedFirstVisualColumn,
        totalRows,
        std::move(visibleRows),
        std::move(rowProjection),
        std::move(hitTargets),
        scrollbarMetrics(totalRows, dimensions.rows, firstRow),
    };
}

RowProjection Viewport::rowProjection(
    std::span<const CellRun> logicalLines,
    uint32_t columns,
    const DiffFileView& diff) const {
    auto projected = projectedWrappedRows(logicalLines, columns, diff);
    std::vector<ProjectedRow> rows;
    rows.reserve(projected.size());
    for (auto& [geometry, source] : projected) {
        (void)geometry;
        rows.push_back(std::move(source));
    }
    return RowProjection{std::move(rows)};
}

RowProjection Viewport::rowProjectionUnwrapped(
    std::string_view documentText,
    const DiffFileView& diff) const {
    return projectedUnwrappedRows(documentText, diff);
}

ViewportViewState Viewport::scrollBy(
    std::span<const CellRun> logicalLines,
    ViewportDimensions dimensions,
    uint32_t currentFirstVisualRow,
    int64_t rowDelta,
    const DiffFileView* diff) const {
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
    return compute(logicalLines, dimensions, requested, diff);
}

ViewportDelta Viewport::deriveDelta(const ViewportViewState& previous,
                                    const ViewportViewState& current) const {
    if (previous == current) {
        return ViewportDelta{false, std::nullopt};
    }
    return ViewportDelta{true, current};
}

}  // namespace ssg
