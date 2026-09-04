#include <tui/Layout.h>
#include "ssg/WholeScreenAssembly.h"
#include "test_helpers.h"

#include <algorithm>
#include <optional>
#include <limits>
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

TEST(responsivePreferredShrinksBeforeRequiredFlex) {
    LayoutNode root{
        UiNodeId{"root"}, Size::flex(), Axis::Row, {},
        {leaf("panel", Size::optionalPreferred(24, 12)),
         leaf("content", Size::minimumFlex(20))}};

    auto wide = solveGridTree(root, {0, 0, 100, 5});
    ASSERT_TRUE(wide.has_value());
    if (wide) {
        ASSERT_EQ(box(*wide, "panel").rect, (Rect{0, 0, 24, 5}));
        ASSERT_EQ(box(*wide, "content").rect, (Rect{24, 0, 76, 5}));
    }

    auto narrow = solveGridTree(root, {0, 0, 35, 5});
    ASSERT_TRUE(narrow.has_value());
    if (narrow) {
        ASSERT_EQ(box(*narrow, "panel").rect, (Rect{0, 0, 15, 5}));
        ASSERT_EQ(box(*narrow, "content").rect, (Rect{15, 0, 20, 5}));
    }
}

TEST(responsiveOptionalChildDropsBelowCombinedFloors) {
    LayoutNode root{
        UiNodeId{"root"}, Size::flex(), Axis::Row, {},
        {leaf("panel", Size::optionalPreferred(24, 12)),
         leaf("content", Size::minimumFlex(20))}};
    auto solved = solveGridTree(root, {0, 0, 31, 5});
    ASSERT_TRUE(solved.has_value());
    if (!solved) return;
    ASSERT_TRUE(solved->find(UiNodeId{"panel"}) == nullptr);
    ASSERT_EQ(box(*solved, "content").rect, (Rect{0, 0, 31, 5}));
}

TEST(responsiveDropRecomputesGapsAndUsesReverseDeclarationOrder) {
    LayoutNode root{
        UiNodeId{"root"}, Size::flex(), Axis::Row, {}, {},
        Gap::of(2)};
    root.children = {
        leaf("left", Size::optionalPreferred(15, 5)),
        leaf("content", Size::minimumFlex(10)),
        leaf("right", Size::optionalPreferred(15, 5)),
    };

    auto oneDropped = solveGridTree(root, {0, 0, 17, 5});
    ASSERT_TRUE(oneDropped.has_value());
    if (oneDropped) {
        ASSERT_TRUE(oneDropped->find(UiNodeId{"right"}) == nullptr);
        ASSERT_EQ(box(*oneDropped, "left").rect, (Rect{0, 0, 5, 5}));
        ASSERT_EQ(box(*oneDropped, "content").rect, (Rect{7, 0, 10, 5}));
    }

    auto bothDropped = solveGridTree(root, {0, 0, 10, 5});
    ASSERT_TRUE(bothDropped.has_value());
    if (bothDropped) {
        ASSERT_TRUE(bothDropped->find(UiNodeId{"left"}) == nullptr);
        ASSERT_TRUE(bothDropped->find(UiNodeId{"right"}) == nullptr);
        ASSERT_EQ(box(*bothDropped, "content").rect, (Rect{0, 0, 10, 5}));
    }
}

TEST(responsivePreferredRangesShareScarceSpaceProportionally) {
    LayoutNode root{
        UiNodeId{"root"}, Size::flex(), Axis::Row, {},
        {leaf("left", Size::optionalPreferred(15, 5)),
         leaf("right", Size::optionalPreferred(25, 5)),
         leaf("content", Size::minimumFlex(10))}};
    auto solved = solveGridTree(root, {0, 0, 40, 5});
    ASSERT_TRUE(solved.has_value());
    if (!solved) return;
    ASSERT_EQ(box(*solved, "left").rect, (Rect{0, 0, 11, 5}));
    ASSERT_EQ(box(*solved, "right").rect, (Rect{11, 0, 19, 5}));
    ASSERT_EQ(box(*solved, "content").rect, (Rect{30, 0, 10, 5}));
}

TEST(responsiveRequiredFloorsStillFailLoudly) {
    LayoutNode root{
        UiNodeId{"root"}, Size::flex(), Axis::Row, {},
        {leaf("left", Size::minimumFlex(10)),
         leaf("right", Size::minimumFlex(10))}};
    ASSERT_FALSE(solveGridTree(root, {0, 0, 19, 5}).has_value());
    ASSERT_THROWS(Size::optionalPreferred(0, 0), std::invalid_argument);
    ASSERT_THROWS(Size::optionalPreferred(10, 11), std::invalid_argument);
    ASSERT_THROWS(Size::minimumFlex(-1), std::invalid_argument);
}

TEST(externalModificationSurfaceIsBoundedAndKeepsSelectionVisible) {
    ExternalModificationViewState external;
    external.message = "Files changed on disk";
    for (int index = 0; index < 6; ++index) {
        ExternalDocumentView file{DiffFileId{"file-" +
                                             std::to_string(index)}};
        file.path = "file-" + std::to_string(index) + ".txt";
        file.statusLabel = "M";
        file.actions.push_back(
            externalActionAffordance(ExternalAction::Reload));
        external.files.push_back(std::move(file));
    }

    external.selected = DiffFileId{"file-4"};

    ASSERT_EQ(measureExternalModificationSurface(external),
              (GridSize{1, 5}));
    const auto solved =
        solveExternalModificationSurface(external, {3, 2, 30, 5});
    ASSERT_EQ(solved.header, (Rect{3, 2, 30, 1}));
    ASSERT_EQ(solved.rows.size(), std::size_t{4});
    ASSERT_EQ(solved.rows.back().text, std::string{"+3 more"});
    ASSERT_TRUE(solved.rows[1].selected);
    ASSERT_EQ(solved.rows[1].fileId,
              std::optional<DiffFileId>{DiffFileId{"file-4"}});
    ASSERT_TRUE(!solved.rows[1].actions.empty());

    const auto tiny =
        solveExternalModificationSurface(external, {3, 2, 30, 2});
    ASSERT_EQ(tiny.rows.size(), std::size_t{1});
    ASSERT_TRUE(tiny.rows.front().selected);
    ASSERT_EQ(tiny.rows.front().fileId,
              std::optional<DiffFileId>{DiffFileId{"file-4"}});

    external.selected.reset();
    const auto unselected =
        solveExternalModificationSurface(external, {3, 2, 30, 5});
    ASSERT_TRUE(std::none_of(
        unselected.rows.begin(), unselected.rows.end(),
        [](const SolvedExternalModificationRow& row) {
            return row.selected;
        }));
}

TEST(tabBarWindowsAroundTheActiveTabAndClipsAtTheBandEdge) {
    TabViewState tabs;
    for (std::uint64_t index = 1; index <= 4; ++index) {
        TabState tab;
        tab.id = TabId{index};
        tab.label = "document-" + std::to_string(index);
        tab.dirty = index == 4;
        tabs.tabs.push_back(std::move(tab));
    }
    tabs.active = TabId{4};
    TabGlyphs glyphs;
    const auto solved = solveTabBar(tabs, glyphs, {2, 3, 18, 1});
    ASSERT_TRUE(!solved.tabs.empty());
    if (solved.tabs.empty()) return;
    ASSERT_TRUE(solved.tabs.size() < tabs.tabs.size());
    ASSERT_TRUE(solved.tabs.front().index > 0);
    ASSERT_EQ(solved.tabs.back().id, TabId{4});
    ASSERT_TRUE(solved.tabs.back().active);
    ASSERT_TRUE(solved.tabs.back().rect.right() <= solved.rect.right());

    tabs.active = TabId{1};
    const auto firstActive = solveTabBar(tabs, glyphs, {2, 3, 18, 1});
    ASSERT_TRUE(!firstActive.tabs.empty());
    if (!firstActive.tabs.empty()) {
        ASSERT_EQ(firstActive.tabs.front().index, std::size_t{0});
        ASSERT_TRUE(firstActive.tabs.front().active);
    }

    tabs.active = TabId{3};
    const auto middleActive = solveTabBar(tabs, glyphs, {2, 3, 18, 1});
    ASSERT_TRUE(std::any_of(
        middleActive.tabs.begin(), middleActive.tabs.end(),
        [](const SolvedTab& tab) { return tab.id == TabId{3} && tab.active; }));

    tabs.active = TabId{4};
    const auto clipped = solveTabBar(tabs, glyphs, {2, 3, 3, 1});
    ASSERT_EQ(clipped.tabs.size(), std::size_t{1});
    if (!clipped.tabs.empty()) {
        ASSERT_EQ(clipped.tabs.front().id, TabId{4});
        ASSERT_EQ(clipped.tabs.front().rect.width, 3);
    }
    ASSERT_TRUE(solveTabBar(tabs, glyphs, {2, 3, 18, 0}).tabs.empty());
}

TEST(paletteSurfaceCarvesOneGutterAndKeepsAbsoluteRows) {
    PaletteReport palette;
    palette.rows = {{"a", "Alpha", ""}, {"b", "Beta", ""},
                    {"c", "Gamma", ""}};
    palette.firstVisible = 20;
    palette.selected = 21;
    const auto solved = solvePaletteSurface(palette, {5, 2, 10, 2}, 1);
    ASSERT_EQ(solved.rows, (Rect{5, 2, 9, 2}));
    ASSERT_EQ(solved.scrollbar, (Rect{14, 2, 1, 2}));
    ASSERT_EQ(solved.visibleRows.size(), std::size_t{2});
    ASSERT_EQ(solved.visibleRows[0].absoluteIndex, std::uint32_t{20});
    ASSERT_FALSE(solved.visibleRows[0].selected);
    ASSERT_EQ(solved.visibleRows[1].absoluteIndex, std::uint32_t{21});
    ASSERT_TRUE(solved.visibleRows[1].selected);
}

TEST(emptyPaletteSurfaceKeepsItsBandsAndHasNoRows) {
    const auto solved =
        solvePaletteSurface(PaletteReport{}, {5, 2, 10, 3}, 2);
    ASSERT_EQ(solved.rows, (Rect{5, 2, 8, 3}));
    ASSERT_EQ(solved.scrollbar, (Rect{13, 2, 2, 3}));
    ASSERT_TRUE(solved.visibleRows.empty());
}

TEST(paletteSurfaceClipsRowsToItsHeight) {
    PaletteReport palette;
    palette.rows = {{"a", "Alpha", ""}, {"b", "Beta", ""},
                    {"c", "Gamma", ""}};
    const auto solved = solvePaletteSurface(palette, {5, 2, 10, 1}, 1);
    ASSERT_EQ(solved.visibleRows.size(), std::size_t{1});
    ASSERT_EQ(solved.visibleRows.front().rect, (Rect{5, 2, 9, 1}));
}

TEST(zeroSizePaletteSurfaceProducesNoPaintableGeometry) {
    PaletteReport palette;
    palette.rows = {{"a", "Alpha", ""}};
    const auto solved = solvePaletteSurface(palette, {5, 2, 0, 0}, 1);
    ASSERT_EQ(solved.rows, (Rect{5, 2, 0, 0}));
    ASSERT_EQ(solved.scrollbar, (Rect{5, 2, 0, 0}));
    ASSERT_TRUE(solved.visibleRows.empty());
}

TEST(panelSurfaceWithoutAProviderIsEmptyAndBounded) {
    SolvedGridNode panel{
        UiNodeId{"panel"}, {3, 2, 12, 6}, {3, 2, 12, 6},
        ScrollAxis::Vertical};
    const auto solved =
        solvePanelSurface(TreeViewState{}, panel, 99, true, Style{});
    ASSERT_EQ(solved.rect, panel.rect);
    ASSERT_EQ(solved.providerLabel, (Rect{3, 2, 12, 1}));
    ASSERT_TRUE(solved.providerText.empty());
    ASSERT_TRUE(solved.rows.empty());
    ASSERT_FALSE(solved.scrollbarGutter.has_value());
    ASSERT_EQ(solved.firstVisible, std::uint32_t{0});
}

TEST(documentSurfaceCarvesGuttersAndProtectsMinimumContentWidth) {
    SolvedGridNode viewport{
        UiNodeId{"document.viewport"}, {3, 2, 30, 6}, {3, 2, 30, 6},
        ScrollAxis::Vertical};
    const auto topology = PaneTopology::initial();
    const auto wide = solveDocumentSurface(
        viewport, topology, true, 100, StyleDimensions{});
    ASSERT_EQ(wide.rect, viewport.rect);
    ASSERT_EQ(wide.lineNumbers, (Rect{3, 2, 4, 6}));
    ASSERT_EQ(wide.content, (Rect{7, 2, 25, 6}));
    ASSERT_EQ(wide.scrollbarGutter, (Rect{32, 2, 1, 6}));

    viewport.rect.width = 21;
    viewport.content.width = 21;
    const auto narrow = solveDocumentSurface(
        viewport, topology, true, 100, StyleDimensions{});
    ASSERT_EQ(narrow.lineNumbers, (Rect{}));
    ASSERT_EQ(narrow.content, (Rect{3, 2, 20, 6}));
    ASSERT_EQ(narrow.scrollbarGutter, (Rect{23, 2, 1, 6}));
}

TEST(documentSurfaceCarvesEachSplitPaneAndIdentifiesTheActiveOne) {
    SolvedGridNode viewport{
        UiNodeId{"document.viewport"}, {0, 0, 61, 8}, {0, 0, 61, 8},
        ScrollAxis::Vertical};
    auto topology = PaneTopology::initial();
    (void)topology.splitActive(SplitAxis::Vertical);
    const auto solved = solveDocumentSurface(
        viewport, topology, true, 100, StyleDimensions{});
    ASSERT_EQ(solved.panes.size(), std::size_t{2});
    ASSERT_EQ(solved.activePaneIndex, std::size_t{1});
    ASSERT_EQ(solved.panes[0].id, PaneId{1});
    ASSERT_EQ(solved.panes[0].frame, (Rect{0, 0, 30, 8}));
    ASSERT_EQ(solved.panes[0].lineNumbers, (Rect{0, 0, 4, 8}));
    ASSERT_EQ(solved.panes[0].content, (Rect{4, 0, 25, 8}));
    ASSERT_EQ(solved.panes[1].id, PaneId{2});
    ASSERT_EQ(solved.panes[1].frame, (Rect{30, 0, 31, 8}));
    ASSERT_EQ(solved.panes[1].lineNumbers, (Rect{30, 0, 4, 8}));
    ASSERT_EQ(solved.panes[1].content, (Rect{34, 0, 26, 8}));
    ASSERT_EQ(solved.content, solved.panes.front().content);
}

TEST(documentSurfaceFallsBackToTheActivePaneWhenSplitsDoNotFit) {
    SolvedGridNode viewport{
        UiNodeId{"document.viewport"}, {2, 3, 3, 4}, {2, 3, 3, 4},
        ScrollAxis::Vertical};
    auto topology = PaneTopology::initial();
    (void)topology.splitActive(SplitAxis::Vertical);

    const auto solved = solveDocumentSurface(
        viewport, topology, false, 1, StyleDimensions{});
    ASSERT_EQ(solved.panes.size(), std::size_t{1});
    ASSERT_EQ(solved.panes.front().id, PaneId{2});
    ASSERT_EQ(solved.panes.front().frame, viewport.rect);
    ASSERT_EQ(solved.activePaneIndex, std::size_t{0});
}

TEST(directionalPaneSelectionUsesSolvedGridGeometry) {
    SolvedGridNode viewport{
        UiNodeId{"document.viewport"}, {0, 0, 80, 24}, {0, 0, 80, 24},
        ScrollAxis::Vertical};
    auto topology = PaneTopology::initial();
    (void)topology.splitActive(SplitAxis::Vertical);
    (void)topology.splitActive(SplitAxis::Horizontal);
    const auto solved = solveDocumentSurface(
        viewport, topology, false, 1, StyleDimensions{});

    ASSERT_EQ(paneInDirection(solved, PaneDirection::Up),
              std::optional<PaneId>{PaneId{2}});
    ASSERT_EQ(paneInDirection(solved, PaneDirection::Left),
              std::optional<PaneId>{PaneId{1}});
    ASSERT_EQ(paneInDirection(solved, PaneDirection::Down),
              std::optional<PaneId>{});
    ASSERT_EQ(paneInDirection(solved, PaneDirection::Right),
              std::optional<PaneId>{});
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

UiSchema validated(UiNode root) {
    UiSchema schema{std::move(root)};
    auto validation = validateUiSchema(schema);
    ASSERT_TRUE(validation.ok());
    return schema;
}

UiNode view(std::string id, Size size = Size::flex(),
            ViewSurface surface = ViewSurface::Document) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::View;
    widget.id = id;
    widget.surface = surface;
    return {UiNodeId{std::move(id)}, size, UiLeaf{std::move(widget)}};
}

TEST(uiFrameSolvesOnlyEffectivelyPresentNodes) {
    UiNode hiddenParent{
        UiNodeId{"hidden"}, Size::exact(2),
        UiContainer{Axis::Column, {}, {},
                    {view("hidden.child", Size::flex())}}};
    hiddenParent.visible = false;
    UiNode root{
        UiNodeId{"root"}, Size::flex(),
        UiContainer{Axis::Column, {}, Gap::of(1),
                    {view("header", Size::exact(1)),
                     std::move(hiddenParent), view("body", Size::flex())}}};
    auto schema = validated(std::move(root));

    auto solved = solveUiFrame(schema, {}, {0, 0, 12, 8});
    ASSERT_TRUE(solved.accepted());
    if (!solved.tree) return;
    ASSERT_EQ(solved.tree->nodes.size(), std::size_t{3});
    ASSERT_TRUE(solved.tree->find(UiNodeId{"hidden"}) == nullptr);
    ASSERT_TRUE(solved.tree->find(UiNodeId{"hidden.child"}) == nullptr);
    ASSERT_EQ(solved.tree->find(UiNodeId{"header"})->rect,
              (Rect{0, 0, 12, 1}));
    ASSERT_EQ(solved.tree->find(UiNodeId{"body"})->rect,
              (Rect{0, 2, 12, 6}));
}

TEST(uiFrameCarriesResolvedStateStyleAndScrollOwnership) {
    auto document = view("document");
    document.style.foreground = SemanticRole::Text;
    document.resolved = UiLeafState{"text", "Document", {}, {}, SemanticRole::Text};
    UiNode root{
        UiNodeId{"root"}, Size::flex(),
        UiContainer{Axis::Column, Inset::of(1, 1, 1, 1), {},
                    {std::move(document)}, ScrollAxis::Vertical}};
    root.style.background = SemanticRole::Canvas;
    auto schema = validated(std::move(root));

    auto solved = solveUiFrame(schema, {}, {0, 0, 10, 6});
    ASSERT_TRUE(solved.accepted());
    if (!solved.tree) return;
    const auto* rootNode = solved.tree->find(UiNodeId{"root"});
    const auto* documentNode = solved.tree->find(UiNodeId{"document"});
    ASSERT_TRUE(rootNode != nullptr && documentNode != nullptr);
    if (!rootNode || !documentNode) return;
    ASSERT_EQ(rootNode->scroll, ScrollAxis::Vertical);
    ASSERT_EQ(documentNode->rect, (Rect{1, 1, 8, 4}));
    ASSERT_EQ(documentNode->style.background,
              std::optional{SemanticRole::Canvas});
    ASSERT_EQ(documentNode->style.foreground,
              std::optional{SemanticRole::Text});
    ASSERT_TRUE(documentNode->leafState.has_value());
    ASSERT_TRUE(documentNode->widget.has_value());
}

TEST(uiFrameResolvesAutoLeavesFromIntrinsicSizes) {
    UiNode root{
        UiNodeId{"root"}, Size::flex(),
        UiContainer{Axis::Row, {}, Gap::of(1),
                    {view("auto", Size::autoSize(), ViewSurface::Notice),
                     view("rest", Size::flex())}}};
    auto schema = validated(std::move(root));
    auto solved = solveUiFrame(schema, {{UiNodeId{"auto"}, GridSize{4, 2}}},
                               {0, 0, 10, 3});
    ASSERT_TRUE(solved.accepted());
    if (!solved.tree) return;
    ASSERT_EQ(solved.tree->find(UiNodeId{"auto"})->rect,
              (Rect{0, 0, 4, 3}));
    ASSERT_EQ(solved.tree->find(UiNodeId{"rest"})->rect,
              (Rect{5, 0, 5, 3}));
}

TEST(uiFrameRejectsUnrepresentableIntrinsicExtent) {
    UiNode root{
        UiNodeId{"root"}, Size::flex(),
        UiContainer{
            Axis::Row, {}, {},
            {view("a", Size::exact(std::numeric_limits<int>::max())),
             view("b", Size::exact(std::numeric_limits<int>::max()))}}};
    auto schema = validated(std::move(root));
    auto solved = solveUiFrame(schema, {}, {0, 0, 8, 4});
    ASSERT_FALSE(solved.accepted());
    ASSERT_FALSE(solved.error.empty());
}

TEST(generatedWholeScreenSolvesEveryPresentNodeExactlyOnce) {
    UiSchema schema{
        assembleWholeScreen("help.open", StyleDimensions{}, "> ")
            .root};
    auto validation = validateUiSchema(schema);
    ASSERT_TRUE(validation.ok());
    if (!validation.ok()) return;
    std::vector<GridIntrinsicSize> intrinsic;
    const auto collect = [&](const auto& self, const UiNode& node) -> void {
        if (node.size.kind() == SizeKind::Auto && node.isLeaf()) {
            intrinsic.push_back({node.id, {1, 1}});
        }
        if (const auto* container = std::get_if<UiContainer>(&node.content)) {
            for (const auto& child : container->children) self(self, child);
        }
    };
    collect(collect, schema.root);

    auto solved = solveUiFrame(schema, intrinsic, {0, 0, 200, 80});
    ASSERT_TRUE(solved.accepted());
    if (!solved.tree) return;
    const auto nodeIds = uiSchemaNodeIds(schema);
    ASSERT_EQ(solved.tree->nodes.size(), nodeIds.size());
    std::set<UiNodeId> solvedIds;
    for (const auto& node : solved.tree->nodes) {
        ASSERT_TRUE(solvedIds.insert(node.id).second);
    }
    ASSERT_EQ(solvedIds, nodeIds);
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

SSG_TEST_SUITE(test_layout_solver) {
    RUN(columnStackPlacesExactThenFillsFlex);
    RUN(responsivePreferredShrinksBeforeRequiredFlex);
    RUN(responsiveOptionalChildDropsBelowCombinedFloors);
    RUN(responsiveDropRecomputesGapsAndUsesReverseDeclarationOrder);
    RUN(responsivePreferredRangesShareScarceSpaceProportionally);
    RUN(responsiveRequiredFloorsStillFailLoudly);
    RUN(externalModificationSurfaceIsBoundedAndKeepsSelectionVisible);
    RUN(tabBarWindowsAroundTheActiveTabAndClipsAtTheBandEdge);
    RUN(paletteSurfaceCarvesOneGutterAndKeepsAbsoluteRows);
    RUN(emptyPaletteSurfaceKeepsItsBandsAndHasNoRows);
    RUN(paletteSurfaceClipsRowsToItsHeight);
    RUN(zeroSizePaletteSurfaceProducesNoPaintableGeometry);
    RUN(panelSurfaceWithoutAProviderIsEmptyAndBounded);
    RUN(documentSurfaceCarvesGuttersAndProtectsMinimumContentWidth);
    RUN(documentSurfaceCarvesEachSplitPaneAndIdentifiesTheActiveOne);
    RUN(documentSurfaceFallsBackToTheActivePaneWhenSplitsDoNotFit);
    RUN(directionalPaneSelectionUsesSolvedGridGeometry);
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
    RUN(uiFrameSolvesOnlyEffectivelyPresentNodes);
    RUN(uiFrameCarriesResolvedStateStyleAndScrollOwnership);
    RUN(uiFrameResolvesAutoLeavesFromIntrinsicSizes);
    RUN(uiFrameRejectsUnrepresentableIntrinsicExtent);
    RUN(generatedWholeScreenSolvesEveryPresentNodeExactlyOnce);
    RUN(constraintsRejectNegativeGeometryAtConstruction);
    RUN(solveGridTreeRejectsAutoSizeDistinctly);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
