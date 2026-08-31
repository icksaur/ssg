#include "ssg/Layout.h"
#include "test_helpers.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ssg;

LayoutNode leaf(std::string id, Size size) {
    return LayoutNode{UiNodeId{std::move(id)}, size};
}

const SolvedGridNode& box(const SolvedGridTree& layout, std::string id) {
    const auto* found = layout.find(UiNodeId{std::move(id)});
    ASSERT_TRUE(found != nullptr);
    static SolvedGridNode empty{};
    return found ? *found : empty;
}

// A vertical stack: fixed header, flex body, fixed footer. Hand-computed.
TEST(columnStackPlacesExactThenFillsFlex) {
    LayoutNode root{
        UiNodeId{"root"}, Size::flex(), Axis::Column, {},
        {leaf("header", Size::exact(1)), leaf("body", Size::flex()),
         leaf("footer", Size::exact(1))}};
    auto solved = solveGridTree(root, {0, 0, 10, 5});
    ASSERT_TRUE(solved.has_value());
    if (!solved) return;
    ASSERT_EQ(box(*solved, "root").rect, (Rect{0, 0, 10, 5}));
    ASSERT_EQ(box(*solved, "header").rect, (Rect{0, 0, 10, 1}));
    ASSERT_EQ(box(*solved, "body").rect, (Rect{0, 1, 10, 3}));
    ASSERT_EQ(box(*solved, "footer").rect, (Rect{0, 4, 10, 1}));
}

// Two flex siblings split the width equally; the odd cell goes to the LAST child
// (reproduces the old pane rule `rect.width - firstWidth`).
TEST(rowFlexSplitsEquallyRemainderToLast) {
    LayoutNode root{UiNodeId{"root"}, Size::flex(), Axis::Row, {},
                    {leaf("left", Size::flex()), leaf("right", Size::flex())}};
    auto solved = solveGridTree(root, {0, 0, 11, 4});
    ASSERT_TRUE(solved.has_value());
    if (!solved) return;
    ASSERT_EQ(box(*solved, "left").rect, (Rect{0, 0, 5, 4}));   // 11/2 == 5
    ASSERT_EQ(box(*solved, "right").rect, (Rect{5, 0, 6, 4}));  // remainder to last
}

// A single flex child takes the whole remainder after the exact sibling.
TEST(singleFlexTakesAllRemainder) {
    LayoutNode root{UiNodeId{"root"}, Size::flex(), Axis::Row, {},
                    {leaf("bar", Size::exact(20)), leaf("content", Size::flex())}};
    auto solved = solveGridTree(root, {0, 0, 80, 24});
    ASSERT_TRUE(solved.has_value());
    if (!solved) return;
    ASSERT_EQ(box(*solved, "bar").rect, (Rect{0, 0, 20, 24}));
    ASSERT_EQ(box(*solved, "content").rect, (Rect{20, 0, 60, 24}));
}

// Inset reserves cells on all four edges before children are laid out; the child
// fills the content rect (frame minus inset).
TEST(insetReservesTheFrameOnEveryEdge) {
    LayoutNode root{
        UiNodeId{"box"}, Size::flex(), Axis::Column,
        Inset::of(2, 3, 1, 4),
        {leaf("inner", Size::flex())}};
    auto solved = solveGridTree(root, {0, 0, 20, 20});
    ASSERT_TRUE(solved.has_value());
    if (!solved) return;
    ASSERT_EQ(box(*solved, "box").rect, (Rect{0, 0, 20, 20}));  // full frame
    // content = x+2, y+1, w-2-3, h-1-4 = {2,1,15,15}
    ASSERT_EQ(box(*solved, "inner").rect, (Rect{2, 1, 15, 15}));
}

// A vertical inset on a row container reserves height; the horizontal children
// share the inset-reduced width and sit at the inset top.
TEST(insetAppliesBeforeChildAxisDistribution) {
    LayoutNode root{
        UiNodeId{"outer"}, Size::flex(), Axis::Row,
        Inset::of(1, 1, 1, 1),
        {leaf("a", Size::exact(4)), leaf("b", Size::flex())}};
    auto solved = solveGridTree(root, {0, 0, 10, 6});
    ASSERT_TRUE(solved.has_value());
    if (!solved) return;
    // content = {1,1,8,4}; a exact 4 at x=1; b flex 4 at x=5
    ASSERT_EQ(box(*solved, "a").rect, (Rect{1, 1, 4, 4}));
    ASSERT_EQ(box(*solved, "b").rect, (Rect{5, 1, 4, 4}));
}

// When the Exact children exceed the available extent, the layout does not fit:
// solveGridTree returns nullopt rather than a clamped/overlapping layout.
TEST(exactOverflowReturnsFailure) {
    LayoutNode root{UiNodeId{"root"}, Size::flex(), Axis::Row, {},
                    {leaf("a", Size::exact(6)), leaf("b", Size::exact(6))}};
    ASSERT_FALSE(solveGridTree(root, {0, 0, 10, 4}).has_value());  // 12 > 10
    // Exactly fitting is not a failure.
    ASSERT_TRUE(solveGridTree(root, {0, 0, 12, 4}).has_value());
}

// Failure propagates from any depth, not just the root.
TEST(exactOverflowInAChildFails) {
    LayoutNode inner{
        UiNodeId{"inner"}, Size::flex(), Axis::Column, {},
        {leaf("x", Size::exact(5)), leaf("y", Size::exact(5))}};
    LayoutNode root{UiNodeId{"root"}, Size::flex(), Axis::Row, {},
                    {leaf("side", Size::exact(2)), std::move(inner)}};
    // root fits (2 + flex), but inner needs height 10 in a height-6 bound.
    ASSERT_FALSE(solveGridTree(root, {0, 0, 20, 6}).has_value());
}

// A container of only Exact children with leftover space is NOT a failure; the
// remainder is simply unused.
TEST(zeroFlexWithLeftoverIsAllowed) {
    LayoutNode root{UiNodeId{"root"}, Size::flex(), Axis::Row, {},
                    {leaf("a", Size::exact(3)), leaf("b", Size::exact(3))}};
    auto solved = solveGridTree(root, {0, 0, 10, 4});
    ASSERT_TRUE(solved.has_value());
    if (!solved) return;
    ASSERT_EQ(box(*solved, "a").rect, (Rect{0, 0, 3, 4}));
    ASSERT_EQ(box(*solved, "b").rect, (Rect{3, 0, 3, 4}));  // 4 cells left unused
}

TEST(nodeIdentityIsPreservedThroughSolving) {
    LayoutNode root{UiNodeId{"root"}, Size::flex(), Axis::Column, {},
                    {leaf("header", Size::exact(1))}};
    auto solved = solveGridTree(root, {0, 0, 8, 3});
    ASSERT_TRUE(solved.has_value());
    if (!solved) return;
    ASSERT_EQ(box(*solved, "root").id, UiNodeId{"root"});
    ASSERT_EQ(box(*solved, "header").id, UiNodeId{"header"});
}

TEST(gapSeparatesChildrenBeforeFlexDistribution) {
    LayoutNode root{UiNodeId{"root"}, Size::flex(), Axis::Row, {},
                    {leaf("fixed", Size::exact(3)),
                     leaf("first", Size::flex()),
                     leaf("second", Size::flex())},
                    Gap::of(2)};
    auto solved = solveGridTree(root, {0, 0, 15, 4});
    ASSERT_TRUE(solved.has_value());
    if (!solved) return;
    ASSERT_EQ(box(*solved, "fixed").rect, (Rect{0, 0, 3, 4}));
    ASSERT_EQ(box(*solved, "first").rect, (Rect{5, 0, 4, 4}));
    ASSERT_EQ(box(*solved, "second").rect, (Rect{11, 0, 4, 4}));
}

TEST(scrollOwnershipSurvivesSolving) {
    LayoutNode root{UiNodeId{"root"}, Size::flex(), Axis::Column, {},
                    {leaf("content", Size::flex())}, {},
                    ScrollAxis::Vertical};
    auto solved = solveGridTree(root, {2, 3, 10, 6});
    ASSERT_TRUE(solved.has_value());
    if (!solved) return;
    ASSERT_EQ(box(*solved, "root").scroll, ScrollAxis::Vertical);
    ASSERT_EQ(box(*solved, "root").content, (Rect{2, 3, 10, 6}));
    ASSERT_EQ(box(*solved, "content").scroll, ScrollAxis::None);
}

TEST(insetAndGapOverflowReturnFailure) {
    LayoutNode inset{UiNodeId{"inset"}, Size::flex(), Axis::Row,
                     Inset::of(4, 4, 0, 0),
                     {leaf("content", Size::flex())}};
    ASSERT_FALSE(solveGridTree(inset, {0, 0, 6, 2}).has_value());

    LayoutNode gaps{UiNodeId{"gaps"}, Size::flex(), Axis::Row, {},
                    {leaf("a", Size::exact(2)),
                     leaf("b", Size::exact(2))},
                    Gap::of(4)};
    ASSERT_FALSE(solveGridTree(gaps, {0, 0, 7, 2}).has_value());
}

TEST(invalidNodeIdentityIsRejected) {
    LayoutNode empty{UiNodeId{}, Size::flex()};
    ASSERT_THROWS(solveGridTree(empty, {0, 0, 4, 2}),
                  std::invalid_argument);

    LayoutNode duplicate{
        UiNodeId{"root"}, Size::flex(), Axis::Row, {},
        {leaf("same", Size::flex()), leaf("same", Size::flex())}};
    ASSERT_THROWS(solveGridTree(duplicate, {0, 0, 4, 2}),
                  std::invalid_argument);
}

// The lifted constraint vocabulary excludes invalid geometry at construction: a
// negative extent or inset edge would make the solver emit a negative or enlarged
// rectangle, so it can never be built.
TEST(constraintsRejectNegativeGeometryAtConstruction) {
    bool sizeThrew = false;
    try {
        (void)Size::exact(-1);
    } catch (const std::invalid_argument&) {
        sizeThrew = true;
    }
    ASSERT_TRUE(sizeThrew);

    bool insetThrew = false;
    try {
        (void)Inset::of(0, -1, 0, 0);
    } catch (const std::invalid_argument&) {
        insetThrew = true;
    }
    ASSERT_TRUE(insetThrew);
}

// The grid box solver does not support Auto (content) sizing; handing it an Auto
// child is a misuse that fails distinctly, not the nullopt that means "no fit".
TEST(solveGridTreeRejectsAutoSizeDistinctly) {
    LayoutNode root{UiNodeId{"root"}, Size::flex(), Axis::Row, {},
                    {leaf("a", Size::autoSize())}};
    bool threw = false;
    try {
        (void)solveGridTree(root, {0, 0, 10, 1});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

}  // namespace

int main() {
    RUN(columnStackPlacesExactThenFillsFlex);
    RUN(rowFlexSplitsEquallyRemainderToLast);
    RUN(singleFlexTakesAllRemainder);
    RUN(insetReservesTheFrameOnEveryEdge);
    RUN(insetAppliesBeforeChildAxisDistribution);
    RUN(exactOverflowReturnsFailure);
    RUN(exactOverflowInAChildFails);
    RUN(zeroFlexWithLeftoverIsAllowed);
    RUN(nodeIdentityIsPreservedThroughSolving);
    RUN(gapSeparatesChildrenBeforeFlexDistribution);
    RUN(scrollOwnershipSurvivesSolving);
    RUN(insetAndGapOverflowReturnFailure);
    RUN(invalidNodeIdentityIsRejected);
    RUN(constraintsRejectNegativeGeometryAtConstruction);
    RUN(solveGridTreeRejectsAutoSizeDistinctly);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
