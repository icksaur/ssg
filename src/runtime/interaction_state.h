#pragma once

// Private owner of node visibility and keyboard-focus capture for one schema.

#include <ssg/KeyboardFocus.h>
#include <ssg/UiTree.h>

#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace ssg {

class UiInteractionState {
public:
    explicit UiInteractionState(UiSchema schema,
                                std::vector<UiNodeId> hidden = {})
        : schema_{std::move(schema)} {
        for (const auto& id : hidden) setUiNodeVisible(schema_, id, false);
    }

    [[nodiscard]] const UiSchema& schema() const noexcept {
        return schema_;
    }
    [[nodiscard]] const KeyboardFocus& focus() const noexcept { return focus_; }
    [[nodiscard]] FocusTarget effectiveFocus() const {
        if (const FocusCapture* capture = focus_.top()) {
            return focusContext(capture->node);
        }
        return focusContext(baseNode(focus_.base()));
    }
    // CONTRACT: The path is ordered base-to-top, contains only nodes from this
    // schema, and ends at a visible node. The base may be temporarily hidden by
    // the transient surface that captured focus above it.
    [[nodiscard]] std::vector<UiNodeId> focusPath() const {
        UiNodeId base = baseNode(focus_.base());
        if (!findUiNode(schema_, base)) {
            throw std::logic_error(
                "UiInteractionState: base focus host is outside the schema");
        }
        std::vector<UiNodeId> path;
        path.reserve(focus_.captures().size() + 1);
        path.push_back(std::move(base));
        for (const FocusCapture& capture : focus_.captures()) {
            const UiNode* node = findUiNode(schema_, capture.node);
            if (!node || !node->focusContext ||
                !isUiNodeVisible(schema_, capture.node)) {
                throw std::logic_error(
                    "UiInteractionState: focus capture host is absent");
            }
            path.push_back(capture.node);
        }
        if (!isUiNodeVisible(schema_, path.back())) {
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
    // schema AND visible, so focus can never be placed on an unknown or hidden
    // node; a second prompt-backed capture is rejected by KeyboardFocus.
    void captureFocus(FocusCapture capture) {
        if (!findUiNode(schema_, capture.node)) {
            throw std::logic_error(
                "UiInteractionState: capturing focus on a node outside the "
                "schema");
        }
        if (!isUiNodeVisible(schema_, capture.node)) {
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

private:
    [[nodiscard]] static UiNodeId baseNode(BaseFocus base) {
        return UiNodeId{std::string{base == BaseFocus::Editor
                                        ? kEditorNodeId
                                        : kPanelNodeId}};
    }

    [[nodiscard]] FocusTarget focusContext(const UiNodeId& id) const {
        const UiNode* node = findUiNode(schema_, id);
        if (!node || !node->focusContext) {
            throw std::logic_error(
                "UiInteractionState: focus host has no declared context");
        }
        return *node->focusContext;
    }

    UiSchema schema_;
    KeyboardFocus focus_;
};

}  // namespace ssg
