// Direct-value oracles for the UI-VM leaf vocabulary. Two independent layers:
// (1) the TUI grid lowering (src/UiRegionProjection) consumes each leaf's
// already-resolved UiNode::resolved directly, with no resolver to reconstruct;
// (2) the private snapshot-publication step (detail::populateUiTree) writes
// UiNode::resolved from typed runtime values by fixed UiNodeId lookup.

#include <ssg/UiRegionProjection.h>
#include <ssg/ShellViewState.h>

#include <ssg/PromptSurface.h>
#include <ssg/StatusFields.h>
#include <ssg/StatusQueue.h>
#include <ssg/Style.h>
#include <ssg/UiTree.h>
#include <ssg/WholeScreenAssembly.h>
#include "test_helpers.h"

#include <ssg/ui_tree_population.h>

#include <optional>
#include <stdexcept>
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

UiNode* mutableNode(UiNode& node, const UiNodeId& id) {
    if (node.id == id) return &node;
    if (auto* container = std::get_if<UiContainer>(&node.content)) {
        for (auto& child : container->children) {
            if (auto* found = mutableNode(child, id)) return found;
        }
    }
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

// --- Snapshot-publication population: writes UiNode::resolved -------------

// The header/footer status fields resolve from the projection's matching
// entry, keep its accessible label/command, take the field's own resolved
// role, and drop (EMPTY-DROP) when the projection carries no entry for them.
TEST(statusFieldsResolveFromProjectionOrDropWhenAbsent) {
    const auto composition =
        assembleWholeScreen("help.open", StyleDimensions{}, "> ");
    UiSchema schema{composition.root};
    StatusFieldProjection status;
    status.header.push_back(
        {std::string{kPathStatusFieldId}, "Current path", "~/proj",
         std::optional<std::string>{"panel.show_files"}});
    // branch is left out of the projection -> dropped.
    detail::populateUiTree(schema,
                           detail::UiTreeValues{status, "", {}, std::nullopt});

    const UiNode* path =
        findUiNode(schema, UiNodeId{std::string{kHeaderPathFieldNodeId}});
    ASSERT_TRUE(path != nullptr && path->resolved.has_value());
    if (path && path->resolved) {
        ASSERT_EQ(path->resolved->value, std::string{"~/proj"});
        ASSERT_EQ(path->resolved->label, std::string{"Current path"});
        ASSERT_TRUE(path->resolved->command ==
                    std::optional<std::string>{"panel.show_files"});
        ASSERT_FALSE(path->resolved->checked.has_value());
        ASSERT_TRUE(path->resolved->role == SemanticRole::Header);
    }

    const UiNode* branch =
        findUiNode(schema, UiNodeId{std::string{kHeaderBranchFieldNodeId}});
    ASSERT_TRUE(branch != nullptr);
    ASSERT_FALSE(branch->resolved.has_value());
}

// The footer hint resolves its keymap-derived label but keeps the widget's own
// static command; an unbound (empty) label drops the hint.
TEST(helpHintResolvesLabelAndKeepsItsStaticCommandOrDropsWhenUnbound) {
    const auto composition =
        assembleWholeScreen("help.open", StyleDimensions{}, "> ");

    UiSchema bound{composition.root};
    detail::populateUiTree(
        bound, detail::UiTreeValues{{}, "Alt+h  help", {}, std::nullopt});
    const UiNode* hint =
        findUiNode(bound, UiNodeId{std::string{kFooterHintNodeId}});
    ASSERT_TRUE(hint != nullptr && hint->resolved.has_value());
    if (hint && hint->resolved) {
        ASSERT_EQ(hint->resolved->value, std::string{"Alt+h  help"});
        ASSERT_EQ(hint->resolved->label, std::string{"Alt+h  help"});
        ASSERT_TRUE(hint->resolved->command ==
                    std::optional<std::string>{"help.open"});
        ASSERT_TRUE(hint->resolved->role == SemanticRole::Footer);
    }

    UiSchema unbound{composition.root};
    detail::populateUiTree(unbound,
                           detail::UiTreeValues{{}, "", {}, std::nullopt});
    const UiNode* dropped =
        findUiNode(unbound, UiNodeId{std::string{kFooterHintNodeId}});
    ASSERT_TRUE(dropped != nullptr);
    ASSERT_FALSE(dropped->resolved.has_value());
}

// A status action resolves by matching UiNodeId, keeping its accessible label
// as both value and label and its command id; an empty label drops it.
TEST(statusActionsResolveByIdAndDropEmptyLabels) {
    const auto composition =
        assembleWholeScreen("help.open", StyleDimensions{}, "> ");
    const UiNodeId actionId{"footer.status_action/1/1/00"};
    const UiNodeId emptyActionId{"footer.status_action/1/1/01"};
    const auto withActions = withStatusActions(
        composition, {{actionId, "Run", "build.run"},
                      {emptyActionId, "", "build.other"}});
    UiSchema schema{withActions.root};
    detail::populateUiTree(
        schema,
        detail::UiTreeValues{
            {}, "",
            {{actionId, "Run", "build.run"}, {emptyActionId, "", "build.other"}},
            std::nullopt});

    const UiNode* action = findUiNode(schema, actionId);
    ASSERT_TRUE(action != nullptr && action->resolved.has_value());
    if (action && action->resolved) {
        ASSERT_EQ(action->resolved->value, std::string{"Run"});
        ASSERT_EQ(action->resolved->label, std::string{"Run"});
        ASSERT_TRUE(action->resolved->command ==
                    std::optional<std::string>{"build.run"});
        ASSERT_TRUE(action->resolved->role == SemanticRole::StatusInfo);
    }
    const UiNode* emptyAction = findUiNode(schema, emptyActionId);
    ASSERT_TRUE(emptyAction != nullptr);
    ASSERT_FALSE(emptyAction->resolved.has_value());
}

// Footer prompt controls: an Input keeps its value/label/command
// and is flagged active exactly when it is the prompt's active input; a
// Toggle is never dropped; a Count is dropped when empty. The header's own
// prompt input stays entirely client-local (never populated).
TEST(footerPromptControlsResolveValuesTogglesAndTheActiveInput) {
    PromptRequest request;
    request.kind = PromptKind::Find;
    request.accessibleLabel = "Find";
    request.inputs.push_back({"find.query", "Find text", "needle"});
    request.toggles.push_back({"find.toggle_case", "Case", true, 6});
    request.matchCount = PromptMatchCount{"find.matches", "Matches", "1/3"};
    PromptSurface prompt;
    ASSERT_TRUE(prompt.open(request).accepted());

    const auto composition = withFooterPrompt(
        assembleWholeScreen("help.open", StyleDimensions{}, "> "), prompt);
    UiSchema schema{composition.root};
    detail::ResolvedPromptControls resolved{resolvePromptControls(request),
                                            prompt.activeInput()};
    detail::populateUiTree(schema,
                           detail::UiTreeValues{{}, "", {}, resolved});

    const auto* queryInput =
        findUiNode(schema, footerPromptControlNodeId("find.query"));
    ASSERT_TRUE(queryInput != nullptr && queryInput->resolved.has_value());
    if (queryInput && queryInput->resolved) {
        ASSERT_EQ(queryInput->resolved->value, std::string{"needle"});
        ASSERT_EQ(queryInput->resolved->label, std::string{"Find text"});
        ASSERT_TRUE(queryInput->resolved->command ==
                    std::optional<std::string>{"find.update_query"});
        ASSERT_TRUE(queryInput->resolved->active.has_value() &&
                    *queryInput->resolved->active);
        ASSERT_FALSE(queryInput->resolved->checked.has_value());
        ASSERT_TRUE(queryInput->resolved->role == SemanticRole::Prompt);
    }

    const auto* caseToggle =
        findUiNode(schema, footerPromptControlNodeId("find.toggle_case"));
    ASSERT_TRUE(caseToggle != nullptr && caseToggle->resolved.has_value());
    if (caseToggle && caseToggle->resolved) {
        ASSERT_EQ(caseToggle->resolved->value, std::string{"Case"});
        ASSERT_EQ(caseToggle->resolved->label, std::string{"Case"});
        ASSERT_TRUE(caseToggle->resolved->checked.has_value() &&
                    *caseToggle->resolved->checked);
        ASSERT_TRUE(caseToggle->resolved->command ==
                    std::optional<std::string>{"find.toggle_case"});
    }

    const auto* matches =
        findUiNode(schema, footerPromptControlNodeId("find.matches"));
    ASSERT_TRUE(matches != nullptr && matches->resolved.has_value());
    if (matches && matches->resolved) {
        ASSERT_EQ(matches->resolved->value, std::string{"1/3"});
        ASSERT_FALSE(matches->resolved->command.has_value());
    }

    const auto* headerInput =
        findUiNode(schema, UiNodeId{std::string{kHeaderPromptInputNodeId}});
    ASSERT_TRUE(headerInput != nullptr);
    if (headerInput) ASSERT_FALSE(headerInput->resolved.has_value());
}

// An authored role name on the widget wins over the caller's default; an
// unknown name falls back to it, exactly as the fixed fields (which author no
// role at all) always do.
TEST(populateHonorsAnAuthoredRoleOrFallsBackWhenItIsUnknown) {
    const auto composition =
        assembleWholeScreen("help.open", StyleDimensions{}, "> ");
    UiSchema schema{composition.root};
    UiNode* path =
        mutableNode(schema.root, UiNodeId{std::string{kHeaderPathFieldNodeId}});
    UiNode* branch = mutableNode(
        schema.root, UiNodeId{std::string{kHeaderBranchFieldNodeId}});
    ASSERT_TRUE(path != nullptr && branch != nullptr);
    if (path) std::get<UiLeaf>(path->content).widget.role = "status_warning";
    if (branch) std::get<UiLeaf>(branch->content).widget.role = "not_a_role";

    StatusFieldProjection status;
    status.header.push_back(
        {std::string{kPathStatusFieldId}, "Current path", "~/proj"});
    status.header.push_back(
        {std::string{kBranchStatusFieldId}, "branch", "main"});
    detail::populateUiTree(schema,
                           detail::UiTreeValues{status, "", {}, std::nullopt});

    const UiNode* resolvedPath =
        findUiNode(schema, UiNodeId{std::string{kHeaderPathFieldNodeId}});
    ASSERT_TRUE(resolvedPath != nullptr && resolvedPath->resolved.has_value());
    if (resolvedPath && resolvedPath->resolved) {
        // A valid authored role wins over the header default.
        ASSERT_TRUE(resolvedPath->resolved->role == SemanticRole::StatusWarning);
    }

    const UiNode* resolvedBranch =
        findUiNode(schema, UiNodeId{std::string{kHeaderBranchFieldNodeId}});
    ASSERT_TRUE(resolvedBranch != nullptr &&
                resolvedBranch->resolved.has_value());
    if (resolvedBranch && resolvedBranch->resolved) {
        // An unparseable role name falls back to the header default.
        ASSERT_TRUE(resolvedBranch->resolved->role == SemanticRole::Header);
    }
}

// Fail loud (matching the existing publication boundary): an expected fixed
// node that is missing, or present with the wrong widget kind, throws rather
// than silently resolving nothing.
TEST(populateThrowsWhenAFixedNodeIsMissingOrTheWrongKind) {
    UiSchema empty;
    ASSERT_THROWS(detail::populateUiTree(empty, detail::UiTreeValues{}),
                 std::logic_error);

    const auto composition =
        assembleWholeScreen("help.open", StyleDimensions{}, "> ");
    UiSchema wrongKind{composition.root};
    UiNode* path = mutableNode(wrongKind.root,
                               UiNodeId{std::string{kHeaderPathFieldNodeId}});
    ASSERT_TRUE(path != nullptr);
    if (path) std::get<UiLeaf>(path->content).widget.kind = WidgetKind::Checkbox;
    ASSERT_THROWS(detail::populateUiTree(wrongKind, detail::UiTreeValues{}),
                 std::logic_error);
}

}  // namespace

SSG_TEST_SUITE(test_ui_node_state) {
    RUN(fieldWithResolvedValueLowersVerbatimAndDropsWhenUnresolved);
    RUN(fieldRoleIsReadDirectlyFromTheResolvedStateNotRederived);
    RUN(checkboxComposesItsGlyphFromResolvedCheckedAndValue);
    RUN(spacerNeverEmitsALeafNode);
    RUN(statusFieldsResolveFromProjectionOrDropWhenAbsent);
    RUN(helpHintResolvesLabelAndKeepsItsStaticCommandOrDropsWhenUnbound);
    RUN(statusActionsResolveByIdAndDropEmptyLabels);
    RUN(footerPromptControlsResolveValuesTogglesAndTheActiveInput);
    RUN(populateHonorsAnAuthoredRoleOrFallsBackWhenItIsUnknown);
    RUN(populateThrowsWhenAFixedNodeIsMissingOrTheWrongKind);
    return failed == 0 ? 0 : 1;
}
