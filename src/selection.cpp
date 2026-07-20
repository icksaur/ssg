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
        const auto& boundaries = line_data(line_containing(raw)).line.boundaries;
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
        const auto& boundaries = line_data(line).line.boundaries;
        const auto index = boundary_index(boundaries, position);
        if (index > 0) return boundaries[index - 1];
        if (line == 0) return boundaries.front();
        return line_data(line - 1).line.boundaries.back();
    }

    [[nodiscard]] const DocumentPosition& next(
        const DocumentPosition& position) const {
        const auto line = static_cast<std::size_t>(position.line.value());
        const auto& boundaries = line_data(line).line.boundaries;
        const auto index = boundary_index(boundaries, position);
        if (index + 1 < boundaries.size()) return boundaries[index + 1];
        if (line + 1 >= line_starts_.size()) return boundaries.back();
        return line_data(line + 1).line.boundaries.front();
    }

    [[nodiscard]] const DocumentPosition& line_start(
        const DocumentPosition& position) const {
        return line_data(static_cast<std::size_t>(position.line.value()))
            .line.boundaries.front();
    }

    [[nodiscard]] const DocumentPosition& line_end(
        const DocumentPosition& position) const {
        return line_data(static_cast<std::size_t>(position.line.value()))
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
                return line_data(current).line.boundaries.back();
            }
        } else {
            target = line_count > current ? 0 : current - line_count;
            if (target == current) {
                return line_data(current).line.boundaries.front();
            }
        }
        return position_for_cell(target, desired_cell);
    }

    [[nodiscard]] DocumentPosition vertical_visual(
        const DocumentPosition& position, bool down, std::size_t row_count,
        CellIndex desired_cell, std::uint32_t columns) const {
        // No wrap: a visual row IS a logical line, so vertical-visual movement is
        // exactly logical-line movement — no whole-document wrapped rows.
        if (columns == kNoWrap) {
            return vertical(position, down, row_count, desired_cell);
        }
        const auto rows = wrapped_rows(columns);
        const auto current = wrapped_row_index(rows, position);
        const auto target =
            down ? std::min(current + row_count, rows.size() - 1)
                 : (row_count > current ? 0 : current - row_count);
        if (target == current) {
            return *resolve(ByteOffset{
                down ? rows[current].end_byte
                     : rows[current].start_byte});
        }
        return position_for_wrapped_cell(rows[target], desired_cell);
    }

    [[nodiscard]] CellIndex visual_column(
        const DocumentPosition& position, std::uint32_t columns) const {
        if (columns == kNoWrap) return position.cell;
        const auto rows = wrapped_rows(columns);
        const auto& row = rows[wrapped_row_index(rows, position)];
        return CellIndex{position.cell.value() - row.start_cell};
    }

    [[nodiscard]] std::size_t line_count() const noexcept {
        return line_starts_.size();
    }

    [[nodiscard]] const LogicalLine& line(std::size_t index) const {
        return line_data(index).line;
    }

    [[nodiscard]] const DocumentPosition& document_start() const {
        return line_data(0).line.boundaries.front();
    }

    [[nodiscard]] const DocumentPosition& document_end() const {
        return line_data(line_starts_.size() - 1).line.boundaries.back();
    }

    [[nodiscard]] DocumentPosition word_left(
        const DocumentPosition& position) const {
        // Walk one boundary left, then keep going while the run stays the same
        // segment category (words/whitespace).  Expressed with previous() so it
        // faults in only the lines it actually crosses (no global position array).
        const auto& start = document_start();
        if (position == start) return start;
        DocumentPosition cursor = previous(position);
        const auto category = category_at(cursor);
        if (category == SegmentCategory::word ||
            category == SegmentCategory::space) {
            while (cursor != start) {
                const auto& candidate = previous(cursor);
                if (category_at(candidate) != category) break;
                cursor = candidate;
            }
        }
        return cursor;
    }

    [[nodiscard]] DocumentPosition word_right(
        const DocumentPosition& position) const {
        const auto& end = document_end();
        if (position == end) return end;
        const auto category = category_at(position);
        DocumentPosition cursor = next(position);
        while (cursor != end && category_at(cursor) == category) {
            cursor = next(cursor);
        }
        return cursor;
    }

    [[nodiscard]] ViewportViewState viewport_state(
        ViewportDimensions dimensions,
        std::uint32_t requested_first_visual_row) const {
        return compute_viewport(all_runs(), dimensions,
                                requested_first_visual_row);
    }

    [[nodiscard]] std::uint32_t visual_row(
        const DocumentPosition& position, std::uint32_t columns) const {
        if (columns == kNoWrap) {
            return static_cast<std::uint32_t>(position.line.value());
        }
        const auto rows = wrapped_rows(columns);
        const auto row = wrapped_row_index(rows, position);
        if (row > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error(
                "selection visual row count exceeds uint32");
        }
        return static_cast<std::uint32_t>(row);
    }

private:
    enum class SegmentCategory : std::uint8_t {
        word,
        space,
        punctuation,
    };

    [[nodiscard]] SegmentCategory category_at(
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
            return SegmentCategory::word;
        }
        if (first == static_cast<unsigned char>(' ') ||
            first == static_cast<unsigned char>('\t') ||
            first == static_cast<unsigned char>('\n') ||
            first == static_cast<unsigned char>('\r')) {
            return SegmentCategory::space;
        }
        return SegmentCategory::punctuation;
    }

    [[nodiscard]] DocumentPosition position_for_cell(
        std::size_t line_index, CellIndex desired_cell) const {
        const auto& boundaries = line_data(line_index).line.boundaries;
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

    [[nodiscard]] std::vector<WrappedRow> wrapped_rows(
        std::uint32_t columns) const {
        std::vector<WrappedRow> rows;
        for (std::size_t line_index = 0; line_index < line_starts_.size();
             ++line_index) {
            const auto& data = line_data(line_index);
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

    [[nodiscard]] std::size_t wrapped_row_index(
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

    [[nodiscard]] DocumentPosition position_for_wrapped_cell(
        const WrappedRow& row, CellIndex desired_cell) const {
        const auto target =
            std::min(row.start_cell + desired_cell.value(),
                     row.end_cell);
        const auto& boundaries = line_data(row.logical_line).line.boundaries;
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

    [[nodiscard]] const LineData& line_data(std::size_t index) const {
        auto& slot = cache_[index];
        if (slot) return *slot;
        const std::size_t start = line_starts_[index];
        const std::size_t end = index + 1 < line_starts_.size()
                                    ? line_starts_[index + 1] - 1
                                    : text_.size();
        auto run =
            compute_cell_run(text_.substr(start, end - start), tab_width_);
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
    [[nodiscard]] std::size_t line_containing(std::size_t raw) const {
        const auto it =
            std::upper_bound(line_starts_.begin(), line_starts_.end(), raw);
        return static_cast<std::size_t>(
            std::distance(line_starts_.begin(), it) - 1);
    }

    [[nodiscard]] std::size_t boundary_index(
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
    [[nodiscard]] std::vector<CellRun> all_runs() const {
        std::vector<CellRun> runs;
        runs.reserve(line_starts_.size());
        for (std::size_t index = 0; index < line_starts_.size(); ++index) {
            runs.push_back(line_data(index).run);
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
            SelectionNavigationError::none,
            SelectionViewDelta{false, std::nullopt},
            {}};
    }
    return SelectionNavigationResult{
        SelectionNavigationError::none,
        SelectionViewDelta{true, std::move(after)},
        {}};
}

bool is_valid_position(const TextModel& model,
                       const DocumentPosition& position) {
    const auto resolved = model.resolve(position.byte_offset);
    return resolved && *resolved == position;
}

std::uint32_t revealed_first_row(const TextModel& model,
                                 const SelectionViewState& state,
                                 ViewportDimensions dimensions,
                                 bool center, bool word_wrap) {
    if (word_wrap) {
        const auto viewport =
            model.viewport_state(dimensions, state.first_visual_row);
        const auto target =
            model.visual_row(state.selections.primary().active,
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
    const auto total = static_cast<std::uint32_t>(model.line_count());
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
std::uint32_t revealed_first_column(const SelectionViewState& state,
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

bool valid_bracket_pairs(std::span<const BracketPair> pairs) {
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

std::unordered_map<std::size_t, BracketMatch> bracket_matches(
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

bool Selection::is_caret() const noexcept {
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
          {"cursor.set_position", SelectionCommand::cursor_set_position},
          {"cursor.left", SelectionCommand::cursor_left},
          {"cursor.right", SelectionCommand::cursor_right},
          {"cursor.word_left", SelectionCommand::cursor_word_left},
          {"cursor.word_right", SelectionCommand::cursor_word_right},
          {"cursor.line_up", SelectionCommand::cursor_line_up},
          {"cursor.line_down", SelectionCommand::cursor_line_down},
          {"cursor.line_start", SelectionCommand::cursor_line_start},
          {"cursor.line_end", SelectionCommand::cursor_line_end},
          {"cursor.page_up", SelectionCommand::cursor_page_up},
          {"cursor.page_down", SelectionCommand::cursor_page_down},
          {"cursor.document_start", SelectionCommand::cursor_document_start},
          {"cursor.document_end", SelectionCommand::cursor_document_end},
          {"select.set_range", SelectionCommand::select_set_range},
          {"select.add_range", SelectionCommand::select_add_range},
          {"select.left", SelectionCommand::select_left},
          {"select.right", SelectionCommand::select_right},
          {"select.word_left", SelectionCommand::select_word_left},
          {"select.word_right", SelectionCommand::select_word_right},
          {"select.line_up", SelectionCommand::select_line_up},
          {"select.line_down", SelectionCommand::select_line_down},
          {"select.line_start", SelectionCommand::select_line_start},
          {"select.line_end", SelectionCommand::select_line_end},
          {"select.page_up", SelectionCommand::select_page_up},
          {"select.page_down", SelectionCommand::select_page_down},
          {"select.document_start", SelectionCommand::select_document_start},
          {"select.document_end", SelectionCommand::select_document_end},
          {"select.all", SelectionCommand::select_all},
          {"select.add_next_occurrence",
           SelectionCommand::select_add_next_occurrence},
          {"select.add_cursor_up", SelectionCommand::select_add_cursor_up},
          {"select.add_cursor_down", SelectionCommand::select_add_cursor_down},
          {"select.split_into_lines",
           SelectionCommand::select_split_into_lines},
          {"select.to_matching_bracket",
           SelectionCommand::select_to_matching_bracket},
          {"goto.matching_bracket",
           SelectionCommand::goto_matching_bracket},
          {"view.reveal_caret", SelectionCommand::view_reveal_caret},
          {"view.center_caret", SelectionCommand::view_center_caret},
      }} {}

const std::array<SelectionCommandDescriptor, 36>&
SelectionNavigationCommandSet::descriptors() const noexcept {
    return descriptors_;
}

SelectionNavigationCommandSet selection_navigation_command_set() {
    return SelectionNavigationCommandSet{};
}

std::optional<DocumentPosition> resolve_document_position(
    std::string_view text, ByteOffset byte_offset, int tab_width) {
    if (tab_width < 1 || tab_width > 16) {
        return std::nullopt;
    }
    return TextModel{text, tab_width}.resolve(byte_offset);
}

SelectionNavigationResult apply_selection_navigation(
    std::string_view text, const SelectionViewState& before,
    SelectionCommand command, ViewportDimensions viewport,
    SelectionCommandArguments arguments,
    std::span<const BracketPair> bracket_pairs, int tab_width, bool word_wrap) {
    if (tab_width < 1 || tab_width > 16) {
        return rejected(SelectionNavigationError::invalid_tab_width,
                        "tab width must be between 1 and 16");
    }
    const TextModel model{text, tab_width};
    const std::uint32_t nav_columns =
        word_wrap ? viewport.columns : std::numeric_limits<std::uint32_t>::max();
    for (const auto& selection : before.selections.items()) {
        if (!is_valid_position(model, selection.anchor) ||
            !is_valid_position(model, selection.active)) {
            return rejected(
                SelectionNavigationError::invalid_position,
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
        word_wrap ? model.viewport_state(viewport, before.first_visual_row)
                  : compute_viewport_unwrapped(text, viewport,
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
    case SelectionCommand::cursor_set_position:
        if (!arguments.position) {
            return rejected(SelectionNavigationError::missing_argument,
                            "cursor.set_position requires a position");
        }
        if (!is_valid_position(model, *arguments.position)) {
            return rejected(SelectionNavigationError::invalid_position,
                            "cursor position does not match the document layout");
        }
        selections = {
            Selection{*arguments.position, *arguments.position}};
        desired_cell.reset();
        break;

    case SelectionCommand::cursor_left:
        replace_with_carets([&](const Selection& selection) {
            return selection.is_caret() ? model.previous(selection.active)
                                        : selection.lower();
        });
        desired_cell.reset();
        break;

    case SelectionCommand::cursor_right:
        replace_with_carets([&](const Selection& selection) {
            return selection.is_caret() ? model.next(selection.active)
                                        : selection.upper();
        });
        desired_cell.reset();
        break;

    case SelectionCommand::cursor_word_left:
        replace_with_carets([&](const Selection& selection) {
            return model.word_left(selection.lower());
        });
        desired_cell.reset();
        break;

    case SelectionCommand::cursor_word_right:
        replace_with_carets([&](const Selection& selection) {
            return model.word_right(selection.upper());
        });
        desired_cell.reset();
        break;

    case SelectionCommand::cursor_line_start:
        replace_with_carets([&](const Selection& selection) {
            return model.line_start(selection.lower());
        });
        desired_cell.reset();
        break;

    case SelectionCommand::cursor_line_end:
        replace_with_carets([&](const Selection& selection) {
            return model.line_end(selection.upper());
        });
        desired_cell.reset();
        break;

    case SelectionCommand::cursor_line_up:
    case SelectionCommand::cursor_line_down:
    case SelectionCommand::cursor_page_up:
    case SelectionCommand::cursor_page_down: {
        const bool down =
            command == SelectionCommand::cursor_line_down ||
            command == SelectionCommand::cursor_page_down;
        const bool page =
            command == SelectionCommand::cursor_page_up ||
            command == SelectionCommand::cursor_page_down;
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
                    : model.visual_column(origin, nav_columns);
            const auto destination =
                model.vertical_visual(origin, down, count, desired,
                                      nav_columns);
            selection = Selection{destination, destination};
            if (index + 1 == selections.size()) {
                primary_desired =
                    model.visual_row(destination, nav_columns) ==
                            model.visual_row(origin, nav_columns)
                        ? model.visual_column(destination,
                                              nav_columns)
                        : desired;
            }
        }
        desired_cell = primary_desired;
        break;
    }

    case SelectionCommand::cursor_document_start: {
        const auto destination = model.document_start();
        selections = {Selection{destination, destination}};
        desired_cell.reset();
        break;
    }

    case SelectionCommand::cursor_document_end: {
        const auto destination = model.document_end();
        selections = {Selection{destination, destination}};
        desired_cell.reset();
        break;
    }

    case SelectionCommand::select_set_range:
    case SelectionCommand::select_add_range:
        if (!arguments.selection) {
            return rejected(
                SelectionNavigationError::missing_argument,
                command == SelectionCommand::select_set_range
                    ? "select.set_range requires a selection"
                    : "select.add_range requires a selection");
        }
        if (!is_valid_position(model, arguments.selection->anchor) ||
            !is_valid_position(model, arguments.selection->active)) {
            return rejected(
                SelectionNavigationError::invalid_position,
                "selection range does not match the document layout");
        }
        if (command == SelectionCommand::select_set_range) {
            selections = {*arguments.selection};
        } else {
            selections.push_back(*arguments.selection);
        }
        desired_cell.reset();
        break;

    case SelectionCommand::select_left:
        extend_active([&](const Selection& selection) {
            return model.previous(selection.active);
        });
        desired_cell.reset();
        break;

    case SelectionCommand::select_right:
        extend_active([&](const Selection& selection) {
            return model.next(selection.active);
        });
        desired_cell.reset();
        break;

    case SelectionCommand::select_word_left:
        extend_active([&](const Selection& selection) {
            return model.word_left(selection.active);
        });
        desired_cell.reset();
        break;

    case SelectionCommand::select_word_right:
        extend_active([&](const Selection& selection) {
            return model.word_right(selection.active);
        });
        desired_cell.reset();
        break;

    case SelectionCommand::select_line_start:
        extend_active([&](const Selection& selection) {
            return model.line_start(selection.active);
        });
        desired_cell.reset();
        break;

    case SelectionCommand::select_line_end:
        extend_active([&](const Selection& selection) {
            return model.line_end(selection.active);
        });
        desired_cell.reset();
        break;

    case SelectionCommand::select_line_up:
    case SelectionCommand::select_line_down:
    case SelectionCommand::select_page_up:
    case SelectionCommand::select_page_down: {
        const bool down =
            command == SelectionCommand::select_line_down ||
            command == SelectionCommand::select_page_down;
        const bool page =
            command == SelectionCommand::select_page_up ||
            command == SelectionCommand::select_page_down;
        const auto count =
            page ? current_viewport.visible_rows.size() : 1;
        std::optional<CellIndex> primary_desired;
        for (std::size_t index = 0; index < selections.size(); ++index) {
            auto& selection = selections[index];
            const auto origin = selection.active;
            const auto desired =
                index + 1 == selections.size() && before.desired_cell
                    ? *before.desired_cell
                    : model.visual_column(origin, nav_columns);
            selection.active =
                model.vertical_visual(origin, down, count, desired,
                                      nav_columns);
            if (index + 1 == selections.size()) {
                primary_desired =
                    model.visual_row(selection.active,
                                     nav_columns) ==
                            model.visual_row(origin, nav_columns)
                        ? model.visual_column(selection.active,
                                              nav_columns)
                        : desired;
            }
        }
        desired_cell = primary_desired;
        break;
    }

    case SelectionCommand::select_document_start:
        extend_active([&](const Selection&) {
            return model.document_start();
        });
        desired_cell.reset();
        break;

    case SelectionCommand::select_document_end:
        extend_active([&](const Selection&) {
            return model.document_end();
        });
        desired_cell.reset();
        break;

    case SelectionCommand::select_all:
        selections = {Selection{model.document_start(),
                                model.document_end()}};
        desired_cell.reset();
        break;

    case SelectionCommand::select_add_next_occurrence: {
        const auto& source = selections.back();
        if (!source.is_caret()) {
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

    case SelectionCommand::select_add_cursor_up:
    case SelectionCommand::select_add_cursor_down: {
        const bool down =
            command == SelectionCommand::select_add_cursor_down;
        std::vector<Selection> added;
        for (const auto& selection : selections) {
            const auto line =
                static_cast<std::size_t>(selection.active.line.value());
            if ((!down && line == 0) ||
                (down && line + 1 == model.line_count())) {
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

    case SelectionCommand::select_split_into_lines: {
        std::vector<Selection> split;
        for (const auto& selection : selections) {
            if (selection.is_caret()) {
                split.push_back(selection);
                continue;
            }
            const auto low = static_cast<std::size_t>(
                selection.lower().byte_offset.value());
            const auto high = static_cast<std::size_t>(
                selection.upper().byte_offset.value());
            for (std::size_t line_index = 0;
                 line_index < model.line_count(); ++line_index) {
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

    case SelectionCommand::select_to_matching_bracket:
    case SelectionCommand::goto_matching_bracket: {
        if (!valid_bracket_pairs(bracket_pairs)) {
            return rejected(
                SelectionNavigationError::invalid_bracket_pairs,
                "bracket tokens must be non-empty, distinct, and unambiguous");
        }
        const auto matches =
            bracket_matches(text, model, bracket_pairs);
        for (auto& selection : selections) {
            const auto found = matches.find(
                static_cast<std::size_t>(
                    selection.active.byte_offset.value()));
            if (found == matches.end()) {
                continue;
            }
            const auto offset =
                command == SelectionCommand::goto_matching_bracket ||
                        !found->second.forward
                    ? found->second.mate_start
                    : found->second.mate_end;
            const auto destination =
                *model.resolve(ByteOffset{offset});
            if (command ==
                SelectionCommand::goto_matching_bracket) {
                selection =
                    Selection{destination, destination};
            } else {
                selection.active = destination;
            }
        }
        desired_cell.reset();
        break;
    }

    case SelectionCommand::view_reveal_caret:
    case SelectionCommand::view_center_caret:
        break;
    }

    SelectionViewState after{
        SelectionSet{std::move(selections)}, first_visual_row,
        before.first_visual_column, desired_cell};
    after.first_visual_row = revealed_first_row(
        model, after, viewport,
        command == SelectionCommand::view_center_caret, word_wrap);
    after.first_visual_column =
        revealed_first_column(after, viewport, word_wrap);
    return accepted(before, std::move(after));
}

} // namespace ssg
