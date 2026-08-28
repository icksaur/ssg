// Parity oracle (algorithm): the semantic dynamic node state agrees with the TUI
// lowering on what it resolves, at a non-collapsing width so resolution is isolated
// from layout. Label/Field entries are compared field-for-field against the
// AccessibilityNode the TUI emits (or its drop); a checkbox is compared against an
// INDEPENDENT expectation derived from its sources, because the TUI node exposes
// only the composed glyph, not the semantic checked/caption/label.

#include "ssg/ChromeLowering.h"

#include "chrome_authoring.h"
#include "ssg/Style.h"
#include "ssg/UiNodeState.h"
#include "ssg/WholeScreenAssembly.h"
#include "test_helpers.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace ssg;

// A wide rect: no widget rank-collapses, so the TUI emits every non-dropped widget.
constexpr int kWideWidth = 1000;

ChromeProviderResolver resolverFrom(
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

const UiNodeState* stateFor(const UiStateSection& section, const UiNodeId& id) {
    for (const auto& node : section.nodes)
        if (node.id == id) return &node;
    return nullptr;
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
    const auto comp = ssgtest::composeFooter(
        {literalField("f.lit", "hello"), providerField("f.prov", "path"),
         providerField("f.empty", "missing")});
    const UiSchema schema{Generation{1}, comp.root};

    const auto section = resolveUiState(ValidatedSchema::validate(schema).takeSchema(), resolver);
    std::vector<AccessibilityNode> nodes;
    const auto lowered = lowerUiChromeRegion(
        footerArea(schema), {0, 0, kWideWidth, 1}, ShellNodeKind::FooterField,
        SemanticRole::Footer, Style{}, resolver, nodes);
    ASSERT_TRUE(lowered.ok());

    std::vector<std::pair<UiNodeId, const WidgetDescriptor*>> leaves;
    collectLeaves(footerArea(schema), leaves);

    for (const auto& [nodeId, widget] : leaves) {
        const UiNodeState* state = stateFor(section, nodeId);
        ASSERT_TRUE(state != nullptr);
        if (!state) continue;
        const AccessibilityNode* tui = nodeFor(nodes, widget->id);
        if (tui == nullptr) {
            // The TUI dropped it (empty value/label): no semantic leaf either.
            ASSERT_FALSE(state->leaf.has_value());
        } else {
            ASSERT_TRUE(state->leaf.has_value());
            if (!state->leaf) continue;
            ASSERT_EQ(state->leaf->value, tui->content);
            ASSERT_EQ(state->leaf->label, tui->label);
            ASSERT_TRUE(state->leaf->command == tui->commandId);
            ASSERT_FALSE(state->leaf->checked.has_value());
            // No widget here declares a role, so each takes the region default.
            ASSERT_TRUE(state->leaf->role == SemanticRole::Footer);
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
    const auto comp = ssgtest::composeFooter({roled, bogus});
    const UiSchema schema{Generation{1}, comp.root};
    const auto section =
        resolveUiState(ValidatedSchema::validate(schema).takeSchema(), empty);

    std::vector<std::pair<UiNodeId, const WidgetDescriptor*>> leaves;
    collectLeaves(footerArea(schema), leaves);
    ASSERT_EQ(leaves.size(), std::size_t{2});

    const UiNodeState* roledState = stateFor(section, leaves[0].first);
    ASSERT_TRUE(roledState != nullptr && roledState->leaf.has_value());
    if (roledState && roledState->leaf)
        ASSERT_TRUE(roledState->leaf->role == SemanticRole::StatusWarning);

    const UiNodeState* bogusState = stateFor(section, leaves[1].first);
    ASSERT_TRUE(bogusState != nullptr && bogusState->leaf.has_value());
    if (bogusState && bogusState->leaf)
        ASSERT_TRUE(bogusState->leaf->role == SemanticRole::Footer);
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

    const auto comp = ssgtest::composeFooter({litBox, provBox});
    const UiSchema schema{Generation{1}, comp.root};
    const auto section = resolveUiState(ValidatedSchema::validate(schema).takeSchema(), resolver);

    std::vector<std::pair<UiNodeId, const WidgetDescriptor*>> leaves;
    collectLeaves(footerArea(schema), leaves);
    ASSERT_EQ(leaves.size(), std::size_t{2});

    // Literal caption: value+label are the caption, checked=true, command from the
    // descriptor.
    const UiNodeState* lit = stateFor(section, leaves[0].first);
    ASSERT_TRUE(lit != nullptr && lit->leaf.has_value());
    if (lit && lit->leaf) {
        ASSERT_EQ(lit->leaf->value, std::string{"case"});
        ASSERT_EQ(lit->leaf->label, std::string{"case"});
        ASSERT_TRUE(lit->leaf->checked.has_value() && *lit->leaf->checked);
        ASSERT_TRUE(lit->leaf->command.has_value());
        ASSERT_EQ(*lit->leaf->command, std::string{"find.toggle_case"});
    }
    // Provider caption: value is the provider value, label the provider label,
    // checked=false from the checked provider, no command.
    const UiNodeState* prov = stateFor(section, leaves[1].first);
    ASSERT_TRUE(prov != nullptr && prov->leaf.has_value());
    if (prov && prov->leaf) {
        ASSERT_EQ(prov->leaf->value, std::string{"Wrap"});
        ASSERT_EQ(prov->leaf->label, std::string{"Wrap mode"});
        ASSERT_TRUE(prov->leaf->checked.has_value() && !*prov->leaf->checked);
        ASSERT_FALSE(prov->leaf->command.has_value());
    }
}

// A spacer is present with no leaf state; every node gets exactly one record.
TEST(spacerIsPresentWithNoLeafAndEveryNodeHasOneRecord) {
    WidgetDescriptor spacer;
    spacer.kind = WidgetKind::Spacer;
    spacer.id = "sp";
    spacer.width = 3;
    const auto comp =
        ssgtest::composeFooter({literalField("f", "x"), spacer});
    const UiSchema schema{Generation{4}, comp.root};
    const auto empty = [](std::string_view) -> std::optional<ResolvedProvider> {
        return std::nullopt;
    };
    const auto section = resolveUiState(ValidatedSchema::validate(schema).takeSchema(), empty);

    ASSERT_EQ(section.generation.value(), std::uint64_t{4});

    std::vector<std::pair<UiNodeId, const WidgetDescriptor*>> leaves;
    collectLeaves(footerArea(schema), leaves);
    for (const auto& [nodeId, widget] : leaves) {
        const UiNodeState* state = stateFor(section, nodeId);
        ASSERT_TRUE(state != nullptr);
        if (state && widget->kind == WidgetKind::Spacer) {
            ASSERT_FALSE(state->leaf.has_value());
        }
    }
    // One record per node id in the schema (no duplicates, no omissions).
    ASSERT_EQ(section.nodes.size(), uiSchemaNodeIds(schema).size());
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
                assembleWholeScreen({}, "help.open", StyleDimensions{}, "> ",
                                    std::nullopt),
                prompt);
            const auto schema = UiSchema{Generation{3}, composition.root};
            const auto resolver = resolverFrom(
                {{"find.query",
                  {"needle", "Find text",
                   std::optional<std::string>{"find.update_query"},
                   std::optional<bool>{true}}},
                 {"find.toggle_case", {"false", "Case", std::nullopt}},
                 {"find.matches", {"1/3", "Matches", std::nullopt}}});
            const auto section =
                resolveUiState(ValidatedSchema::validate(schema).takeSchema(), resolver);

            const auto* footerInput =
                stateFor(section, UiNodeId{"footer.prompt.control.find.query"});
            ASSERT_TRUE(footerInput != nullptr && footerInput->leaf.has_value());
            if (footerInput && footerInput->leaf) {
                ASSERT_EQ(footerInput->leaf->value, std::string{"needle"});
                ASSERT_TRUE(footerInput->leaf->active.has_value() &&
                            *footerInput->leaf->active);
                ASSERT_FALSE(footerInput->leaf->checked.has_value());
            }

            const auto* headerInput =
                stateFor(section, UiNodeId{std::string{kHeaderPromptInputNodeId}});
            ASSERT_TRUE(headerInput != nullptr);
            if (headerInput) ASSERT_FALSE(headerInput->leaf.has_value());
}

}  // namespace

int main() {
    RUN(labelFieldStateMatchesTuiNodeOrDrop);
    RUN(explicitRoleOverridesRegionDefault);
    RUN(checkboxStateMatchesIndependentExpectation);
    RUN(spacerIsPresentWithNoLeafAndEveryNodeHasOneRecord);
    RUN(footerTextInputIsStatefulWhileHeaderPickerInputRemainsLocal);
    return 0;
}
