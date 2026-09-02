// Oracle for the UI-VM tree schema validator (spec §Three layers: schema). The
// validator is the precondition every later consumer relies on: node ids are
// unique within a generation and region roles do not repeat. These tests are the
// independently-knowable answers (which schema is well-formed) written against
// the rules, not the implementation.

#include "ssg/UiTree.h"
#include "ssg/WholeScreenAssembly.h"
#include "test_helpers.h"

#include <string>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace {

using ssg::Axis;
using ssg::Generation;
using ssg::Size;
using ssg::UiContainer;
using ssg::UiLeaf;
using ssg::UiNode;
using ssg::UiNodeId;
using ssg::UiSchema;
using ssg::validateUiSchema;
using ssg::WidgetDescriptor;
using ssg::WidgetKind;

UiSchema canonicalWholeScreenSchema() {
    ssg::StyleDimensions dimensions;
    UiSchema schema;
    schema.root =
        ssg::assembleWholeScreen({}, "help.open", dimensions,
                                 ssg::Style{}.inputLineSigil)
            .root;
    return schema;
}

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
    schema.root = container(
        "root", {container("header", {leaf("path"), leaf("branch")}),
                 container("footer", {leaf("hint")})});
    ASSERT_TRUE(validateUiSchema(schema).ok());
}

// A node id must be unique across the WHOLE generation, not merely among
// siblings: the id addresses patches/state/focus, so a collision anywhere is a
// rejection.
TEST(duplicateNodeIdAcrossSubtreesIsRejected) {
    UiSchema schema;
    schema.root = container(
        "root", {container("sub_a", {leaf("shared")}),
                 container("sub_b", {leaf("shared")})});
    ASSERT_TRUE(!validateUiSchema(schema).ok());
}

TEST(duplicateNodeIdAmongSiblingsIsRejected) {
    UiSchema schema;
    schema.root = container("root", {leaf("dup"), leaf("dup")});
    ASSERT_TRUE(!validateUiSchema(schema).ok());
}

TEST(emptyNodeIdIsRejected) {
    UiSchema schema;
    schema.root = container("root", {leaf("")});
    ASSERT_TRUE(!validateUiSchema(schema).ok());
}

// A minimal valid schema is a single root node with a non-empty id.
TEST(minimalRootValidates) {
    UiSchema schema;
    schema.root = container("root", {});
    ASSERT_TRUE(validateUiSchema(schema).ok());
}

TEST(responsiveAndAutoDirectSiblingsAreRejected) {
    UiSchema schema;
    schema.root = container(
        "root",
        {UiNode{UiNodeId{"responsive"}, Size::minimumFlex(10),
                UiContainer{}},
         UiNode{UiNodeId{"auto"}, Size::autoSize(), UiContainer{}}});
    ASSERT_FALSE(validateUiSchema(schema).ok());

    schema.root = container(
        "root",
        {UiNode{UiNodeId{"responsive"}, Size::minimumFlex(10),
                UiContainer{Axis::Column, {}, {},
                            {UiNode{UiNodeId{"nested-auto"},
                                    Size::autoSize(), UiContainer{}}}}}});
    ASSERT_TRUE(validateUiSchema(schema).ok());
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

TEST(nodeStyleResolvesEachChannelFromTheNearestAssignment) {
    UiSchema schema;
    schema.root = container(
        "root",
        {container("branch", {leaf("inherited"), leaf("overridden")}),
         leaf("sibling")});
    schema.root.style.foreground = ssg::SemanticRole::Text;
    auto& root = std::get<UiContainer>(schema.root.content);
    root.children[0].style.background = ssg::SemanticRole::HeaderBackground;
    auto& branch = std::get<UiContainer>(root.children[0].content);
    branch.children[1].style.foreground = ssg::SemanticRole::StatusWarning;
    root.children[1].style.background = ssg::SemanticRole::FooterBackground;

    const auto inherited = ssg::resolveUiNodeStyle(schema, "inherited");
    ASSERT_TRUE(inherited.has_value());
    ASSERT_TRUE(inherited->foreground == ssg::SemanticRole::Text);
    ASSERT_TRUE(inherited->background == ssg::SemanticRole::HeaderBackground);

    const auto overridden = ssg::resolveUiNodeStyle(schema, "overridden");
    ASSERT_TRUE(overridden.has_value());
    ASSERT_TRUE(overridden->foreground == ssg::SemanticRole::StatusWarning);
    ASSERT_TRUE(overridden->background == ssg::SemanticRole::HeaderBackground);

    const auto sibling = ssg::resolveUiNodeStyle(schema, "sibling");
    ASSERT_TRUE(sibling.has_value());
    ASSERT_TRUE(sibling->foreground == ssg::SemanticRole::Text);
    ASSERT_TRUE(sibling->background == ssg::SemanticRole::FooterBackground);
    ASSERT_FALSE(ssg::resolveUiNodeStyle(schema, "missing").has_value());
}

// A View leaf names a client-rendered surface and is sized Exact or Flex.
TEST(wellFormedViewLeafValidates) {
    UiSchema schema;
    schema.root = container(
        "body",
        {viewLeaf("tv", ssg::ViewSurface::Document, Size::flex())});
    ASSERT_TRUE(validateUiSchema(schema).ok());
}

// A View leaf without a surface is malformed (its surface is required).
TEST(viewLeafWithoutSurfaceIsRejected) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::View;
    widget.id = "tv";
    UiSchema schema;
    schema.root = container(
        "body", {UiNode{UiNodeId{"tv"}, Size::flex(), UiLeaf{std::move(widget)}}});
    ASSERT_TRUE(!validateUiSchema(schema).ok());
}

// An opaque View has no content to hug, so an Auto-sized View leaf is rejected.
TEST(autoSizedViewLeafIsRejected) {
    UiSchema schema;
    schema.root = container(
        "body",
        {viewLeaf("tv", ssg::ViewSurface::Document, Size::autoSize())});
    ASSERT_TRUE(!validateUiSchema(schema).ok());
}

// A surface belongs to a View leaf alone; a Label carrying one is malformed.
TEST(surfaceOnNonViewLeafIsRejected) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::Label;
    widget.id = "lbl";
    widget.surface = static_cast<ssg::ViewSurface>(2);
    UiSchema schema;
    schema.root = container(
        "root",
        {UiNode{UiNodeId{"lbl"}, Size::flex(), UiLeaf{std::move(widget)}}});
    ASSERT_TRUE(!validateUiSchema(schema).ok());
}

// A StatusActions leaf carries only its id; unlike a View it may be Auto-sized (it
// has intrinsic content -- a variable action list rendered from promptStatus).
TEST(wellFormedStatusActionsLeafValidates) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::StatusActions;
    widget.id = "sa";
    UiSchema schema;
    schema.root = container(
        "root",
        {UiNode{UiNodeId{"sa"}, Size::autoSize(), UiLeaf{std::move(widget)}}});
    ASSERT_TRUE(validateUiSchema(schema).ok());
}

// A StatusActions leaf carrying any widget-only field (here a command) is malformed:
// its data rides promptStatus, not the schema, and it dispatches by invocation, not a
// commandId.
TEST(statusActionsLeafWithAWidgetFieldIsRejected) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::StatusActions;
    widget.id = "sa";
    widget.command = "some.command";
    UiSchema schema;
    schema.root = container(
        "root",
        {UiNode{UiNodeId{"sa"}, Size::autoSize(), UiLeaf{std::move(widget)}}});
    ASSERT_TRUE(!validateUiSchema(schema).ok());
}

// The whole-screen well-known-area contract (validated at the wire boundary): the
// canonical whole-screen shape passes.
TEST(wellKnownAreasAcceptTheCanonicalShape) {
    UiSchema schema = canonicalWholeScreenSchema();
    ASSERT_TRUE(ssg::validateWellKnownAreas(schema).ok());
}

// The root node must carry the "root" id.
TEST(wellKnownAreasRejectAMisnamedRoot) {
    UiSchema schema;
    schema.root = container("body", {});
    ASSERT_TRUE(!ssg::validateWellKnownAreas(schema).ok());
}

// A well-known area must be a container, not a bare leaf.
TEST(wellKnownAreasRejectALeafHeader) {
    UiSchema schema = canonicalWholeScreenSchema();
    auto& root = std::get<UiContainer>(schema.root.content);
    root.children[0] = leaf("header");
    ASSERT_TRUE(!ssg::validateWellKnownAreas(schema).ok());
}

// A well-known area must sit in its canonical position: header directly under root,
// not buried in a sub-container.
TEST(wellKnownAreasRejectAMisplacedHeader) {
    UiSchema schema = canonicalWholeScreenSchema();
    auto& root = std::get<UiContainer>(schema.root.content);
    root.children[0] = std::move(root.children[1]);
    ASSERT_TRUE(!ssg::validateWellKnownAreas(schema).ok());
}

TEST(wellKnownAreasRejectAMissingBody) {
    UiSchema schema = canonicalWholeScreenSchema();
    auto& root = std::get<UiContainer>(schema.root.content);
    root.children.erase(root.children.begin() + 1);
    ASSERT_TRUE(!ssg::validateWellKnownAreas(schema).ok());
}

TEST(wellKnownAreasRejectAPanelViewWithTheWrongSurface) {
    UiSchema schema = canonicalWholeScreenSchema();
    auto& root = std::get<UiContainer>(schema.root.content);
    auto& body = std::get<UiContainer>(root.children[1].content);
    auto& panel = std::get<UiContainer>(body.children[0].content);
    auto& tree = std::get<UiLeaf>(panel.children[0].content);
    tree.widget.surface = static_cast<ssg::ViewSurface>(2);
    ASSERT_TRUE(!ssg::validateWellKnownAreas(schema).ok());
}

TEST(wellKnownAreasRejectAnAdditionalPanelChild) {
    UiSchema schema = canonicalWholeScreenSchema();
    auto& root = std::get<UiContainer>(schema.root.content);
    auto& body = std::get<UiContainer>(root.children[1].content);
    auto& panel = std::get<UiContainer>(body.children[0].content);
    panel.children.push_back(panel.children.front());
    ASSERT_TRUE(!ssg::validateWellKnownAreas(schema).ok());
}

}  // namespace

int main() {
    RUN(wellFormedSchemaValidates);
    RUN(duplicateNodeIdAcrossSubtreesIsRejected);
    RUN(duplicateNodeIdAmongSiblingsIsRejected);
    RUN(emptyNodeIdIsRejected);
    RUN(minimalRootValidates);
    RUN(responsiveAndAutoDirectSiblingsAreRejected);
    RUN(nodeIsExactlyContainerOrLeaf);
    RUN(nodeStyleResolvesEachChannelFromTheNearestAssignment);
    RUN(wellFormedViewLeafValidates);
    RUN(viewLeafWithoutSurfaceIsRejected);
    RUN(autoSizedViewLeafIsRejected);
    RUN(surfaceOnNonViewLeafIsRejected);
    RUN(wellFormedStatusActionsLeafValidates);
    RUN(statusActionsLeafWithAWidgetFieldIsRejected);
    RUN(wellKnownAreasAcceptTheCanonicalShape);
    RUN(wellKnownAreasRejectAMisnamedRoot);
    RUN(wellKnownAreasRejectALeafHeader);
    RUN(wellKnownAreasRejectAMisplacedHeader);
    RUN(wellKnownAreasRejectAMissingBody);
    RUN(wellKnownAreasRejectAPanelViewWithTheWrongSurface);
    RUN(wellKnownAreasRejectAnAdditionalPanelChild);
    return failed == 0 ? 0 : 1;
}
