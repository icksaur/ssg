#include <ssg/Selection.h>
#include <ssg/DiffModel.h>

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
        ssg::resolveSelectionPosition(text, ByteOffset{byteOffset}, tabWidth);
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
                actual, ssg::navigateSelection(
                            text, actual, command, dimensions, arguments));
            const auto descriptor = std::find_if(
                ssg::kSelectionCommands.begin(), ssg::kSelectionCommands.end(),
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
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::CursorRight, dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{12});
    ASSERT_EQ(view.selections.primary().active.cell, CellIndex{3});

    view = resultingState(
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::CursorLeft, dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{1});

    view = state(text, {{14, 14}});
    view = resultingState(
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::CursorRight, dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{17});
}

TEST(verticalMovementUsesCellsAndPreservesDesiredCell) {
    const std::string text = "12345\n\xE4\xB8\xAD\n12345";
    auto view = state(text, {{4, 4}});
    const auto dimensions = ViewportDimensions{20, 3};

    view = resultingState(
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::CursorLineDown, dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{9});
    ASSERT_EQ(view.selections.primary().active.cell, CellIndex{2});
    ASSERT_EQ(view.desiredCell, std::optional<CellIndex>{CellIndex{4}});

    view = resultingState(
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::CursorLineDown, dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{14});
    ASSERT_EQ(view.selections.primary().active.cell, CellIndex{4});

    const std::string tabbed = "1234\n\tX";
    view = state(tabbed, {{4, 4}}, 0, 4);
    view = resultingState(
        view, ssg::navigateSelection(
                  tabbed, view, SelectionCommand::CursorLineDown, dimensions,
                  {}, {}, 4));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{6});
    ASSERT_EQ(view.selections.primary().active.cell, CellIndex{4});

    const std::string combined = "x\n" "e\xCC\x81x";
    view = state(combined, {{1, 1}});
    view = resultingState(
        view, ssg::navigateSelection(
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
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::CursorLineDown,
                  dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{4});
    ASSERT_EQ(view.desiredCell, std::optional<CellIndex>{CellIndex{1}});
    ASSERT_EQ(view.firstVisualRow, 0u);

    view = resultingState(
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::CursorLineDown,
                  dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{8});
    ASSERT_EQ(view.firstVisualRow, 1u);

    view = state(text, {{1, 1}});
    view = resultingState(
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::CursorPageDown,
                  dimensions));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{8});
}

TEST(phantomRowsAreSkippedByCaretSelectionAndCountedByReveal) {
    const std::string text = "one\ntwo\nthree";
    ssg::DiffFileView diff{ssg::DiffFileId{"doc.txt"}};
    diff.currentContent = text;
    diff.hunks.push_back({.baselineStart = 1,
                          .targetStart = 1,
                          .baselineLines = {"removed\n"},
                          .targetLines = {}});
    const auto dimensions = ViewportDimensions{20, 2};

    auto caret = state(text, {{1, 1}});
    caret = resultingState(
        caret, ssg::navigateSelection(
                   text, caret, SelectionCommand::CursorLineDown, dimensions,
                   {}, {}, 4, true, &diff));
    ASSERT_EQ(caret.selections.primary().active.byteOffset, ByteOffset{5});
    ASSERT_EQ(caret.firstVisualRow, std::uint32_t{1});

    auto paged = state(text, {{1, 1}});
    paged = resultingState(
        paged, ssg::navigateSelection(
                   text, paged, SelectionCommand::CursorPageDown, dimensions,
                   {}, {}, 4, true, &diff));
    ASSERT_EQ(paged.selections.primary().active.byteOffset, ByteOffset{5});

    auto selected = state(text, {{1, 1}});
    selected = resultingState(
        selected, ssg::navigateSelection(
                      text, selected, SelectionCommand::SelectLineDown,
                      dimensions, {}, {}, 4, true, &diff));
    ASSERT_EQ(byteRanges(selected),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{1, 5}}));

    auto centered = resultingState(
        caret, ssg::navigateSelection(
                   text, caret, SelectionCommand::ViewCenterCaret,
                   ViewportDimensions{20, 3}, {}, {}, 4, true, &diff));
    ASSERT_EQ(centered.firstVisualRow, std::uint32_t{1});
}

TEST(mergedInlineModifiedRowNavigatesByRealBytesUnaffectedByGhostSpans) {
    // "gamma modified line two" is the REAL document text on line 1; its
    // diff pairs it with baseline "gamma original line two" via inline
    // segments (Unchanged/Removed/Separator/Added/Unchanged), rendered as
    // ONE merged row with the ghost "original " text visually spliced in.
    // Caret navigation must be entirely unaffected by that ghost text: it
    // does not exist in the document, so CursorLeft/CursorRight/vertical
    // movement need no special ghost-span handling.
    const std::string text = "one\ngamma modified line two\nthree";
    ssg::DiffFileView diff{ssg::DiffFileId{"doc.txt"}};
    diff.currentContent = text;
    diff.hunks.push_back({.baselineStart = 1,
                          .targetStart = 1,
                          .baselineLines = {"gamma original line two\n"},
                          .targetLines = {"gamma modified line two\n"}});
    diff.changedLines.push_back(
        {.kind = ssg::DiffLineKind::Modified,
         .baselineLine = std::size_t{1},
         .targetLine = std::size_t{1},
         .inlineWordSegments = {
             {ssg::InlineWordSegment::Kind::Unchanged, "gamma "},
             {ssg::InlineWordSegment::Kind::Removed, "original"},
             {ssg::InlineWordSegment::Kind::Separator, " "},
             {ssg::InlineWordSegment::Kind::Added, "modified"},
             {ssg::InlineWordSegment::Kind::Unchanged, " line two\n"},
         }});
    const auto dimensions = ViewportDimensions{40, 3};

    // Caret at the end of line 0 ("one", byte 3, cell 3); moving down lands
    // on the merged row at the SAME real cell 3 ('m' in "gamma"), byte 7 --
    // desiredCell is real-cell-space throughout, so the ghost-widened
    // merged row needs no translation.
    auto view = state(text, {{3, 3}});
    view = resultingState(
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::CursorLineDown, dimensions,
                  {}, {}, 4, true, &diff));
    ASSERT_EQ(view.selections.primary().active.line, ssg::LineIndex{1});
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{7});

    // CursorRight steps through the REAL text one grapheme at a time (there
    // is no ghost byte to skip over -- "original" only ever exists in
    // Viewport's display text, never in this document).
    for (int i = 0; i < 4; ++i) {
        view = resultingState(
            view, ssg::navigateSelection(
                      text, view, SelectionCommand::CursorRight, dimensions,
                      {}, {}, 4, true, &diff));
    }
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{11});

    // Continuing down lands on line 2 ("three"): the merged row's real line
    // count/byte range are unaffected by the ghost text it visually adds.
    view = resultingState(
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::CursorLineDown, dimensions,
                  {}, {}, 4, true, &diff));
    ASSERT_EQ(view.selections.primary().active.line, ssg::LineIndex{2});
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
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::CursorLineDown, dimensions,
                  {}, {}, 4, /*word_wrap=*/false));
    ASSERT_EQ(cursor.selections.primary().active.line, ssg::LineIndex{1});
    ASSERT_EQ(cursor.selections.primary().active.byteOffset, ByteOffset{8});

    // select_line_down must ALSO move by logical line (the #3 fix); the wrapped
    // path would have extended into a wrap-row of "abcdef".
    auto selected = resultingState(
        view, ssg::navigateSelection(
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
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::SelectPageDown, dimensions));
    ASSERT_EQ(byteRanges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{1, 10}}));
    ASSERT_EQ(view.firstVisualRow, 1u);
    ASSERT_EQ(view.desiredCell, std::optional<CellIndex>{CellIndex{1}});

    view = resultingState(
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::SelectPageUp, dimensions));
    ASSERT_EQ(byteRanges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{1, 1}}));
    ASSERT_EQ(view.selections.primary().anchor.byteOffset, ByteOffset{1});

    view = state(text, {{1, 1}});
    view = resultingState(
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::CursorPageDown, dimensions));
    ASSERT_EQ(byteRanges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{10, 10}}));
    view = resultingState(
        view, ssg::navigateSelection(
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
            view, ssg::navigateSelection(
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
        view, ssg::navigateSelection(
                  cursors, view, SelectionCommand::SelectAddCursorDown,
                  dimensions));
    assertMatchesReference(view, expected);
    ref::select_add_cursor_up(expected);
    view = resultingState(
        view, ssg::navigateSelection(
                  cursors, view, SelectionCommand::SelectAddCursorUp,
                  dimensions));
    assertMatchesReference(view, expected);

    const std::string lines = "abc\ndef\nghi";
    view = state(lines, {{1, 10}});
    expected = ref::make_editor(lines, 1, 10);
    ref::select_split_into_lines(expected);
    view = resultingState(
        view, ssg::navigateSelection(
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
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::GotoMatchingBracket,
                  dimensions, {}, pairs));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{7});

    view = state(text, {{0, 0}});
    view = resultingState(
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::SelectToMatchingBracket,
                  dimensions, {}, pairs));
    ASSERT_EQ(byteRanges(view),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 8}}));

    const std::string injected = "<%a<%b%>c%>";
    const std::array customPairs{BracketPair{"<%", "%>"}};
    view = state(injected, {{0, 0}});
    view = resultingState(
        view, ssg::navigateSelection(
                  injected, view, SelectionCommand::GotoMatchingBracket,
                  dimensions, {}, customPairs));
    ASSERT_EQ(view.selections.primary().active.byteOffset, ByteOffset{9});
}

TEST(revealIsMinimalAndCenterClamps) {
    const std::string text = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9";
    const auto dimensions = ViewportDimensions{10, 3};
    auto view = state(text, {{10, 10}}, 0);

    auto result = ssg::navigateSelection(
        text, view, SelectionCommand::ViewRevealCaret, dimensions);
    view = resultingState(view, result);
    ASSERT_EQ(view.firstVisualRow, 3u);

    result = ssg::navigateSelection(
        text, view, SelectionCommand::ViewRevealCaret, dimensions);
    ASSERT_TRUE(result.accepted());
    ASSERT_FALSE(result.delta.changed);
    ASSERT_FALSE(result.delta.replacement.has_value());

    view = resultingState(
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::ViewCenterCaret, dimensions));
    ASSERT_EQ(view.firstVisualRow, 4u);

    view = state(text, {{18, 18}}, 0);
    view = resultingState(
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::ViewCenterCaret, dimensions));
    ASSERT_EQ(view.firstVisualRow, 7u);

    view = state(text, {{4, 4}}, 0);
    view = resultingState(
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::CursorLineDown, dimensions));
    ASSERT_EQ(view.firstVisualRow, 1u);
}

TEST(invalidPositionsAndMissingArgumentsAreTypedRejections) {
    const std::string text = "a\xE4\xB8\xAD";
    const auto dimensions = ViewportDimensions{10, 3};
    const auto view = state(text, {{0, 0}});

    auto result = ssg::navigateSelection(
        text, view, SelectionCommand::CursorSetPosition, dimensions);
    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.error, ssg::SelectionNavigationError::MissingArgument);
    ASSERT_FALSE(result.delta.changed);

    const DocumentPosition inconsistent{
        ByteOffset{1}, ssg::LineIndex{9}, CellIndex{9}};
    result = ssg::navigateSelection(
        text, view, SelectionCommand::CursorSetPosition, dimensions,
        {.position = inconsistent});
    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.error, ssg::SelectionNavigationError::InvalidPosition);

    ASSERT_FALSE(
        ssg::resolveSelectionPosition(text, ByteOffset{2}).has_value());
}

TEST(invalidTabWidthIsTypedAndAtomic) {
    const std::string text = "text";
    const auto before = state(text, {{0, 0}});

    const auto result = ssg::navigateSelection(
        text, before, SelectionCommand::CursorRight,
        ViewportDimensions{20, 4}, {}, {}, 17);

    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.error, SelectionNavigationError::InvalidTabWidth);
    ASSERT_FALSE(result.delta.changed);
    ASSERT_FALSE(result.delta.replacement.has_value());
    ASSERT_FALSE(ssg::resolveSelectionPosition(text, ByteOffset{0}, 0).has_value());
}

TEST(selectSetRangesReplacesTheSetAndNormalizes) {
    const std::string text = "alpha beta\nxy\nalpha beta";
    const auto dimensions = ViewportDimensions{80, 20};
    using Ranges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

    // An empty list is a typed rejection, mirroring select.set_range's
    // missing-selection guard.
    auto empty = ssg::navigateSelection(
        text, state(text, {{0, 0}}), SelectionCommand::SelectSetRanges,
        dimensions, SelectionCommandArguments{});
    ASSERT_FALSE(empty.accepted());
    ASSERT_EQ(empty.error, ssg::SelectionNavigationError::MissingArgument);

    // An out-of-layout position is rejected like select.set_range.
    SelectionCommandArguments invalid;
    invalid.selections = {Selection{
        DocumentPosition{ByteOffset{1}, ssg::LineIndex{9}, CellIndex{9}},
        DocumentPosition{ByteOffset{1}, ssg::LineIndex{9}, CellIndex{9}}}};
    auto invalidResult = ssg::navigateSelection(
        text, state(text, {{0, 0}}), SelectionCommand::SelectSetRanges,
        dimensions, invalid);
    ASSERT_FALSE(invalidResult.accepted());
    ASSERT_EQ(invalidResult.error, ssg::SelectionNavigationError::InvalidPosition);

    // Two disjoint baseline carets plus a dragged range (supplied out of order)
    // replace the whole set and normalize: sorted by lower offset, three
    // selections because none overlaps.
    auto view = state(text, {{text.size(), text.size()}});
    SelectionCommandArguments disjoint;
    disjoint.selections = {selection(text, 8, 4), selection(text, 2, 2),
                           selection(text, 20, 20)};
    view = resultingState(
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::SelectSetRanges, dimensions,
                  disjoint));
    ASSERT_EQ(byteRanges(view), (Ranges{{2, 2}, {8, 4}, {20, 20}}));

    // Rebuilding from the SAME baseline with the dragged range pushed upward so
    // it now overlaps the LOWER baseline caret merges those two, while the upper
    // baseline caret is preserved untouched: the full-recompute has no stale
    // target to corrupt (proves the reorder/overlap contract).
    SelectionCommandArguments merged;
    merged.selections = {selection(text, 2, 2), selection(text, 20, 20),
                         selection(text, 1, 8)};
    auto overlapView = resultingState(
        view, ssg::navigateSelection(
                  text, view, SelectionCommand::SelectSetRanges, dimensions,
                  merged));
    ASSERT_EQ(byteRanges(overlapView), (Ranges{{1, 8}, {20, 20}}));
}

} // namespace

TEST(selectWordAtPositionSelectsTheSameCategoryRun) {
    const auto dims = ViewportDimensions{80, 20};
    auto wordAt = [&](std::string_view text, std::uint64_t offset) {
        auto const before = state(std::string{text}, {{0, 0}});
        auto const result = ssg::navigateSelection(
            text, before, SelectionCommand::SelectWordAtPosition, dims,
            {.position = position(text, offset)});
        ASSERT_TRUE(result.accepted());
        return byteRanges(resultingState(before, result));
    };
    using Ranges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

    // "alpha beta": word [0,5), space [5,6), word [6,10).
    const std::string t = "alpha beta";
    ASSERT_EQ(wordAt(t, 2), (Ranges{{0, 5}}));   // mid-word
    ASSERT_EQ(wordAt(t, 0), (Ranges{{0, 5}}));   // first byte of the word
    ASSERT_EQ(wordAt(t, 4), (Ranges{{0, 5}}));   // last byte of the word
    ASSERT_EQ(wordAt(t, 5), (Ranges{{5, 6}}));   // the space run
    ASSERT_EQ(wordAt(t, 6), (Ranges{{6, 10}}));  // the second word
    ASSERT_EQ(wordAt(t, 10), (Ranges{{10, 10}}));  // document end -> empty

    // Punctuation run: "a==b" -> word [0,1), punct [1,3), word [3,4).
    const std::string p = "a==b";
    ASSERT_EQ(wordAt(p, 1), (Ranges{{1, 3}}));
    ASSERT_EQ(wordAt(p, 2), (Ranges{{1, 3}}));

    // A word never crosses a newline (newline is Space): "ab\ncd".
    const std::string nl = "ab\ncd";
    ASSERT_EQ(wordAt(nl, 0), (Ranges{{0, 2}}));  // stops before '\n'
    ASSERT_EQ(wordAt(nl, 2), (Ranges{{2, 3}}));  // the newline is its own space run
    ASSERT_EQ(wordAt(nl, 3), (Ranges{{3, 5}}));  // next line's word

    // A multi-byte UTF-8 word selects its whole byte range: "he" + U+00E9 ("é",
    // 2 bytes) -> all word bytes, [0,4).
    const std::string utf = "he\xC3\xA9";
    ASSERT_EQ(wordAt(utf, 0), (Ranges{{0, 4}}));
    ASSERT_EQ(wordAt(utf, 2), (Ranges{{0, 4}}));  // first byte of 'é'
}

TEST(selectWordAtPositionRejectsAMissingOrInvalidPosition) {
    const std::string text = "word";
    const auto before = state(text, {{0, 0}});
    const auto dims = ViewportDimensions{20, 4};

    auto const missing = ssg::navigateSelection(
        text, before, SelectionCommand::SelectWordAtPosition, dims, {});
    ASSERT_FALSE(missing.accepted());
    ASSERT_EQ(missing.error, SelectionNavigationError::MissingArgument);

    // A position that does not match the layout is rejected, selection intact.
    DocumentPosition bogus{ByteOffset{999}, ssg::LineIndex{9}, CellIndex{999}};
    auto const invalid = ssg::navigateSelection(
        text, before, SelectionCommand::SelectWordAtPosition, dims,
        {.position = bogus});
    ASSERT_FALSE(invalid.accepted());
    ASSERT_EQ(invalid.error, SelectionNavigationError::InvalidPosition);
    ASSERT_FALSE(invalid.delta.changed);
}

TEST(wordOrCoveredTextReturnsCaretWordOrSelectionSubstring) {
    const std::string text = "foo bar_baz  qux";
    auto pos = [](std::uint64_t off) {
        return DocumentPosition{ByteOffset{off}, ssg::LineIndex{0}, CellIndex{0}};
    };
    auto caret = [&](std::uint64_t off) { return Selection{pos(off), pos(off)}; };

    // Caret inside a word -> the whole word ('_' is a word byte).
    ASSERT_EQ(caret(5).wordOrCoveredText(text), std::string{"bar_baz"});
    // Caret at a word's trailing boundary -> that word (the after-word branch).
    ASSERT_EQ(caret(3).wordOrCoveredText(text), std::string{"foo"});
    // Caret surrounded by non-word bytes -> empty.
    ASSERT_EQ(caret(12).wordOrCoveredText(text), std::string{});
    // A range selection -> exactly the covered substring, verbatim.
    ASSERT_EQ((Selection{pos(0), pos(3)}).wordOrCoveredText(text),
              std::string{"foo"});
    // A range spanning a space is returned verbatim, NOT word-trimmed.
    ASSERT_EQ((Selection{pos(0), pos(7)}).wordOrCoveredText(text),
              std::string{"foo bar"});
}

SSG_TEST_SUITE(test_selection) {
    RUN(selectionSetNormalizesOrderDuplicatesAndOverlaps);
    RUN(asciiCommandsMatchIndependentReferenceEditor);
    RUN(horizontalMovementUsesExtendedGraphemeBoundaries);
    RUN(verticalMovementUsesCellsAndPreservesDesiredCell);
    RUN(wrappedVerticalAndPageMovementUseVisualRows);
    RUN(phantomRowsAreSkippedByCaretSelectionAndCountedByReveal);
    RUN(wordWrapOffVerticalMovementIsByLogicalLine);
    RUN(selectionExtensionKeepsAnchorAndPageUsesVisibleRows);
    RUN(multicursorOccurrenceAndLineSplittingMatchOracles);
    RUN(injectedBracketsMatchWithNesting);
    RUN(revealIsMinimalAndCenterClamps);
    RUN(invalidPositionsAndMissingArgumentsAreTypedRejections);
    RUN(selectSetRangesReplacesTheSetAndNormalizes);
    RUN(invalidTabWidthIsTypedAndAtomic);
    RUN(selectWordAtPositionSelectsTheSameCategoryRun);
    RUN(selectWordAtPositionRejectsAMissingOrInvalidPosition);
    RUN(wordOrCoveredTextReturnsCaretWordOrSelectionSubstring);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
