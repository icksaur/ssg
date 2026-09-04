// Direct-value oracles for the UI-VM leaf vocabulary consumed by the TUI grid
// lowering (src/UiRegionProjection).

#include <ssg/UiRegionProjection.h>
#include <ssg/ShellViewState.h>

#include <ssg/Style.h>
#include <ssg/UiTree.h>
#include "test_helpers.h"

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace ssg;

// A footer region: a Row of [left, middle, right] groups, with `leaves` as the
// left group's children verbatim -- each leaf's UiNodeId, WidgetDescriptor, and
// (optionally already-set) resolved state are exactly what the caller built.
UiComposition composeFooter(std::vector<UiNode> leaves) {
    UiContainer left;
    left.axis = Axis::Row;
    left.children = std::move(leaves);
    UiContainer footer;
    footer.axis = Axis::Row;
    footer.children.push_back(
        UiNode{UiNodeId{"footer.left"}, Size::autoSize(), std::move(left)});
    footer.children.push_back(
        UiNode{UiNodeId{"footer.middle"}, Size::flex(), UiContainer{Axis::Row}});
    footer.children.push_back(
        UiNode{UiNodeId{"footer.right"}, Size::autoSize(), UiContainer{Axis::Row}});
    UiContainer root;
    root.children.push_back(
        UiNode{UiNodeId{std::string{kFooterNodeId}}, Size::exact(1),
               std::move(footer)});
    return UiComposition{
        UiNode{UiNodeId{std::string{kRootNodeId}}, Size::flex(),
               std::move(root)}};
}

UiRegionProjectionResult projectRegion(
    const UiNode& region, Rect rect, ShellNodeKind kind,
    SemanticRole role, const Style& style,
    std::vector<AccessibilityNode>& out) {
    SolvedUiRegion solved;
    auto result = ssg::projectUiRegion(region, rect, role, style, solved);
    for (const auto& item : solved.items) {
        out.push_back({kind, item.id, item.label, item.rect, item.role,
                       item.content, item.command});
    }
    return result;
}

// A wide rect: no widget rank-collapses, so the TUI emits every non-dropped widget.
constexpr int kWideWidth = 1000;

// A leaf UiNode whose widget id and node id are both `id`, with `resolved`
// already set (or absent, as production leaves an unpopulated/dropped leaf).
UiNode leaf(std::string id, WidgetKind kind,
           std::optional<UiLeafState> resolved) {
    WidgetDescriptor widget;
    widget.kind = kind;
    widget.id = id;
    UiNode node{UiNodeId{id}, Size::autoSize(), UiLeaf{std::move(widget)}};
    node.resolved = std::move(resolved);
    return node;
}

// The footer area subtree (the sole child of a footer-only composition's root),
// which is the 3-group Row the chrome lowering consumes.
const UiNode& footerArea(const UiSchema& schema) {
    return std::get<UiContainer>(schema.root.content).children.front();
}

const AccessibilityNode* nodeFor(const std::vector<AccessibilityNode>& nodes,
                                 const std::string& id) {
    for (const auto& node : nodes)
        if (node.id == id) return &node;
    return nullptr;
}

// --- Grid lowering: consumes UiNode::resolved directly --------------------

// Label/Field: an already-resolved leaf lowers to a matching AccessibilityNode
// verbatim; an unresolved leaf (dropped at publication, EMPTY-DROP) emits none.
TEST(fieldWithResolvedValueLowersVerbatimAndDropsWhenUnresolved) {
    const auto comp = composeFooter({
        leaf("f.lit", WidgetKind::Field,
             UiLeafState{"hello", "hello", std::nullopt, std::nullopt,
                        SemanticRole::Footer}),
        leaf("f.resolved", WidgetKind::Field,
             UiLeafState{"~/proj", "Current path",
                        std::optional<std::string>{"panel.show_files"},
                        std::nullopt, SemanticRole::Footer}),
        leaf("f.empty", WidgetKind::Field, std::nullopt),
    });
    const UiSchema schema{comp.root};
    std::vector<AccessibilityNode> nodes;
    const auto lowered = projectRegion(
        footerArea(schema), {0, 0, kWideWidth, 1}, ShellNodeKind::FooterField,
        SemanticRole::Footer, Style{}, nodes);
    ASSERT_TRUE(lowered.ok());

    ASSERT_TRUE(nodeFor(nodes, "f.lit") != nullptr);
    const auto* resolved = nodeFor(nodes, "f.resolved");
    ASSERT_TRUE(resolved != nullptr);
    if (resolved) {
        ASSERT_EQ(resolved->content, std::string{"~/proj"});
        ASSERT_EQ(resolved->label, std::string{"Current path"});
        ASSERT_TRUE(resolved->commandId ==
                    std::optional<std::string>{"panel.show_files"});
    }
    ASSERT_TRUE(nodeFor(nodes, "f.empty") == nullptr);
}

// The grid reads a resolved leaf's role directly; it never re-derives one from
// the widget's own (here absent) authored role name and the region default.
TEST(fieldRoleIsReadDirectlyFromTheResolvedStateNotRederived) {
    const auto comp = composeFooter({
        leaf("f.warn", WidgetKind::Field,
             UiLeafState{"warn", "warn", std::nullopt, std::nullopt,
                        SemanticRole::StatusWarning}),
        leaf("f.info", WidgetKind::Field,
             UiLeafState{"info", "info", std::nullopt, std::nullopt,
                        SemanticRole::StatusInfo}),
    });
    const UiSchema schema{comp.root};
    std::vector<AccessibilityNode> nodes;
    const auto lowered = projectRegion(
        footerArea(schema), {0, 0, kWideWidth, 1}, ShellNodeKind::FooterField,
        SemanticRole::Footer, Style{}, nodes);
    ASSERT_TRUE(lowered.ok());
    const auto* warn = nodeFor(nodes, "f.warn");
    const auto* info = nodeFor(nodes, "f.info");
    ASSERT_TRUE(warn != nullptr && info != nullptr);
    if (warn) ASSERT_TRUE(warn->role == SemanticRole::StatusWarning);
    if (info) ASSERT_TRUE(info->role == SemanticRole::StatusInfo);
}

// Checkbox: composes its glyph from the resolved checked/value, and falls back
// to that glyph as its label only when the resolved label is empty.
TEST(checkboxComposesItsGlyphFromResolvedCheckedAndValue) {
    const auto comp = composeFooter({
        leaf("c.checked", WidgetKind::Checkbox,
             UiLeafState{"Wrap", "Wrap mode", std::nullopt,
                        std::optional<bool>{true}, SemanticRole::Footer}),
        leaf("c.unchecked", WidgetKind::Checkbox,
             UiLeafState{"Wrap", "", std::nullopt, std::optional<bool>{false},
                        SemanticRole::Footer}),
    });
    const UiSchema schema{comp.root};
    std::vector<AccessibilityNode> nodes;
    const auto lowered = projectRegion(
        footerArea(schema), {0, 0, kWideWidth, 1}, ShellNodeKind::FooterField,
        SemanticRole::Footer, Style{}, nodes);
    ASSERT_TRUE(lowered.ok());
    const auto* checked = nodeFor(nodes, "c.checked");
    const auto* unchecked = nodeFor(nodes, "c.unchecked");
    ASSERT_TRUE(checked != nullptr && unchecked != nullptr);
    if (checked) ASSERT_EQ(checked->label, std::string{"Wrap mode"});
    if (unchecked) ASSERT_EQ(unchecked->label, unchecked->content);
    if (checked && unchecked) {
        ASSERT_TRUE(checked->content != unchecked->content);
    }
}

// A spacer never carries a resolved leaf state and never emits a node, but
// stays structurally present in the schema.
TEST(spacerNeverEmitsALeafNode) {
    WidgetDescriptor spacerWidget;
    spacerWidget.kind = WidgetKind::Spacer;
    spacerWidget.id = "sp";
    spacerWidget.width = 3;
    UiNode spacerNode{UiNodeId{"sp"}, Size::autoSize(),
                      UiLeaf{std::move(spacerWidget)}};

    const auto comp = composeFooter({
        leaf("f", WidgetKind::Field,
             UiLeafState{"x", "x", std::nullopt, std::nullopt,
                        SemanticRole::Footer}),
        std::move(spacerNode),
    });
    const UiSchema schema{comp.root};
    std::vector<AccessibilityNode> nodes;
    const auto lowered = projectRegion(
        footerArea(schema), {0, 0, kWideWidth, 1}, ShellNodeKind::FooterField,
        SemanticRole::Footer, Style{}, nodes);
    ASSERT_TRUE(lowered.ok());
    ASSERT_TRUE(nodeFor(nodes, "f") != nullptr);
    ASSERT_TRUE(nodeFor(nodes, "sp") == nullptr);
    const UiNode* spacer = findUiNode(schema, UiNodeId{"sp"});
    ASSERT_TRUE(spacer != nullptr);
    if (spacer) ASSERT_FALSE(spacer->resolved.has_value());
}

}  // namespace

SSG_TEST_SUITE(test_ui_node_state) {
    RUN(fieldWithResolvedValueLowersVerbatimAndDropsWhenUnresolved);
    RUN(fieldRoleIsReadDirectlyFromTheResolvedStateNotRederived);
    RUN(checkboxComposesItsGlyphFromResolvedCheckedAndValue);
    RUN(spacerNeverEmitsALeafNode);
    return failed == 0 ? 0 : 1;
}
