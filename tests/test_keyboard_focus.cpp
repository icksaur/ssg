// Seam test for the canonical keyboard-focus owner (spec §Keyboard focus: a
// preserved base context plus a transient capture stack). The load-bearing rules:
// the effective focus is derived from base + capture stack; after any hide, focus
// references a present node; at most one prompt-backed capture exists.

#include "ssg/KeyboardFocus.h"
#include "ssg/MutationPatch.h"
#include "test_helpers.h"

#include <stdexcept>
#include <string>

namespace {

using ssg::BaseFocus;
using ssg::FocusCapture;
using ssg::FocusTarget;
using ssg::KeyboardFocus;
using ssg::PresenceConfig;
using ssg::UiNodeId;

FocusCapture prompt(std::string node) {
    return FocusCapture{UiNodeId{std::move(node)}, FocusTarget::Prompt};
}

// With no capture, the effective focus is the base context.
TEST(effectiveFocusIsTheBaseWhenNoCapture) {
    KeyboardFocus focus;
    ASSERT_TRUE(focus.effectiveTarget() == FocusTarget::Editor);
    focus.setBase(BaseFocus::Panel);
    ASSERT_TRUE(focus.effectiveTarget() == FocusTarget::Panel);
}

// A capture overrides the base; popping returns focus to what was beneath.
TEST(captureOverridesBaseAndPopRestoresIt) {
    KeyboardFocus focus;
    focus.setBase(BaseFocus::Editor);
    focus.pushCapture(prompt("palette"));
    ASSERT_TRUE(focus.effectiveTarget() == FocusTarget::Prompt);
    focus.popCapture();
    ASSERT_TRUE(focus.effectiveTarget() == FocusTarget::Editor);
}

// Only one prompt-backed capture may exist at a time -- the one-active-prompt
// guarantee, enforced by construction.
TEST(onlyOnePromptBackedCaptureIsAllowed) {
    KeyboardFocus focus;
    focus.pushCapture(prompt("palette"));
    bool threw = false;
    try {
        focus.pushCapture(prompt("finder"));
    } catch (const std::logic_error&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

// THE invariant: after a hide that removes a captured node's presence, reconcile
// pops it so the effective focus never references a hidden node.
TEST(focusNeverReferencesAHiddenNodeAfterReconcile) {
    KeyboardFocus focus;
    focus.pushCapture(prompt("palette"));
    ASSERT_TRUE(focus.effectiveTarget() == FocusTarget::Prompt);

    PresenceConfig presence;  // palette absent (hidden)
    focus.reconcile(presence);
    ASSERT_TRUE(!focus.hasCapture());
    ASSERT_TRUE(focus.effectiveTarget() == FocusTarget::Editor);
}

// Reconcile keeps a still-present capture and drops only the absent one.
TEST(reconcileDropsOnlyAbsentCaptures) {
    KeyboardFocus focus;
    // A non-prompt transient capture beneath, a prompt capture on top.
    focus.pushCapture(FocusCapture{UiNodeId{"panelOverlay"}, FocusTarget::Panel});
    focus.pushCapture(prompt("palette"));

    // A schema with both nodes; presence hides only the top (palette).
    ssg::UiSchema rawSchema;
    rawSchema.root =
        ssg::UiNode{UiNodeId{"panelOverlay"}, ssg::Size::flex(),
                    ssg::UiContainer{ssg::Axis::Column,
                                     {},
                                     {},
                                     {ssg::UiNode{UiNodeId{"palette"},
                                                  ssg::Size::flex(),
                                                  ssg::UiLeaf{}}}}};
    auto vr = ssg::ValidatedSchema::validate(rawSchema);
    const ssg::ValidatedSchema schema = vr.takeSchema();
    const PresenceConfig presence =
        PresenceConfig::initial(schema, {UiNodeId{"palette"}});
    focus.reconcile(presence);

    ASSERT_TRUE(focus.hasCapture());
    ASSERT_TRUE(focus.effectiveTarget() == FocusTarget::Panel);
}

}  // namespace

int main() {
    RUN(effectiveFocusIsTheBaseWhenNoCapture);
    RUN(captureOverridesBaseAndPopRestoresIt);
    RUN(onlyOnePromptBackedCaptureIsAllowed);
    RUN(focusNeverReferencesAHiddenNodeAfterReconcile);
    RUN(reconcileDropsOnlyAbsentCaptures);
    return failed == 0 ? 0 : 1;
}
