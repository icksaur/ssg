#include <ssg/Selection.h>
#include <ssg/WordClassification.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ssg {
namespace {

struct LogicalLine {
    std::size_t start;
    std::size_t end;
    std::vector<DocumentPosition> boundaries;
};

struct WrappedRow {
    std::size_t logicalLine;
    std::size_t startByte;
    std::size_t endByte;
    std::uint64_t startCell;
    std::uint64_t endCell;
};

class TextModel {
public:
    // A sentinel column count meaning "word wrap OFF": one logical line is exactly
    // one visual row, so the visual-geometry queries collapse to logical-line
    // arithmetic and never build whole-document wrapped rows (M12 VP-2b).
    static constexpr std::uint32_t kNoWrap =
        std::numeric_limits<std::uint32_t>::max();

    // Construction is CHEAP: a single '\n' byte scan records each logical line's
    // start offset.  No grapheme segmentation happens here — each line's CellRun
    // and boundary positions are computed on first use and cached (line_data),
    // so no-wrap navigation/reveal touches only the caret's and target lines
    // (O(visible/moved lines)), not the whole document (M12 INV-viewport-bounded-
    // work).  The wrap-ON path still faults in every line via wrapped_rows /
    // all_runs, which is the accepted O(document) cost for exact wrapped geometry.
    TextModel(std::string_view text, int tabWidth)
        : text_(text), tabWidth_(tabWidth) {
        lineStarts_.push_back(0);
        for (std::size_t newline = text.find('\n');
             newline != std::string_view::npos;
             newline = text.find('\n', newline + 1)) {
            lineStarts_.push_back(newline + 1);
        }
        cache_.resize(lineStarts_.size());
    }

    [[nodiscard]] std::optional<DocumentPosition> resolve(
        ByteOffset offset) const {
        const auto raw = static_cast<std::size_t>(offset.value());
        if (offset.value() > text_.size()) return std::nullopt;
        const auto& boundaries = lineData(lineContaining(raw)).line.boundaries;
        const auto found = std::lower_bound(
            boundaries.begin(), boundaries.end(), raw,
            [](const DocumentPosition& candidate, std::size_t value) {
                return candidate.byteOffset.value() < value;
            });
        if (found == boundaries.end() ||
            found->byteOffset.value() != offset.value()) {
            return std::nullopt;
        }
        return *found;
    }

    [[nodiscard]] const DocumentPosition& previous(
        const DocumentPosition& position) const {
        const auto line = static_cast<std::size_t>(position.line.value());
        const auto& boundaries = lineData(line).line.boundaries;
        const auto index = boundaryIndex(boundaries, position);
        if (index > 0) return boundaries[index - 1];
        if (line == 0) return boundaries.front();
        return lineData(line - 1).line.boundaries.back();
    }

    [[nodiscard]] const DocumentPosition& next(
        const DocumentPosition& position) const {
        const auto line = static_cast<std::size_t>(position.line.value());
        const auto& boundaries = lineData(line).line.boundaries;
        const auto index = boundaryIndex(boundaries, position);
        if (index + 1 < boundaries.size()) return boundaries[index + 1];
        if (line + 1 >= lineStarts_.size()) return boundaries.back();
        return lineData(line + 1).line.boundaries.front();
    }

    [[nodiscard]] const DocumentPosition& lineStart(
        const DocumentPosition& position) const {
        return lineData(static_cast<std::size_t>(position.line.value()))
            .line.boundaries.front();
    }

    [[nodiscard]] const DocumentPosition& lineEnd(
        const DocumentPosition& position) const {
        return lineData(static_cast<std::size_t>(position.line.value()))
            .line.boundaries.back();
    }

    [[nodiscard]] DocumentPosition vertical(
        const DocumentPosition& position, bool down, std::size_t lineCount,
        CellIndex desiredCell) const {
        const auto current = static_cast<std::size_t>(position.line.value());
        const auto lastLine = lineStarts_.size() - 1;
        std::size_t target = current;
        if (down) {
            target = lineCount > lastLine - current ? lastLine
                                                       : current + lineCount;
            if (target == current) {
                return lineData(current).line.boundaries.back();
            }
        } else {
            target = lineCount > current ? 0 : current - lineCount;
            if (target == current) {
                return lineData(current).line.boundaries.front();
            }
        }
        return positionForCell(target, desiredCell);
    }

    [[nodiscard]] DocumentPosition verticalVisual(
        const DocumentPosition& position, bool down, std::size_t rowCount,
        CellIndex desiredCell, std::uint32_t columns) const {
        // No wrap: a visual row IS a logical line, so vertical-visual movement is
        // exactly logical-line movement — no whole-document wrapped rows.
        if (columns == kNoWrap) {
            return vertical(position, down, rowCount, desiredCell);
        }
        const auto rows = wrappedRows(columns);
        const auto current = wrappedRowIndex(rows, position);
        const auto target =
            down ? std::min(current + rowCount, rows.size() - 1)
                 : (rowCount > current ? 0 : current - rowCount);
        if (target == current) {
            return *resolve(ByteOffset{
                down ? rows[current].endByte
                     : rows[current].startByte});
        }
        return positionForWrappedCell(rows[target], desiredCell);
    }

    [[nodiscard]] DocumentPosition verticalProjected(
        const DocumentPosition& position, bool down, std::size_t rowCount,
        CellIndex desiredCell, std::uint32_t columns,
        const RowProjection& projection) const {
        const auto visualRow = projection.visualRowForPosition(position);
        const auto distance = static_cast<int64_t>(std::min<std::size_t>(
            rowCount,
            static_cast<std::size_t>(std::numeric_limits<int64_t>::max())));
        const auto targetVisualRow =
            projection.movedRealRow(visualRow, down ? distance : -distance);
        const auto* target =
            std::get_if<RealRow>(&projection.row(targetVisualRow));
        if (target == nullptr || targetVisualRow == visualRow) {
            const auto& current =
                std::get<RealRow>(projection.row(visualRow));
            return *resolve(ByteOffset{
                down ? current.endByteOffset : current.startByteOffset});
        }
        if (columns == kNoWrap) {
            return positionForCell(target->bufferLine, desiredCell);
        }
        return positionForProjectedCell(*target, desiredCell);
    }

    [[nodiscard]] CellIndex projectedVisualColumn(
        const DocumentPosition& position,
        const RowProjection& projection) const {
        const auto& real = std::get<RealRow>(
            projection.row(projection.visualRowForPosition(position)));
        return CellIndex{position.cell.value() - real.startCell};
    }

    [[nodiscard]] CellIndex visualColumn(
        const DocumentPosition& position, std::uint32_t columns) const {
        if (columns == kNoWrap) return position.cell;
        const auto rows = wrappedRows(columns);
        const auto& row = rows[wrappedRowIndex(rows, position)];
        return CellIndex{position.cell.value() - row.startCell};
    }

    [[nodiscard]] std::size_t lineCount() const noexcept {
        return lineStarts_.size();
    }

    [[nodiscard]] const LogicalLine& line(std::size_t index) const {
        return lineData(index).line;
    }

    [[nodiscard]] const DocumentPosition& documentStart() const {
        return lineData(0).line.boundaries.front();
    }

    [[nodiscard]] const DocumentPosition& documentEnd() const {
        return lineData(lineStarts_.size() - 1).line.boundaries.back();
    }

    [[nodiscard]] DocumentPosition wordLeft(
        const DocumentPosition& position) const {
        // Walk one boundary left, then keep going while the run stays the same
        // segment category (words/whitespace).  Expressed with previous() so it
        // faults in only the lines it actually crosses (no global position array).
        const auto& start = documentStart();
        if (position == start) return start;
        DocumentPosition cursor = previous(position);
        const auto category = categoryAt(cursor);
        if (category == SegmentCategory::Word ||
            category == SegmentCategory::Space) {
            while (cursor != start) {
                const auto& candidate = previous(cursor);
                if (categoryAt(candidate) != category) break;
                cursor = candidate;
            }
        }
        return cursor;
    }

    [[nodiscard]] DocumentPosition wordRight(
        const DocumentPosition& position) const {
        const auto& end = documentEnd();
        if (position == end) return end;
        const auto category = categoryAt(position);
        DocumentPosition cursor = next(position);
        while (cursor != end && categoryAt(cursor) == category) {
            cursor = next(cursor);
        }
        return cursor;
    }

    // The maximal run of the SAME category as the character at `position`,
    // returned as [start, end).  Unlike wordLeft/wordRight (caret motions that
    // special-case categories), this treats Word/Space/Punctuation uniformly so a
    // double-click selects the whole word, punctuation run, or whitespace run it
    // lands in.  A click at documentEnd() (or an empty document) has no character
    // to classify, so it yields an empty range there.
    [[nodiscard]] std::pair<DocumentPosition, DocumentPosition> wordRangeAt(
        const DocumentPosition& position) const {
        const auto& start = documentStart();
        const auto& end = documentEnd();
        if (position == end) return {position, position};
        const auto category = categoryAt(position);
        DocumentPosition runStart = position;
        while (runStart != start) {
            const auto& candidate = previous(runStart);
            if (categoryAt(candidate) != category) break;
            runStart = candidate;
        }
        DocumentPosition runEnd = next(position);
        while (runEnd != end && categoryAt(runEnd) == category) {
            runEnd = next(runEnd);
        }
        return {runStart, runEnd};
    }

    [[nodiscard]] ViewportViewState viewportState(
        ViewportDimensions dimensions,
        std::uint32_t requestedFirstVisualRow,
        const DiffFileView* diff = nullptr) const {
        return Viewport{}.compute(allRuns(), dimensions,
                                  requestedFirstVisualRow, diff);
    }

    [[nodiscard]] RowProjection rowProjection(
        std::uint32_t columns, const DiffFileView& diff) const {
        if (columns == kNoWrap) {
            return Viewport{}.rowProjectionUnwrapped(text_, diff);
        }
        return Viewport{}.rowProjection(allRuns(), columns, diff);
    }

    [[nodiscard]] std::uint32_t visualRow(
        const DocumentPosition& position, std::uint32_t columns) const {
        if (columns == kNoWrap) {
            return static_cast<std::uint32_t>(position.line.value());
        }
        const auto rows = wrappedRows(columns);
        const auto row = wrappedRowIndex(rows, position);
        if (row > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error(
                "selection visual row count exceeds uint32");
        }
        return static_cast<std::uint32_t>(row);
    }

private:
    enum class SegmentCategory : std::uint8_t {
        Word,
        Space,
        Punctuation,
    };

    [[nodiscard]] SegmentCategory categoryAt(
        const DocumentPosition& position) const {
        const auto start =
            static_cast<std::size_t>(position.byteOffset.value());
        const auto first = static_cast<unsigned char>(text_[start]);
        if (isWordByte(first)) {
            return SegmentCategory::Word;
        }
        if (first == static_cast<unsigned char>(' ') ||
            first == static_cast<unsigned char>('\t') ||
            first == static_cast<unsigned char>('\n') ||
            first == static_cast<unsigned char>('\r')) {
            return SegmentCategory::Space;
        }
        return SegmentCategory::Punctuation;
    }

    [[nodiscard]] DocumentPosition positionForCell(
        std::size_t lineIndex, CellIndex desiredCell) const {
        const auto& boundaries = lineData(lineIndex).line.boundaries;
        const DocumentPosition* chosen = &boundaries.front();
        for (const auto& boundary : boundaries) {
            if (boundary.cell > desiredCell) {
                break;
            }
            if (boundary.cell > chosen->cell) {
                chosen = &boundary;
            }
        }
        return *chosen;
    }

    [[nodiscard]] DocumentPosition positionForProjectedCell(
        const RealRow& row, CellIndex desiredCell) const {
        const auto target =
            std::min<uint64_t>(row.startCell + desiredCell.value(),
                               row.endCell);
        const auto& boundaries = lineData(row.bufferLine).line.boundaries;
        const DocumentPosition* chosen = nullptr;
        for (const auto& boundary : boundaries) {
            const auto byteOffset = boundary.byteOffset.value();
            if (byteOffset < row.startByteOffset) continue;
            if (byteOffset > row.endByteOffset || boundary.cell.value() > target) {
                break;
            }
            chosen = &boundary;
        }
        return chosen != nullptr ? *chosen : boundaries.front();
    }

    [[nodiscard]] std::vector<WrappedRow> wrappedRows(
        std::uint32_t columns) const {
        std::vector<WrappedRow> rows;
        for (std::size_t lineIndex = 0; lineIndex < lineStarts_.size();
             ++lineIndex) {
            const auto& data = lineData(lineIndex);
            const auto& line = data.line;
            const auto& run = data.run;
            if (run.spans.empty()) {
                rows.push_back(WrappedRow{
                    lineIndex, line.start, line.end, 0, 0});
                continue;
            }

            std::size_t firstSpan = 0;
            std::size_t spanCount = 0;
            std::uint64_t startCell = 0;
            std::uint32_t contentCells = 0;
            const auto finishRow = [&] {
                const auto& first = run.spans[firstSpan];
                const auto& last =
                    run.spans[firstSpan + spanCount - 1];
                rows.push_back(WrappedRow{
                    lineIndex,
                    line.start + first.byteOffset,
                    line.start + last.byteOffset + last.byteLen,
                    startCell,
                    startCell + contentCells,
                });
            };

            for (std::size_t spanIndex = 0;
                 spanIndex < run.spans.size(); ++spanIndex) {
                const auto width = run.spans[spanIndex].cellWidth;
                if (width > 0 && contentCells > 0 &&
                    width >
                        columns - std::min(contentCells, columns)) {
                    finishRow();
                    firstSpan = spanIndex;
                    spanCount = 0;
                    startCell += contentCells;
                    contentCells = 0;
                }
                ++spanCount;
                contentCells += width;
            }
            finishRow();
        }
        return rows;
    }

    [[nodiscard]] std::size_t wrappedRowIndex(
        const std::vector<WrappedRow>& rows,
        const DocumentPosition& position) const {
        const auto byteOffset =
            static_cast<std::size_t>(position.byteOffset.value());
        const auto logicalLine =
            static_cast<std::size_t>(position.line.value());
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto& row = rows[index];
            if (row.logicalLine != logicalLine ||
                byteOffset < row.startByte) {
                continue;
            }
            const bool finalRow =
                index + 1 == rows.size() ||
                rows[index + 1].logicalLine != logicalLine;
            if (byteOffset < row.endByte ||
                (finalRow && byteOffset == row.endByte)) {
                return index;
            }
        }
        throw std::logic_error(
            "valid document position has no wrapped visual row");
    }

    [[nodiscard]] DocumentPosition positionForWrappedCell(
        const WrappedRow& row, CellIndex desiredCell) const {
        const auto target =
            std::min(row.startCell + desiredCell.value(),
                     row.endCell);
        const auto& boundaries = lineData(row.logicalLine).line.boundaries;
        const DocumentPosition* chosen = nullptr;
        for (const auto& boundary : boundaries) {
            const auto byteOffset = boundary.byteOffset.value();
            if (byteOffset < row.startByte) {
                continue;
            }
            if (byteOffset > row.endByte ||
                boundary.cell.value() > target) {
                break;
            }
            if (chosen == nullptr ||
                boundary.cell > chosen->cell) {
                chosen = &boundary;
            }
        }
        return chosen != nullptr
                   ? *chosen
                   : *resolve(ByteOffset{row.startByte});
    }

    // One logical line's segmentation, computed on first use and cached with
    // STABLE addresses (unique_ptr) so references returned by previous()/next()/
    // line_start()/etc. stay valid as later lines are faulted in.
    struct LineData {
        LogicalLine line;
        CellRun run;
    };

    [[nodiscard]] const LineData& lineData(std::size_t index) const {
        auto& slot = cache_[index];
        if (slot) return *slot;
        const std::size_t start = lineStarts_[index];
        const std::size_t end = index + 1 < lineStarts_.size()
                                    ? lineStarts_[index + 1] - 1
                                    : text_.size();
        auto run =
            GraphemeLayout{}.computeRun(text_.substr(start, end - start), tabWidth_);
        std::vector<DocumentPosition> boundaries;
        boundaries.reserve(run.spans.size() + 1);
        std::uint64_t cell = 0;
        boundaries.push_back(
            DocumentPosition{ByteOffset{start}, LineIndex{index}, CellIndex{0}});
        for (const auto& span : run.spans) {
            cell += span.cellWidth;
            boundaries.push_back(DocumentPosition{
                ByteOffset{start + span.byteOffset + span.byteLen},
                LineIndex{index}, CellIndex{cell}});
        }
        slot = std::make_unique<LineData>(
            LineData{LogicalLine{start, end, std::move(boundaries)},
                     std::move(run)});
        return *slot;
    }

    // The logical line index containing byte offset `raw` (the last line whose
    // start is <= raw), via the cheap line-start index — no segmentation.
    [[nodiscard]] std::size_t lineContaining(std::size_t raw) const {
        const auto it =
            std::upper_bound(lineStarts_.begin(), lineStarts_.end(), raw);
        return static_cast<std::size_t>(
            std::distance(lineStarts_.begin(), it) - 1);
    }

    [[nodiscard]] std::size_t boundaryIndex(
        const std::vector<DocumentPosition>& boundaries,
        const DocumentPosition& position) const {
        const auto found = std::lower_bound(
            boundaries.begin(), boundaries.end(), position.byteOffset,
            [](const DocumentPosition& candidate, ByteOffset value) {
                return candidate.byteOffset < value;
            });
        return static_cast<std::size_t>(
            std::distance(boundaries.begin(), found));
    }

    // Every line's CellRun (faults in all lines) — the wrap-ON viewport path only.
    [[nodiscard]] std::vector<CellRun> allRuns() const {
        std::vector<CellRun> runs;
        runs.reserve(lineStarts_.size());
        for (std::size_t index = 0; index < lineStarts_.size(); ++index) {
            runs.push_back(lineData(index).run);
        }
        return runs;
    }

    std::string_view text_;
    int tabWidth_;
    std::vector<std::size_t> lineStarts_;
    mutable std::vector<std::unique_ptr<LineData>> cache_;
};

SelectionNavigationResult rejected(SelectionNavigationError error,
                                   std::string message) {
    return SelectionNavigationResult{
        error, SelectionViewDelta{false, std::nullopt}, std::move(message)};
}

SelectionNavigationResult accepted(const SelectionViewState& before,
                                   SelectionViewState after) {
    if (after == before) {
        return SelectionNavigationResult{
            SelectionNavigationError::None,
            SelectionViewDelta{false, std::nullopt},
            {}};
    }
    return SelectionNavigationResult{
        SelectionNavigationError::None,
        SelectionViewDelta{true, std::move(after)},
        {}};
}

bool isValidPosition(const TextModel& model,
                       const DocumentPosition& position) {
    const auto resolved = model.resolve(position.byteOffset);
    return resolved && *resolved == position;
}

std::uint32_t revealedFirstRow(const TextModel& model,
                                 const SelectionViewState& state,
                                 ViewportDimensions dimensions,
                                 bool center, bool wordWrap,
                                 const ViewportViewState& viewport,
                                 const RowProjection* projection) {
    const auto columns =
        wordWrap ? dimensions.columns
                 : std::numeric_limits<std::uint32_t>::max();
    const auto target =
        projection == nullptr
            ? model.visualRow(state.selections.primary().active, columns)
            : projection->visualRowForPosition(
                  state.selections.primary().active);
    const auto maximum = viewport.scrollbar.maximumFirstRow;
    if (center) {
        const auto half = dimensions.rows / 2;
        const auto requested = target > half ? target - half : 0;
        return std::min(requested, maximum);
    }
    if (target < viewport.firstVisualRow) {
        return target;
    }
    const auto visibleEnd =
        static_cast<std::uint64_t>(viewport.firstVisualRow) +
        viewport.visibleRows.size();
    if (target >= visibleEnd) {
        const auto requested =
            target - static_cast<std::uint32_t>(viewport.visibleRows.size()) +
            1;
        return std::min(requested, maximum);
    }
    return viewport.firstVisualRow;
}

// The horizontal scroll offset (word wrap OFF only) that keeps the primary
// caret's cell column within the pane, scrolling minimally.  The caret's visual
// column under no-wrap is its per-line cell index (the row starts at cell 0), so
// this needs no document scan.  Returns 0 when word wrap is on.
std::uint32_t revealedFirstColumn(const SelectionViewState& state,
                                    ViewportDimensions dimensions,
                                    bool wordWrap) {
    if (wordWrap) return 0;
    const auto caretCell = static_cast<std::uint32_t>(
        state.selections.primary().active.cell.value());
    std::uint32_t first = state.firstVisualColumn;
    if (caretCell < first) {
        first = caretCell;
    } else if (caretCell >= first + dimensions.columns) {
        first = caretCell - dimensions.columns + 1;
    }
    return first;
}

bool validBracketPairs(std::span<const BracketPair> pairs) {
    std::vector<std::string_view> tokens;
    tokens.reserve(pairs.size() * 2);
    for (const auto& pair : pairs) {
        if (pair.opening.empty() || pair.closing.empty() ||
            pair.opening == pair.closing) {
            return false;
        }
        tokens.push_back(pair.opening);
        tokens.push_back(pair.closing);
    }
    for (std::size_t left = 0; left < tokens.size(); ++left) {
        for (std::size_t right = left + 1; right < tokens.size(); ++right) {
            if (tokens[left].starts_with(tokens[right]) ||
                tokens[right].starts_with(tokens[left])) {
                return false;
            }
        }
    }
    return true;
}

struct BracketMatch {
    std::size_t mateStart;
    std::size_t mateEnd;
    bool forward;
};

std::unordered_map<std::size_t, BracketMatch> bracketMatches(
    std::string_view text, const TextModel& model,
    std::span<const BracketPair> pairs) {
    struct StackEntry {
        std::size_t pair;
        std::size_t start;
        std::size_t end;
    };
    std::vector<StackEntry> stack;
    std::unordered_map<std::size_t, BracketMatch> matches;

    std::size_t offset = 0;
    while (offset < text.size()) {
        std::optional<std::pair<std::size_t, bool>> token;
        for (std::size_t pair = 0; pair < pairs.size(); ++pair) {
            if (text.substr(offset).starts_with(pairs[pair].opening)) {
                token = std::pair{pair, true};
                break;
            }
            if (text.substr(offset).starts_with(pairs[pair].closing)) {
                token = std::pair{pair, false};
                break;
            }
        }

        if (token) {
            const auto& value =
                token->second ? pairs[token->first].opening
                              : pairs[token->first].closing;
            const auto end = offset + value.size();
            if (model.resolve(ByteOffset{end})) {
                if (token->second) {
                    stack.push_back(
                        StackEntry{token->first, offset, end});
                } else if (!stack.empty() &&
                           stack.back().pair == token->first) {
                    const auto opening = stack.back();
                    stack.pop_back();
                    matches.emplace(
                        opening.start,
                        BracketMatch{offset, end, true});
                    matches.emplace(
                        offset,
                        BracketMatch{opening.start, opening.end, false});
                }
                offset = end;
                continue;
            }
        }

        const auto current = model.resolve(ByteOffset{offset});
        if (!current) {
            ++offset;
            continue;
        }
        const auto next = model.next(*current).byteOffset.value();
        if (next <= offset) {
            break;
        }
        offset = static_cast<std::size_t>(next);
    }
    return matches;
}

} // namespace

const DocumentPosition& Selection::lower() const noexcept {
    return anchor.byteOffset <= active.byteOffset ? anchor : active;
}

const DocumentPosition& Selection::upper() const noexcept {
    return anchor.byteOffset <= active.byteOffset ? active : anchor;
}

bool Selection::isCaret() const noexcept {
    return anchor.byteOffset == active.byteOffset;
}

std::string Selection::wordOrCoveredText(std::string_view text) const {
    if (!isCaret()) {
        auto const low = static_cast<std::size_t>(lower().byteOffset.value());
        auto const high = static_cast<std::size_t>(upper().byteOffset.value());
        if (low <= high && high <= text.size()) {
            return std::string{text.substr(low, high - low)};
        }
        return {};
    }
    auto const offset = static_cast<std::size_t>(active.byteOffset.value());
    const bool onWord =
        offset < text.size() && isWordByte(static_cast<unsigned char>(text[offset]));
    const bool afterWord =
        offset > 0 && offset <= text.size() &&
        isWordByte(static_cast<unsigned char>(text[offset - 1]));
    if (!onWord && !afterWord) return {};
    std::size_t begin = offset;
    std::size_t end = offset;
    while (begin > 0 && isWordByte(static_cast<unsigned char>(text[begin - 1]))) --begin;
    while (end < text.size() && isWordByte(static_cast<unsigned char>(text[end]))) ++end;
    return std::string{text.substr(begin, end - begin)};
}

SelectionSet::SelectionSet(std::vector<Selection> selections)
    : selections_(std::move(selections)) {
    if (selections_.empty()) {
        throw std::invalid_argument(
            "selection set must contain at least one selection");
    }
    std::sort(selections_.begin(), selections_.end(),
              [](const Selection& left, const Selection& right) {
                  if (left.lower().byteOffset != right.lower().byteOffset) {
                      return left.lower().byteOffset <
                             right.lower().byteOffset;
                  }
                  return left.upper().byteOffset <
                         right.upper().byteOffset;
              });

    std::vector<Selection> normalized;
    normalized.reserve(selections_.size());
    normalized.push_back(selections_.front());
    for (std::size_t index = 1; index < selections_.size(); ++index) {
        auto& previous = normalized.back();
        const auto& current = selections_[index];
        if (current == previous) {
            continue;
        }
        if (current.lower().byteOffset < previous.upper().byteOffset) {
            const auto& upper =
                current.upper().byteOffset > previous.upper().byteOffset
                    ? current.upper()
                    : previous.upper();
            previous = Selection{previous.lower(), upper};
            continue;
        }
        normalized.push_back(current);
    }
    selections_ = std::move(normalized);
}

const std::vector<Selection>& SelectionSet::items() const noexcept {
    return selections_;
}

const Selection& SelectionSet::primary() const noexcept {
    return selections_.back();
}

const std::array<SelectionCommandDescriptor, 38> kSelectionCommands{{
          {"cursor.set_position", SelectionCommand::CursorSetPosition},
          {"cursor.left", SelectionCommand::CursorLeft},
          {"cursor.right", SelectionCommand::CursorRight},
          {"cursor.word_left", SelectionCommand::CursorWordLeft},
          {"cursor.word_right", SelectionCommand::CursorWordRight},
          {"cursor.line_up", SelectionCommand::CursorLineUp},
          {"cursor.line_down", SelectionCommand::CursorLineDown},
          {"cursor.line_start", SelectionCommand::CursorLineStart},
          {"cursor.line_end", SelectionCommand::CursorLineEnd},
          {"cursor.page_up", SelectionCommand::CursorPageUp},
          {"cursor.page_down", SelectionCommand::CursorPageDown},
          {"cursor.document_start", SelectionCommand::CursorDocumentStart},
          {"cursor.document_end", SelectionCommand::CursorDocumentEnd},
          {"select.set_range", SelectionCommand::SelectSetRange},
          {"select.set_ranges", SelectionCommand::SelectSetRanges},
          {"select.add_range", SelectionCommand::SelectAddRange},
          {"select.left", SelectionCommand::SelectLeft},
          {"select.right", SelectionCommand::SelectRight},
          {"select.word_left", SelectionCommand::SelectWordLeft},
          {"select.word_right", SelectionCommand::SelectWordRight},
          {"select.line_up", SelectionCommand::SelectLineUp},
          {"select.line_down", SelectionCommand::SelectLineDown},
          {"select.line_start", SelectionCommand::SelectLineStart},
          {"select.line_end", SelectionCommand::SelectLineEnd},
          {"select.page_up", SelectionCommand::SelectPageUp},
          {"select.page_down", SelectionCommand::SelectPageDown},
          {"select.document_start", SelectionCommand::SelectDocumentStart},
          {"select.document_end", SelectionCommand::SelectDocumentEnd},
          {"select.all", SelectionCommand::SelectAll},
          {"select.add_next_occurrence",
           SelectionCommand::SelectAddNextOccurrence},
          {"select.add_cursor_up", SelectionCommand::SelectAddCursorUp},
          {"select.add_cursor_down", SelectionCommand::SelectAddCursorDown},
          {"select.split_into_lines",
           SelectionCommand::SelectSplitIntoLines},
          {"select.to_matching_bracket",
           SelectionCommand::SelectToMatchingBracket},
          {"goto.matching_bracket",
           SelectionCommand::GotoMatchingBracket},
          {"select.word_at_position",
           SelectionCommand::SelectWordAtPosition},
          {"view.reveal_caret", SelectionCommand::ViewRevealCaret},
    {"view.center_caret", SelectionCommand::ViewCenterCaret},
}};

std::optional<DocumentPosition> SelectionNavigator::resolvePosition(
    std::string_view text, ByteOffset byteOffset, int tabWidth) {
    if (tabWidth < 1 || tabWidth > 16) {
        return std::nullopt;
    }
    return TextModel{text, tabWidth}.resolve(byteOffset);
}

SelectionNavigationResult SelectionNavigator::apply(
    std::string_view text, const SelectionViewState& before,
    SelectionCommand command, ViewportDimensions viewport,
    SelectionCommandArguments arguments,
    std::span<const BracketPair> bracketPairs, int tabWidth,
    bool wordWrap, const DiffFileView* diff) const {
    if (tabWidth < 1 || tabWidth > 16) {
        return rejected(SelectionNavigationError::InvalidTabWidth,
                        "tab width must be between 1 and 16");
    }
    const TextModel model{text, tabWidth};
    const std::uint32_t navColumns =
        wordWrap ? viewport.columns : std::numeric_limits<std::uint32_t>::max();
    for (const auto& selection : before.selections.items()) {
        if (!isValidPosition(model, selection.anchor) ||
            !isValidPosition(model, selection.active)) {
            return rejected(
                SelectionNavigationError::InvalidPosition,
                "selection endpoint does not match the document layout");
        }
    }

    auto selections = before.selections.items();
    auto desiredCell = before.desiredCell;
    // The current viewport supplies the page size (visible row count) and the base
    // scroll row.  Under no-wrap, use the O(visible rows) unwrapped projection so
    // this does NOT segment the whole document (M12 VP-2b); the wrap-ON path keeps
    // the exact wrapped viewport.
    const auto currentViewport =
        wordWrap ? model.viewportState(viewport, before.firstVisualRow, diff)
                  : Viewport{}.computeUnwrapped(text, viewport,
                                               before.firstVisualRow, 0,
                                               tabWidth, diff);
    auto rowProjection =
        diff == nullptr
            ? std::optional<RowProjection>{}
            : std::optional<RowProjection>{
                  model.rowProjection(navColumns, *diff)};
    std::uint32_t firstVisualRow =
        currentViewport.firstVisualRow;

    const auto replaceWithCarets = [&](auto&& destination) {
        for (auto& selection : selections) {
            const auto position = destination(selection);
            selection = Selection{position, position};
        }
    };
    const auto extendActive = [&](auto&& destination) {
        for (auto& selection : selections) {
            selection.active = destination(selection);
        }
    };

    switch (command) {
    case SelectionCommand::CursorSetPosition:
        if (!arguments.position) {
            return rejected(SelectionNavigationError::MissingArgument,
                            "cursor.set_position requires a position");
        }
        if (!isValidPosition(model, *arguments.position)) {
            return rejected(SelectionNavigationError::InvalidPosition,
                            "cursor position does not match the document layout");
        }
        selections = {
            Selection{*arguments.position, *arguments.position}};
        desiredCell.reset();
        break;

    case SelectionCommand::CursorLeft:
        replaceWithCarets([&](const Selection& selection) {
            return selection.isCaret() ? model.previous(selection.active)
                                        : selection.lower();
        });
        desiredCell.reset();
        break;

    case SelectionCommand::CursorRight:
        replaceWithCarets([&](const Selection& selection) {
            return selection.isCaret() ? model.next(selection.active)
                                        : selection.upper();
        });
        desiredCell.reset();
        break;

    case SelectionCommand::CursorWordLeft:
        replaceWithCarets([&](const Selection& selection) {
            return model.wordLeft(selection.lower());
        });
        desiredCell.reset();
        break;

    case SelectionCommand::CursorWordRight:
        replaceWithCarets([&](const Selection& selection) {
            return model.wordRight(selection.upper());
        });
        desiredCell.reset();
        break;

    case SelectionCommand::CursorLineStart:
        replaceWithCarets([&](const Selection& selection) {
            return model.lineStart(selection.lower());
        });
        desiredCell.reset();
        break;

    case SelectionCommand::CursorLineEnd:
        replaceWithCarets([&](const Selection& selection) {
            return model.lineEnd(selection.upper());
        });
        desiredCell.reset();
        break;

    case SelectionCommand::CursorLineUp:
    case SelectionCommand::CursorLineDown:
    case SelectionCommand::CursorPageUp:
    case SelectionCommand::CursorPageDown: {
        const bool down =
            command == SelectionCommand::CursorLineDown ||
            command == SelectionCommand::CursorPageDown;
        const bool page =
            command == SelectionCommand::CursorPageUp ||
            command == SelectionCommand::CursorPageDown;
        const auto count =
            page ? currentViewport.visibleRows.size() : 1;
        std::optional<CellIndex> primaryDesired;
        for (std::size_t index = 0; index < selections.size(); ++index) {
            auto& selection = selections[index];
            const auto origin = down ? selection.upper()
                                     : selection.lower();
            const auto desired =
                index + 1 == selections.size() && before.desiredCell
                    ? *before.desiredCell
                    : rowProjection
                          ? model.projectedVisualColumn(origin, *rowProjection)
                          : model.visualColumn(origin, navColumns);
            const auto destination = rowProjection
                                         ? model.verticalProjected(
                                               origin, down, count, desired,
                                               navColumns, *rowProjection)
                                         : model.verticalVisual(
                                               origin, down, count, desired,
                                               navColumns);
            selection = Selection{destination, destination};
            if (index + 1 == selections.size()) {
                primaryDesired =
                    (rowProjection
                         ? rowProjection->visualRowForPosition(destination) ==
                               rowProjection->visualRowForPosition(origin)
                         : model.visualRow(destination, navColumns) ==
                               model.visualRow(origin, navColumns))
                        ? (rowProjection
                               ? model.projectedVisualColumn(destination,
                                                            *rowProjection)
                               : model.visualColumn(destination, navColumns))
                        : desired;
            }
        }
        desiredCell = primaryDesired;
        break;
    }

    case SelectionCommand::CursorDocumentStart: {
        const auto destination = model.documentStart();
        selections = {Selection{destination, destination}};
        desiredCell.reset();
        break;
    }

    case SelectionCommand::CursorDocumentEnd: {
        const auto destination = model.documentEnd();
        selections = {Selection{destination, destination}};
        desiredCell.reset();
        break;
    }

    case SelectionCommand::SelectSetRange:
    case SelectionCommand::SelectAddRange:
        if (!arguments.selection) {
            return rejected(
                SelectionNavigationError::MissingArgument,
                command == SelectionCommand::SelectSetRange
                    ? "select.set_range requires a selection"
                    : "select.add_range requires a selection");
        }
        if (!isValidPosition(model, arguments.selection->anchor) ||
            !isValidPosition(model, arguments.selection->active)) {
            return rejected(
                SelectionNavigationError::InvalidPosition,
                "selection range does not match the document layout");
        }
        if (command == SelectionCommand::SelectSetRange) {
            selections = {*arguments.selection};
        } else {
            selections.push_back(*arguments.selection);
        }
        desiredCell.reset();
        break;

    case SelectionCommand::SelectSetRanges:
        if (arguments.selections.empty()) {
            return rejected(
                SelectionNavigationError::MissingArgument,
                "select.set_ranges requires at least one selection");
        }
        for (auto const& candidate : arguments.selections) {
            if (!isValidPosition(model, candidate.anchor) ||
                !isValidPosition(model, candidate.active)) {
                return rejected(
                    SelectionNavigationError::InvalidPosition,
                    "selection range does not match the document layout");
            }
        }
        selections = arguments.selections;
        desiredCell.reset();
        break;

    case SelectionCommand::SelectWordAtPosition:
        if (!arguments.position) {
            return rejected(SelectionNavigationError::MissingArgument,
                            "select.word_at_position requires a position");
        }
        if (!isValidPosition(model, *arguments.position)) {
            return rejected(
                SelectionNavigationError::InvalidPosition,
                "word position does not match the document layout");
        }
        {
            auto const range = model.wordRangeAt(*arguments.position);
            selections = {Selection{range.first, range.second}};
        }
        desiredCell.reset();
        break;

    case SelectionCommand::SelectLeft:
        extendActive([&](const Selection& selection) {
            return model.previous(selection.active);
        });
        desiredCell.reset();
        break;

    case SelectionCommand::SelectRight:
        extendActive([&](const Selection& selection) {
            return model.next(selection.active);
        });
        desiredCell.reset();
        break;

    case SelectionCommand::SelectWordLeft:
        extendActive([&](const Selection& selection) {
            return model.wordLeft(selection.active);
        });
        desiredCell.reset();
        break;

    case SelectionCommand::SelectWordRight:
        extendActive([&](const Selection& selection) {
            return model.wordRight(selection.active);
        });
        desiredCell.reset();
        break;

    case SelectionCommand::SelectLineStart:
        extendActive([&](const Selection& selection) {
            return model.lineStart(selection.active);
        });
        desiredCell.reset();
        break;

    case SelectionCommand::SelectLineEnd:
        extendActive([&](const Selection& selection) {
            return model.lineEnd(selection.active);
        });
        desiredCell.reset();
        break;

    case SelectionCommand::SelectLineUp:
    case SelectionCommand::SelectLineDown:
    case SelectionCommand::SelectPageUp:
    case SelectionCommand::SelectPageDown: {
        const bool down =
            command == SelectionCommand::SelectLineDown ||
            command == SelectionCommand::SelectPageDown;
        const bool page =
            command == SelectionCommand::SelectPageUp ||
            command == SelectionCommand::SelectPageDown;
        const auto count =
            page ? currentViewport.visibleRows.size() : 1;
        std::optional<CellIndex> primaryDesired;
        for (std::size_t index = 0; index < selections.size(); ++index) {
            auto& selection = selections[index];
            const auto origin = selection.active;
            const auto desired =
                index + 1 == selections.size() && before.desiredCell
                    ? *before.desiredCell
                    : rowProjection
                          ? model.projectedVisualColumn(origin, *rowProjection)
                          : model.visualColumn(origin, navColumns);
            selection.active =
                rowProjection
                    ? model.verticalProjected(origin, down, count, desired,
                                              navColumns, *rowProjection)
                    : model.verticalVisual(origin, down, count, desired,
                                           navColumns);
            if (index + 1 == selections.size()) {
                primaryDesired =
                    (rowProjection
                         ? rowProjection->visualRowForPosition(
                               selection.active) ==
                               rowProjection->visualRowForPosition(origin)
                         : model.visualRow(selection.active, navColumns) ==
                               model.visualRow(origin, navColumns))
                        ? (rowProjection
                               ? model.projectedVisualColumn(selection.active,
                                                            *rowProjection)
                               : model.visualColumn(selection.active,
                                                    navColumns))
                        : desired;
            }
        }
        desiredCell = primaryDesired;
        break;
    }

    case SelectionCommand::SelectDocumentStart:
        extendActive([&](const Selection&) {
            return model.documentStart();
        });
        desiredCell.reset();
        break;

    case SelectionCommand::SelectDocumentEnd:
        extendActive([&](const Selection&) {
            return model.documentEnd();
        });
        desiredCell.reset();
        break;

    case SelectionCommand::SelectAll:
        selections = {Selection{model.documentStart(),
                                model.documentEnd()}};
        desiredCell.reset();
        break;

    case SelectionCommand::SelectAddNextOccurrence: {
        const auto& source = selections.back();
        if (!source.isCaret()) {
            const auto low = static_cast<std::size_t>(
                source.lower().byteOffset.value());
            const auto high = static_cast<std::size_t>(
                source.upper().byteOffset.value());
            const auto needle = text.substr(low, high - low);
            const auto findValid =
                [&](std::size_t start,
                    std::size_t limit) -> std::optional<Selection> {
                auto found = text.find(needle, start);
                while (found != std::string_view::npos &&
                       found < limit) {
                    const auto end = found + needle.size();
                    if (end <= limit) {
                        const auto anchor =
                            model.resolve(ByteOffset{found});
                        const auto active =
                            model.resolve(ByteOffset{end});
                        if (anchor && active) {
                            return Selection{*anchor, *active};
                        }
                    }
                    found = text.find(needle, found + 1);
                }
                return std::nullopt;
            };
            auto occurrence = findValid(high, text.size());
            if (!occurrence) {
                occurrence = findValid(0, low);
            }
            if (occurrence) {
                selections.push_back(*occurrence);
            }
        }
        desiredCell.reset();
        break;
    }

    case SelectionCommand::SelectAddCursorUp:
    case SelectionCommand::SelectAddCursorDown: {
        const bool down =
            command == SelectionCommand::SelectAddCursorDown;
        std::vector<Selection> added;
        for (const auto& selection : selections) {
            const auto line =
                static_cast<std::size_t>(selection.active.line.value());
            if ((!down && line == 0) ||
                (down && line + 1 == model.lineCount())) {
                continue;
            }
            const auto destination = model.vertical(
                selection.active, down, 1, selection.active.cell);
            added.push_back(Selection{destination, destination});
        }
        selections.insert(selections.end(), added.begin(), added.end());
        desiredCell.reset();
        break;
    }

    case SelectionCommand::SelectSplitIntoLines: {
        std::vector<Selection> split;
        for (const auto& selection : selections) {
            if (selection.isCaret()) {
                split.push_back(selection);
                continue;
            }
            const auto low = static_cast<std::size_t>(
                selection.lower().byteOffset.value());
            const auto high = static_cast<std::size_t>(
                selection.upper().byteOffset.value());
            for (std::size_t lineIndex = 0;
                 lineIndex < model.lineCount(); ++lineIndex) {
                const auto& line = model.line(lineIndex);
                const auto segmentStart = std::max(low, line.start);
                const auto segmentEnd = std::min(high, line.end);
                if (segmentStart < segmentEnd) {
                    split.push_back(Selection{
                        *model.resolve(ByteOffset{segmentStart}),
                        *model.resolve(ByteOffset{segmentEnd})});
                }
            }
        }
        if (!split.empty()) {
            selections = std::move(split);
        }
        desiredCell.reset();
        break;
    }

    case SelectionCommand::SelectToMatchingBracket:
    case SelectionCommand::GotoMatchingBracket: {
        if (!validBracketPairs(bracketPairs)) {
            return rejected(
                SelectionNavigationError::InvalidBracketPairs,
                "bracket tokens must be non-empty, distinct, and unambiguous");
        }
        const auto matches =
            bracketMatches(text, model, bracketPairs);
        for (auto& selection : selections) {
            const auto found = matches.find(
                static_cast<std::size_t>(
                    selection.active.byteOffset.value()));
            if (found == matches.end()) {
                continue;
            }
            const auto offset =
                command == SelectionCommand::GotoMatchingBracket ||
                        !found->second.forward
                    ? found->second.mateStart
                    : found->second.mateEnd;
            const auto destination =
                *model.resolve(ByteOffset{offset});
            if (command ==
                SelectionCommand::GotoMatchingBracket) {
                selection =
                    Selection{destination, destination};
            } else {
                selection.active = destination;
            }
        }
        desiredCell.reset();
        break;
    }

    case SelectionCommand::ViewRevealCaret:
    case SelectionCommand::ViewCenterCaret:
        break;
    }

    SelectionViewState after{
        SelectionSet{std::move(selections)}, firstVisualRow,
        before.firstVisualColumn, desiredCell};
    after.firstVisualRow = revealedFirstRow(
        model, after, viewport,
        command == SelectionCommand::ViewCenterCaret, wordWrap,
        currentViewport, rowProjection ? &*rowProjection : nullptr);
    after.firstVisualColumn =
        revealedFirstColumn(after, viewport, wordWrap);
    return accepted(before, std::move(after));
}

} // namespace ssg
