#pragma once

// Private owner of node visibility and keyboard-focus capture for one schema.

#include <ssg/UiTree.h>
#include <ssg/focus.h>

#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace ssg {

enum class BaseFocus : std::uint8_t { Editor, Panel };

struct FocusCapture {
    UiNodeId node;

    friend bool operator==(const FocusCapture&, const FocusCapture&) = default;
};

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
    // CONTRACT: The path is ordered base-to-top, contains only nodes from this
    // schema, and ends at a visible node. The base may be temporarily hidden by
    // the transient surface that captured focus above it.
    [[nodiscard]] std::vector<UiNodeId> focusPath() const {
        UiNodeId base = baseNode(baseFocus_);
        if (!findUiNode(schema_, base)) {
            throw std::logic_error(
                "UiInteractionState: base focus host is outside the schema");
        }
        std::vector<UiNodeId> path;
        path.reserve(captures_.size() + 1);
        path.push_back(std::move(base));
        for (const FocusCapture& capture : captures_) {
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
        baseFocus_ = base;
    }

    // Capture focus onto a transient surface. The node must be a node of this
    // schema AND visible, so focus can never be placed on an unknown or hidden
    // node; a second prompt-backed capture is rejected.
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
            for (const auto& held : captures_) {
                if (focusContext(held.node) == FocusTarget::Prompt) {
                    throw std::logic_error(
                        "UiInteractionState: a second prompt-backed capture");
                }
            }
        }
        captures_.push_back(std::move(capture));
    }

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
    BaseFocus baseFocus_ = BaseFocus::Editor;
    std::vector<FocusCapture> captures_;
};

}  // namespace ssg
