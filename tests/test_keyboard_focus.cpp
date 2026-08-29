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
    ASSERT_TRUE(focus.captures().size() == 1);
    ASSERT_TRUE(focus.captures().front().node == UiNodeId{"palette"});
    focus.popCapture();
    ASSERT_TRUE(focus.captures().empty());
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

FocusCapture external(std::string node) {
    return FocusCapture{UiNodeId{std::move(node)}, FocusTarget::ExternalModification};
}

// The legacy wire projection never widens the closed {Editor,Panel,Prompt} set:
// while the internal ExternalModification capture is the true effective focus, the
// legacy projection is the surface an old client would see beneath it -- the base,
// or the top non-external capture when a prompt is also held.
TEST(theFocusWireFieldStaysInTheLegacyEnumSetWhenExternalHoldsFocus) {
    KeyboardFocus focus;
    focus.setBase(BaseFocus::Editor);
    focus.pushCapture(external("externalmod"));
    ASSERT_TRUE(focus.effectiveTarget() == FocusTarget::ExternalModification);
    ASSERT_TRUE(focus.legacyEffectiveTarget() == FocusTarget::Editor);

    // External beneath a prompt: the true focus is the prompt, and the legacy
    // projection is the prompt too (the top non-external capture), never external.
    focus.pushCapture(prompt("palette"));
    ASSERT_TRUE(focus.effectiveTarget() == FocusTarget::Prompt);
    ASSERT_TRUE(focus.legacyEffectiveTarget() == FocusTarget::Prompt);
}

// The legacy projection is always a value an old three-value decode array can
// accept, so a snapshot published while external focus is held never fails an old
// client's focus decode.
TEST(anOldClientDecodeToleratesAFocusHeldExternalSnapshot) {
    auto isLegacy = [](FocusTarget t) {
        return t == FocusTarget::Editor || t == FocusTarget::Panel ||
               t == FocusTarget::Prompt;
    };
    for (BaseFocus base : {BaseFocus::Editor, BaseFocus::Panel}) {
        KeyboardFocus focus;
        focus.setBase(base);
        focus.pushCapture(external("externalmod"));
        ASSERT_TRUE(isLegacy(focus.legacyEffectiveTarget()));
        focus.pushCapture(prompt("palette"));
        ASSERT_TRUE(isLegacy(focus.legacyEffectiveTarget()));
    }
}

// The additive `external_focus_held` wire bool is published from the EFFECTIVE
// (top) focus, not mere presence of the external capture on the stack. A prompt
// captured above external makes external NOT effective, so the bool is false and
// the legacy `focus` field (Prompt) reconstructs focus unambiguously.
TEST(aPromptAboveExternalPublishesPromptAndExternalNotEffective) {
    KeyboardFocus focus;
    focus.setBase(BaseFocus::Editor);
    focus.pushCapture(external("externalmod"));
    focus.pushCapture(prompt("palette"));

    // Legacy focus published to the wire is Prompt (the top non-external capture).
    ASSERT_TRUE(focus.legacyEffectiveTarget() == FocusTarget::Prompt);
    // The additive bool source: external is NOT the effective top, so false.
    ASSERT_TRUE(!(focus.effectiveTarget() == FocusTarget::ExternalModification));
    // A new client reconstructs: bool false => use legacy focus => Prompt.
}

// The additive bool is true ONLY when external is the top capture; a prompt above
// it, or no external capture at all, both yield false.
TEST(externalIsEffectiveOnlyWhenItIsTheTopCapture) {
    KeyboardFocus focus;
    focus.setBase(BaseFocus::Editor);
    ASSERT_TRUE(!(focus.effectiveTarget() == FocusTarget::ExternalModification));

    focus.pushCapture(external("externalmod"));
    ASSERT_TRUE(focus.effectiveTarget() == FocusTarget::ExternalModification);

    focus.pushCapture(prompt("palette"));
    ASSERT_TRUE(!(focus.effectiveTarget() == FocusTarget::ExternalModification));

    focus.popCapture();
    ASSERT_TRUE(focus.effectiveTarget() == FocusTarget::ExternalModification);
}

}  // namespace

int main() {
    RUN(effectiveFocusIsTheBaseWhenNoCapture);
    RUN(captureOverridesBaseAndPopRestoresIt);
    RUN(onlyOnePromptBackedCaptureIsAllowed);
    RUN(focusNeverReferencesAHiddenNodeAfterReconcile);
    RUN(reconcileDropsOnlyAbsentCaptures);
    RUN(theFocusWireFieldStaysInTheLegacyEnumSetWhenExternalHoldsFocus);
    RUN(aPromptAboveExternalPublishesPromptAndExternalNotEffective);
    RUN(externalIsEffectiveOnlyWhenItIsTheTopCapture);
    RUN(anOldClientDecodeToleratesAFocusHeldExternalSnapshot);
    return failed == 0 ? 0 : 1;
}
