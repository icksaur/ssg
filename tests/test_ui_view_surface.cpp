#include <ssg/UiRegionProjection.h>
#include <ssg/UiTree.h>
#include <ssg/Widget.h>
#include "test_helpers.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

// A projected item's fields, without the production accessibility-tree
// vocabulary these tests do not otherwise exercise.
struct ProjectedItem {
    std::string id;
    std::string label;
    ssg::Rect rect;
    ssg::SemanticRole role = ssg::SemanticRole::Canvas;
    std::string content;
    std::optional<std::string> commandId;
};

ssg::UiRegionProjectionResult projectRegionForTest(
    const ssg::UiNode& region, ssg::Rect rect,
    ssg::SemanticRole role, const ssg::Style& style,
    std::vector<ProjectedItem>& out) {
    ssg::SolvedUiRegion solved;
    auto result = ssg::projectUiRegion(region, rect, role, style, solved);
    for (const auto& item : solved.items) {
        out.push_back({item.id, item.label, item.rect, item.role,
                       item.content, item.command});
    }
    return result;
}

using ssg::Axis;
using ssg::Size;
using ssg::UiContainer;
using ssg::UiLeaf;
using ssg::UiNode;
using ssg::UiNodeId;
using ssg::ViewSurface;
using ssg::WidgetDescriptor;
using ssg::WidgetKind;

// Grid UI-region projection cannot render an opaque View; a View reaching it is
// a loud conformance failure, never silent empty content. A left/right leaf must be
// Auto-sized while a View must be Exact/Flex, so the only View shape reachable
// through a header/footer region is the Flex/Exact center -- exercise that.
TEST(gridUiRegionProjectionRefusesAViewCenter) {
    WidgetDescriptor view;
    view.kind = WidgetKind::View;
    view.id = "footer.middle.0";
    view.surface = ViewSurface::Tree;

    // Row[ left(Auto), middle(Flex) with a Flex View center, right(Auto) ].
    UiContainer left{Axis::Row, {}, {}, {}};
    UiContainer middle{Axis::Row, {}, {},
                       {UiNode{UiNodeId{"footer.middle.0"}, Size::flex(),
                               UiLeaf{view}}}};
    UiContainer right{Axis::Row, {}, {}, {}};
    UiContainer root{
        Axis::Row, {}, {},
        {UiNode{UiNodeId{"footer.left"}, Size::autoSize(), std::move(left)},
         UiNode{UiNodeId{"footer.middle"}, Size::flex(), std::move(middle)},
         UiNode{UiNodeId{"footer.right"}, Size::autoSize(), std::move(right)}}};
    UiNode regionRoot{UiNodeId{"footer"}, Size::flex(), std::move(root)};

    std::vector<ProjectedItem> out;
    const auto result = projectRegionForTest(
        regionRoot, {0, 0, 100, 1},
        ssg::SemanticRole::Footer, ssg::Style{}, out);
    ASSERT_TRUE(!result.ok());
}

TEST(gridUiRegionProjectionRefusesMalformedShape) {
    UiContainer root{Axis::Column, {}, {}, {}};
    UiNode malformed{UiNodeId{"footer"}, Size::flex(), std::move(root)};
    std::vector<ProjectedItem> out;
    const auto result = projectRegionForTest(
        malformed, {0, 0, 100, 1},
        ssg::SemanticRole::Footer, ssg::Style{}, out);
    ASSERT_TRUE(!result.ok());
    ASSERT_EQ(result.error, std::string{"UI region root must be a Row container"});
    ASSERT_TRUE(out.empty());
}

}  // namespace

SSG_TEST_SUITE(test_ui_view_surface) {
    RUN(gridUiRegionProjectionRefusesAViewCenter);
    RUN(gridUiRegionProjectionRefusesMalformedShape);
    return failed == 0 ? 0 : 1;
}
