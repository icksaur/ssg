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

DocumentPosition position(std::string_view text, std::uint64_t byteOffset,
                          int tabWidth = 4) {
    const auto resolved =
        ssg::SelectionNavigator::resolvePosition(text, ByteOffset{byteOffset}, tabWidth);
    ASSERT_TRUE(resolved.has_value());
    return resolved.value_or(
        DocumentPosition{ByteOffset{0}, ssg::LineIndex{0}, CellIndex{0}});
}

Selection selection(std::string_view text, std::uint64_t anchor,
                    std::uint64_t active, int tabWidth = 4) {
    return Selection{position(text, anchor, tabWidth),
                     position(text, active, tabWidth)};
}

SelectionViewState state(
    std::string_view text,
    std::initializer_list<std::pair<std::uint64_t, std::uint64_t>> ranges,
    std::uint32_t firstVisualRow = 0, int tabWidth = 4,
    std::optional<CellIndex> desiredCell = std::nullopt) {
    std::vector<Selection> selections;
    for (const auto [anchor, active] : ranges) {
        selections.push_back(selection(text, anchor, active, tabWidth));
    }
    return SelectionViewState{SelectionSet{std::move(selections)},
                              firstVisualRow, 0, desiredCell};
}

SelectionViewState resultingState(const SelectionViewState& before,
                                   const SelectionNavigationResult& result) {
    ASSERT_TRUE(result.accepted());
    return result.delta.replacement.value_or(before);
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> byteRanges(
    const SelectionViewState& view) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> result;
    for (const auto& item : view.selections.items()) {
        result.emplace_back(item.anchor.byteOffset.value(),
                            item.active.byteOffset.value());
    }
    return result;
}

void assertMatchesReference(const SelectionViewState& actual,
                              const ref::Editor& expected,
                              std::string_view context = {}) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> expectedRanges;
    for (const auto& item : expected.selections) {
        expectedRanges.emplace_back(item.anchor, item.active);
    }
    if (byteRanges(actual) != expectedRanges && !context.empty()) {
        std::cerr << "  oracle mismatch after " << context << "\n";
        std::cerr << "    actual:";
        for (const auto& [anchor, active] : byteRanges(actual)) {
            std::cerr << " (" << anchor << "," << active << ")";
        }
        std::cerr << "\n    expected:";
        for (const auto& [anchor, active] : expectedRanges) {
            std::cerr << " (" << anchor << "," << active << ")";
        }
        std::cerr << "\n";
    }
    ASSERT_EQ(byteRanges(actual), expectedRanges);
}

TEST(commandSetIsExactAndImmutable) {
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

    const auto commands = ssg::selectionNavigationCommandSet();
    ASSERT_EQ(commands.descriptors().size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        ASSERT_EQ(commands.descriptors()[index].id, expected[index]);
        ASSERT_FALSE(commands.descriptors()[index].id.starts_with("text."));
        ASSERT_FALSE(commands.descriptors()[index].id.starts_with("edit."));
    }
}

TEST(selectionSetNormalizesOrderDuplicatesAndOverlaps) {
    const std::string text = "0123456789";
    SelectionSet selections{{
        selection(text, 5, 8),
        selection(text, 2, 6),
        selection(text, 2, 6),
        selection(text, 10, 8),
        selection(text, 0, 0),
    }};

    ASSERT_EQ(byteRanges(SelectionViewState{selections, 0, 0, std::nullopt}),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                  {0, 0}, {2, 8}, {10, 8}}));
    ASSERT_EQ(selections.primary().anchor.byteOffset, ByteOffset{10});
    ASSERT_THROWS(SelectionSet{std::vector<Selection>{}},
                  std::invalid_argument);
}

TEST(asciiCommandsMatchIndependentReferenceEditor) {
    const std::string text = "alpha beta\nxy\nalpha beta";
    auto expected = ref::make_editor(text);
    auto actual = state(text, {{text.size(), text.size()}});
    const auto dimensions = ViewportDimensions{80, 20};

    const auto run =
        [&](SelectionCommand command, const std::function<void()>& oracle,
            SelectionCommandArguments arguments = {}) {
            oracle();
            actual = resultingState(
                actual, ssg::SelectionNavigator{}.apply(
                            text, actual, command, dimensions, arguments));
            const auto commands = ssg::selectionNavigationCommandSet();
            const auto descriptor = std::find_if(
                commands.descriptors().begin(), commands.descriptors().end(),
                [&](const auto& item) { return item.command == command; });
            assertMatchesReference(actual, expected, descriptor->id);
            ASSERT_EQ(actual.firstVisualRow, 0u);
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

TEST(horizontalMovementUsesExtendedGraphemeBoundaries) {
    const std::string text =
        "A"
        "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB"
        "B e"
        "\xCC\x81"
        "Z";
    auto view = state(text, {{1, 1}});
    const auto dimensions = ViewportDimensions{80, 4};

    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::CursorRight, dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{12});
    ASSERT_EQ(view.selections.primary().active.cell, CellIndex{3});

    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::CursorLeft, dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{1});

    view = state(text, {{14, 14}});
    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::CursorRight, dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{17});
}

TEST(verticalMovementUsesCellsAndPreservesDesiredCell) {
    const std::string text = "12345\n\xE4\xB8\xAD\n12345";
    auto view = state(text, {{4, 4}});
    const auto dimensions = ViewportDimensions{20, 3};

    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::CursorLineDown, dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{9});
    ASSERT_EQ(view.selections.primary().active.cell, CellIndex{2});
    ASSERT_EQ(view.desiredCell, std::optional<CellIndex>{CellIndex{4}});

    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::CursorLineDown, dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{14});
    ASSERT_EQ(view.selections.primary().active.cell, CellIndex{4});

    const std::string tabbed = "1234\n\tX";
    view = state(tabbed, {{4, 4}}, 0, 4);
    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  tabbed, view, SelectionCommand::CursorLineDown, dimensions,
                  {}, {}, 4));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{6});
    ASSERT_EQ(view.selections.primary().active.cell, CellIndex{4});

    const std::string combined = "x\n" "e\xCC\x81x";
    view = state(combined, {{1, 1}});
    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  combined, view, SelectionCommand::CursorLineDown,
                  dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{5});
    ASSERT_EQ(view.selections.primary().active.cell, CellIndex{1});
}

TEST(wrappedVerticalAndPageMovementUseVisualRows) {
    const std::string text = "abcdef\nxy\nuvwxyz";
    const auto dimensions = ViewportDimensions{3, 2};
    auto view = state(text, {{1, 1}});

    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::CursorLineDown,
                  dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{4});
    ASSERT_EQ(view.desiredCell, std::optional<CellIndex>{CellIndex{1}});
    ASSERT_EQ(view.firstVisualRow, 0u);

    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::CursorLineDown,
                  dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{8});
    ASSERT_EQ(view.firstVisualRow, 1u);

    view = state(text, {{1, 1}});
    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::CursorPageDown,
                  dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{8});
}

// M12 VP-H / review-fold #3: with word wrap OFF, BOTH cursor and selection
// vertical movement are by LOGICAL line, never through wrap-chunks of a long
// line. The wrapped test above (columns=3) moves within wrap rows of "abcdef";
// with word_wrap=false the same down-move jumps straight to the next logical line.
TEST(wordWrapOffVerticalMovementIsByLogicalLine) {
    const std::string text = "abcdef\nxy\nuvwxyz";  // line 0 is 6 cells wide
    const auto dimensions = ViewportDimensions{3, 4};  // narrower than line 0
    auto view = state(text, {{1, 1}});  // caret in "abcdef"

    // cursor_line_down: wrapped path would land inside "abcdef" (byte 4); no-wrap
    // jumps to the next LOGICAL line "xy", preserving desired cell 1 (byte 8).
    auto cursor = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::CursorLineDown, dimensions,
                  {}, {}, 4, /*word_wrap=*/false));
    ASSERT_EQ(cursor.selections.primary().active.line, ssg::LineIndex{1});
    ASSERT_EQ(cursor.selections.primary().active.byteOffset, ByteOffset{8});

    // select_line_down must ALSO move by logical line (the #3 fix); the wrapped
    // path would have extended into a wrap-row of "abcdef".
    auto selected = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::SelectLineDown, dimensions,
                  {}, {}, 4, /*word_wrap=*/false));
    ASSERT_EQ(selected.selections.primary().active.line, ssg::LineIndex{1});
    ASSERT_EQ(selected.selections.primary().active.byteOffset, ByteOffset{8});
    ASSERT_EQ(selected.selections.primary().anchor.byteOffset, ByteOffset{1});
}

TEST(selectionExtensionKeepsAnchorAndPageUsesVisibleRows) {
    const std::string text = "a0\na1\na2\na3\na4\na5\na6";
    auto view = state(text, {{1, 1}});
    const auto dimensions = ViewportDimensions{20, 3};

    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::SelectPageDown, dimensions));
    ASSERT_EQ(byteRanges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{1, 10}}));
    ASSERT_EQ(view.firstVisualRow, 1u);
    ASSERT_EQ(view.desiredCell, std::optional<CellIndex>{CellIndex{1}});

    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::SelectPageUp, dimensions));
    ASSERT_EQ(byteRanges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{1, 1}}));
    ASSERT_EQ(view.selections.primary().anchor.byteOffset, ByteOffset{1});

    view = state(text, {{1, 1}});
    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::CursorPageDown, dimensions));
    ASSERT_EQ(byteRanges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{10, 10}}));
    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::CursorPageUp, dimensions));
    ASSERT_EQ(byteRanges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{1, 1}}));
}

TEST(multicursorOccurrenceAndLineSplittingMatchOracles) {
    const auto dimensions = ViewportDimensions{80, 10};
    const std::string occurrences = "cat dog cat cat";
    auto view = state(occurrences, {{0, 3}});
    auto expected = ref::make_editor(occurrences, 0, 3);

    for (const auto expectedCount : {2u, 3u, 3u}) {
        ref::select_add_next_occurrence(expected);
        view = resultingState(
            view, ssg::SelectionNavigator{}.apply(
                      occurrences, view,
                      SelectionCommand::SelectAddNextOccurrence,
                      dimensions));
        ASSERT_EQ(view.selections.items().size(), expectedCount);
        assertMatchesReference(view, expected);
    }
    ASSERT_EQ(byteRanges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                  {0, 3}, {8, 11}, {12, 15}}));

    const std::string cursors = "ab\nx\nabcd";
    view = state(cursors, {{2, 2}});
    expected = ref::make_editor(cursors, 2, 2);
    ref::select_add_cursor_down(expected);
    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  cursors, view, SelectionCommand::SelectAddCursorDown,
                  dimensions));
    assertMatchesReference(view, expected);
    ref::select_add_cursor_up(expected);
    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  cursors, view, SelectionCommand::SelectAddCursorUp,
                  dimensions));
    assertMatchesReference(view, expected);

    const std::string lines = "abc\ndef\nghi";
    view = state(lines, {{1, 10}});
    expected = ref::make_editor(lines, 1, 10);
    ref::select_split_into_lines(expected);
    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  lines, view, SelectionCommand::SelectSplitIntoLines,
                  dimensions));
    assertMatchesReference(view, expected);
    ASSERT_EQ(byteRanges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                  {1, 3}, {4, 7}, {8, 10}}));
}

TEST(injectedBracketsMatchWithNesting) {
    const std::string text = "(a[()]b)";
    const std::array pairs{BracketPair{"(", ")"}, BracketPair{"[", "]"}};
    const auto dimensions = ViewportDimensions{80, 5};
    auto view = state(text, {{0, 0}});

    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::GotoMatchingBracket,
                  dimensions, {}, pairs));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{7});

    view = state(text, {{0, 0}});
    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::SelectToMatchingBracket,
                  dimensions, {}, pairs));
    ASSERT_EQ(byteRanges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 8}}));

    const std::string injected = "<%a<%b%>c%>";
    const std::array customPairs{BracketPair{"<%", "%>"}};
    view = state(injected, {{0, 0}});
    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  injected, view, SelectionCommand::GotoMatchingBracket,
                  dimensions, {}, customPairs));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{9});
}

TEST(revealIsMinimalAndCenterClamps) {
    const std::string text = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9";
    const auto dimensions = ViewportDimensions{10, 3};
    auto view = state(text, {{10, 10}}, 0);

    auto result = ssg::SelectionNavigator{}.apply(
        text, view, SelectionCommand::ViewRevealCaret, dimensions);
    view = resultingState(view, result);
    ASSERT_EQ(view.firstVisualRow, 3u);

    result = ssg::SelectionNavigator{}.apply(
        text, view, SelectionCommand::ViewRevealCaret, dimensions);
    ASSERT_TRUE(result.accepted());
    ASSERT_FALSE(result.delta.changed);
    ASSERT_FALSE(result.delta.replacement.has_value());

    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::ViewCenterCaret, dimensions));
    ASSERT_EQ(view.firstVisualRow, 4u);

    view = state(text, {{18, 18}}, 0);
    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::ViewCenterCaret, dimensions));
    ASSERT_EQ(view.firstVisualRow, 7u);

    view = state(text, {{4, 4}}, 0);
    view = resultingState(
        view, ssg::SelectionNavigator{}.apply(
                  text, view, SelectionCommand::CursorLineDown, dimensions));
    ASSERT_EQ(view.firstVisualRow, 1u);
}

TEST(invalidPositionsAndMissingArgumentsAreTypedRejections) {
    const std::string text = "a\xE4\xB8\xAD";
    const auto dimensions = ViewportDimensions{10, 3};
    const auto view = state(text, {{0, 0}});

    auto result = ssg::SelectionNavigator{}.apply(
        text, view, SelectionCommand::CursorSetPosition, dimensions);
    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.error, ssg::SelectionNavigationError::MissingArgument);
    ASSERT_FALSE(result.delta.changed);

    const DocumentPosition inconsistent{
        ByteOffset{1}, ssg::LineIndex{9}, CellIndex{9}};
    result = ssg::SelectionNavigator{}.apply(
        text, view, SelectionCommand::CursorSetPosition, dimensions,
        {.position = inconsistent});
    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.error, ssg::SelectionNavigationError::InvalidPosition);

    ASSERT_FALSE(
        ssg::SelectionNavigator::resolvePosition(text, ByteOffset{2}).has_value());
}

TEST(invalidTabWidthIsTypedAndAtomic) {
    const std::string text = "text";
    const auto before = state(text, {{0, 0}});

    const auto result = ssg::SelectionNavigator{}.apply(
        text, before, SelectionCommand::CursorRight,
        ViewportDimensions{20, 4}, {}, {}, 17);

    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.error, SelectionNavigationError::InvalidTabWidth);
    ASSERT_FALSE(result.delta.changed);
    ASSERT_FALSE(result.delta.replacement.has_value());
    ASSERT_FALSE(ssg::SelectionNavigator::resolvePosition(text, ByteOffset{0}, 0).has_value());
}

} // namespace

int main() {
    RUN(commandSetIsExactAndImmutable);
    RUN(selectionSetNormalizesOrderDuplicatesAndOverlaps);
    RUN(asciiCommandsMatchIndependentReferenceEditor);
    RUN(horizontalMovementUsesExtendedGraphemeBoundaries);
    RUN(verticalMovementUsesCellsAndPreservesDesiredCell);
    RUN(wrappedVerticalAndPageMovementUseVisualRows);
    RUN(wordWrapOffVerticalMovementIsByLogicalLine);
    RUN(selectionExtensionKeepsAnchorAndPageUsesVisibleRows);
    RUN(multicursorOccurrenceAndLineSplittingMatchOracles);
    RUN(injectedBracketsMatchWithNesting);
    RUN(revealIsMinimalAndCenterClamps);
    RUN(invalidPositionsAndMissingArgumentsAreTypedRejections);
    RUN(invalidTabWidthIsTypedAndAtomic);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
