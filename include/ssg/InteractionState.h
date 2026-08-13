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
    [[nodiscard]] FocusTarget effectiveFocus() const noexcept {
        return focus_.effectiveTarget();
    }

    void setBaseFocus(BaseFocus base) noexcept { focus_.setBase(base); }

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
    ValidatedSchema schema_;
    PresenceConfig presence_;
    KeyboardFocus focus_;
};

}  // namespace ssg
