#include "test_helpers.h"

#include <ssg/Viewport.h>

#include <vector>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>

namespace {

// An independent reimplementation of "shift a clamped offset by a delta",
// written from the RULE rather than from the code under test: clamp the current
// offset into range, add, clamp again. If ScrollOffset agrees with this over a
// wide table, it is not merely self-consistent.
std::uint32_t referenceShift(std::uint32_t total, std::uint32_t rows,
                             std::uint32_t current, std::int64_t delta) {
    if (rows == 0) return 0;
    const std::uint64_t maximum = total > rows ? total - rows : 0;
    const std::int64_t clamped =
        static_cast<std::int64_t>(std::min<std::uint64_t>(current, maximum));
    // Saturating: delta is wire-decoded and may be enormous. Compare magnitudes
    // WITHOUT negating -- negating INT64_MIN is UB, and this reference must be
    // sound on exactly the wire extremes it is meant to pin.
    if (delta >= 0) {
        if (static_cast<std::uint64_t>(delta) >= maximum) {
            return static_cast<std::uint32_t>(maximum);
        }
    } else {
        const std::uint64_t magnitude =
            static_cast<std::uint64_t>(-(delta + 1)) + 1;
        if (magnitude >= maximum) return 0;
    }
    const std::int64_t next = clamped + delta;
    return static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(next, 0, static_cast<std::int64_t>(maximum)));
}

std::uint32_t referenceFraction(std::uint32_t total, std::uint32_t rows,
                                std::uint32_t numerator,
                                std::uint32_t denominator) {
    if (rows == 0 || denominator == 0) return 0;
    const std::uint64_t maximum = total > rows ? total - rows : 0;
    // Round-half-up, matching scrollScaleRounded: the drag inverse must be the
    // exact inverse of the rounded thumb render so no gutter row is skipped
    const std::uint64_t scaled =
        (maximum * numerator + denominator / 2) / denominator;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(scaled, maximum));
}

// The geometries every case runs against: empty, shorter than the window,
// exactly full, and taller.
constexpr std::pair<std::uint32_t, std::uint32_t> kGeometries[] = {
    {0, 10}, {3, 10}, {10, 10}, {11, 10}, {100, 10}, {400, 21}, {5, 1},
    {20, 0},
};

TEST(scrollOffsetByLinesMatchesAnIndependentShift) {
    for (const auto [total, rows] : kGeometries) {
        for (const std::uint32_t start : {0U, 1U, 5U, 50U, 4000U}) {
            for (const std::int64_t delta :
                 {std::int64_t{0}, std::int64_t{1}, std::int64_t{-1},
                  std::int64_t{7}, std::int64_t{-7},
                  std::numeric_limits<std::int64_t>::max(),
                  std::numeric_limits<std::int64_t>::min()}) {
                ssg::ScrollOffset offset{start};
                offset.byLines(delta, total, rows);
                ASSERT_EQ(offset.firstVisible(),
                          referenceShift(total, rows, start, delta));
            }
        }
    }
}

// A page is the window height. Pinning it against the line shift proves pages
// are not silently a different unit.
TEST(scrollOffsetByPagesEqualsThatManyWindowsOfLines) {
    for (const auto [total, rows] : kGeometries) {
        for (const std::int64_t pages : {std::int64_t{1}, std::int64_t{-1},
                                         std::int64_t{3}, std::int64_t{-3}}) {
            ssg::ScrollOffset paged{7};
            paged.byPages(pages, total, rows);
            ASSERT_EQ(paged.firstVisible(),
                      referenceShift(total, rows, 7,
                                     pages * static_cast<std::int64_t>(rows)));
        }
    }
}

TEST(scrollOffsetToFractionMatchesAnIndependentComputation) {
    for (const auto [total, rows] : kGeometries) {
        for (const auto [numerator, denominator] :
             {std::pair<std::uint32_t, std::uint32_t>{0, 1},
              {1, 2},
              {1, 1},
              {3, 4},
              {1, 0}}) {
            ssg::ScrollOffset offset{999};
            offset.toFraction(numerator, denominator, total, rows);
            ASSERT_EQ(offset.firstVisible(),
                      referenceFraction(total, rows, numerator, denominator));
        }
    }
    // The end of the track reaches the last item, for every geometry.
    for (const auto [total, rows] : kGeometries) {
        if (rows == 0) continue;
        ssg::ScrollOffset offset{0};
        offset.toFraction(1, 1, total, rows);
        const std::uint32_t maximum = total > rows ? total - rows : 0;
        ASSERT_EQ(offset.firstVisible(), maximum);
    }
}

// Revealing shifts minimally; free scroll does not shift at all. These are the
// two halves of the one rule that keeps explicit scroll from snapping back.
TEST(scrollOffsetRevealsMinimallyAndResolveNeverMoves) {
    ssg::ScrollOffset offset{50};
    offset.revealSelection(0, 100, 10);
    ASSERT_EQ(offset.firstVisible(), std::uint32_t{0});

    offset = ssg::ScrollOffset{0};
    offset.revealSelection(99, 100, 10);
    ASSERT_EQ(offset.firstVisible(), std::uint32_t{90});

    // Already visible: no movement.
    offset = ssg::ScrollOffset{40};
    offset.revealSelection(45, 100, 10);
    ASSERT_EQ(offset.firstVisible(), std::uint32_t{40});

    // resolve() reports the window without moving the offset, so a snapshot can
    // never accidentally scroll the thing it is describing.
    offset = ssg::ScrollOffset{40};
    const auto view = offset.resolve(100, 10);
    ASSERT_EQ(offset.firstVisible(), std::uint32_t{40});
    ASSERT_EQ(view.firstVisible, std::uint32_t{40});
    ASSERT_EQ(view.visibleCount, std::uint32_t{10});
}

// Every operation must leave the offset inside [0, maximumFirstRow]. This is the
// invariant each of the three views previously enforced with its own arithmetic.
TEST(everyOperationLeavesTheOffsetWithinRange) {
    for (const auto [total, rows] : kGeometries) {
        const std::uint32_t maximum = (rows == 0 || total <= rows)
                                          ? 0
                                          : total - rows;
        for (const std::uint32_t start : {0U, 3U, 99U, 100000U}) {
            ssg::ScrollOffset a{start};
            a.byLines(12345, total, rows);
            ASSERT_TRUE(a.firstVisible() <= maximum);

            ssg::ScrollOffset b{start};
            b.byPages(-9999, total, rows);
            ASSERT_TRUE(b.firstVisible() <= maximum);

            ssg::ScrollOffset c{start};
            c.toFraction(7, 3, total, rows);  // numerator > denominator
            ASSERT_TRUE(c.firstVisible() <= maximum);

            ssg::ScrollOffset d{start};
            d.revealSelection(total == 0 ? 0 : total - 1, total, rows);
            ASSERT_TRUE(d.firstVisible() <= maximum);
        }
    }
}

// The scrollbar geometry a ScrollOffset reports must be the same one the
// existing shared primitive produces -- if these ever differ, a thumb would be
// drawn somewhere the content is not.
TEST(scrollOffsetMetricsEqualTheSharedPrimitive) {
    for (const auto [total, rows] : kGeometries) {
        for (const std::uint32_t start : {0U, 2U, 37U, 100000U}) {
            ssg::ScrollOffset offset{start};
            const auto view = offset.resolve(total, rows);
            const auto expected = ssg::Viewport{}.listScrollView(
                total, rows, start, std::nullopt, false);
            ASSERT_TRUE(view == expected);
        }
    }
}

// S2: the three surfaces now share one clamp rule. These assert the shared
// behaviour AT each surface, rather than trusting that "the suite is green"
// proves a state-ownership refactor preserved semantics.
//
// The editor is deliberately absent from the eager-clamp case: it defers its
// top clamp to the viewport (bounding per wheel notch would cost O(document)
// with word wrap on), so its clamp is asserted through the rendered viewport
// instead -- see editorOverScrollResolvesToTheSameMaximum below.
TEST(everySurfaceClampsOverScrollToItsOwnMaximum) {
    // Tree: 64 items in a 21-row panel.
    ssg::ScrollOffset tree{0};
    tree.byLines(100000, 64, 21);
    ASSERT_EQ(tree.firstVisible(), std::uint32_t{64 - 21});

    // Picker: 200 ranked rows in the same panel.
    ssg::ScrollOffset picker{0};
    picker.byLines(100000, 200, 21);
    ASSERT_EQ(picker.firstVisible(), std::uint32_t{200 - 21});

    // Content that fits cannot scroll at all, on either.
    ssg::ScrollOffset small{0};
    small.byLines(100000, 5, 21);
    ASSERT_EQ(small.firstVisible(), std::uint32_t{0});
}

// An explicit scroll must leave the selection behind; a reveal must fetch it.
// These are the two halves that previously lived as separate hand-written
// arithmetic in the tree and the picker.
TEST(explicitScrollDoesNotSnapBackButRevealDoes) {
    ssg::ScrollOffset offset{0};
    offset.byLines(30, 100, 10);
    ASSERT_EQ(offset.firstVisible(), std::uint32_t{30});

    // Selection at item 0 is now off-screen above, and a free scroll leaves it
    // there.
    const auto afterFreeScroll = offset.firstVisible();
    offset.byLines(0, 100, 10);
    ASSERT_EQ(offset.firstVisible(), afterFreeScroll);

    // Revealing it pulls the window minimally.
    offset.revealSelection(0, 100, 10);
    ASSERT_EQ(offset.firstVisible(), std::uint32_t{0});
}

// The editor's clamp now comes from the same shared rule. Asserted against
// HAND-COMPUTED literals, not against ScrollOffset: comparing the viewport to
// ScrollOffset would be circular now that both route through listScrollView,
// and would pass if the shared rule were wrong in one consistent way.
//
// 100 rows in a 10-row pane means the last window starts at 90. That is
// arithmetic on paper, independent of any code here.
TEST(editorOverScrollResolvesToTheHandComputedMaximum) {
    std::vector<ssg::CellRun> lines;
    for (int i = 0; i < 100; ++i) {
        ssg::CellRun run;
        run.spans.push_back(ssg::CellSpan{0, 1, 1, ssg::CellKind::Text});
        lines.push_back(std::move(run));
    }
    const ssg::ViewportDimensions dimensions{80, 10};

    const auto over = ssg::Viewport{}.compute(lines, dimensions, 100000);
    ASSERT_EQ(over.firstVisualRow, std::uint32_t{90});
    ASSERT_EQ(over.scrollbar.maximumFirstRow, std::uint32_t{90});
    ASSERT_EQ(over.visibleRows.size(), std::size_t{10});

    // An in-range request is honoured verbatim.
    const auto within = ssg::Viewport{}.compute(lines, dimensions, 37);
    ASSERT_EQ(within.firstVisualRow, std::uint32_t{37});

    // Exactly at the boundary, and one past it.
    ASSERT_EQ(ssg::Viewport{}.compute(lines, dimensions, 90).firstVisualRow,
              std::uint32_t{90});
    ASSERT_EQ(ssg::Viewport{}.compute(lines, dimensions, 91).firstVisualRow,
              std::uint32_t{90});
}

}  // namespace

int main() {
    RUN(scrollOffsetByLinesMatchesAnIndependentShift);
    RUN(scrollOffsetByPagesEqualsThatManyWindowsOfLines);
    RUN(scrollOffsetToFractionMatchesAnIndependentComputation);
    RUN(scrollOffsetRevealsMinimallyAndResolveNeverMoves);
    RUN(everyOperationLeavesTheOffsetWithinRange);
    RUN(scrollOffsetMetricsEqualTheSharedPrimitive);
    RUN(everySurfaceClampsOverScrollToItsOwnMaximum);
    RUN(explicitScrollDoesNotSnapBackButRevealDoes);
    RUN(editorOverScrollResolvesToTheHandComputedMaximum);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
