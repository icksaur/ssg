#include "test_helpers.h"

#include <ssg/Style.h>

#include <string>

namespace {

// A style whose every glyph is distinctive and multi-character.  Nothing here
// resembles the defaults, so an implementation that ignored its configuration
// and returned a hardcoded `|`/`#` could not pass a single one of these.
ssg::Style configuredStyle() {
    ssg::Style style;
    style.scrollbar.gutter = "_";
    style.scrollbar.track = ".";
    style.scrollbar.single = "S";
    style.scrollbar.top = "T";
    style.scrollbar.body = "B";
    style.scrollbar.bottom = "E";
    return style;
}

// The whole scrollbar column as one string, which is how a reader actually
// judges a scrollbar: shape first, cells second.
std::string column(ssg::Style const& style, int thumbStart, int thumbSize,
                   int trackHeight) {
    std::string out;
    for (int row = 0; row < trackHeight; ++row) {
        out += style.scrollbarCell(row, thumbStart, thumbSize, trackHeight);
    }
    return out;
}

TEST(scrollbarUsesTheGlyphsItWasConfiguredWith) {
    auto const style = configuredStyle();

    // Size 3+: caps at the ends, body between.  The track fills the rest.
    ASSERT_EQ(column(style, 2, 4, 8), std::string{"..TBBE.."});

    // Size 2: two caps, no body -- there is no room for one.
    ASSERT_EQ(column(style, 1, 2, 5), std::string{".TE.."});

    // Size 1: a single cap.  Neither `top` nor `bottom` appears, because a
    // one-row thumb has no ends to distinguish.
    ASSERT_EQ(column(style, 3, 1, 5), std::string{"...S."});

    // Size 0 means nothing scrolls: the column is gutter, not track.  This is
    // the common case for a short tree, not a hypothetical.
    ASSERT_EQ(column(style, 0, 0, 4), std::string{"____"});
}

TEST(aUniformThumbNeedsNoCapAwareness) {
    // The default shape: all four thumb entries equal.  A style that does not
    // care about caps configures one glyph and gets a solid thumb at every
    // size, which is why caps are a capability rather than a cost.
    ssg::Style style;
    style.scrollbar.track = ".";
    style.scrollbar.single = "#";
    style.scrollbar.top = "#";
    style.scrollbar.body = "#";
    style.scrollbar.bottom = "#";

    ASSERT_EQ(column(style, 1, 1, 4), std::string{".#.."});
    ASSERT_EQ(column(style, 1, 2, 4), std::string{".##."});
    ASSERT_EQ(column(style, 1, 3, 4), std::string{".###"});
}

TEST(theResolvedThumbAlwaysCoversExactlyItsRequestedRows) {
    // A property rather than a table: whatever the size, the number of
    // non-track cells equals the size.  A cap rule that dropped or duplicated a
    // row would break this at some size even if the hand cases above passed.
    auto const style = configuredStyle();
    for (int height = 1; height <= 12; ++height) {
        for (int size = 1; size <= height; ++size) {
            for (int start = 0; start + size <= height; ++start) {
                std::string const rendered = column(style, start, size, height);
                int thumbCells = 0;
                for (char cell : rendered) {
                    if (cell != '.') ++thumbCells;
                }
                ASSERT_EQ(thumbCells, size);
            }
        }
    }
}

TEST(outOfRangeScrollbarGeometryIsClampedNotTrusted) {
    auto const style = configuredStyle();

    // A thumb longer than its track: clamped to fill the track rather than
    // indexing past the bottom cap.  Metrics reach this from a wire-decoded
    // payload, so "cannot happen" is not a defence.
    ASSERT_EQ(column(style, 0, 99, 3), std::string{"TBE"});

    // A start that would push the thumb off the end is pulled back so the
    // thumb stays whole.
    ASSERT_EQ(column(style, 10, 2, 4), std::string{"..TE"});

    // Degenerate tracks produce no thumb rather than misdrawn one.
    ASSERT_EQ(column(style, 0, 1, 0), std::string{""});
    ASSERT_EQ(style.scrollbarCell(-1, 0, 2, 4), style.scrollbar.gutter);
    ASSERT_EQ(style.scrollbarCell(9, 0, 2, 4), style.scrollbar.gutter);
}

TEST(sigilWidthIsMeasuredFromTheSigilNotDeclaredBesideIt) {
    ssg::Style style;

    style.inputLineSigil = "> ";
    ASSERT_EQ(style.sigilWidth(), 2);

    // Change the sigil and the width follows with no second constant to update.
    // This is the pairing that would otherwise drift and misalign the input
    // line (doc/spec-input-line.md).
    style.inputLineSigil = ":";
    ASSERT_EQ(style.sigilWidth(), 1);

    // Display width, not byte length: a wide glyph occupies two cells despite
    // being three bytes.
    style.inputLineSigil = "\xe2\x96\xb8";  // U+25B8, narrow
    ASSERT_EQ(style.sigilWidth(), 1);
    style.inputLineSigil = "\xef\xbc\x9e";  // U+FF1E fullwidth '>', wide
    ASSERT_EQ(style.sigilWidth(), 2);
}

TEST(inputLineReservationTracksTheConfiguredSigilAndBudget) {
    ssg::Style style;
    style.inputLineSigil = ">>>";
    style.dimensions.inputLineSeparator = 1;
    style.dimensions.inputLineQueryBudget = 8;
    ASSERT_EQ(style.inputLineReservation(), 12);

    // The reservation is deliberately independent of the typed query, so it
    // moves only when the style says so.
    style.dimensions.inputLineQueryBudget = 4;
    ASSERT_EQ(style.inputLineReservation(), 8);
}

TEST(chromeGlyphsAreConfigurableRatherThanCompiledIn) {
    // Every remaining chrome glyph is settable.  The point is not which glyph
    // is the default but that a caller can change it in one place -- these are
    // the literals that were previously scattered across three files.
    ssg::Style style;
    style.tree.expanded = "v";
    style.tree.collapsed = ">";
    style.tree.indentPerDepth = 4;
    style.tab.dirtySuffix = "*";
    style.tab.liveDiffPrefix = "diff:";
    style.truncation = "~";
    style.dimensions.labelPadding = 3;

    ASSERT_EQ(style.tree.expanded, std::string{"v"});
    ASSERT_EQ(style.tree.collapsed, std::string{">"});
    ASSERT_EQ(style.tree.indentPerDepth, 4);
    ASSERT_EQ(style.tab.dirtySuffix, std::string{"*"});
    ASSERT_EQ(style.tab.liveDiffPrefix, std::string{"diff:"});
    ASSERT_EQ(style.truncation, std::string{"~"});
    ASSERT_EQ(style.dimensions.labelPadding, 3);
}

}  // namespace

int main() {
    RUN(scrollbarUsesTheGlyphsItWasConfiguredWith);
    RUN(aUniformThumbNeedsNoCapAwareness);
    RUN(theResolvedThumbAlwaysCoversExactlyItsRequestedRows);
    RUN(outOfRangeScrollbarGeometryIsClampedNotTrusted);
    RUN(sigilWidthIsMeasuredFromTheSigilNotDeclaredBesideIt);
    RUN(inputLineReservationTracksTheConfiguredSigilAndBudget);
    RUN(chromeGlyphsAreConfigurableRatherThanCompiledIn);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
