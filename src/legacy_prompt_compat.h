#pragma once

#include <ssg/PromptSurface.h>
#include <ssg/StatusQueue.h>
#include <ssg/UiFrame.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ssg::detail {

struct LegacyPromptViewResult {
    bool valid = false;
    std::optional<PromptView> view;
};

inline bool promptNodeEffectivelyPresent(
    const UiNode& node, const UiNodeId& wanted, bool ancestorsPresent,
    const std::map<UiNodeId, bool>& direct) {
    const auto found = direct.find(node.id);
    if (found == direct.end()) return false;
    const bool present = ancestorsPresent && found->second;
    if (node.id == wanted) return present;
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children) {
            if (promptNodeEffectivelyPresent(
                    child, wanted, present, direct)) {
                return true;
            }
        }
    }
    return false;
}

inline LegacyPromptViewResult legacyPromptView(
    const PromptStatusViewState& status, const UiFrame& frame) {
    const UiNodeId rootId{std::string{kFooterPromptNodeId}};
    const UiNode* root = findUiNode(frame.schema(), rootId);
    const auto* rootContainer =
        root ? std::get_if<UiContainer>(&root->content) : nullptr;
    if (!root || !rootContainer) return {};

    std::map<UiNodeId, bool> direct;
    for (const auto& record : frame.presence().nodes) {
        direct.emplace(record.id, record.present);
    }
    const bool rootPresent = promptNodeEffectivelyPresent(
        frame.schema().root, rootId, true, direct);
    if (!status.activeKind ||
        promptFocusRegion(*status.activeKind) != PromptRegion::Footer) {
        return {!rootPresent || rootContainer->children.empty(), std::nullopt};
    }
    if (!rootPresent || !root->accessibleLabel) return {};

    std::map<UiNodeId, const UiNodeState*> states;
    for (const auto& state : frame.state().nodes) {
        states.emplace(state.id, &state);
    }
    PromptView view;
    view.kind = *status.activeKind;
    view.accessibleLabel = *root->accessibleLabel;
    std::size_t inputIndex = 0;
    std::optional<std::size_t> activeInput;
    const auto addControl = [&](const UiNode& node,
                                PromptControlKind kind) -> bool {
        const auto* leaf = std::get_if<UiLeaf>(&node.content);
        const auto state = states.find(node.id);
        if (!leaf || state == states.end() || !state->second->leaf) {
            return false;
        }
        const WidgetDescriptor& widget = leaf->widget;
        const UiLeafState& value = *state->second->leaf;
        PromptControl control;
        control.kind = kind;
        control.id = widget.id;
        control.accessibleLabel = value.label;
        control.value = value.value;
        control.checked = value.checked.value_or(false);
        control.command = value.command.value_or("");
        if (kind == PromptControlKind::Input) {
            if (widget.kind != WidgetKind::TextInput || !value.active ||
                control.command.empty()) {
                return false;
            }
            if (*value.active) {
                if (activeInput) return false;
                activeInput = inputIndex;
            }
            ++inputIndex;
        } else if (kind == PromptControlKind::Toggle) {
            if (widget.kind != WidgetKind::Checkbox || !value.checked ||
                value.active || control.command.empty()) {
                return false;
            }
        } else if (widget.kind != WidgetKind::Label || value.checked ||
                   value.active || value.command) {
            return false;
        }
        view.controls.push_back(std::move(control));
        return true;
    };

    bool sawOptions = false;
    for (const auto& child : rootContainer->children) {
        if (child.id.value() == kFooterPromptOptionsNodeId) {
            if (sawOptions) return {};
            sawOptions = true;
            const auto* options = std::get_if<UiContainer>(&child.content);
            if (!options) return {};
            bool sawCount = false;
            for (const auto& option : options->children) {
                const auto* leaf = std::get_if<UiLeaf>(&option.content);
                if (!leaf) return {};
                if (leaf->widget.kind == WidgetKind::Checkbox) {
                    if (sawCount ||
                        !addControl(option, PromptControlKind::Toggle)) {
                        return {};
                    }
                } else if (leaf->widget.kind == WidgetKind::Label) {
                    if (sawCount ||
                        !addControl(option, PromptControlKind::Count)) {
                        return {};
                    }
                    sawCount = true;
                } else {
                    return {};
                }
            }
        } else {
            if (sawOptions ||
                !addControl(child, PromptControlKind::Input)) {
                return {};
            }
        }
    }
    if (!activeInput || inputIndex == 0) return {};
    view.activeInput = *activeInput;
    return {true, std::move(view)};
}

}  // namespace ssg::detail
