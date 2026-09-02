// Seam test for the node-only keyboard-focus stack. Schema-owned context
// validation belongs to UiInteractionState.

#include "ssg/KeyboardFocus.h"
#include "ssg/MutationPatch.h"
#include "test_helpers.h"

#include <stdexcept>
#include <string>

namespace {

using ssg::BaseFocus;
using ssg::FocusCapture;
using ssg::KeyboardFocus;
using ssg::PresenceConfig;
using ssg::UiNodeId;

FocusCapture prompt(std::string node) {
    return FocusCapture{UiNodeId{std::move(node)}};
}

TEST(baseFocusIsRetainedWhenNoCapture) {
    KeyboardFocus focus;
    ASSERT_TRUE(focus.base() == BaseFocus::Editor);
    focus.setBase(BaseFocus::Panel);
    ASSERT_TRUE(focus.base() == BaseFocus::Panel);
}

// A capture overrides the base; popping returns focus to what was beneath.
TEST(captureOverridesBaseAndPopRestoresIt) {
    KeyboardFocus focus;
    focus.setBase(BaseFocus::Editor);
    focus.pushCapture(prompt("palette"));
    ASSERT_TRUE(focus.captures().size() == 1);
    ASSERT_TRUE(focus.captures().front().node == UiNodeId{"palette"});
    focus.popCapture();
    ASSERT_TRUE(focus.captures().empty());
}

// THE invariant: after a hide that removes a captured node's presence, reconcile
// pops it so the effective focus never references a hidden node.
TEST(focusNeverReferencesAHiddenNodeAfterReconcile) {
    KeyboardFocus focus;
    focus.pushCapture(prompt("palette"));

    PresenceConfig presence;  // palette absent (hidden)
    focus.reconcile(presence);
    ASSERT_TRUE(!focus.hasCapture());
}

// Reconcile keeps a still-present capture and drops only the absent one.
TEST(reconcileDropsOnlyAbsentCaptures) {
    KeyboardFocus focus;
    focus.pushCapture(FocusCapture{UiNodeId{"panelOverlay"}});
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
    ASSERT_TRUE(focus.top()->node == UiNodeId{"panelOverlay"});
}

}  // namespace

SSG_TEST_SUITE(test_keyboard_focus) {
    RUN(baseFocusIsRetainedWhenNoCapture);
    RUN(captureOverridesBaseAndPopRestoresIt);
    RUN(focusNeverReferencesAHiddenNodeAfterReconcile);
    RUN(reconcileDropsOnlyAbsentCaptures);
    return failed == 0 ? 0 : 1;
}
