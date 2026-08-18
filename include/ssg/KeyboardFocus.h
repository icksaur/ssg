#pragma once

// The canonical keyboard-focus owner: a persistent base context plus a transient
// LIFO capture stack, coupled to node presence in one type.
//
// Focus changes in two ways that one flat value cannot model. Moving between the
// editor and a panel happens while both stay present -- a change of base context,
// not a push or pop. Moving onto a transient surface (palette, finder, a footer
// prompt) is a CAPTURE that must be returned when the surface closes. So the base
// context is FocusTarget without its transient Prompt member (Editor, Panel), and
// transient surfaces live on a capture stack layered above it.
//
// The EFFECTIVE focus -- the single FocusTarget keymap routing consumes, including
// today's FocusTarget::Prompt -- is DERIVED: the top capture's context if the
// stack is non-empty, else the base context. So keymapContexts() still yields the
// same closed set, but the prompt context now comes from the top capture, giving
// prompt focus one authority instead of two.
//
// Presence is coupled here: reconcile() removes every capture whose node is no
// longer present, so focus can never reference a hidden node. Hiding a captured
// surface (or an ancestor of one) pops it; the invariant is enforced against the
// authoritative presence, not predicted per client.

#include <ssg/MutationPatch.h>  // PresenceConfig
#include <ssg/UiTree.h>         // UiNodeId
#include <ssg/focus.h>          // FocusTarget

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ssg {

// The persistent surfaces that are always present: FocusTarget without its
// transient Prompt member. A base value is never a capture-stack entry.
enum class BaseFocus : std::uint8_t { Editor, Panel };

// A transient surface that captured focus: the node holding it and the keymap
// context it routes to (e.g. FocusTarget::Prompt for a prompt-backed surface).
struct FocusCapture {
    UiNodeId node;
    FocusTarget context = FocusTarget::Prompt;

    friend bool operator==(const FocusCapture&, const FocusCapture&) = default;
};

class KeyboardFocus {
public:
    KeyboardFocus() = default;

    void setBase(BaseFocus base) noexcept { base_ = base; }
    [[nodiscard]] BaseFocus base() const noexcept { return base_; }

    // Push a transient capture. At most ONE prompt-backed (context == Prompt)
    // capture may exist at a time -- the same one-active-prompt guarantee
    // PromptSurface gives -- so a second is rejected by construction.
    void pushCapture(FocusCapture capture) {
        if (capture.context == FocusTarget::Prompt && hasPromptCapture()) {
            throw std::logic_error(
                "KeyboardFocus: a second prompt-backed capture");
        }
        captures_.push_back(std::move(capture));
    }

    void popCapture() noexcept {
        if (!captures_.empty()) captures_.pop_back();
    }

    [[nodiscard]] bool hasCapture() const noexcept {
        return !captures_.empty();
    }
    [[nodiscard]] const FocusCapture* top() const noexcept {
        return captures_.empty() ? nullptr : &captures_.back();
    }

    // The single FocusTarget keymap routing consumes: the top capture's context,
    // else the base context. This is the ONE authority for the effective focus.
    [[nodiscard]] FocusTarget effectiveTarget() const noexcept {
        if (!captures_.empty()) return captures_.back().context;
        return base_ == BaseFocus::Editor ? FocusTarget::Editor
                                          : FocusTarget::Panel;
    }

    // The effective focus AS A LEGACY CLIENT SEES IT: the top capture whose
    // context is not ExternalModification, else the base. The legacy wire `focus`
    // field projects through this so the closed {Editor,Panel,Prompt} set an old
    // client can decode is never widened by the internal ExternalModification
    // capture; the external-focus state travels as its own additive wire field.
    [[nodiscard]] FocusTarget legacyEffectiveTarget() const noexcept {
        for (auto it = captures_.rbegin(); it != captures_.rend(); ++it) {
            if (it->context != FocusTarget::ExternalModification) {
                return it->context;
            }
        }
        return base_ == BaseFocus::Editor ? FocusTarget::Editor
                                          : FocusTarget::Panel;
    }

    // Remove every capture whose node is not present. After this, the effective
    // focus references a present node (base surfaces are always present), so a
    // hide can never strand focus on a hidden node.
    void reconcile(const PresenceConfig& presence) {
        std::erase_if(captures_, [&](const FocusCapture& capture) {
            return !presence.isPresent(capture.node);
        });
    }

private:
    [[nodiscard]] bool hasPromptCapture() const noexcept {
        for (const auto& capture : captures_) {
            if (capture.context == FocusTarget::Prompt) return true;
        }
        return false;
    }

    BaseFocus base_ = BaseFocus::Editor;
    std::vector<FocusCapture> captures_;
};

}  // namespace ssg
