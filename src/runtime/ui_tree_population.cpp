#include "ui_tree_population.h"

#include <ssg/Theme.h>  // semanticRoleFromName

#include <algorithm>
#include <stdexcept>
#include <variant>

namespace ssg::detail {

namespace {

UiNode* mutableNode(UiNode& node, const UiNodeId& id) {
    if (node.id == id) return &node;
    if (auto* container = std::get_if<UiContainer>(&node.content)) {
        for (auto& child : container->children) {
            if (auto* found = mutableNode(child, id)) return found;
        }
    }
    return nullptr;
}

UiNode& requireNode(UiSchema& schema, std::string_view id) {
    UiNode* node = mutableNode(schema.root, UiNodeId{std::string{id}});
    if (!node) {
        throw std::logic_error("populateUiTree: missing fixed node \"" +
                               std::string{id} + "\"");
    }
    return *node;
}

WidgetDescriptor& requireLeaf(UiNode& node, WidgetKind kind,
                             std::string_view id) {
    auto* leaf = std::get_if<UiLeaf>(&node.content);
    if (!leaf || leaf->widget.kind != kind) {
        throw std::logic_error("populateUiTree: node \"" + std::string{id} +
                               "\" is not the expected widget kind");
    }
    return leaf->widget;
}

UiContainer& requireContainer(UiNode& node, std::string_view id) {
    auto* container = std::get_if<UiContainer>(&node.content);
    if (!container) {
        throw std::logic_error("populateUiTree: node \"" + std::string{id} +
                               "\" is not a container");
    }
    return *container;
}

// An authored role wins when present and valid; otherwise the caller's
// context-specific default (the fixed field's own region, or the widget
// kind's fixed role) applies. Resolved ONCE here rather than re-derived by
// consumers (DIRECT-VALUE).
SemanticRole roleOr(const std::optional<std::string>& authored,
                    SemanticRole fallback) {
    if (authored) {
        if (const auto parsed = semanticRoleFromName(*authored)) return *parsed;
    }
    return fallback;
}

const StatusField* findStatusField(const std::vector<StatusField>& fields,
                                   std::string_view id) {
    for (const auto& field : fields) {
        if (field.id == id) return &field;
    }
    return nullptr;
}

// A fixed header/footer status field: dropped (EMPTY-DROP) when its region's
// projection carries no entry for `fieldId`, or that entry's value/label is
// empty.
void populateStatusField(UiSchema& schema, std::string_view nodeId,
                         const std::vector<StatusField>& fields,
                         std::string_view fieldId, SemanticRole defaultRole) {
    UiNode& node = requireNode(schema, nodeId);
    WidgetDescriptor& widget = requireLeaf(node, WidgetKind::Field, nodeId);
    const StatusField* field = findStatusField(fields, fieldId);
    if (!field || field->value.empty() || field->accessibleLabel.empty()) {
        node.resolved.reset();
        return;
    }
    node.resolved = UiLeafState{field->value, field->accessibleLabel,
                               field->commandId, std::nullopt,
                               roleOr(widget.role, defaultRole)};
}

// The footer help hint: its command is the widget's own static command
// (ACTION-BINDING); its value/label is the keymap-derived hint text, dropped
// when unbound (EMPTY-DROP).
void populateHelpHint(UiSchema& schema, const std::string& helpHintLabel) {
    UiNode& node = requireNode(schema, kFooterHintNodeId);
    WidgetDescriptor& widget =
        requireLeaf(node, WidgetKind::Field, kFooterHintNodeId);
    if (helpHintLabel.empty()) {
        node.resolved.reset();
        return;
    }
    node.resolved = UiLeafState{helpHintLabel, helpHintLabel, widget.command,
                               std::nullopt,
                               roleOr(widget.role, SemanticRole::Footer)};
}

// The footer status-action container's children were rebuilt from this exact
// `actions` list (WholeScreenAssembly::withStatusActions), in the same order
// and with the same ids, so each child's resolved value is its matching
// action's accessible label/command (ACTION-BINDING), dropped only were an
// action to carry an empty label (EMPTY-DROP).
void populateStatusActions(UiSchema& schema,
                           const std::vector<StatusActionNode>& actions) {
    UiNode& node = requireNode(schema, kFooterStatusActionsNodeId);
    UiContainer& container =
        requireContainer(node, kFooterStatusActionsNodeId);
    for (auto& child : container.children) {
        WidgetDescriptor& widget =
            requireLeaf(child, WidgetKind::Field, child.id.value());
        const auto found = std::find_if(
            actions.begin(), actions.end(), [&](const StatusActionNode& action) {
                return action.id == child.id;
            });
        if (found == actions.end() || found->accessibleLabel.empty()) {
            child.resolved.reset();
            continue;
        }
        child.resolved = UiLeafState{
            found->accessibleLabel, found->accessibleLabel,
            std::optional<std::string>{found->commandId}, std::nullopt,
            roleOr(widget.role, SemanticRole::StatusInfo)};
    }
}

WidgetKind expectedPromptControlKind(PromptControlKind kind) {
    switch (kind) {
    case PromptControlKind::Input: return WidgetKind::TextInput;
    case PromptControlKind::Toggle: return WidgetKind::Checkbox;
    case PromptControlKind::Count: return WidgetKind::Label;
    }
    throw std::logic_error("populateUiTree: unknown prompt control kind");
}

// Every footer-region prompt control the active prompt resolved
// (detail::resolveRuntimePromptControls), each found by its own fixed control
// node id. A Toggle is never dropped (its checked state is always
// meaningful); an Input or Count is dropped when its label is empty
// (EMPTY-DROP). `active` marks exactly the Nth Input among all controls that
// the prompt's own activeInput names.
void populatePromptControls(UiSchema& schema,
                            const ResolvedPromptControls& prompt) {
    std::size_t inputIndex = 0;
    for (const auto& control : prompt.controls) {
        const UiNodeId nodeId = footerPromptControlNodeId(control.id);
        UiNode& node = requireNode(schema, nodeId.value());
        WidgetDescriptor& widget = requireLeaf(
            node, expectedPromptControlKind(control.kind), nodeId.value());
        const SemanticRole role = roleOr(widget.role, SemanticRole::Prompt);
        const std::optional<std::string> command =
            control.command.empty()
                ? std::nullopt
                : std::optional<std::string>{control.command};
        switch (control.kind) {
        case PromptControlKind::Input: {
            const bool active = inputIndex == prompt.activeInput;
            ++inputIndex;
            if (control.accessibleLabel.empty()) {
                node.resolved.reset();
                break;
            }
            node.resolved = UiLeafState{control.value, control.accessibleLabel,
                                       command, std::nullopt, role,
                                       std::optional<bool>{active}};
            break;
        }
        case PromptControlKind::Toggle:
            node.resolved = UiLeafState{
                control.accessibleLabel, control.accessibleLabel, command,
                std::optional<bool>{control.checked}, role};
            break;
        case PromptControlKind::Count:
            if (control.value.empty() || control.accessibleLabel.empty()) {
                node.resolved.reset();
                break;
            }
            node.resolved = UiLeafState{control.value, control.accessibleLabel,
                                       command, std::nullopt, role};
            break;
        }
    }
}

}  // namespace

void populateUiTree(UiSchema& schema, const UiTreeValues& values) {
    populateStatusField(schema, kHeaderPathFieldNodeId, values.status.header,
                        kPathStatusFieldId, SemanticRole::Header);
    populateStatusField(schema, kHeaderBranchFieldNodeId, values.status.header,
                        kBranchStatusFieldId, SemanticRole::Header);
    populateStatusField(schema, kFooterStatusFieldNodeId, values.status.footer,
                        kStatusValueFieldId, SemanticRole::Footer);
    populateStatusField(schema, kFooterFollowFieldNodeId, values.status.footer,
                        kFollowStatusFieldId, SemanticRole::Footer);
    populateHelpHint(schema, values.helpHintLabel);
    populateStatusActions(schema, values.statusActions);
    if (values.prompt) populatePromptControls(schema, *values.prompt);
}

}  // namespace ssg::detail
