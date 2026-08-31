#pragma once

#include <ssg/UiPresence.h>
#include <ssg/UiTree.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg::detail {

inline bool legacyFocusEffectivelyPresent(
    const UiNode& node, const UiNodeId& wanted, bool ancestorsPresent,
    const std::map<UiNodeId, bool>& direct) {
    const auto found = direct.find(node.id);
    if (found == direct.end()) return false;
    const bool present = ancestorsPresent && found->second;
    if (node.id == wanted) return present;
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children) {
            if (legacyFocusEffectivelyPresent(child, wanted, present, direct)) {
                return true;
            }
        }
    }
    return false;
}

inline std::optional<std::vector<UiNodeId>> legacyFocusPath(
    const UiSchema& schema, const UiPresenceSection& presence,
    FocusTarget focus, bool externalFocusHeld) {
    if (focus == FocusTarget::ExternalModification ||
        (focus == FocusTarget::Prompt && externalFocusHeld)) {
        return std::nullopt;
    }

    const auto id = [](std::string_view value) {
        return UiNodeId{std::string{value}};
    };
    std::map<UiNodeId, bool> direct;
    for (const auto& record : presence.nodes) {
        direct.emplace(record.id, record.present);
    }
    const auto effectivelyPresent = [&](const UiNodeId& wanted) {
        return legacyFocusEffectivelyPresent(
            schema.root, wanted, true, direct);
    };

    UiNodeId base =
        id(focus == FocusTarget::Panel ? kPanelNodeId : kEditorNodeId);
    const UiNode* baseNode = findUiNode(schema, base);
    const FocusTarget baseContext =
        focus == FocusTarget::Panel ? FocusTarget::Panel
                                    : FocusTarget::Editor;
    if (!baseNode || baseNode->focusContext != baseContext) {
        return std::nullopt;
    }

    std::vector<UiNodeId> path{base};
    if (focus == FocusTarget::Prompt) {
        std::vector<UiNodeId> prompts;
        for (const auto candidate :
             {kHeaderPromptInputNodeId, kFooterPromptNodeId}) {
            UiNodeId prompt = id(candidate);
            const UiNode* node = findUiNode(schema, prompt);
            if (node && node->focusContext == FocusTarget::Prompt &&
                effectivelyPresent(prompt)) {
                prompts.push_back(std::move(prompt));
            }
        }
        if (prompts.size() != 1) return std::nullopt;
        path.push_back(std::move(prompts.front()));
    } else if (!effectivelyPresent(base)) {
        return std::nullopt;
    }

    if (externalFocusHeld) {
        UiNodeId external = id(kExternalModNodeId);
        const UiNode* node = findUiNode(schema, external);
        if (!node ||
            node->focusContext != FocusTarget::ExternalModification ||
            !effectivelyPresent(external)) {
            return std::nullopt;
        }
        path.push_back(std::move(external));
    }
    return path;
}

}  // namespace ssg::detail
