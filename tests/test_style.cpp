#include "test_helpers.h"

#include <ssg/Style.h>

#include <fstream>
#include <algorithm>
#include <iostream>
#include <iterator>
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
        out += style.scrollbarCell(row, thumbStart, thumbSize, trackHeight).glyph;
    }
    return out;
}

// The same column expressed as kinds rather than glyphs.  The kind is what
// selects the color role, so it has to be right independently of the glyph.
std::string kinds(ssg::Style const& style, int thumbStart, int thumbSize,
                  int trackHeight) {
    std::string out;
    for (int row = 0; row < trackHeight; ++row) {
        switch (style.scrollbarCell(row, thumbStart, thumbSize, trackHeight).kind) {
            case ssg::ScrollbarCellKind::Gutter: out += 'g'; break;
            case ssg::ScrollbarCellKind::Track: out += 't'; break;
            case ssg::ScrollbarCellKind::Thumb: out += 'T'; break;
        }
    }
    return out;
}


// The glyph a key currently holds. Spelled out here on purpose: this test's job
// is to notice when Style.h grows a field, so it must NOT share the table it is
// checking. If a new field is added, this list fails to compile or the count
// assertion fails -- both of which are the point.
std::string currentGlyph(ssg::Style const& style, std::string const& key) {
    if (key == "scrollbar_gutter") return style.scrollbar.gutter;
    if (key == "scrollbar_track") return style.scrollbar.track;
    if (key == "scrollbar_single") return style.scrollbar.single;
    if (key == "scrollbar_top") return style.scrollbar.top;
    if (key == "scrollbar_body") return style.scrollbar.body;
    if (key == "scrollbar_bottom") return style.scrollbar.bottom;
    if (key == "tree_expanded") return style.tree.expanded;
    if (key == "tree_collapsed") return style.tree.collapsed;
    if (key == "tab_dirty_suffix") return style.tab.dirtySuffix;
    if (key == "tab_live_diff_prefix") return style.tab.liveDiffPrefix;
    if (key == "tab_read_only_suffix") return style.tab.readOnlySuffix;
    if (key == "tab_left_edge") return style.tab.leftEdge;
    if (key == "tab_right_edge") return style.tab.rightEdge;
    if (key == "tab_separator") return style.tab.separator;
    if (key == "toggle_checked") return style.toggle.checked;
    if (key == "toggle_unchecked") return style.toggle.unchecked;
    if (key == "truncation") return style.truncation;
    if (key == "input_line_sigil") return style.inputLineSigil;
    if (key == "unrenderable") return style.unrenderable;
    if (key == "prompt_label_separator") return style.promptLabelSeparator;
    if (key == "cwd_prefix") return style.cwdPrefix;
    return {};
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

TEST(cellKindDistinguishesGutterFromTrackAndThumb) {
    auto const style = configuredStyle();

    // Track and gutter are different KINDS even when a style gives them the
    // same glyph, because they take different color roles.  An empty column is
    // gutter throughout; a populated one is track around the thumb.
    ASSERT_EQ(kinds(style, 1, 2, 5), std::string{"tTTtt"});
    ASSERT_EQ(kinds(style, 0, 0, 4), std::string{"gggg"});

    ssg::Style ambiguous;
    ambiguous.scrollbar.gutter = "=";
    ambiguous.scrollbar.track = "=";
    ASSERT_EQ(kinds(ambiguous, 0, 0, 3), std::string{"ggg"});
    ASSERT_EQ(kinds(ambiguous, 0, 1, 3), std::string{"Ttt"});
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
    ASSERT_EQ(style.scrollbarCell(-1, 0, 2, 4).glyph, style.scrollbar.gutter);
    ASSERT_EQ(style.scrollbarCell(9, 0, 2, 4).glyph, style.scrollbar.gutter);
}

TEST(sigilWidthIsMeasuredFromTheSigilNotDeclaredBesideIt) {
    ssg::Style style;

    style.inputLineSigil = "> ";
    ASSERT_EQ(style.sigilWidth(), 2);

    // Change the sigil and the width follows with no second constant to update.
    // This is the pairing that would otherwise drift and misalign the input
    // line.
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
    style.unrenderable = "?";
    style.promptLabelSeparator = " = ";
    style.dimensions.labelPadding = 3;

    ASSERT_EQ(style.tree.expanded, std::string{"v"});
    ASSERT_EQ(style.tree.collapsed, std::string{">"});
    ASSERT_EQ(style.tree.indentPerDepth, 4);
    ASSERT_EQ(style.tab.dirtySuffix, std::string{"*"});
    ASSERT_EQ(style.tab.liveDiffPrefix, std::string{"diff:"});
    ASSERT_EQ(style.truncation, std::string{"~"});
    ASSERT_EQ(style.unrenderable, std::string{"?"});
    ASSERT_EQ(style.promptLabelSeparator, std::string{" = "});
    ASSERT_EQ(style.dimensions.labelPadding, 3);
}

TEST(styleDefineReplacesOnlyTheNamedFields) {
    ssg::Style base;
    base.scrollbar.track = "|";
    base.tree.expanded = "v";

    ssg::StyleDefineArguments args;
    args.values = {{"scrollbar_track", ":"}, {"dim_header_height", "3"}};
    auto const result = base.withDefine(args);
    ASSERT_TRUE(result.accepted());
    if (!result.accepted()) return;

    // The two named fields changed...
    ASSERT_EQ(result.style.scrollbar.track, std::string{":"});
    ASSERT_EQ(result.style.dimensions.headerHeight, 3);
    // ...and everything else is untouched (partial table).
    ASSERT_EQ(result.style.tree.expanded, std::string{"v"});
    ASSERT_EQ(result.style.scrollbar.single, ssg::Style{}.scrollbar.single);
}

TEST(styleDefineRejectsUnknownKeysWholesale) {
    ssg::Style base;
    base.scrollbar.track = "|";

    ssg::StyleDefineArguments args;
    // One good key and one unknown key: the whole call must reject, and the
    // good key must NOT have leaked through (all-or-nothing).
    args.values = {{"scrollbar_track", ":"}, {"not_a_style_field", "x"}};
    auto const result = base.withDefine(args);
    ASSERT_FALSE(result.accepted());
    ASSERT_TRUE(result.error.has_value());
}

TEST(styleDefineRejectsMalformedAndNegativeDimensions) {
    ssg::Style base;

    ssg::StyleDefineArguments notInt;
    notInt.values = {{"dim_header_height", "tall"}};
    ASSERT_FALSE(base.withDefine(notInt).accepted());

    ssg::StyleDefineArguments trailing;
    trailing.values = {{"dim_header_height", "3px"}};
    ASSERT_FALSE(base.withDefine(trailing).accepted());

    ssg::StyleDefineArguments negative;
    negative.values = {{"dim_header_height", "-1"}};
    ASSERT_FALSE(base.withDefine(negative).accepted());
}

TEST(styleDefineWithAnEmptyTableIsANoOp) {
    ssg::Style base;
    base.scrollbar.track = "%";
    auto const result = base.withDefine(ssg::StyleDefineArguments{});
    ASSERT_TRUE(result.accepted());
    if (!result.accepted()) return;
    ASSERT_TRUE(result.style == base);
}

}  // namespace


// A glyph is the one string that reaches a terminal cell without passing
// through GraphemeLayout on the way, so it is the one place a control byte can
// reach the terminal and change how it reads what follows.
TEST(aStyleGlyphThatEmitsAModeIsRejectedNamingItsKey) {
    ssg::Style const base{};
    // ESC ( 0 switches the terminal's character set: every later byte draws as
    // line art. This reached a real terminal before it was rejected here.
    auto const escaped = base.withDefine(ssg::StyleDefineArguments{{{"scrollbar_track", "\x1b(0"}}});
    ASSERT_FALSE(escaped.accepted());
    if (escaped.error) {
        ASSERT_TRUE(escaped.error->message.find("scrollbar_track") !=
                    std::string::npos);
        ASSERT_TRUE(escaped.error->message.find("control character") !=
                    std::string::npos);
    }

    // A bare control byte, and a shift-out, are refused for the same reason.
    // "a\x1b" "b" is split so the hex escape cannot swallow the following 'b'
    // as a third hex digit -- \x1bb is out of range, and what it would mean is
    // not what this case is testing.
    for (auto const* value : {"\x0e", "\x07", "a\x1b" "b"}) {
        auto const rejected = base.withDefine(ssg::StyleDefineArguments{{{"truncation", value}}});
        ASSERT_FALSE(rejected.accepted());
    }
}

TEST(aStyleGlyphOfTheWrongWidthIsRejectedNamingItsKey) {
    ssg::Style const base{};
    // scrollbar_track's default is one column, so a two-column glyph would push
    // the row's remaining cells sideways.
    for (auto const* value : {"XY", "\xe4\xb8\xad", "ABCDEFGHIJ", ""}) {
        auto const rejected = base.withDefine(ssg::StyleDefineArguments{{{"scrollbar_track", value}}});
        ASSERT_FALSE(rejected.accepted());
        if (rejected.error) {
            ASSERT_TRUE(rejected.error->message.find("scrollbar_track") !=
                        std::string::npos);
        }
    }
}

TEST(aStyleGlyphMatchingItsFieldsWidthIsAccepted) {
    ssg::Style const base{};
    // One column for a one-column field...
    auto const narrow = base.withDefine(ssg::StyleDefineArguments{{{"scrollbar_track", ":"}}});
    ASSERT_TRUE(narrow.accepted());
    ASSERT_EQ(narrow.style.scrollbar.track, std::string{":"});

    // ...and the width is the FIELD's, not one: tree_expanded's default is two
    // columns, so a two-column replacement is correct and one would not be.
    auto const wide = base.withDefine(ssg::StyleDefineArguments{{{"tree_expanded", "v "}}});
    ASSERT_TRUE(wide.accepted());
    ASSERT_FALSE(base.withDefine(ssg::StyleDefineArguments{{{"tree_expanded", "v"}}})
                     .accepted());

    // A wide CJK glyph is two columns, so it fits a two-column field.
    ASSERT_TRUE(base.withDefine(ssg::StyleDefineArguments{{{"tree_expanded", "\xe4\xb8\xad"}}})
                    .accepted());
}

TEST(aRejectedGlyphChangesNothing) {
    ssg::Style const base{};
    // Rejection is all or nothing, like the unknown-key and bad-dimension cases.
    auto const rejected = base.withDefine(ssg::StyleDefineArguments{{{"truncation", "."},
                                         {"scrollbar_track", "\x1b(0"}}});
    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(base.truncation, ssg::Style{}.truncation);
}


// Every glyph a user can set must go through validation, and every default must
// itself be valid.
//
// This closes the CLASS rather than the instance. Validation lives in
// applyStyleDefine, which reaches a field only through the glyph table -- so a
// new Style string field is either in that table, and therefore validated, or
// unreachable from style.define and therefore not an input at all. What the
// scan catches is the third case: a field added to Style.h and to the table
// with a default that would itself be rejected, which would make the editor
// unable to reproduce its own starting state.
TEST(everyGlyphFieldIsValidatedAndEveryDefaultIsValid) {
    ssg::Style const defaults{};

    // Round-trip: setting each key to its own current value must be accepted.
    // A default that fails its own rule would mean the defaults and the
    // validator disagree about what a legal glyph is.
    auto const keys = ssg::Style::defineKeys();
    ASSERT_FALSE(keys.empty());
    std::size_t glyphKeys = 0;
    for (auto const& key : keys) {
        if (key.rfind("dim_", 0) == 0 || key == "tree_indent") continue;
        ++glyphKeys;
        auto const applied = defaults.withDefine(ssg::StyleDefineArguments{{{key, currentGlyph(defaults, key)}}});
        if (!applied.accepted() && applied.error) {
            std::cout << "  offending key: " << key << " -- "
                      << applied.error->message << "\n";
        }
        ASSERT_TRUE(applied.accepted());
    }

    // Every std::string field declared in Style.h must be one of those keys, so
    // a newly added glyph cannot quietly skip validation by never being listed.
    std::ifstream header{std::string{SSG_TEST_SOURCE_DIR} +
                         "/include/ssg/Style.h"};
    std::string const source{std::istreambuf_iterator<char>{header},
                             std::istreambuf_iterator<char>{}};
    ASSERT_FALSE(source.empty());
    std::size_t declared = 0;
    for (std::size_t at = source.find("std::string "); at != std::string::npos;
         at = source.find("std::string ", at + 1)) {
        // StyleDefineError::message is a diagnostic, not a drawn glyph.
        auto const lineEnd = source.find('\n', at);
        auto const line = source.substr(at, lineEnd - at);
        if (line.find("message") != std::string::npos) continue;
        ++declared;
    }
    ASSERT_EQ(declared, glyphKeys);
}

TEST(aVariableWidthTabGlyphAcceptsAnyWidthWhileFixedGlyphsDoNot) {
    ssg::Style const defaults{};

    // Tab edges and separator are layout-affecting: the tab row recomputes its
    // geometry from the configured width, so a value wider than the default is
    // legal.  leftEdge defaults to "" (width 0); "[" (width 1) must be accepted.
    for (auto const& key : {std::string{"tab_left_edge"},
                            std::string{"tab_right_edge"},
                            std::string{"tab_separator"}}) {
        auto const applied = defaults.withDefine(ssg::StyleDefineArguments{{{key, " | "}}});
        ASSERT_TRUE(applied.accepted());
    }

    // A fixed-slot glyph of the wrong width is still refused: the contrast is
    // the whole point of the two categories.
    auto const fixed = defaults.withDefine(ssg::StyleDefineArguments{{{"tab_dirty_suffix", " *!"}}});
    ASSERT_FALSE(fixed.accepted());

    // A variable glyph still rejects control characters / invalid bytes.
    auto const control = defaults.withDefine(ssg::StyleDefineArguments{{{"tab_separator", "\x1b(0"}}});
    ASSERT_FALSE(control.accepted());
}

TEST(styleDefineKeysAreUniqueSoNoGlyphLivesInTwoCategories) {
    // A key present in both glyphSetters() and variableGlyphSetters() would make
    // validation order a hidden contract.  styleDefineKeys() concatenates every
    // map, so a duplicate here proves an overlap.
    auto keys = ssg::Style::defineKeys();
    std::sort(keys.begin(), keys.end());
    ASSERT_TRUE(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
}

SSG_TEST_SUITE(test_style) {
    RUN(aStyleGlyphOfTheWrongWidthIsRejectedNamingItsKey);
    RUN(aStyleGlyphMatchingItsFieldsWidthIsAccepted);
    RUN(aRejectedGlyphChangesNothing);
    RUN(everyGlyphFieldIsValidatedAndEveryDefaultIsValid);
    RUN(aVariableWidthTabGlyphAcceptsAnyWidthWhileFixedGlyphsDoNot);
    RUN(styleDefineKeysAreUniqueSoNoGlyphLivesInTwoCategories);
    RUN(scrollbarUsesTheGlyphsItWasConfiguredWith);
    RUN(aUniformThumbNeedsNoCapAwareness);
    RUN(theResolvedThumbAlwaysCoversExactlyItsRequestedRows);
    RUN(cellKindDistinguishesGutterFromTrackAndThumb);
    RUN(outOfRangeScrollbarGeometryIsClampedNotTrusted);
    RUN(sigilWidthIsMeasuredFromTheSigilNotDeclaredBesideIt);
    RUN(inputLineReservationTracksTheConfiguredSigilAndBudget);
    RUN(chromeGlyphsAreConfigurableRatherThanCompiledIn);
    RUN(styleDefineReplacesOnlyTheNamedFields);
    RUN(styleDefineRejectsUnknownKeysWholesale);
    RUN(styleDefineRejectsMalformedAndNegativeDimensions);
    RUN(styleDefineWithAnEmptyTableIsANoOp);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
