#pragma once

// The keyboard-focus stack: a persistent base plus transient node captures.
//
// Focus changes in two ways that one flat value cannot model. Moving between the
// editor and a panel happens while both stay present -- a change of base context,
// not a push or pop. Moving onto a transient surface (palette, finder, a footer
// prompt) is a CAPTURE that must be returned when the surface closes. So the base
// context is FocusTarget without its transient Prompt member (Editor, Panel), and
// transient surfaces live on a capture stack layered above it.
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

// A transient surface that captured focus. Its keymap context belongs to the
// schema node and is resolved by the interaction authority.
struct FocusCapture {
    UiNodeId node;

    friend bool operator==(const FocusCapture&, const FocusCapture&) = default;
};

class KeyboardFocus {
public:
    KeyboardFocus() = default;

    void setBase(BaseFocus base) noexcept { base_ = base; }
    [[nodiscard]] BaseFocus base() const noexcept { return base_; }

    void pushCapture(FocusCapture capture) {
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
    [[nodiscard]] const std::vector<FocusCapture>& captures() const noexcept {
        return captures_;
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
    BaseFocus base_ = BaseFocus::Editor;
    std::vector<FocusCapture> captures_;
};

}  // namespace ssg
