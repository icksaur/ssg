#include <tui/WidgetLayout.h>
#include "test_helpers.h"

#include <string>
#include <vector>

namespace {

using namespace ssg;

FitItem item(std::string id, int desired, int rank = 0) {
    return FitItem{std::move(id), desired, rank};
}

// Start alignment packs from offset 0 with a single-cell separator between
// adjacent items, matching addFields' left-to-right placement. Hand-computed:
// A at 0 (size 2), one separator cell at 2, B at 3 (size 3).
TEST(fitRowStartPacksLeftWithSeparators) {
    const RowFit fit = fitRow({item("a", 2), item("b", 3)}, 10, 1, Align::Start);
    ASSERT_EQ(fit.placed.size(), std::size_t{2});
    ASSERT_EQ(fit.placed[0], (PlacedItem{"a", 0, 2, 0}));
    ASSERT_EQ(fit.placed[1], (PlacedItem{"b", 3, 3, 1}));
}

// End alignment packs the whole retained run flush to the trailing edge. Total
// used = 2 + 1(sep) + 3 = 6; start = extent(10) - 6 = 4. A at 4, B at 7.
TEST(fitRowEndPacksFlushRight) {
    const RowFit fit = fitRow({item("a", 2), item("b", 3)}, 10, 1, Align::End);
    ASSERT_EQ(fit.placed.size(), std::size_t{2});
    ASSERT_EQ(fit.placed[0], (PlacedItem{"a", 4, 2, 0}));
    ASSERT_EQ(fit.placed[1], (PlacedItem{"b", 7, 3, 1}));
}

// THE discriminating edge (spec Step 1 oracle): stop-at-first-non-fit is NOT
// drop-until-fits. Items in original order A,B,C with ranks 0,1,2 and desired
// 3,5,2 in an extent of 6 (separator 1). Rank order is already A,B,C.
// Forward scan: A fits (used 3); B needs 3+1+5=9 > 6 -> STOP. The scan ends at
// B, so C is NEVER considered even though A + C (3+1+2 = 6) fits exactly. A
// naive drop-until-fits would drop B and retain {A, C}; the correct rule
// retains ONLY {A}.
TEST(fitRowStopsAtFirstNonFitNotDropUntilFits) {
    const RowFit fit = fitRow(
        {item("a", 3, 0), item("b", 5, 1), item("c", 2, 2)}, 6, 1, Align::Start);
    ASSERT_EQ(fit.placed.size(), std::size_t{1});
    ASSERT_EQ(fit.placed[0], (PlacedItem{"a", 0, 3, 0}));
}

// Collapse priority is by rank, not input order: a later, higher-priority (lower
// rank) item is retained over an earlier low-priority one when only one fits,
// and the retained set is emitted in ORIGINAL order. Items A(rank2,d4),
// B(rank0,d4) in extent 4: rank order is B,A; B fits (4), A needs 4+1+4 -> stop.
// Only B retained, at its original position.
TEST(fitRowCollapsesByRankAndRestoresOriginalOrder) {
    const RowFit fit =
        fitRow({item("a", 4, 2), item("b", 4, 0)}, 4, 1, Align::Start);
    ASSERT_EQ(fit.placed.size(), std::size_t{1});
    ASSERT_EQ(fit.placed[0], (PlacedItem{"b", 0, 4, 1}));
}

// When everything fits, all items are retained in original order regardless of
// how ranks reorder the scan. A(rank1,d2), B(rank0,d2), extent 10: rank order
// B,A both fit; emitted A,B in original order at 0 and 3.
TEST(fitRowRetainsAllInOriginalOrderWhenEverythingFits) {
    const RowFit fit =
        fitRow({item("a", 2, 1), item("b", 2, 0)}, 10, 1, Align::Start);
    ASSERT_EQ(fit.placed.size(), std::size_t{2});
    ASSERT_EQ(fit.placed[0], (PlacedItem{"a", 0, 2, 0}));
    ASSERT_EQ(fit.placed[1], (PlacedItem{"b", 3, 2, 1}));
}

// The fit test is `used + sep + desired > extent` -> exact fit is NOT a
// non-fit. A(3) + sep(1) + B(2) = 6 == extent 6 fits; both retained.
TEST(fitRowExactFitIsRetained) {
    const RowFit fit = fitRow({item("a", 3), item("b", 2)}, 6, 1, Align::Start);
    ASSERT_EQ(fit.placed.size(), std::size_t{2});
    ASSERT_EQ(fit.placed[1], (PlacedItem{"b", 4, 2, 1}));
}

// A single item wider than the extent fits nothing (the scan stops on the first
// item). Guards the empty-result path.
TEST(fitRowDropsAnItemWiderThanTheExtent) {
    const RowFit fit = fitRow({item("a", 12)}, 6, 1, Align::Start);
    ASSERT_TRUE(fit.placed.empty());
}

// A field's desired width is its display cells + 2 padding, floored at 1
// (matching addFields' `max(1, displayCells(value) + 2)`). ASCII cells == length.
TEST(measureFieldCellsIsDisplayCellsPlusPadding) {
    ASSERT_EQ(measureFieldCells("main"), 6);   // 4 + 2
    ASSERT_EQ(measureFieldCells("x"), 3);       // 1 + 2
    ASSERT_EQ(measureFieldCells(""), 2);        // 0 + 2 (still above the floor of 1)
}

// A non-positive-width item is SKIPPED: it is neither retained nor does it end
// the scan nor consume a separator cell. B(desired 0) sits between A and C; the
// result is A and C packed as if B were absent (A at 0, C at 3 after one
// separator), matching addFields dropping empty-value fields.
TEST(fitRowSkipsNonPositiveWidthItemsWithoutBlocking) {
    const RowFit fit = fitRow(
        {item("a", 2), item("b", 0), item("c", 3)}, 10, 1, Align::Start);
    ASSERT_EQ(fit.placed.size(), std::size_t{2});
    ASSERT_EQ(fit.placed[0], (PlacedItem{"a", 0, 2, 0}));
    ASSERT_EQ(fit.placed[1], (PlacedItem{"c", 3, 3, 2}));
}

// packEnd fills flush-right, last item rightmost, no separators. [a(2),b(3)] in
// extent 10: b at 7 (rightmost, size 3), a at 5 (size 2, left of b).
TEST(packEndFillsFlushRightLastItemRightmost) {
    const RowFit fit = packEnd({item("a", 2), item("b", 3)}, 10);
    ASSERT_EQ(fit.placed.size(), std::size_t{2});
    ASSERT_EQ(fit.placed[0], (PlacedItem{"a", 5, 2, 0}));
    ASSERT_EQ(fit.placed[1], (PlacedItem{"b", 7, 3, 1}));
}

// packEnd TRUNCATES a partially-fitting item rather than dropping it, and the
// leftmost item is the one clamped when space runs out (fill is right-first).
// [a(5),b(4)] in extent 6: b (rightmost) gets its full 4 at offset 2; a gets the
// remaining 2 (truncated from 5) at offset 0.
TEST(packEndTruncatesTheLeftmostItemWhenSpaceRunsOut) {
    const RowFit fit = packEnd({item("a", 5), item("b", 4)}, 6);
    ASSERT_EQ(fit.placed.size(), std::size_t{2});
    ASSERT_EQ(fit.placed[0], (PlacedItem{"a", 0, 2, 0}));  // truncated 5 -> 2
    ASSERT_EQ(fit.placed[1], (PlacedItem{"b", 2, 4, 1}));
}

// An item with no room left is DROPPED (not placed at width 0). [a(4),b(4)] in
// extent 4: b takes all 4; a has zero room and is dropped.
TEST(packEndDropsAnItemWithNoRoomLeft) {
    const RowFit fit = packEnd({item("a", 4), item("b", 4)}, 4);
    ASSERT_EQ(fit.placed.size(), std::size_t{1});
    ASSERT_EQ(fit.placed[0], (PlacedItem{"b", 0, 4, 1}));
}

// checkboxText composes the Style glyph + caption; checked/unchecked select the
// glyph. Reference strings hand-composed with the default glyphs.
TEST(checkboxTextPrependsTheStateGlyphToTheCaption) {
    ToggleGlyphs glyphs;  // defaults: "[x] " / "[ ] "
    ASSERT_EQ(checkboxText(true, "case", glyphs), std::string{"[x] case"});
    ASSERT_EQ(checkboxText(false, "case", glyphs), std::string{"[ ] case"});
    // Custom glyphs are honored (the widget owns the glyph, not the renderer).
    ToggleGlyphs custom{"(*) ", "( ) "};
    ASSERT_EQ(checkboxText(true, "x", custom), std::string{"(*) x"});
    ASSERT_EQ(checkboxText(false, "x", custom), std::string{"( ) x"});
}

// textInputText concatenates prefix + separator + value. The three shapes it
// covers: a prompt input (label + ": " + value), the picker input line (sigil +
// "" + tail), and a bare label (text + "" + "").
TEST(textInputTextConcatenatesPrefixSeparatorValue) {
    ASSERT_EQ(textInputText("find", ": ", "cat"), std::string{"find: cat"});
    ASSERT_EQ(textInputText("> ", "", "query"), std::string{"> query"});
    ASSERT_EQ(textInputText("label", "", ""), std::string{"label"});
}

// visibleTail keeps the END of a growing value on screen. Hand-computed on
// ASCII (one cell per char): a value that fits is returned whole; an over-long
// value is sliced from the right taking clusters while they fit; zero cells
// yields nothing.
TEST(visibleTailKeepsTheEndWithinTheCellBudget) {
    ASSERT_EQ(visibleTail("hello", 10), std::string{"hello"});
    ASSERT_EQ(visibleTail("hello", 3), std::string{"llo"});
    ASSERT_EQ(visibleTail("hello", 0), std::string{""});
}

// layoutTextInput reserves one column for the caret (drawable = available - 1),
// pins the sigil, and scrolls the value's tail into the room after it.
// Hand-computed with sigil "> " (2 cells):
//  - available 10, "hi": drawable 9, textRoom 7, tail "hi", text "> hi" width 4.
//  - available 6, "abcdef": drawable 5, textRoom 3, tail "def", text "> def"
//    clamped to drawable 5.
//  - available 1: drawable 0, textRoom 0, tail "", text "> " clamped to 0 so
//    the caret column never escapes the field.
TEST(layoutTextInputReservesCaretAndScrollsTail) {
    const auto wide = layoutTextInput("> ", "hi", 10);
    ASSERT_EQ(wide.text, std::string{"> hi"});
    ASSERT_EQ(wide.width, 4);

    const auto scrolled = layoutTextInput("> ", "abcdef", 6);
    ASSERT_EQ(scrolled.text, std::string{"> def"});
    ASSERT_EQ(scrolled.width, 5);

    const auto tiny = layoutTextInput("> ", "x", 1);
    ASSERT_EQ(tiny.text, std::string{"> "});
    ASSERT_EQ(tiny.width, 0);
}

// layoutInputLine bundles the query (via layoutTextInput) and the ghost that
// fills the remaining cells. Hand-computed with sigil "> " (2 cells), available
// 20: query "hi" -> text "> hi" width 4; remaining 20-4=16; ghost "story" (5
// cells) fits whole -> ghostWidth 5, ghostText "story".
TEST(layoutInputLineBundlesQueryAndGhost) {
    const auto line = layoutInputLine("> ", "hi", "story", 20);
    ASSERT_EQ(line.text, std::string{"> hi"});
    ASSERT_EQ(line.width, 4);
    ASSERT_EQ(line.ghostText, std::string{"story"});
    ASSERT_EQ(line.ghostWidth, 5);
}

// The ghost is clamped to the cells the query left (its text stays whole; the
// renderer clips). available 7, query "ab" -> "> ab" width 4; remaining 3; ghost
// "longer" (6 cells) clamps to 3, text kept whole.
TEST(layoutInputLineClampsGhostToRemainingCells) {
    const auto line = layoutInputLine("> ", "ab", "longer", 7);
    ASSERT_EQ(line.width, 4);
    ASSERT_EQ(line.ghostWidth, 3);
    ASSERT_EQ(line.ghostText, std::string{"longer"});
}

// No ghost is placed when there is no room at all (available 0) or no ghost
// text. (The caret reservation means a non-empty query never fully consumes a
// positive row, so the no-room case is the zero-width row.)
TEST(layoutInputLineDropsGhostWithNoRoomOrNoGhost) {
    const auto noRoom = layoutInputLine("> ", "abcd", "ghost", 0);
    ASSERT_EQ(noRoom.ghostWidth, 0);
    ASSERT_EQ(noRoom.ghostText, std::string{});

    const auto noGhost = layoutInputLine("> ", "hi", "", 20);
    ASSERT_EQ(noGhost.ghostWidth, 0);
    ASSERT_EQ(noGhost.ghostText, std::string{});
}

// --- WidgetStack -----------------------------------------------------------

StackItem stackItem(std::string id, int desired, int rank = 0) {
    StackItem item;
    item.id = std::move(id);
    item.desired = desired;
    item.rank = rank;
    item.content = "x";  // content text is irrelevant to geometry-only cases
    return item;
}

// packLeft alone packs from offset 0 with the stack separator, exactly like
// fitRow(Align::Start). a at 0 (size 2), sep at 2, b at 3 (size 3).
TEST(widgetStackPackLeftPacksFromLeadingEdge) {
    const auto out =
        WidgetStack{1}.packLeft(stackItem("a", 2)).packLeft(stackItem("b", 3))
            .resolve(10);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->placed.size(), std::size_t{2});
    ASSERT_EQ(out->placed[0].id, std::string{"a"});
    ASSERT_EQ(out->placed[0].offset, 0);
    ASSERT_EQ(out->placed[0].size, 2);
    ASSERT_EQ(out->placed[1].offset, 3);
    ASSERT_EQ(out->placed[1].size, 3);
}

// packRight fills flush to the trailing edge; z (last call) is rightmost.
// extent 10: z desired 3 -> [7,10); x desired 2 -> [5,7). Emitted in original
// call order x then z.
TEST(widgetStackPackRightFlushesRightInCallOrder) {
    const auto out =
        WidgetStack{}.packRight(stackItem("x", 2)).packRight(stackItem("z", 3))
            .resolve(10);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->placed.size(), std::size_t{2});
    ASSERT_EQ(out->placed[0], (StackPlacement{"x", 5, 2, "x"}));
    ASSERT_EQ(out->placed[1], (StackPlacement{"z", 7, 3, "x"}));
}

// Left fills the space to the LEFT of the right group. right z -> [7,10);
// rightStart 7; left a laid out in extent 7 -> a at 0 (size 2).
TEST(widgetStackLeftFillsSpaceLeftOfRightGroup) {
    const auto out =
        WidgetStack{1}.packLeft(stackItem("a", 2)).packRight(stackItem("z", 3))
            .resolve(10);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->placed.size(), std::size_t{2});
    ASSERT_EQ(out->placed[0], (StackPlacement{"a", 0, 2, "x"}));
    ASSERT_EQ(out->placed[1], (StackPlacement{"z", 7, 3, "x"}));
}

// A Flex center takes the whole gap between left end and right start. left a
// [0,2), right z [7,10), gap [2,7) width 5. Emission order: left, center, right.
TEST(widgetStackFlexCenterTakesTheGap) {
    const auto out =
        WidgetStack{1}.packLeft(stackItem("a", 2)).packRight(stackItem("z", 3))
            .center(stackItem("c", 0), CenterWidth::Flex)
            .resolve(10);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->placed.size(), std::size_t{3});
    ASSERT_EQ(out->placed[0].id, std::string{"a"});
    ASSERT_EQ(out->placed[1], (StackPlacement{"c", 2, 5, "x"}));
    ASSERT_EQ(out->placed[2].id, std::string{"z"});
}

// A Fixed center wider than the gap clamps to the gap (never fail-loud). Same
// geometry: gap 5, center fixed 8 -> size 5. A fixed 3 would take 3.
TEST(widgetStackFixedCenterClampsToGap) {
    const auto wide =
        WidgetStack{1}.packLeft(stackItem("a", 2)).packRight(stackItem("z", 3))
            .center(stackItem("c", 0), CenterWidth::Fixed, 8)
            .resolve(10);
    ASSERT_TRUE(wide.has_value());
    ASSERT_EQ(wide->placed[1], (StackPlacement{"c", 2, 5, "x"}));

    const auto narrow =
        WidgetStack{1}.packLeft(stackItem("a", 2)).packRight(stackItem("z", 3))
            .center(stackItem("c", 0), CenterWidth::Fixed, 3)
            .resolve(10);
    ASSERT_TRUE(narrow.has_value());
    ASSERT_EQ(narrow->placed[1], (StackPlacement{"c", 2, 3, "x"}));
}

// A second center() call is a fail-loud authoring error: resolve -> nullopt.
TEST(widgetStackSecondCenterFailsLoud) {
    const auto out =
        WidgetStack{}.center(stackItem("c", 0), CenterWidth::Flex)
            .center(stackItem("d", 0), CenterWidth::Flex)
            .resolve(10);
    ASSERT_FALSE(out.has_value());
}

// Left collapse is rank-ordered and STOPS at the first non-fit (not
// drop-until-fits). Original order a,b,c; ranks a=1(desired5), b=0(desired2),
// c=2(desired2); sep 1, extent 8. Rank scan b,a,c: b used 2; a 2+1+5=8 ok;
// c 8+1+2>8 STOP. Retained b,a; emitted original order a,b: a@0(5), b@6(2); c
// dropped.
TEST(widgetStackLeftCollapseStopsAtFirstNonFit) {
    const auto out =
        WidgetStack{1}.packLeft(stackItem("a", 5, 1))
            .packLeft(stackItem("b", 2, 0)).packLeft(stackItem("c", 2, 2))
            .resolve(8);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->placed.size(), std::size_t{2});
    ASSERT_EQ(out->placed[0], (StackPlacement{"a", 0, 5, "x"}));
    ASSERT_EQ(out->placed[1], (StackPlacement{"b", 6, 2, "x"}));
}

// Right group clamp-truncates the leftmost item that has room and drops one with
// none. extent 6, three items a,b,c each desired 4: c [2,6) full; b room 2 ->
// [0,2) truncated; a no room -> dropped. Emitted original order b,c.
TEST(widgetStackRightGroupClampTruncatesAndDrops) {
    StackItem a = stackItem("a", 4);
    a.overflow = Overflow::Truncate;
    StackItem b = stackItem("b", 4);
    b.overflow = Overflow::Truncate;
    StackItem c = stackItem("c", 4);
    c.overflow = Overflow::Truncate;
    const auto out =
        WidgetStack{}.packRight(a).packRight(b).packRight(c).resolve(6);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->placed.size(), std::size_t{2});
    ASSERT_EQ(out->placed[0].id, std::string{"b"});
    ASSERT_EQ(out->placed[0].offset, 0);
    ASSERT_EQ(out->placed[0].size, 2);          // clamp-truncated to remaining room
    ASSERT_EQ(out->placed[0].text, std::string{"x"});  // content stays whole
    ASSERT_EQ(out->placed[1], (StackPlacement{"c", 2, 4, "x"}));
}

// A ScrollTail center composes sigil + value tail via layoutTextInput. gap 5,
// content "abcdef", sigil "> ": drawable 4, sigil 2, textRoom 2 -> tail "ef",
// text "> ef" width 4.  (Fixed 6 clamps to the gap of 5.)
TEST(widgetStackScrollTailCenterShowsValueTail) {
    StackItem query;
    query.id = "q";
    query.content = "abcdef";
    query.sigil = "> ";
    query.overflow = Overflow::ScrollTail;
    const auto out =
        WidgetStack{1}.packLeft(stackItem("a", 2)).packRight(stackItem("z", 3))
            .center(query, CenterWidth::Fixed, 6)
            .resolve(10);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->placed[1].id, std::string{"q"});
    ASSERT_EQ(out->placed[1].offset, 2);
    ASSERT_EQ(out->placed[1].text, std::string{"> ef"});
    ASSERT_EQ(out->placed[1].size, 4);
}

// A `keep` left item is never collapsed and pre-consumes budget; the remaining
// collapse item fits only in what is left. keep k desired 3, collapse a desired
// 4, sep 1, extent 6: keepFootprint 3, joinSep 1, collapse extent 6-3-1=2 -> a
// (desired 4) does not fit and drops; only k survives at [0,3).
TEST(widgetStackKeepLeftItemSurvivesCollapse) {
    StackItem keep = stackItem("k", 3);
    keep.keep = true;
    const auto out =
        WidgetStack{1}.packLeft(keep).packLeft(stackItem("a", 4, 0)).resolve(6);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->placed.size(), std::size_t{1});
    ASSERT_EQ(out->placed[0], (StackPlacement{"k", 0, 3, "x"}));
}

// An over-budget `keep` item truncates instead of overlapping the right group
// (non-overlap invariant). extent 10, right z desired 8 -> [2,10), rightStart 2;
// left keep k desired 5 clamps to the 2 cells before the right group: k [0,2).
TEST(widgetStackKeepItemTruncatesRatherThanOverlappingRight) {
    StackItem keep = stackItem("k", 5);
    keep.keep = true;
    const auto out =
        WidgetStack{1}.packLeft(keep).packRight(stackItem("z", 8)).resolve(10);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->placed.size(), std::size_t{2});
    ASSERT_EQ(out->placed[0], (StackPlacement{"k", 0, 2, "x"}));
    ASSERT_EQ(out->placed[1], (StackPlacement{"z", 2, 8, "x"}));
}

}  // namespace

int main() {
    RUN(fitRowStartPacksLeftWithSeparators);
    RUN(fitRowEndPacksFlushRight);
    RUN(fitRowStopsAtFirstNonFitNotDropUntilFits);
    RUN(fitRowCollapsesByRankAndRestoresOriginalOrder);
    RUN(fitRowRetainsAllInOriginalOrderWhenEverythingFits);
    RUN(fitRowExactFitIsRetained);
    RUN(fitRowDropsAnItemWiderThanTheExtent);
    RUN(fitRowSkipsNonPositiveWidthItemsWithoutBlocking);
    RUN(packEndFillsFlushRightLastItemRightmost);
    RUN(packEndTruncatesTheLeftmostItemWhenSpaceRunsOut);
    RUN(packEndDropsAnItemWithNoRoomLeft);
    RUN(checkboxTextPrependsTheStateGlyphToTheCaption);
    RUN(textInputTextConcatenatesPrefixSeparatorValue);
    RUN(visibleTailKeepsTheEndWithinTheCellBudget);
    RUN(layoutTextInputReservesCaretAndScrollsTail);
    RUN(layoutInputLineBundlesQueryAndGhost);
    RUN(layoutInputLineClampsGhostToRemainingCells);
    RUN(layoutInputLineDropsGhostWithNoRoomOrNoGhost);
    RUN(widgetStackPackLeftPacksFromLeadingEdge);
    RUN(widgetStackPackRightFlushesRightInCallOrder);
    RUN(widgetStackLeftFillsSpaceLeftOfRightGroup);
    RUN(widgetStackFlexCenterTakesTheGap);
    RUN(widgetStackFixedCenterClampsToGap);
    RUN(widgetStackSecondCenterFailsLoud);
    RUN(widgetStackLeftCollapseStopsAtFirstNonFit);
    RUN(widgetStackRightGroupClampTruncatesAndDrops);
    RUN(widgetStackScrollTailCenterShowsValueTail);
    RUN(widgetStackKeepLeftItemSurvivesCollapse);
    RUN(widgetStackKeepItemTruncatesRatherThanOverlappingRight);
    RUN(measureFieldCellsIsDisplayCellsPlusPadding);
    return 0;
}
