#include <ssg/selection.h>

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
    std::size_t logical_line;
    std::size_t start_byte;
    std::size_t end_byte;
    std::uint64_t start_cell;
    std::uint64_t end_cell;
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
    TextModel(std::string_view text, int tab_width)
        : text_(text), tab_width_(tab_width) {
        line_starts_.push_back(0);
        for (std::size_t newline = text.find('\n');
             newline != std::string_view::npos;
             newline = text.find('\n', newline + 1)) {
            line_starts_.push_back(newline + 1);
        }
        cache_.resize(line_starts_.size());
    }

    [[nodiscard]] std::optional<DocumentPosition> resolve(
        ByteOffset offset) const {
        const auto raw = static_cast<std::size_t>(offset.value());
        if (offset.value() > text_.size()) return std::nullopt;
        const auto& boundaries = lineData(lineContaining(raw)).line.boundaries;
        const auto found = std::lower_bound(
            boundaries.begin(), boundaries.end(), raw,
            [](const DocumentPosition& candidate, std::size_t value) {
                return candidate.byte_offset.value() < value;
            });
        if (found == boundaries.end() ||
            found->byte_offset.value() != offset.value()) {
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
        if (line + 1 >= line_starts_.size()) return boundaries.back();
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
        const DocumentPosition& position, bool down, std::size_t line_count,
        CellIndex desired_cell) const {
        const auto current = static_cast<std::size_t>(position.line.value());
        const auto last_line = line_starts_.size() - 1;
        std::size_t target = current;
        if (down) {
            target = line_count > last_line - current ? last_line
                                                       : current + line_count;
            if (target == current) {
                return lineData(current).line.boundaries.back();
            }
        } else {
            target = line_count > current ? 0 : current - line_count;
            if (target == current) {
                return lineData(current).line.boundaries.front();
            }
        }
        return positionForCell(target, desired_cell);
    }

    [[nodiscard]] DocumentPosition verticalVisual(
        const DocumentPosition& position, bool down, std::size_t row_count,
        CellIndex desired_cell, std::uint32_t columns) const {
        // No wrap: a visual row IS a logical line, so vertical-visual movement is
        // exactly logical-line movement — no whole-document wrapped rows.
        if (columns == kNoWrap) {
            return vertical(position, down, row_count, desired_cell);
        }
        const auto rows = wrappedRows(columns);
        const auto current = wrappedRowIndex(rows, position);
        const auto target =
            down ? std::min(current + row_count, rows.size() - 1)
                 : (row_count > current ? 0 : current - row_count);
        if (target == current) {
            return *resolve(ByteOffset{
                down ? rows[current].end_byte
                     : rows[current].start_byte});
        }
        return positionForWrappedCell(rows[target], desired_cell);
    }

    [[nodiscard]] CellIndex visualColumn(
        const DocumentPosition& position, std::uint32_t columns) const {
        if (columns == kNoWrap) return position.cell;
        const auto rows = wrappedRows(columns);
        const auto& row = rows[wrappedRowIndex(rows, position)];
        return CellIndex{position.cell.value() - row.start_cell};
    }

    [[nodiscard]] std::size_t lineCount() const noexcept {
        return line_starts_.size();
    }

    [[nodiscard]] const LogicalLine& line(std::size_t index) const {
        return lineData(index).line;
    }

    [[nodiscard]] const DocumentPosition& documentStart() const {
        return lineData(0).line.boundaries.front();
    }

    [[nodiscard]] const DocumentPosition& documentEnd() const {
        return lineData(line_starts_.size() - 1).line.boundaries.back();
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

    [[nodiscard]] ViewportViewState viewportState(
        ViewportDimensions dimensions,
        std::uint32_t requested_first_visual_row) const {
        return computeViewport(allRuns(), dimensions,
                                requested_first_visual_row);
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
            static_cast<std::size_t>(position.byte_offset.value());
        const auto first = static_cast<unsigned char>(text_[start]);
        if (first >= 0x80 ||
            (first >= static_cast<unsigned char>('a') &&
             first <= static_cast<unsigned char>('z')) ||
            (first >= static_cast<unsigned char>('A') &&
             first <= static_cast<unsigned char>('Z')) ||
            (first >= static_cast<unsigned char>('0') &&
             first <= static_cast<unsigned char>('9')) ||
            first == static_cast<unsigned char>('_')) {
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
        std::size_t line_index, CellIndex desired_cell) const {
        const auto& boundaries = lineData(line_index).line.boundaries;
        const DocumentPosition* chosen = &boundaries.front();
        for (const auto& boundary : boundaries) {
            if (boundary.cell > desired_cell) {
                break;
            }
            if (boundary.cell > chosen->cell) {
                chosen = &boundary;
            }
        }
        return *chosen;
    }

    [[nodiscard]] std::vector<WrappedRow> wrappedRows(
        std::uint32_t columns) const {
        std::vector<WrappedRow> rows;
        for (std::size_t line_index = 0; line_index < line_starts_.size();
             ++line_index) {
            const auto& data = lineData(line_index);
            const auto& line = data.line;
            const auto& run = data.run;
            if (run.spans.empty()) {
                rows.push_back(WrappedRow{
                    line_index, line.start, line.end, 0, 0});
                continue;
            }

            std::size_t first_span = 0;
            std::size_t span_count = 0;
            std::uint64_t start_cell = 0;
            std::uint32_t content_cells = 0;
            const auto finish_row = [&] {
                const auto& first = run.spans[first_span];
                const auto& last =
                    run.spans[first_span + span_count - 1];
                rows.push_back(WrappedRow{
                    line_index,
                    line.start + first.byte_offset,
                    line.start + last.byte_offset + last.byte_len,
                    start_cell,
                    start_cell + content_cells,
                });
            };

            for (std::size_t span_index = 0;
                 span_index < run.spans.size(); ++span_index) {
                const auto width = run.spans[span_index].cell_width;
                if (width > 0 && content_cells > 0 &&
                    width >
                        columns - std::min(content_cells, columns)) {
                    finish_row();
                    first_span = span_index;
                    span_count = 0;
                    start_cell += content_cells;
                    content_cells = 0;
                }
                ++span_count;
                content_cells += width;
            }
            finish_row();
        }
        return rows;
    }

    [[nodiscard]] std::size_t wrappedRowIndex(
        const std::vector<WrappedRow>& rows,
        const DocumentPosition& position) const {
        const auto byte_offset =
            static_cast<std::size_t>(position.byte_offset.value());
        const auto logical_line =
            static_cast<std::size_t>(position.line.value());
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto& row = rows[index];
            if (row.logical_line != logical_line ||
                byte_offset < row.start_byte) {
                continue;
            }
            const bool final_row =
                index + 1 == rows.size() ||
                rows[index + 1].logical_line != logical_line;
            if (byte_offset < row.end_byte ||
                (final_row && byte_offset == row.end_byte)) {
                return index;
            }
        }
        throw std::logic_error(
            "valid document position has no wrapped visual row");
    }

    [[nodiscard]] DocumentPosition positionForWrappedCell(
        const WrappedRow& row, CellIndex desired_cell) const {
        const auto target =
            std::min(row.start_cell + desired_cell.value(),
                     row.end_cell);
        const auto& boundaries = lineData(row.logical_line).line.boundaries;
        const DocumentPosition* chosen = nullptr;
        for (const auto& boundary : boundaries) {
            const auto byte_offset = boundary.byte_offset.value();
            if (byte_offset < row.start_byte) {
                continue;
            }
            if (byte_offset > row.end_byte ||
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
                   : *resolve(ByteOffset{row.start_byte});
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
        const std::size_t start = line_starts_[index];
        const std::size_t end = index + 1 < line_starts_.size()
                                    ? line_starts_[index + 1] - 1
                                    : text_.size();
        auto run =
            computeCellRun(text_.substr(start, end - start), tab_width_);
        std::vector<DocumentPosition> boundaries;
        boundaries.reserve(run.spans.size() + 1);
        std::uint64_t cell = 0;
        boundaries.push_back(
            DocumentPosition{ByteOffset{start}, LineIndex{index}, CellIndex{0}});
        for (const auto& span : run.spans) {
            cell += span.cell_width;
            boundaries.push_back(DocumentPosition{
                ByteOffset{start + span.byte_offset + span.byte_len},
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
            std::upper_bound(line_starts_.begin(), line_starts_.end(), raw);
        return static_cast<std::size_t>(
            std::distance(line_starts_.begin(), it) - 1);
    }

    [[nodiscard]] std::size_t boundaryIndex(
        const std::vector<DocumentPosition>& boundaries,
        const DocumentPosition& position) const {
        const auto found = std::lower_bound(
            boundaries.begin(), boundaries.end(), position.byte_offset,
            [](const DocumentPosition& candidate, ByteOffset value) {
                return candidate.byte_offset < value;
            });
        return static_cast<std::size_t>(
            std::distance(boundaries.begin(), found));
    }

    // Every line's CellRun (faults in all lines) — the wrap-ON viewport path only.
    [[nodiscard]] std::vector<CellRun> allRuns() const {
        std::vector<CellRun> runs;
        runs.reserve(line_starts_.size());
        for (std::size_t index = 0; index < line_starts_.size(); ++index) {
            runs.push_back(lineData(index).run);
        }
        return runs;
    }

    std::string_view text_;
    int tab_width_;
    std::vector<std::size_t> line_starts_;
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
    const auto resolved = model.resolve(position.byte_offset);
    return resolved && *resolved == position;
}

std::uint32_t revealedFirstRow(const TextModel& model,
                                 const SelectionViewState& state,
                                 ViewportDimensions dimensions,
                                 bool center, bool word_wrap) {
    if (word_wrap) {
        const auto viewport =
            model.viewportState(dimensions, state.first_visual_row);
        const auto target =
            model.visualRow(state.selections.primary().active,
                             dimensions.columns);
        const auto maximum = viewport.scrollbar.maximum_first_row;
        if (center) {
            const auto half = dimensions.rows / 2;
            const auto requested = target > half ? target - half : 0;
            return std::min(requested, maximum);
        }
        if (target < viewport.first_visual_row) {
            return target;
        }
        const auto visible_end =
            static_cast<std::uint64_t>(viewport.first_visual_row) +
            viewport.visible_rows.size();
        if (target >= visible_end) {
            const auto requested =
                target - static_cast<std::uint32_t>(
                             viewport.visible_rows.size()) +
                1;
            return std::min(requested, maximum);
        }
        return viewport.first_visual_row;
    }

    // Word wrap OFF (M12 VP-H): one logical line is one visual row, so the caret's
    // visual row is its logical line index and the total is the line count — no
    // O(document) wrapped counting.
    const auto total = static_cast<std::uint32_t>(model.lineCount());
    const std::uint32_t maximum =
        total > dimensions.rows ? total - dimensions.rows : 0;
    const std::uint32_t current_first =
        std::min(state.first_visual_row, maximum);
    const auto target = static_cast<std::uint32_t>(
        state.selections.primary().active.line.value());
    if (center) {
        const auto half = dimensions.rows / 2;
        const auto requested = target > half ? target - half : 0;
        return std::min(requested, maximum);
    }
    if (target < current_first) {
        return target;
    }
    if (target >= current_first + dimensions.rows) {
        return std::min(target - dimensions.rows + 1, maximum);
    }
    return current_first;
}

// The horizontal scroll offset (word wrap OFF only) that keeps the primary
// caret's cell column within the pane, scrolling minimally.  The caret's visual
// column under no-wrap is its per-line cell index (the row starts at cell 0), so
// this needs no document scan.  Returns 0 when word wrap is on.
std::uint32_t revealedFirstColumn(const SelectionViewState& state,
                                    ViewportDimensions dimensions,
                                    bool word_wrap) {
    if (word_wrap) return 0;
    const auto caret_cell = static_cast<std::uint32_t>(
        state.selections.primary().active.cell.value());
    std::uint32_t first = state.first_visual_column;
    if (caret_cell < first) {
        first = caret_cell;
    } else if (caret_cell >= first + dimensions.columns) {
        first = caret_cell - dimensions.columns + 1;
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
    std::size_t mate_start;
    std::size_t mate_end;
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
        const auto next = model.next(*current).byte_offset.value();
        if (next <= offset) {
            break;
        }
        offset = static_cast<std::size_t>(next);
    }
    return matches;
}

} // namespace

const DocumentPosition& Selection::lower() const noexcept {
    return anchor.byte_offset <= active.byte_offset ? anchor : active;
}

const DocumentPosition& Selection::upper() const noexcept {
    return anchor.byte_offset <= active.byte_offset ? active : anchor;
}

bool Selection::isCaret() const noexcept {
    return anchor.byte_offset == active.byte_offset;
}

SelectionSet::SelectionSet(std::vector<Selection> selections)
    : selections_(std::move(selections)) {
    if (selections_.empty()) {
        throw std::invalid_argument(
            "selection set must contain at least one selection");
    }
    std::sort(selections_.begin(), selections_.end(),
              [](const Selection& left, const Selection& right) {
                  if (left.lower().byte_offset != right.lower().byte_offset) {
                      return left.lower().byte_offset <
                             right.lower().byte_offset;
                  }
                  return left.upper().byte_offset <
                         right.upper().byte_offset;
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
        if (current.lower().byte_offset < previous.upper().byte_offset) {
            const auto& upper =
                current.upper().byte_offset > previous.upper().byte_offset
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

SelectionNavigationCommandSet::SelectionNavigationCommandSet()
    : descriptors_{{
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
          {"view.reveal_caret", SelectionCommand::ViewRevealCaret},
          {"view.center_caret", SelectionCommand::ViewCenterCaret},
      }} {}

const std::array<SelectionCommandDescriptor, 36>&
SelectionNavigationCommandSet::descriptors() const noexcept {
    return descriptors_;
}

SelectionNavigationCommandSet selectionNavigationCommandSet() {
    return SelectionNavigationCommandSet{};
}

std::optional<DocumentPosition> resolveDocumentPosition(
    std::string_view text, ByteOffset byte_offset, int tab_width) {
    if (tab_width < 1 || tab_width > 16) {
        return std::nullopt;
    }
    return TextModel{text, tab_width}.resolve(byte_offset);
}

SelectionNavigationResult applySelectionNavigation(
    std::string_view text, const SelectionViewState& before,
    SelectionCommand command, ViewportDimensions viewport,
    SelectionCommandArguments arguments,
    std::span<const BracketPair> bracket_pairs, int tab_width, bool word_wrap) {
    if (tab_width < 1 || tab_width > 16) {
        return rejected(SelectionNavigationError::InvalidTabWidth,
                        "tab width must be between 1 and 16");
    }
    const TextModel model{text, tab_width};
    const std::uint32_t nav_columns =
        word_wrap ? viewport.columns : std::numeric_limits<std::uint32_t>::max();
    for (const auto& selection : before.selections.items()) {
        if (!isValidPosition(model, selection.anchor) ||
            !isValidPosition(model, selection.active)) {
            return rejected(
                SelectionNavigationError::InvalidPosition,
                "selection endpoint does not match the document layout");
        }
    }

    auto selections = before.selections.items();
    auto desired_cell = before.desired_cell;
    // The current viewport supplies the page size (visible row count) and the base
    // scroll row.  Under no-wrap, use the O(visible rows) unwrapped projection so
    // this does NOT segment the whole document (M12 VP-2b); the wrap-ON path keeps
    // the exact wrapped viewport.
    const auto current_viewport =
        word_wrap ? model.viewportState(viewport, before.first_visual_row)
                  : computeViewportUnwrapped(text, viewport,
                                               before.first_visual_row, 0,
                                               tab_width);
    std::uint32_t first_visual_row =
        current_viewport.first_visual_row;

    const auto replace_with_carets = [&](auto&& destination) {
        for (auto& selection : selections) {
            const auto position = destination(selection);
            selection = Selection{position, position};
        }
    };
    const auto extend_active = [&](auto&& destination) {
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
        desired_cell.reset();
        break;

    case SelectionCommand::CursorLeft:
        replace_with_carets([&](const Selection& selection) {
            return selection.isCaret() ? model.previous(selection.active)
                                        : selection.lower();
        });
        desired_cell.reset();
        break;

    case SelectionCommand::CursorRight:
        replace_with_carets([&](const Selection& selection) {
            return selection.isCaret() ? model.next(selection.active)
                                        : selection.upper();
        });
        desired_cell.reset();
        break;

    case SelectionCommand::CursorWordLeft:
        replace_with_carets([&](const Selection& selection) {
            return model.wordLeft(selection.lower());
        });
        desired_cell.reset();
        break;

    case SelectionCommand::CursorWordRight:
        replace_with_carets([&](const Selection& selection) {
            return model.wordRight(selection.upper());
        });
        desired_cell.reset();
        break;

    case SelectionCommand::CursorLineStart:
        replace_with_carets([&](const Selection& selection) {
            return model.lineStart(selection.lower());
        });
        desired_cell.reset();
        break;

    case SelectionCommand::CursorLineEnd:
        replace_with_carets([&](const Selection& selection) {
            return model.lineEnd(selection.upper());
        });
        desired_cell.reset();
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
            page ? current_viewport.visible_rows.size() : 1;
        std::optional<CellIndex> primary_desired;
        for (std::size_t index = 0; index < selections.size(); ++index) {
            auto& selection = selections[index];
            const auto origin = down ? selection.upper()
                                     : selection.lower();
            const auto desired =
                index + 1 == selections.size() && before.desired_cell
                    ? *before.desired_cell
                    : model.visualColumn(origin, nav_columns);
            const auto destination =
                model.verticalVisual(origin, down, count, desired,
                                      nav_columns);
            selection = Selection{destination, destination};
            if (index + 1 == selections.size()) {
                primary_desired =
                    model.visualRow(destination, nav_columns) ==
                            model.visualRow(origin, nav_columns)
                        ? model.visualColumn(destination,
                                              nav_columns)
                        : desired;
            }
        }
        desired_cell = primary_desired;
        break;
    }

    case SelectionCommand::CursorDocumentStart: {
        const auto destination = model.documentStart();
        selections = {Selection{destination, destination}};
        desired_cell.reset();
        break;
    }

    case SelectionCommand::CursorDocumentEnd: {
        const auto destination = model.documentEnd();
        selections = {Selection{destination, destination}};
        desired_cell.reset();
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
        desired_cell.reset();
        break;

    case SelectionCommand::SelectLeft:
        extend_active([&](const Selection& selection) {
            return model.previous(selection.active);
        });
        desired_cell.reset();
        break;

    case SelectionCommand::SelectRight:
        extend_active([&](const Selection& selection) {
            return model.next(selection.active);
        });
        desired_cell.reset();
        break;

    case SelectionCommand::SelectWordLeft:
        extend_active([&](const Selection& selection) {
            return model.wordLeft(selection.active);
        });
        desired_cell.reset();
        break;

    case SelectionCommand::SelectWordRight:
        extend_active([&](const Selection& selection) {
            return model.wordRight(selection.active);
        });
        desired_cell.reset();
        break;

    case SelectionCommand::SelectLineStart:
        extend_active([&](const Selection& selection) {
            return model.lineStart(selection.active);
        });
        desired_cell.reset();
        break;

    case SelectionCommand::SelectLineEnd:
        extend_active([&](const Selection& selection) {
            return model.lineEnd(selection.active);
        });
        desired_cell.reset();
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
            page ? current_viewport.visible_rows.size() : 1;
        std::optional<CellIndex> primary_desired;
        for (std::size_t index = 0; index < selections.size(); ++index) {
            auto& selection = selections[index];
            const auto origin = selection.active;
            const auto desired =
                index + 1 == selections.size() && before.desired_cell
                    ? *before.desired_cell
                    : model.visualColumn(origin, nav_columns);
            selection.active =
                model.verticalVisual(origin, down, count, desired,
                                      nav_columns);
            if (index + 1 == selections.size()) {
                primary_desired =
                    model.visualRow(selection.active,
                                     nav_columns) ==
                            model.visualRow(origin, nav_columns)
                        ? model.visualColumn(selection.active,
                                              nav_columns)
                        : desired;
            }
        }
        desired_cell = primary_desired;
        break;
    }

    case SelectionCommand::SelectDocumentStart:
        extend_active([&](const Selection&) {
            return model.documentStart();
        });
        desired_cell.reset();
        break;

    case SelectionCommand::SelectDocumentEnd:
        extend_active([&](const Selection&) {
            return model.documentEnd();
        });
        desired_cell.reset();
        break;

    case SelectionCommand::SelectAll:
        selections = {Selection{model.documentStart(),
                                model.documentEnd()}};
        desired_cell.reset();
        break;

    case SelectionCommand::SelectAddNextOccurrence: {
        const auto& source = selections.back();
        if (!source.isCaret()) {
            const auto low = static_cast<std::size_t>(
                source.lower().byte_offset.value());
            const auto high = static_cast<std::size_t>(
                source.upper().byte_offset.value());
            const auto needle = text.substr(low, high - low);
            const auto find_valid =
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
            auto occurrence = find_valid(high, text.size());
            if (!occurrence) {
                occurrence = find_valid(0, low);
            }
            if (occurrence) {
                selections.push_back(*occurrence);
            }
        }
        desired_cell.reset();
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
        desired_cell.reset();
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
                selection.lower().byte_offset.value());
            const auto high = static_cast<std::size_t>(
                selection.upper().byte_offset.value());
            for (std::size_t line_index = 0;
                 line_index < model.lineCount(); ++line_index) {
                const auto& line = model.line(line_index);
                const auto segment_start = std::max(low, line.start);
                const auto segment_end = std::min(high, line.end);
                if (segment_start < segment_end) {
                    split.push_back(Selection{
                        *model.resolve(ByteOffset{segment_start}),
                        *model.resolve(ByteOffset{segment_end})});
                }
            }
        }
        if (!split.empty()) {
            selections = std::move(split);
        }
        desired_cell.reset();
        break;
    }

    case SelectionCommand::SelectToMatchingBracket:
    case SelectionCommand::GotoMatchingBracket: {
        if (!validBracketPairs(bracket_pairs)) {
            return rejected(
                SelectionNavigationError::InvalidBracketPairs,
                "bracket tokens must be non-empty, distinct, and unambiguous");
        }
        const auto matches =
            bracketMatches(text, model, bracket_pairs);
        for (auto& selection : selections) {
            const auto found = matches.find(
                static_cast<std::size_t>(
                    selection.active.byte_offset.value()));
            if (found == matches.end()) {
                continue;
            }
            const auto offset =
                command == SelectionCommand::GotoMatchingBracket ||
                        !found->second.forward
                    ? found->second.mate_start
                    : found->second.mate_end;
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
        desired_cell.reset();
        break;
    }

    case SelectionCommand::ViewRevealCaret:
    case SelectionCommand::ViewCenterCaret:
        break;
    }

    SelectionViewState after{
        SelectionSet{std::move(selections)}, first_visual_row,
        before.first_visual_column, desired_cell};
    after.first_visual_row = revealedFirstRow(
        model, after, viewport,
        command == SelectionCommand::ViewCenterCaret, word_wrap);
    after.first_visual_column =
        revealedFirstColumn(after, viewport, word_wrap);
    return accepted(before, std::move(after));
}

} // namespace ssg
