#include "ssg/Layout.h"
#include "test_helpers.h"

#include <optional>
#include <string>
#include <vector>

namespace {

using namespace ssg;

LayoutNode leaf(std::string id, Size size) {
    return LayoutNode{std::move(id), ShellNodeKind::Pane, size, Axis::Column, {}, {}};
}

const SolvedBox& box(const SolvedLayout& layout, std::string_view id) {
    const auto* found = layout.find(id);
    ASSERT_TRUE(found != nullptr);
    static SolvedBox empty{};
    return found ? *found : empty;
}

// A vertical stack: fixed header, flex body, fixed footer. Hand-computed.
TEST(columnStackPlacesExactThenFillsFlex) {
    LayoutNode root{
        "root", std::nullopt, Size::flex(), Axis::Column, {},
        {leaf("header", Size::exact(1)), leaf("body", Size::flex()),
         leaf("footer", Size::exact(1))}};
    auto solved = solveLayout(root, {0, 0, 10, 5});
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
    LayoutNode root{"root", std::nullopt, Size::flex(), Axis::Row, {},
                    {leaf("left", Size::flex()), leaf("right", Size::flex())}};
    auto solved = solveLayout(root, {0, 0, 11, 4});
    ASSERT_TRUE(solved.has_value());
    if (!solved) return;
    ASSERT_EQ(box(*solved, "left").rect, (Rect{0, 0, 5, 4}));   // 11/2 == 5
    ASSERT_EQ(box(*solved, "right").rect, (Rect{5, 0, 6, 4}));  // remainder to last
}

// A single flex child takes the whole remainder after the exact sibling.
TEST(singleFlexTakesAllRemainder) {
    LayoutNode root{"root", std::nullopt, Size::flex(), Axis::Row, {},
                    {leaf("bar", Size::exact(20)), leaf("content", Size::flex())}};
    auto solved = solveLayout(root, {0, 0, 80, 24});
    ASSERT_TRUE(solved.has_value());
    if (!solved) return;
    ASSERT_EQ(box(*solved, "bar").rect, (Rect{0, 0, 20, 24}));
    ASSERT_EQ(box(*solved, "content").rect, (Rect{20, 0, 60, 24}));
}

// Inset reserves cells on all four edges before children are laid out; the child
// fills the content rect (frame minus inset).
TEST(insetReservesTheFrameOnEveryEdge) {
    LayoutNode root{
        "box", std::nullopt, Size::flex(), Axis::Column, {2, 3, 1, 4},
        {leaf("inner", Size::flex())}};
    auto solved = solveLayout(root, {0, 0, 20, 20});
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
        "outer", std::nullopt, Size::flex(), Axis::Row, {1, 1, 1, 1},
        {leaf("a", Size::exact(4)), leaf("b", Size::flex())}};
    auto solved = solveLayout(root, {0, 0, 10, 6});
    ASSERT_TRUE(solved.has_value());
    if (!solved) return;
    // content = {1,1,8,4}; a exact 4 at x=1; b flex 4 at x=5
    ASSERT_EQ(box(*solved, "a").rect, (Rect{1, 1, 4, 4}));
    ASSERT_EQ(box(*solved, "b").rect, (Rect{5, 1, 4, 4}));
}

// When the Exact children exceed the available extent, the layout does not fit:
// solveLayout returns nullopt rather than a clamped/overlapping layout.
TEST(exactOverflowReturnsFailure) {
    LayoutNode root{"root", std::nullopt, Size::flex(), Axis::Row, {},
                    {leaf("a", Size::exact(6)), leaf("b", Size::exact(6))}};
    ASSERT_FALSE(solveLayout(root, {0, 0, 10, 4}).has_value());  // 12 > 10
    // Exactly fitting is not a failure.
    ASSERT_TRUE(solveLayout(root, {0, 0, 12, 4}).has_value());
}

// Failure propagates from any depth, not just the root.
TEST(exactOverflowInAChildFails) {
    LayoutNode inner{
        "inner", std::nullopt, Size::flex(), Axis::Column, {},
        {leaf("x", Size::exact(5)), leaf("y", Size::exact(5))}};
    LayoutNode root{"root", std::nullopt, Size::flex(), Axis::Row, {},
                    {leaf("side", Size::exact(2)), std::move(inner)}};
    // root fits (2 + flex), but inner needs height 10 in a height-6 bound.
    ASSERT_FALSE(solveLayout(root, {0, 0, 20, 6}).has_value());
}

// A container of only Exact children with leftover space is NOT a failure; the
// remainder is simply unused.
TEST(zeroFlexWithLeftoverIsAllowed) {
    LayoutNode root{"root", std::nullopt, Size::flex(), Axis::Row, {},
                    {leaf("a", Size::exact(3)), leaf("b", Size::exact(3))}};
    auto solved = solveLayout(root, {0, 0, 10, 4});
    ASSERT_TRUE(solved.has_value());
    if (!solved) return;
    ASSERT_EQ(box(*solved, "a").rect, (Rect{0, 0, 3, 4}));
    ASSERT_EQ(box(*solved, "b").rect, (Rect{3, 0, 3, 4}));  // 4 cells left unused
}

// Structural nodes carry no kind; leaves carry theirs. `find` returns them all.
TEST(structuralKindIsPreservedThroughSolving) {
    LayoutNode root{"root", std::nullopt, Size::flex(), Axis::Column, {},
                    {leaf("header", Size::exact(1))}};
    auto solved = solveLayout(root, {0, 0, 8, 3});
    ASSERT_TRUE(solved.has_value());
    if (!solved) return;
    ASSERT_FALSE(box(*solved, "root").kind.has_value());
    ASSERT_TRUE(box(*solved, "header").kind.has_value());
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
    RUN(structuralKindIsPreservedThroughSolving);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
