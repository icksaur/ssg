#include <ssg/selection.h>

#include "reference_editor.h"
#include "test_helpers.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using ssg::BracketPair;
using ssg::ByteOffset;
using ssg::CellIndex;
using ssg::DocumentPosition;
using ssg::Selection;
using ssg::SelectionCommand;
using ssg::SelectionCommandArguments;
using ssg::SelectionNavigationError;
using ssg::SelectionNavigationResult;
using ssg::SelectionSet;
using ssg::SelectionViewState;
using ssg::ViewportDimensions;

DocumentPosition position(std::string_view text, std::uint64_t byte_offset,
                          int tab_width = 4) {
    const auto resolved =
        ssg::resolve_document_position(text, ByteOffset{byte_offset}, tab_width);
    ASSERT_TRUE(resolved.has_value());
    return resolved.value_or(
        DocumentPosition{ByteOffset{0}, ssg::LineIndex{0}, CellIndex{0}});
}

Selection selection(std::string_view text, std::uint64_t anchor,
                    std::uint64_t active, int tab_width = 4) {
    return Selection{position(text, anchor, tab_width),
                     position(text, active, tab_width)};
}

SelectionViewState state(
    std::string_view text,
    std::initializer_list<std::pair<std::uint64_t, std::uint64_t>> ranges,
    std::uint32_t first_visual_row = 0, int tab_width = 4,
    std::optional<CellIndex> desired_cell = std::nullopt) {
    std::vector<Selection> selections;
    for (const auto [anchor, active] : ranges) {
        selections.push_back(selection(text, anchor, active, tab_width));
    }
    return SelectionViewState{SelectionSet{std::move(selections)},
                              first_visual_row, 0, desired_cell};
}

SelectionViewState resulting_state(const SelectionViewState& before,
                                   const SelectionNavigationResult& result) {
    ASSERT_TRUE(result.accepted());
    return result.delta.replacement.value_or(before);
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> byte_ranges(
    const SelectionViewState& view) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> result;
    for (const auto& item : view.selections.items()) {
        result.emplace_back(item.anchor.byte_offset.value(),
                            item.active.byte_offset.value());
    }
    return result;
}

void assert_matches_reference(const SelectionViewState& actual,
                              const ref::Editor& expected,
                              std::string_view context = {}) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> expected_ranges;
    for (const auto& item : expected.selections) {
        expected_ranges.emplace_back(item.anchor, item.active);
    }
    if (byte_ranges(actual) != expected_ranges && !context.empty()) {
        std::cerr << "  oracle mismatch after " << context << "\n";
        std::cerr << "    actual:";
        for (const auto& [anchor, active] : byte_ranges(actual)) {
            std::cerr << " (" << anchor << "," << active << ")";
        }
        std::cerr << "\n    expected:";
        for (const auto& [anchor, active] : expected_ranges) {
            std::cerr << " (" << anchor << "," << active << ")";
        }
        std::cerr << "\n";
    }
    ASSERT_EQ(byte_ranges(actual), expected_ranges);
}

TEST(command_set_is_exact_and_immutable) {
    static_assert(!std::is_copy_assignable_v<ssg::SelectionNavigationCommandSet>);
    constexpr std::array<std::string_view, 36> expected{{
        "cursor.set_position",
        "cursor.left",
        "cursor.right",
        "cursor.word_left",
        "cursor.word_right",
        "cursor.line_up",
        "cursor.line_down",
        "cursor.line_start",
        "cursor.line_end",
        "cursor.page_up",
        "cursor.page_down",
        "cursor.document_start",
        "cursor.document_end",
        "select.set_range",
        "select.add_range",
        "select.left",
        "select.right",
        "select.word_left",
        "select.word_right",
        "select.line_up",
        "select.line_down",
        "select.line_start",
        "select.line_end",
        "select.page_up",
        "select.page_down",
        "select.document_start",
        "select.document_end",
        "select.all",
        "select.add_next_occurrence",
        "select.add_cursor_up",
        "select.add_cursor_down",
        "select.split_into_lines",
        "select.to_matching_bracket",
        "goto.matching_bracket",
        "view.reveal_caret",
        "view.center_caret",
    }};

    const auto commands = ssg::selection_navigation_command_set();
    ASSERT_EQ(commands.descriptors().size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        ASSERT_EQ(commands.descriptors()[index].id, expected[index]);
        ASSERT_FALSE(commands.descriptors()[index].id.starts_with("text."));
        ASSERT_FALSE(commands.descriptors()[index].id.starts_with("edit."));
    }
}

TEST(selection_set_normalizes_order_duplicates_and_overlaps) {
    const std::string text = "0123456789";
    SelectionSet selections{{
        selection(text, 5, 8),
        selection(text, 2, 6),
        selection(text, 2, 6),
        selection(text, 10, 8),
        selection(text, 0, 0),
    }};

    ASSERT_EQ(byte_ranges(SelectionViewState{selections, 0, 0, std::nullopt}),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                  {0, 0}, {2, 8}, {10, 8}}));
    ASSERT_EQ(selections.primary().anchor.byte_offset, ByteOffset{10});
    ASSERT_THROWS(SelectionSet{std::vector<Selection>{}},
                  std::invalid_argument);
}

TEST(ascii_commands_match_independent_reference_editor) {
    const std::string text = "alpha beta\nxy\nalpha beta";
    auto expected = ref::make_editor(text);
    auto actual = state(text, {{text.size(), text.size()}});
    const auto dimensions = ViewportDimensions{80, 20};

    const auto run =
        [&](SelectionCommand command, const std::function<void()>& oracle,
            SelectionCommandArguments arguments = {}) {
            oracle();
            actual = resulting_state(
                actual, ssg::apply_selection_navigation(
                            text, actual, command, dimensions, arguments));
            const auto commands = ssg::selection_navigation_command_set();
            const auto descriptor = std::find_if(
                commands.descriptors().begin(), commands.descriptors().end(),
                [&](const auto& item) { return item.command == command; });
            assert_matches_reference(actual, expected, descriptor->id);
            ASSERT_EQ(actual.first_visual_row, 0u);
        };

    run(SelectionCommand::CursorSetPosition,
        [&] { ref::cursor_set_position(expected, 8); },
        {.position = position(text, 8)});
    run(SelectionCommand::CursorLeft, [&] { ref::cursor_left(expected); });
    run(SelectionCommand::CursorRight, [&] { ref::cursor_right(expected); });
    run(SelectionCommand::CursorWordLeft,
        [&] { ref::cursor_word_left(expected); });
    run(SelectionCommand::CursorWordRight,
        [&] { ref::cursor_word_right(expected); });
    run(SelectionCommand::CursorLineStart,
        [&] { ref::cursor_line_start(expected); });
    run(SelectionCommand::CursorLineEnd,
        [&] { ref::cursor_line_end(expected); });
    run(SelectionCommand::CursorLineUp,
        [&] { ref::cursor_line_up(expected); });
    run(SelectionCommand::CursorLineDown,
        [&] { ref::cursor_line_down(expected); });
    run(SelectionCommand::CursorDocumentStart,
        [&] { ref::cursor_doc_start(expected); });
    run(SelectionCommand::CursorDocumentEnd,
        [&] { ref::cursor_doc_end(expected); });

    run(SelectionCommand::SelectSetRange,
        [&] { ref::select_set_range(expected, 0, 5); },
        {.selection = selection(text, 0, 5)});
    run(SelectionCommand::SelectAddRange,
        [&] { ref::select_add_range(expected, 14, 19); },
        {.selection = selection(text, 14, 19)});
    run(SelectionCommand::SelectLeft, [&] { ref::select_left(expected); });
    run(SelectionCommand::SelectRight, [&] { ref::select_right(expected); });
    run(SelectionCommand::SelectWordLeft,
        [&] { ref::select_word_left(expected); });
    run(SelectionCommand::SelectWordRight,
        [&] { ref::select_word_right(expected); });
    run(SelectionCommand::SelectLineStart,
        [&] { ref::select_line_start(expected); });
    run(SelectionCommand::SelectLineEnd,
        [&] { ref::select_line_end(expected); });
    run(SelectionCommand::SelectLineUp,
        [&] { ref::select_line_up(expected); });
    run(SelectionCommand::SelectLineEnd,
        [&] { ref::select_line_end(expected); });
    run(SelectionCommand::SelectLineDown,
        [&] { ref::select_line_down(expected); });
    run(SelectionCommand::SelectDocumentStart,
        [&] { ref::select_doc_start(expected); });
    run(SelectionCommand::SelectDocumentEnd,
        [&] { ref::select_doc_end(expected); });
    run(SelectionCommand::SelectAll, [&] { ref::select_all(expected); });
}

TEST(horizontal_movement_uses_extended_grapheme_boundaries) {
    const std::string text =
        "A"
        "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB"
        "B e"
        "\xCC\x81"
        "Z";
    auto view = state(text, {{1, 1}});
    const auto dimensions = ViewportDimensions{80, 4};

    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::CursorRight, dimensions));
    ASSERT_EQ(view.selections.primary().active.byte_offset, ByteOffset{12});
    ASSERT_EQ(view.selections.primary().active.cell, CellIndex{3});

    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::CursorLeft, dimensions));
    ASSERT_EQ(view.selections.primary().active.byte_offset, ByteOffset{1});

    view = state(text, {{14, 14}});
    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::CursorRight, dimensions));
    ASSERT_EQ(view.selections.primary().active.byte_offset, ByteOffset{17});
}

TEST(vertical_movement_uses_cells_and_preserves_desired_cell) {
    const std::string text = "12345\n\xE4\xB8\xAD\n12345";
    auto view = state(text, {{4, 4}});
    const auto dimensions = ViewportDimensions{20, 3};

    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::CursorLineDown, dimensions));
    ASSERT_EQ(view.selections.primary().active.byte_offset, ByteOffset{9});
    ASSERT_EQ(view.selections.primary().active.cell, CellIndex{2});
    ASSERT_EQ(view.desired_cell, std::optional<CellIndex>{CellIndex{4}});

    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::CursorLineDown, dimensions));
    ASSERT_EQ(view.selections.primary().active.byte_offset, ByteOffset{14});
    ASSERT_EQ(view.selections.primary().active.cell, CellIndex{4});

    const std::string tabbed = "1234\n\tX";
    view = state(tabbed, {{4, 4}}, 0, 4);
    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  tabbed, view, SelectionCommand::CursorLineDown, dimensions,
                  {}, {}, 4));
    ASSERT_EQ(view.selections.primary().active.byte_offset, ByteOffset{6});
    ASSERT_EQ(view.selections.primary().active.cell, CellIndex{4});

    const std::string combined = "x\n" "e\xCC\x81x";
    view = state(combined, {{1, 1}});
    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  combined, view, SelectionCommand::CursorLineDown,
                  dimensions));
    ASSERT_EQ(view.selections.primary().active.byte_offset, ByteOffset{5});
    ASSERT_EQ(view.selections.primary().active.cell, CellIndex{1});
}

TEST(wrapped_vertical_and_page_movement_use_visual_rows) {
    const std::string text = "abcdef\nxy\nuvwxyz";
    const auto dimensions = ViewportDimensions{3, 2};
    auto view = state(text, {{1, 1}});

    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::CursorLineDown,
                  dimensions));
    ASSERT_EQ(view.selections.primary().active.byte_offset, ByteOffset{4});
    ASSERT_EQ(view.desired_cell, std::optional<CellIndex>{CellIndex{1}});
    ASSERT_EQ(view.first_visual_row, 0u);

    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::CursorLineDown,
                  dimensions));
    ASSERT_EQ(view.selections.primary().active.byte_offset, ByteOffset{8});
    ASSERT_EQ(view.first_visual_row, 1u);

    view = state(text, {{1, 1}});
    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::CursorPageDown,
                  dimensions));
    ASSERT_EQ(view.selections.primary().active.byte_offset, ByteOffset{8});
}

// M12 VP-H / review-fold #3: with word wrap OFF, BOTH cursor and selection
// vertical movement are by LOGICAL line, never through wrap-chunks of a long
// line. The wrapped test above (columns=3) moves within wrap rows of "abcdef";
// with word_wrap=false the same down-move jumps straight to the next logical line.
TEST(word_wrap_off_vertical_movement_is_by_logical_line) {
    const std::string text = "abcdef\nxy\nuvwxyz";  // line 0 is 6 cells wide
    const auto dimensions = ViewportDimensions{3, 4};  // narrower than line 0
    auto view = state(text, {{1, 1}});  // caret in "abcdef"

    // cursor_line_down: wrapped path would land inside "abcdef" (byte 4); no-wrap
    // jumps to the next LOGICAL line "xy", preserving desired cell 1 (byte 8).
    auto cursor = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::CursorLineDown, dimensions,
                  {}, {}, 4, /*word_wrap=*/false));
    ASSERT_EQ(cursor.selections.primary().active.line, ssg::LineIndex{1});
    ASSERT_EQ(cursor.selections.primary().active.byte_offset, ByteOffset{8});

    // select_line_down must ALSO move by logical line (the #3 fix); the wrapped
    // path would have extended into a wrap-row of "abcdef".
    auto selected = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::SelectLineDown, dimensions,
                  {}, {}, 4, /*word_wrap=*/false));
    ASSERT_EQ(selected.selections.primary().active.line, ssg::LineIndex{1});
    ASSERT_EQ(selected.selections.primary().active.byte_offset, ByteOffset{8});
    ASSERT_EQ(selected.selections.primary().anchor.byte_offset, ByteOffset{1});
}

TEST(selection_extension_keeps_anchor_and_page_uses_visible_rows) {
    const std::string text = "a0\na1\na2\na3\na4\na5\na6";
    auto view = state(text, {{1, 1}});
    const auto dimensions = ViewportDimensions{20, 3};

    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::SelectPageDown, dimensions));
    ASSERT_EQ(byte_ranges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{1, 10}}));
    ASSERT_EQ(view.first_visual_row, 1u);
    ASSERT_EQ(view.desired_cell, std::optional<CellIndex>{CellIndex{1}});

    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::SelectPageUp, dimensions));
    ASSERT_EQ(byte_ranges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{1, 1}}));
    ASSERT_EQ(view.selections.primary().anchor.byte_offset, ByteOffset{1});

    view = state(text, {{1, 1}});
    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::CursorPageDown, dimensions));
    ASSERT_EQ(byte_ranges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{10, 10}}));
    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::CursorPageUp, dimensions));
    ASSERT_EQ(byte_ranges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{1, 1}}));
}

TEST(multicursor_occurrence_and_line_splitting_match_oracles) {
    const auto dimensions = ViewportDimensions{80, 10};
    const std::string occurrences = "cat dog cat cat";
    auto view = state(occurrences, {{0, 3}});
    auto expected = ref::make_editor(occurrences, 0, 3);

    for (const auto expected_count : {2u, 3u, 3u}) {
        ref::select_add_next_occurrence(expected);
        view = resulting_state(
            view, ssg::apply_selection_navigation(
                      occurrences, view,
                      SelectionCommand::SelectAddNextOccurrence,
                      dimensions));
        ASSERT_EQ(view.selections.items().size(), expected_count);
        assert_matches_reference(view, expected);
    }
    ASSERT_EQ(byte_ranges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                  {0, 3}, {8, 11}, {12, 15}}));

    const std::string cursors = "ab\nx\nabcd";
    view = state(cursors, {{2, 2}});
    expected = ref::make_editor(cursors, 2, 2);
    ref::select_add_cursor_down(expected);
    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  cursors, view, SelectionCommand::SelectAddCursorDown,
                  dimensions));
    assert_matches_reference(view, expected);
    ref::select_add_cursor_up(expected);
    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  cursors, view, SelectionCommand::SelectAddCursorUp,
                  dimensions));
    assert_matches_reference(view, expected);

    const std::string lines = "abc\ndef\nghi";
    view = state(lines, {{1, 10}});
    expected = ref::make_editor(lines, 1, 10);
    ref::select_split_into_lines(expected);
    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  lines, view, SelectionCommand::SelectSplitIntoLines,
                  dimensions));
    assert_matches_reference(view, expected);
    ASSERT_EQ(byte_ranges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                  {1, 3}, {4, 7}, {8, 10}}));
}

TEST(injected_brackets_match_with_nesting) {
    const std::string text = "(a[()]b)";
    const std::array pairs{BracketPair{"(", ")"}, BracketPair{"[", "]"}};
    const auto dimensions = ViewportDimensions{80, 5};
    auto view = state(text, {{0, 0}});

    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::GotoMatchingBracket,
                  dimensions, {}, pairs));
    ASSERT_EQ(view.selections.primary().active.byte_offset, ByteOffset{7});

    view = state(text, {{0, 0}});
    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::SelectToMatchingBracket,
                  dimensions, {}, pairs));
    ASSERT_EQ(byte_ranges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 8}}));

    const std::string injected = "<%a<%b%>c%>";
    const std::array custom_pairs{BracketPair{"<%", "%>"}};
    view = state(injected, {{0, 0}});
    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  injected, view, SelectionCommand::GotoMatchingBracket,
                  dimensions, {}, custom_pairs));
    ASSERT_EQ(view.selections.primary().active.byte_offset, ByteOffset{9});
}

TEST(reveal_is_minimal_and_center_clamps) {
    const std::string text = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9";
    const auto dimensions = ViewportDimensions{10, 3};
    auto view = state(text, {{10, 10}}, 0);

    auto result = ssg::apply_selection_navigation(
        text, view, SelectionCommand::ViewRevealCaret, dimensions);
    view = resulting_state(view, result);
    ASSERT_EQ(view.first_visual_row, 3u);

    result = ssg::apply_selection_navigation(
        text, view, SelectionCommand::ViewRevealCaret, dimensions);
    ASSERT_TRUE(result.accepted());
    ASSERT_FALSE(result.delta.changed);
    ASSERT_FALSE(result.delta.replacement.has_value());

    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::ViewCenterCaret, dimensions));
    ASSERT_EQ(view.first_visual_row, 4u);

    view = state(text, {{18, 18}}, 0);
    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::ViewCenterCaret, dimensions));
    ASSERT_EQ(view.first_visual_row, 7u);

    view = state(text, {{4, 4}}, 0);
    view = resulting_state(
        view, ssg::apply_selection_navigation(
                  text, view, SelectionCommand::CursorLineDown, dimensions));
    ASSERT_EQ(view.first_visual_row, 1u);
}

TEST(invalid_positions_and_missing_arguments_are_typed_rejections) {
    const std::string text = "a\xE4\xB8\xAD";
    const auto dimensions = ViewportDimensions{10, 3};
    const auto view = state(text, {{0, 0}});

    auto result = ssg::apply_selection_navigation(
        text, view, SelectionCommand::CursorSetPosition, dimensions);
    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.error, ssg::SelectionNavigationError::MissingArgument);
    ASSERT_FALSE(result.delta.changed);

    const DocumentPosition inconsistent{
        ByteOffset{1}, ssg::LineIndex{9}, CellIndex{9}};
    result = ssg::apply_selection_navigation(
        text, view, SelectionCommand::CursorSetPosition, dimensions,
        {.position = inconsistent});
    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.error, ssg::SelectionNavigationError::InvalidPosition);

    ASSERT_FALSE(
        ssg::resolve_document_position(text, ByteOffset{2}).has_value());
}

TEST(invalid_tab_width_is_typed_and_atomic) {
    const std::string text = "text";
    const auto before = state(text, {{0, 0}});

    const auto result = apply_selection_navigation(
        text, before, SelectionCommand::CursorRight,
        ViewportDimensions{20, 4}, {}, {}, 17);

    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.error, SelectionNavigationError::InvalidTabWidth);
    ASSERT_FALSE(result.delta.changed);
    ASSERT_FALSE(result.delta.replacement.has_value());
    ASSERT_FALSE(resolve_document_position(text, ByteOffset{0}, 0).has_value());
}

} // namespace

int main() {
    RUN(command_set_is_exact_and_immutable);
    RUN(selection_set_normalizes_order_duplicates_and_overlaps);
    RUN(ascii_commands_match_independent_reference_editor);
    RUN(horizontal_movement_uses_extended_grapheme_boundaries);
    RUN(vertical_movement_uses_cells_and_preserves_desired_cell);
    RUN(wrapped_vertical_and_page_movement_use_visual_rows);
    RUN(word_wrap_off_vertical_movement_is_by_logical_line);
    RUN(selection_extension_keeps_anchor_and_page_uses_visible_rows);
    RUN(multicursor_occurrence_and_line_splitting_match_oracles);
    RUN(injected_brackets_match_with_nesting);
    RUN(reveal_is_minimal_and_center_clamps);
    RUN(invalid_positions_and_missing_arguments_are_typed_rejections);
    RUN(invalid_tab_width_is_typed_and_atomic);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
