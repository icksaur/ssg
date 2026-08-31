#pragma once

// The canonical owner of focus-affecting presence AND the keyboard-focus capture
// stack, together. This is the type the runtime uses so the two can never drift:
// applying a patch updates presence and reconciles focus in ONE call (a hide and
// its induced capture removal are atomic, never a two-step a caller could half
// do), and a focus capture is admitted only onto a present node. KeyboardFocus
// and PresenceConfig remain separately testable, but a caller drives them through
// this owner rather than mutating either alone.

#include <ssg/KeyboardFocus.h>
#include <ssg/MutationPatch.h>
#include <ssg/UiTree.h>

#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace ssg {

class UiInteractionState {
public:
    // Own one schema and derive the initial presence from it, so presence, focus,
    // and patches can never reference a different schema. `hidden` names nodes
    // present-by-default-off; an id outside the schema is rejected.
    explicit UiInteractionState(ValidatedSchema schema,
                                std::vector<UiNodeId> hidden = {})
        : schema_{std::move(schema)},
          presence_{PresenceConfig::initial(schema_, hidden)} {}

    [[nodiscard]] const ValidatedSchema& schema() const noexcept {
        return schema_;
    }
    [[nodiscard]] const PresenceConfig& presence() const noexcept {
        return presence_;
    }
    [[nodiscard]] const KeyboardFocus& focus() const noexcept { return focus_; }
    [[nodiscard]] FocusTarget effectiveFocus() const {
        if (const FocusCapture* capture = focus_.top()) {
            return focusContext(capture->node);
        }
        return focusContext(baseNode(focus_.base()));
    }
    [[nodiscard]] FocusTarget legacyEffectiveFocus() const {
        for (auto it = focus_.captures().rbegin();
             it != focus_.captures().rend(); ++it) {
            const FocusTarget context = focusContext(it->node);
            if (context != FocusTarget::ExternalModification) return context;
        }
        return focusContext(baseNode(focus_.base()));
    }
    // CONTRACT: The path is ordered base-to-top, contains only nodes from this
    // schema, and ends at a present node. The base may be temporarily hidden by
    // the transient surface that captured focus above it.
    [[nodiscard]] std::vector<UiNodeId> focusPath() const {
        UiNodeId base = baseNode(focus_.base());
        if (!schema_.contains(base)) {
            throw std::logic_error(
                "UiInteractionState: base focus host is outside the schema");
        }
        std::vector<UiNodeId> path;
        path.reserve(focus_.captures().size() + 1);
        path.push_back(std::move(base));
        for (const FocusCapture& capture : focus_.captures()) {
            if (!schema_.contains(capture.node) ||
                !schema_.find(capture.node)->focusContext ||
                !presence_.isPresent(capture.node)) {
                throw std::logic_error(
                    "UiInteractionState: focus capture host is absent");
            }
            path.push_back(capture.node);
        }
        if (!presence_.isPresent(path.back())) {
            throw std::logic_error(
                "UiInteractionState: effective focus host is absent");
        }
        return path;
    }

    void setBaseFocus(BaseFocus base) {
        const FocusTarget expected = base == BaseFocus::Editor
                                         ? FocusTarget::Editor
                                         : FocusTarget::Panel;
        if (focusContext(baseNode(base)) != expected) {
            throw std::logic_error(
                "UiInteractionState: base host has the wrong focus context");
        }
        focus_.setBase(base);
    }

    // Capture focus onto a transient surface. The node must be a node of this
    // schema AND present, so focus can never be placed on an unknown or hidden
    // node; a second prompt-backed capture is rejected by KeyboardFocus.
    void captureFocus(FocusCapture capture) {
        if (!schema_.contains(capture.node)) {
            throw std::logic_error(
                "UiInteractionState: capturing focus on a node outside the "
                "schema");
        }
        if (!presence_.isPresent(capture.node)) {
            throw std::logic_error(
                "UiInteractionState: capturing focus on an absent node");
        }
        const FocusTarget context = focusContext(capture.node);
        if (context == FocusTarget::Prompt) {
            for (const auto& held : focus_.captures()) {
                if (focusContext(held.node) == FocusTarget::Prompt) {
                    throw std::logic_error(
                        "UiInteractionState: a second prompt-backed capture");
                }
            }
        }
        focus_.pushCapture(std::move(capture));
    }
    void releaseFocus() noexcept { focus_.popCapture(); }

    // Apply a patch atomically against the owned schema: update presence, then
    // reconcile focus against the new presence so any capture the patch hid is
    // popped in the SAME operation. Returns an error message on rejection, leaving
    // state unchanged; nullopt on success.
    [[nodiscard]] std::optional<std::string> apply(const MutationPatch& patch) {
        PatchResult result = applyMutationPatch(schema_, presence_, patch);
        if (!result.ok()) return result.error;
        presence_ = std::move(*result.post);
        focus_.reconcile(presence_);
        return std::nullopt;
    }

private:
    [[nodiscard]] static UiNodeId baseNode(BaseFocus base) {
        return UiNodeId{std::string{base == BaseFocus::Editor
                                        ? kEditorNodeId
                                        : kPanelNodeId}};
    }

    [[nodiscard]] FocusTarget focusContext(const UiNodeId& id) const {
        const UiNode* node = schema_.find(id);
        if (!node || !node->focusContext) {
            throw std::logic_error(
                "UiInteractionState: focus host has no declared context");
        }
        return *node->focusContext;
    }

    ValidatedSchema schema_;
    PresenceConfig presence_;
    KeyboardFocus focus_;
};

}  // namespace ssg
