// Parity oracle (algorithm): the semantic dynamic node state agrees with the TUI
// lowering on what it resolves, at a non-collapsing width so resolution is isolated
// from layout. Label/Field entries are compared field-for-field against the
// AccessibilityNode the TUI emits (or its drop); a checkbox is compared against an
// INDEPENDENT expectation derived from its sources, because the TUI node exposes
// only the composed glyph, not the semantic checked/caption/label.

#include <tui/UiRegionProjection.h>
#include <tui/ShellViewState.h>

#include "ssg/Style.h"
#include "ssg/UiTree.h"
#include "ssg/WholeScreenAssembly.h"
#include "test_helpers.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace ssg;

UiComposition composeFooter(std::vector<WidgetDescriptor> widgets) {
    UiContainer left;
    left.axis = Axis::Row;
    for (std::size_t index = 0; index < widgets.size(); ++index) {
        left.children.push_back(
            UiNode{UiNodeId{"footer.left." + std::to_string(index)},
                   Size::autoSize(), UiLeaf{std::move(widgets[index])}});
    }
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
    const WidgetProviderResolver& resolver,
    std::vector<AccessibilityNode>& out) {
    SolvedUiRegion solved;
    auto result =
        ssg::projectUiRegion(region, rect, role, style, resolver, solved);
    for (const auto& item : solved.items) {
        out.push_back({kind, item.id, item.label, item.rect, item.role,
                       item.content, item.command});
    }
    return result;
}

// A wide rect: no widget rank-collapses, so the TUI emits every non-dropped widget.
constexpr int kWideWidth = 1000;

WidgetProviderResolver resolverFrom(
    std::vector<std::pair<std::string, ResolvedProvider>> table) {
    return [table = std::move(table)](
               std::string_view id) -> std::optional<ResolvedProvider> {
        for (const auto& [key, value] : table)
            if (key == id) return value;
        return std::nullopt;
    };
}

WidgetDescriptor literalField(std::string id, std::string text) {
    WidgetDescriptor w;
    w.kind = WidgetKind::Field;
    w.id = std::move(id);
    w.value = ValueSource{false, std::move(text), ""};
    return w;
}

WidgetDescriptor providerField(std::string id, std::string provider) {
    WidgetDescriptor w;
    w.kind = WidgetKind::Field;
    w.id = std::move(id);
    w.value = ValueSource{true, "", std::move(provider)};
    return w;
}

// The (tree node id, widget) of every leaf in a region, in tree order.
void collectLeaves(const UiNode& node,
                   std::vector<std::pair<UiNodeId, const WidgetDescriptor*>>& out) {
    if (const auto* leaf = std::get_if<UiLeaf>(&node.content))
        out.emplace_back(node.id, &leaf->widget);
    if (const auto* container = std::get_if<UiContainer>(&node.content))
        for (const auto& child : container->children) collectLeaves(child, out);
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

// Label/Field: the resolved-state entry matches the TUI's emitted node exactly, and
// a TUI drop matches an absent leaf state.
TEST(labelFieldStateMatchesTuiNodeOrDrop) {
    const auto resolver = resolverFrom(
        {{"path", {"~/proj", "Current path",
                   std::optional<std::string>{"panel.show_files"}}}});
    const auto comp = composeFooter(
        {literalField("f.lit", "hello"), providerField("f.prov", "path"),
         providerField("f.empty", "missing")});
    const UiSchema schema{comp.root};

    const auto section = resolveUiTree(schema, resolver);
    std::vector<AccessibilityNode> nodes;
    const auto lowered = projectRegion(
        footerArea(schema), {0, 0, kWideWidth, 1}, ShellNodeKind::FooterField,
        SemanticRole::Footer, Style{}, resolver, nodes);
    ASSERT_TRUE(lowered.ok());

    std::vector<std::pair<UiNodeId, const WidgetDescriptor*>> leaves;
    collectLeaves(footerArea(schema), leaves);

    for (const auto& [nodeId, widget] : leaves) {
        const UiNode* state = findUiNode(section, nodeId);
        ASSERT_TRUE(state != nullptr);
        if (!state) continue;
        const AccessibilityNode* tui = nodeFor(nodes, widget->id);
        if (tui == nullptr) {
            // The TUI dropped it (empty value/label): no semantic leaf either.
            ASSERT_FALSE(state->resolved.has_value());
        } else {
            ASSERT_TRUE(state->resolved.has_value());
            if (!state->resolved) continue;
            ASSERT_EQ(state->resolved->value, tui->content);
            ASSERT_EQ(state->resolved->label, tui->label);
            ASSERT_TRUE(state->resolved->command == tui->commandId);
            ASSERT_FALSE(state->resolved->checked.has_value());
            // No widget here declares a role, so each takes the region default.
            ASSERT_TRUE(state->resolved->role == SemanticRole::Footer);
        }
    }
}

// A widget's own valid role name overrides the region default; an unknown or
// absent name falls back to the default.
TEST(explicitRoleOverridesRegionDefault) {
    const auto empty = [](std::string_view) -> std::optional<ResolvedProvider> {
        return std::nullopt;
    };
    WidgetDescriptor roled = literalField("f.roled", "warn");
    roled.role = "status_warning";
    WidgetDescriptor bogus = literalField("f.bogus", "plain");
    bogus.role = "not_a_role";
    const auto comp = composeFooter({roled, bogus});
    const UiSchema schema{comp.root};
    const auto section =
        resolveUiTree(schema, empty);

    std::vector<std::pair<UiNodeId, const WidgetDescriptor*>> leaves;
    collectLeaves(footerArea(schema), leaves);
    ASSERT_EQ(leaves.size(), std::size_t{2});

    const UiNode* roledState = findUiNode(section, leaves[0].first);
    ASSERT_TRUE(roledState != nullptr && roledState->resolved.has_value());
    if (roledState && roledState->resolved)
        ASSERT_TRUE(roledState->resolved->role == SemanticRole::StatusWarning);

    const UiNode* bogusState = findUiNode(section, leaves[1].first);
    ASSERT_TRUE(bogusState != nullptr && bogusState->resolved.has_value());
    if (bogusState && bogusState->resolved)
        ASSERT_TRUE(bogusState->resolved->role == SemanticRole::Footer);
}

// Checkbox: never dropped, and its semantic fields match an expectation derived
// from its sources -- NOT from the TUI node, whose content is the composed glyph.
TEST(checkboxStateMatchesIndependentExpectation) {
    const auto resolver = resolverFrom(
        {{"wrapcap", {"Wrap", "Wrap mode", std::nullopt}},
         {"wrapchk", {"false", "", std::nullopt}}});

    WidgetDescriptor litBox;
    litBox.kind = WidgetKind::Checkbox;
    litBox.id = "c.lit";
    litBox.value = ValueSource{false, "case", ""};
    litBox.checked = ValueSource{false, "true", ""};
    litBox.command = "find.toggle_case";

    WidgetDescriptor provBox;
    provBox.kind = WidgetKind::Checkbox;
    provBox.id = "c.prov";
    provBox.value = ValueSource{true, "", "wrapcap"};
    provBox.checked = ValueSource{true, "", "wrapchk"};

    const auto comp = composeFooter({litBox, provBox});
    const UiSchema schema{comp.root};
    const auto section = resolveUiTree(schema, resolver);

    std::vector<std::pair<UiNodeId, const WidgetDescriptor*>> leaves;
    collectLeaves(footerArea(schema), leaves);
    ASSERT_EQ(leaves.size(), std::size_t{2});

    // Literal caption: value+label are the caption, checked=true, command from the
    // descriptor.
    const UiNode* lit = findUiNode(section, leaves[0].first);
    ASSERT_TRUE(lit != nullptr && lit->resolved.has_value());
    if (lit && lit->resolved) {
        ASSERT_EQ(lit->resolved->value, std::string{"case"});
        ASSERT_EQ(lit->resolved->label, std::string{"case"});
        ASSERT_TRUE(lit->resolved->checked.has_value() && *lit->resolved->checked);
        ASSERT_TRUE(lit->resolved->command.has_value());
        ASSERT_EQ(*lit->resolved->command, std::string{"find.toggle_case"});
    }
    // Provider caption: value is the provider value, label the provider label,
    // checked=false from the checked provider, no command.
    const UiNode* prov = findUiNode(section, leaves[1].first);
    ASSERT_TRUE(prov != nullptr && prov->resolved.has_value());
    if (prov && prov->resolved) {
        ASSERT_EQ(prov->resolved->value, std::string{"Wrap"});
        ASSERT_EQ(prov->resolved->label, std::string{"Wrap mode"});
        ASSERT_TRUE(prov->resolved->checked.has_value() && !*prov->resolved->checked);
        ASSERT_FALSE(prov->resolved->command.has_value());
    }
}

// A spacer is present with no leaf state; every node gets exactly one record.
TEST(spacerIsPresentWithNoLeafAndEveryNodeHasOneRecord) {
    WidgetDescriptor spacer;
    spacer.kind = WidgetKind::Spacer;
    spacer.id = "sp";
    spacer.width = 3;
    const auto comp =
        composeFooter({literalField("f", "x"), spacer});
    const UiSchema schema{comp.root};
    const auto empty = [](std::string_view) -> std::optional<ResolvedProvider> {
        return std::nullopt;
    };
    const auto section = resolveUiTree(schema, empty);

    std::vector<std::pair<UiNodeId, const WidgetDescriptor*>> leaves;
    collectLeaves(footerArea(schema), leaves);
    for (const auto& [nodeId, widget] : leaves) {
        const UiNode* state = findUiNode(section, nodeId);
        ASSERT_TRUE(state != nullptr);
        if (state && widget->kind == WidgetKind::Spacer) {
            ASSERT_FALSE(state->resolved.has_value());
        }
    }
    // Resolution neither adds nor renames nodes: the resolved tree carries
    // exactly the schema's own node ids.
    ASSERT_TRUE(uiSchemaNodeIds(section) == uiSchemaNodeIds(schema));
}

TEST(footerTextInputIsStatefulWhileHeaderPickerInputRemainsLocal) {
            PromptRequest request;
            request.kind = PromptKind::Find;
            request.accessibleLabel = "Find";
            request.inputs.push_back({"find.query", "Find text", "needle"});
            request.toggles.push_back({"find.toggle_case", "Case", false, 6});
            request.matchCount = PromptMatchCount{"find.matches", "Matches", "1/3"};
            PromptSurface prompt;
            ASSERT_TRUE(prompt.open(request).accepted());

            const auto composition = withFooterPrompt(
                assembleWholeScreen({}, "help.open", StyleDimensions{}, "> "),
                prompt);
            const auto schema = UiSchema{composition.root};
            const auto resolver = resolverFrom(
                {{"find.query",
                  {"needle", "Find text",
                   std::optional<std::string>{"find.update_query"},
                   std::optional<bool>{true}}},
                 {"find.toggle_case", {"false", "Case", std::nullopt}},
                 {"find.matches", {"1/3", "Matches", std::nullopt}}});
            const auto section =
                resolveUiTree(schema, resolver);

            const auto* footerInput =
                findUiNode(section, UiNodeId{"footer.prompt.control.find.query"});
            ASSERT_TRUE(footerInput != nullptr && footerInput->resolved.has_value());
            if (footerInput && footerInput->resolved) {
                ASSERT_EQ(footerInput->resolved->value, std::string{"needle"});
                ASSERT_TRUE(footerInput->resolved->active.has_value() &&
                            *footerInput->resolved->active);
                ASSERT_FALSE(footerInput->resolved->checked.has_value());
            }

            const auto* headerInput =
                findUiNode(section, UiNodeId{std::string{kHeaderPromptInputNodeId}});
            ASSERT_TRUE(headerInput != nullptr);
            if (headerInput) ASSERT_FALSE(headerInput->resolved.has_value());
}

}  // namespace

SSG_TEST_SUITE(test_ui_node_state) {
    RUN(labelFieldStateMatchesTuiNodeOrDrop);
    RUN(explicitRoleOverridesRegionDefault);
    RUN(checkboxStateMatchesIndependentExpectation);
    RUN(spacerIsPresentWithNoLeafAndEveryNodeHasOneRecord);
    RUN(footerTextInputIsStatefulWhileHeaderPickerInputRemainsLocal);
    return 0;
}
