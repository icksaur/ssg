// Reference-interpreter oracle for the atomic mutation-patch model (spec §The
// mutation patch is one atomic, validated, ordered model). Each rule is an
// independently-knowable answer written against the spec, not the code:
// generation match, no dangling target, conflict rejection, parent/child
// expansion, contradiction rejection, and replay determinism.

#include "ssg/MutationPatch.h"
#include "ssg/UiTree.h"
#include "test_helpers.h"

#include <string>
#include <vector>

namespace {

using ssg::ApplicationId;
using ssg::applyMutationPatch;
using ssg::Axis;
using ssg::Generation;
using ssg::MutationOp;
using ssg::MutationOpKind;
using ssg::MutationPatch;
using ssg::PatchResult;
using ssg::PresenceConfig;
using ssg::RegionRole;
using ssg::Size;
using ssg::UiContainer;
using ssg::UiLeaf;
using ssg::UiNode;
using ssg::UiNodeId;
using ssg::UiRegion;
using ssg::UiSchema;
using ssg::WidgetDescriptor;
using ssg::WidgetKind;

UiNode leaf(std::string id) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::Label;
    widget.id = id;
    return UiNode{UiNodeId{std::move(id)}, Size::flex(), UiLeaf{std::move(widget)}};
}

UiNode container(std::string id, std::vector<UiNode> children) {
    return UiNode{UiNodeId{std::move(id)}, Size::flex(),
                  UiContainer{Axis::Column, {}, std::move(children)}};
}

// One region "top" whose tree is: group[ bar, overlay[ palette ] ].
UiSchema schema(Generation gen = Generation{1}) {
    UiSchema s;
    s.generation = gen;
    s.regions = {UiRegion{
        RegionRole::Top,
        container("group",
                  {leaf("bar"), container("overlay", {leaf("palette")})})}};
    return s;
}

MutationOp show(std::string id) {
    return MutationOp{MutationOpKind::Show, UiNodeId{std::move(id)}};
}
MutationOp hide(std::string id) {
    return MutationOp{MutationOpKind::Hide, UiNodeId{std::move(id)}};
}
MutationOp toggle(std::string id) {
    return MutationOp{MutationOpKind::Toggle, UiNodeId{std::move(id)}};
}

bool present(const PresenceConfig& c, std::string id) {
    return c.isPresent(UiNodeId{std::move(id)});
}

// Showing a node implies showing its ancestors on the path.
TEST(showingANodeShowsItsAncestors) {
    const UiSchema s = schema();
    PresenceConfig pre;  // everything absent
    const PatchResult r =
        applyMutationPatch(s, pre, {Generation{1}, ApplicationId{0}, {show("palette")}});
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(present(*r.post, "palette"));
    ASSERT_TRUE(present(*r.post, "overlay"));  // ancestor implied
    ASSERT_TRUE(present(*r.post, "group"));    // ancestor implied
    ASSERT_TRUE(!present(*r.post, "bar"));     // untouched
}

// Hiding a container hides its whole subtree.
TEST(hidingAContainerHidesItsSubtree) {
    const UiSchema s = schema();
    const PresenceConfig pre = PresenceConfig::allPresent(s);
    const PatchResult r =
        applyMutationPatch(s, pre, {Generation{1}, ApplicationId{0}, {hide("overlay")}});
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(!present(*r.post, "overlay"));
    ASSERT_TRUE(!present(*r.post, "palette"));  // descendant hidden
    ASSERT_TRUE(present(*r.post, "group"));      // ancestor untouched
    ASSERT_TRUE(present(*r.post, "bar"));
}

// Toggle flips against the pre-state.
TEST(toggleFlipsAgainstThePreState) {
    const UiSchema s = schema();
    PresenceConfig pre;
    pre.set(UiNodeId{"bar"}, true);
    const PatchResult off = applyMutationPatch(
        s, pre, {Generation{1}, ApplicationId{0}, {toggle("bar")}});
    ASSERT_TRUE(off.ok());
    ASSERT_TRUE(!present(*off.post, "bar"));

    PresenceConfig pre2;
    pre2.set(UiNodeId{"bar"}, false);
    const PatchResult on = applyMutationPatch(
        s, pre2, {Generation{1}, ApplicationId{0}, {toggle("bar")}});
    ASSERT_TRUE(on.ok());
    ASSERT_TRUE(present(*on.post, "bar"));
}

// Two ops on one node are a conflict, rejected rather than order-resolved.
TEST(conflictingOpsOnOneNodeAreRejected) {
    const UiSchema s = schema();
    const PresenceConfig pre = PresenceConfig::allPresent(s);
    const PatchResult r = applyMutationPatch(
        s, pre, {Generation{1}, ApplicationId{0}, {show("bar"), hide("bar")}});
    ASSERT_TRUE(!r.ok());
}

// Hiding an ancestor while showing a descendant is a contradiction: reject.
TEST(hidingAnAncestorWhileShowingADescendantIsRejected) {
    const UiSchema s = schema();
    const PresenceConfig pre = PresenceConfig::allPresent(s);
    const PatchResult r = applyMutationPatch(
        s, pre,
        {Generation{1}, ApplicationId{0}, {hide("overlay"), show("palette")}});
    ASSERT_TRUE(!r.ok());
}

// An op naming a node not in the schema is rejected.
TEST(unknownTargetIsRejected) {
    const UiSchema s = schema();
    const PresenceConfig pre = PresenceConfig::allPresent(s);
    const PatchResult r = applyMutationPatch(
        s, pre, {Generation{1}, ApplicationId{0}, {show("nonesuch")}});
    ASSERT_TRUE(!r.ok());
}

// A patch that names a different generation is rejected (structure may differ).
TEST(generationMismatchIsRejected) {
    const UiSchema s = schema(Generation{5});
    const PresenceConfig pre = PresenceConfig::allPresent(s);
    const PatchResult r = applyMutationPatch(
        s, pre, {Generation{4}, ApplicationId{0}, {show("bar")}});
    ASSERT_TRUE(!r.ok());
}

// Replay: the same patch from the same pre-state yields the identical post-state,
// regardless of op order within the patch (disjoint show/hide after expansion).
TEST(replayIsDeterministicAndOrderIndependent) {
    const UiSchema s = schema();
    const PresenceConfig pre = PresenceConfig::allPresent(s);
    const MutationPatch a{Generation{1}, ApplicationId{0}, {hide("bar"), show("palette")}};
    const MutationPatch b{Generation{1}, ApplicationId{0}, {show("palette"), hide("bar")}};
    const PatchResult ra = applyMutationPatch(s, pre, a);
    const PatchResult rb = applyMutationPatch(s, pre, b);
    ASSERT_TRUE(ra.ok());
    ASSERT_TRUE(rb.ok());
    ASSERT_TRUE(*ra.post == *rb.post);
    // And applying twice from the same pre gives the same answer.
    const PatchResult ra2 = applyMutationPatch(s, pre, a);
    ASSERT_TRUE(*ra.post == *ra2.post);
}

// A reconciled patch carries the application id the acknowledgment will name.
TEST(reconciledPatchCarriesItsApplicationId) {
    const MutationPatch p{Generation{1}, ApplicationId{42}, {show("bar")}};
    ASSERT_EQ(p.applicationId, ApplicationId{42});
}

}  // namespace

int main() {
    RUN(showingANodeShowsItsAncestors);
    RUN(hidingAContainerHidesItsSubtree);
    RUN(toggleFlipsAgainstThePreState);
    RUN(conflictingOpsOnOneNodeAreRejected);
    RUN(hidingAnAncestorWhileShowingADescendantIsRejected);
    RUN(unknownTargetIsRejected);
    RUN(generationMismatchIsRejected);
    RUN(replayIsDeterministicAndOrderIndependent);
    RUN(reconciledPatchCarriesItsApplicationId);
    return failed == 0 ? 0 : 1;
}
