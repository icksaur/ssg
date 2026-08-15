// Seam test for the canonical interaction-state owner (spec §Keyboard focus): a
// hide and its induced focus-capture removal are ONE atomic operation, and focus
// can only be captured onto a present node.

#include "ssg/InteractionState.h"
#include "ssg/MutationPatch.h"
#include "ssg/UiTree.h"
#include "test_helpers.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

using ssg::ApplicationId;
using ssg::Axis;
using ssg::BaseFocus;
using ssg::FocusCapture;
using ssg::FocusTarget;
using ssg::Generation;
using ssg::MutationOp;
using ssg::MutationOpKind;
using ssg::MutationPatch;
using ssg::PresenceBasis;
using ssg::PresenceConfig;
using ssg::RegionRole;
using ssg::Size;
using ssg::UiContainer;
using ssg::UiInteractionState;
using ssg::UiLeaf;
using ssg::UiNode;
using ssg::UiNodeId;
using ssg::UiSchema;
using ssg::ValidatedSchema;
using ssg::WidgetDescriptor;
using ssg::WidgetKind;

UiNode leaf(std::string id) {
    WidgetDescriptor w;
    w.kind = WidgetKind::Label;
    w.id = id;
    return UiNode{UiNodeId{std::move(id)}, Size::flex(), UiLeaf{std::move(w)}};
}
UiNode container(std::string id, std::vector<UiNode> children) {
    return UiNode{UiNodeId{std::move(id)}, Size::flex(),
                  UiContainer{Axis::Column, {}, {}, std::move(children)}};
}

ValidatedSchema validated() {
    UiSchema s;
    s.generation = Generation{1};
    s.root = container("group", {container("overlay", {leaf("palette")})});
    auto r = ValidatedSchema::validate(s);
    if (!r.ok()) throw std::logic_error("schema must validate");
    return r.takeSchema();
}

MutationPatch hidePatch(std::string id, PresenceBasis basis) {
    return MutationPatch{Generation{1}, basis, ApplicationId{0},
                         {MutationOp{MutationOpKind::Hide, UiNodeId{std::move(id)}}}};
}

// Focus can only be captured onto a present node.
TEST(captureOnAnAbsentNodeIsRejected) {
    UiInteractionState state{validated(), {UiNodeId{"palette"}}};

    bool threw = false;
    try {
        state.captureFocus(FocusCapture{UiNodeId{"palette"}, FocusTarget::Prompt});
    } catch (const std::logic_error&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

// THE atomicity rule: applying a hide patch that removes the focused node's
// presence pops its capture in the SAME call -- a caller never observes focus on
// a hidden node.
TEST(applyingAHideThatHidesTheFocusPopsItAtomically) {
    UiInteractionState state{validated()};
    state.captureFocus(FocusCapture{UiNodeId{"palette"}, FocusTarget::Prompt});
    ASSERT_TRUE(state.effectiveFocus() == FocusTarget::Prompt);

    // Hiding the overlay hides its subtree (palette), and the same apply reconciles
    // focus back to the base.
    const auto error =
        state.apply(hidePatch("overlay", state.presence().basis()));
    ASSERT_TRUE(!error.has_value());
    ASSERT_TRUE(!state.focus().hasCapture());
    ASSERT_TRUE(state.effectiveFocus() == FocusTarget::Editor);
}

// A rejected patch leaves state unchanged (atomic: whole or nothing).
TEST(aRejectedPatchLeavesStateUnchanged) {
    UiInteractionState state{validated()};
    state.captureFocus(FocusCapture{UiNodeId{"palette"}, FocusTarget::Prompt});

    // A stale-basis patch is rejected; focus and presence must be untouched.
    const auto error = state.apply(hidePatch("overlay", PresenceBasis{99}));
    ASSERT_TRUE(error.has_value());
    ASSERT_TRUE(state.focus().hasCapture());
    ASSERT_TRUE(state.effectiveFocus() == FocusTarget::Prompt);
}

}  // namespace

int main() {
    RUN(captureOnAnAbsentNodeIsRejected);
    RUN(applyingAHideThatHidesTheFocusPopsItAtomically);
    RUN(aRejectedPatchLeavesStateUnchanged);
    return failed == 0 ? 0 : 1;
}
