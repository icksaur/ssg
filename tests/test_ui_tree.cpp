// Oracle for the UI-VM tree schema validator (spec §Three layers: schema). The
// validator is the precondition every later consumer relies on: node ids are
// unique within a generation and region roles do not repeat. These tests are the
// independently-knowable answers (which schema is well-formed) written against
// the rules, not the implementation.

#include "ssg/UiTree.h"
#include "test_helpers.h"

#include <string>
#include <variant>
#include <vector>

namespace {

using ssg::Axis;
using ssg::Generation;
using ssg::RegionRole;
using ssg::Size;
using ssg::UiContainer;
using ssg::UiLeaf;
using ssg::UiNode;
using ssg::UiNodeId;
using ssg::UiRegion;
using ssg::UiSchema;
using ssg::validateUiSchema;
using ssg::WidgetDescriptor;
using ssg::WidgetKind;

UiNode leaf(std::string id, WidgetKind kind = WidgetKind::Label) {
    WidgetDescriptor widget;
    widget.kind = kind;
    widget.id = id;
    return UiNode{UiNodeId{std::move(id)}, Size::flex(), UiLeaf{std::move(widget)}};
}

UiNode container(std::string id, std::vector<UiNode> children) {
    return UiNode{UiNodeId{std::move(id)}, Size::flex(),
                  UiContainer{Axis::Row, {}, {}, std::move(children)}};
}

UiNode viewLeaf(std::string id, ssg::ViewSurface surface, Size size) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::View;
    widget.id = id;
    widget.surface = surface;
    return UiNode{UiNodeId{std::move(id)}, size, UiLeaf{std::move(widget)}};
}

TEST(wellFormedSchemaValidates) {
    UiSchema schema;
    schema.generation = Generation{1};
    schema.regions = {
        UiRegion{RegionRole::Top,
                 container("header", {leaf("path"), leaf("branch")})},
        UiRegion{RegionRole::Bottom, container("footer", {leaf("hint")})},
    };
    ASSERT_TRUE(validateUiSchema(schema).ok());
}

// A node id must be unique across the WHOLE generation, not merely among
// siblings: the id addresses patches/state/focus, so a collision anywhere is a
// rejection.
TEST(duplicateNodeIdAcrossRegionsIsRejected) {
    UiSchema schema;
    schema.regions = {
        UiRegion{RegionRole::Top, container("root_a", {leaf("shared")})},
        UiRegion{RegionRole::Bottom, container("root_b", {leaf("shared")})},
    };
    const auto result = validateUiSchema(schema);
    ASSERT_TRUE(!result.ok());
}

TEST(duplicateNodeIdAmongSiblingsIsRejected) {
    UiSchema schema;
    schema.regions = {
        UiRegion{RegionRole::Top,
                 container("root", {leaf("dup"), leaf("dup")})},
    };
    ASSERT_TRUE(!validateUiSchema(schema).ok());
}

TEST(emptyNodeIdIsRejected) {
    UiSchema schema;
    schema.regions = {UiRegion{RegionRole::Top, container("root", {leaf("")})}};
    ASSERT_TRUE(!validateUiSchema(schema).ok());
}

// A region role may root only one tree; two Top regions is malformed.
TEST(duplicateRegionRoleIsRejected) {
    UiSchema schema;
    schema.regions = {
        UiRegion{RegionRole::Top, container("a", {})},
        UiRegion{RegionRole::Top, container("b", {})},
    };
    ASSERT_TRUE(!validateUiSchema(schema).ok());
}

TEST(emptySchemaValidates) {
    ASSERT_TRUE(validateUiSchema(UiSchema{}).ok());
}

// A node is exactly one of container or leaf, by construction (the variant).
TEST(nodeIsExactlyContainerOrLeaf) {
    const UiNode c = container("c", {});
    const UiNode l = leaf("l");
    ASSERT_TRUE(c.isContainer());
    ASSERT_TRUE(!c.isLeaf());
    ASSERT_TRUE(l.isLeaf());
    ASSERT_TRUE(!l.isContainer());
}

// A View leaf names a client-rendered surface and is sized Exact or Flex.
TEST(wellFormedViewLeafValidates) {
    UiSchema schema;
    schema.regions = {UiRegion{
        RegionRole::Overlay,
        container("body", {viewLeaf("tv", ssg::ViewSurface::TabView,
                                    Size::flex())})}};
    ASSERT_TRUE(validateUiSchema(schema).ok());
}

// A View leaf without a surface is malformed (its surface is required).
TEST(viewLeafWithoutSurfaceIsRejected) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::View;
    widget.id = "tv";
    UiSchema schema;
    schema.regions = {UiRegion{
        RegionRole::Overlay,
        container("body", {UiNode{UiNodeId{"tv"}, Size::flex(),
                                  UiLeaf{std::move(widget)}}})}};
    ASSERT_TRUE(!validateUiSchema(schema).ok());
}

// An opaque View has no content to hug, so an Auto-sized View leaf is rejected.
TEST(autoSizedViewLeafIsRejected) {
    UiSchema schema;
    schema.regions = {UiRegion{
        RegionRole::Overlay,
        container("body", {viewLeaf("tv", ssg::ViewSurface::TabView,
                                    Size::autoSize())})}};
    ASSERT_TRUE(!validateUiSchema(schema).ok());
}

// A surface belongs to a View leaf alone; a Label carrying one is malformed.
TEST(surfaceOnNonViewLeafIsRejected) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::Label;
    widget.id = "lbl";
    widget.surface = ssg::ViewSurface::GitStatus;
    UiSchema schema;
    schema.regions = {UiRegion{
        RegionRole::Top,
        container("root", {UiNode{UiNodeId{"lbl"}, Size::flex(),
                                  UiLeaf{std::move(widget)}}})}};
    ASSERT_TRUE(!validateUiSchema(schema).ok());
}

}  // namespace

int main() {
    RUN(wellFormedSchemaValidates);
    RUN(duplicateNodeIdAcrossRegionsIsRejected);
    RUN(duplicateNodeIdAmongSiblingsIsRejected);
    RUN(emptyNodeIdIsRejected);
    RUN(duplicateRegionRoleIsRejected);
    RUN(emptySchemaValidates);
    RUN(nodeIsExactlyContainerOrLeaf);
    RUN(wellFormedViewLeafValidates);
    RUN(viewLeafWithoutSurfaceIsRejected);
    RUN(autoSizedViewLeafIsRejected);
    RUN(surfaceOnNonViewLeafIsRejected);
    return failed == 0 ? 0 : 1;
}
