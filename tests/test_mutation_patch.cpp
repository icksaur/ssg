// Reference-interpreter oracle for the atomic mutation-patch model (spec §The
// mutation patch is one atomic, validated, ordered model). Each rule is an
// independently-knowable answer written against the spec, not the code:
// generation match, basis (stale) rejection, no dangling/corrupt op, conflict
// rejection, parent/child expansion, contradiction rejection, and replay
// determinism.

#include "ssg/MutationPatch.h"
#include "ssg/UiTree.h"
#include "test_helpers.h"

#include <optional>
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
using ssg::PresenceBasis;
using ssg::PresenceConfig;
using ssg::RegionRole;
using ssg::Size;
using ssg::UiContainer;
using ssg::UiLeaf;
using ssg::UiNode;
using ssg::UiNodeId;
using ssg::UiSchema;
using ssg::ValidatedSchema;
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
                  UiContainer{Axis::Column, {}, {}, std::move(children)}};
}

// One region "top" whose tree is: group[ bar, overlay[ palette ] ].
UiSchema rawSchema(Generation gen = Generation{1}) {
    UiSchema s;
    s.generation = gen;
    s.root =
        container("group",
                  {leaf("bar"), container("overlay", {leaf("palette")})});
    return s;
}

ValidatedSchema validated(Generation gen = Generation{1}) {
    auto r = ValidatedSchema::validate(rawSchema(gen));
    if (!r.ok()) throw std::logic_error("test schema must validate");
    return r.takeSchema();
}

// An all-absent presence at the schema's generation (so only the generation, not
// the presence, is what a test varies).
PresenceConfig allAbsent(const ValidatedSchema& s) {
    return PresenceConfig::initial(
        s, {UiNodeId{"group"}, UiNodeId{"bar"}, UiNodeId{"overlay"},
            UiNodeId{"palette"}});
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

MutationPatch patch(std::vector<MutationOp> ops, Generation gen = Generation{1},
                    PresenceBasis basis = PresenceBasis{0}) {
    return MutationPatch{gen, basis, ApplicationId{0}, std::move(ops)};
}

bool present(const PresenceConfig& c, std::string id) {
    return c.isPresent(UiNodeId{std::move(id)});
}

// Showing a node implies showing its ancestors on the path.
TEST(showingANodeShowsItsAncestors) {
    const ValidatedSchema s = validated();
    const PresenceConfig pre = allAbsent(s);
    const PatchResult r = applyMutationPatch(s, pre, patch({show("palette")}));
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(present(*r.post, "palette"));
    ASSERT_TRUE(present(*r.post, "overlay"));  // ancestor implied
    ASSERT_TRUE(present(*r.post, "group"));    // ancestor implied
    ASSERT_TRUE(!present(*r.post, "bar"));     // untouched
}

// Hiding a container hides its whole subtree.
TEST(hidingAContainerHidesItsSubtree) {
    const ValidatedSchema s = validated();
    const PresenceConfig pre = PresenceConfig::allPresent(s);
    const PatchResult r = applyMutationPatch(s, pre, patch({hide("overlay")}));
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(!present(*r.post, "overlay"));
    ASSERT_TRUE(!present(*r.post, "palette"));  // descendant hidden
    ASSERT_TRUE(present(*r.post, "group"));      // ancestor untouched
    ASSERT_TRUE(present(*r.post, "bar"));
}

// Toggle flips against the pre-state.
TEST(toggleFlipsAgainstThePreState) {
    const ValidatedSchema s = validated();
    // bar present (its ancestor group stays present); hide only the overlay
    // subtree. Toggling bar hides it.
    PresenceConfig pre = PresenceConfig::initial(s, {UiNodeId{"overlay"}});
    const PatchResult off = applyMutationPatch(s, pre, patch({toggle("bar")}));
    ASSERT_TRUE(off.ok());
    ASSERT_TRUE(!present(*off.post, "bar"));

    PresenceConfig pre2 = allAbsent(s);  // bar absent
    const PatchResult on = applyMutationPatch(s, pre2, patch({toggle("bar")}));
    ASSERT_TRUE(on.ok());
    ASSERT_TRUE(present(*on.post, "bar"));
}

// Two ops on one node are a conflict, rejected rather than order-resolved.
TEST(conflictingOpsOnOneNodeAreRejected) {
    const ValidatedSchema s = validated();
    const PresenceConfig pre = PresenceConfig::allPresent(s);
    const PatchResult r =
        applyMutationPatch(s, pre, patch({show("bar"), hide("bar")}));
    ASSERT_TRUE(!r.ok());
}

// Hiding an ancestor while showing a descendant is a contradiction: reject.
TEST(hidingAnAncestorWhileShowingADescendantIsRejected) {
    const ValidatedSchema s = validated();
    const PresenceConfig pre = PresenceConfig::allPresent(s);
    const PatchResult r =
        applyMutationPatch(s, pre, patch({hide("overlay"), show("palette")}));
    ASSERT_TRUE(!r.ok());
}

// An op naming a node not in the schema is rejected.
TEST(unknownTargetIsRejected) {
    const ValidatedSchema s = validated();
    const PresenceConfig pre = PresenceConfig::allPresent(s);
    const PatchResult r = applyMutationPatch(s, pre, patch({show("nonesuch")}));
    ASSERT_TRUE(!r.ok());
}

// A corrupt operation kind is rejected, never silently treated as a destructive
// Hide.
TEST(corruptOperationKindIsRejected) {
    const ValidatedSchema s = validated();
    const PresenceConfig pre = PresenceConfig::allPresent(s);
    MutationOp corrupt{static_cast<MutationOpKind>(200), UiNodeId{"bar"}};
    const PatchResult r = applyMutationPatch(s, pre, patch({corrupt}));
    ASSERT_TRUE(!r.ok());
}

// A patch that names a different generation is rejected (structure may differ).
TEST(generationMismatchIsRejected) {
    const ValidatedSchema s = validated(Generation{5});
    const PresenceConfig pre = PresenceConfig::allPresent(s);
    const PatchResult r =
        applyMutationPatch(s, pre, patch({show("bar")}, Generation{4}));
    ASSERT_TRUE(!r.ok());
}

// Presence from another generation cannot be applied against this schema.
TEST(presenceFromAnotherGenerationIsRejected) {
    const ValidatedSchema s = validated(Generation{2});
    const ValidatedSchema stranger = validated(Generation{1});
    const PresenceConfig strangerPre =
        PresenceConfig::allPresent(stranger);  // generation 1
    const PatchResult r =
        applyMutationPatch(s, strangerPre, patch({show("bar")}, Generation{2}));
    ASSERT_TRUE(!r.ok());
}

// A patch predicted against a stale basis is rejected: the authoritative presence
// has already advanced within the generation.
TEST(stalePresenceBasisIsRejected) {
    const ValidatedSchema s = validated();
    const PresenceConfig pre = PresenceConfig::allPresent(s);
    const PatchResult first = applyMutationPatch(s, pre, patch({hide("bar")}));
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(first.post->basis() == pre.basis().next());  // basis advanced

    // A second patch still predicted against the original basis is now stale.
    const PatchResult stale = applyMutationPatch(
        s, *first.post, patch({show("bar")}, Generation{1}, pre.basis()));
    ASSERT_TRUE(!stale.ok());
}

// Replay: the same patch from the same pre-state yields the identical post-state,
// regardless of op order within the patch (disjoint show/hide after expansion).
TEST(replayIsDeterministicAndOrderIndependent) {
    const ValidatedSchema s = validated();
    const PresenceConfig pre = PresenceConfig::allPresent(s);
    const PatchResult ra =
        applyMutationPatch(s, pre, patch({hide("bar"), show("palette")}));
    const PatchResult rb =
        applyMutationPatch(s, pre, patch({show("palette"), hide("bar")}));
    ASSERT_TRUE(ra.ok());
    ASSERT_TRUE(rb.ok());
    ASSERT_TRUE(*ra.post == *rb.post);
    const PatchResult ra2 =
        applyMutationPatch(s, pre, patch({hide("bar"), show("palette")}));
    ASSERT_TRUE(*ra.post == *ra2.post);
}

// Initial hiding applies subtree semantics: hiding a container seeds its whole
// subtree absent, exactly as a patch hide would.
TEST(initialHidingHidesTheSubtree) {
    const ValidatedSchema s = validated();
    const PresenceConfig pre = PresenceConfig::initial(s, {UiNodeId{"overlay"}});
    ASSERT_TRUE(!pre.isPresent(UiNodeId{"overlay"}));
    ASSERT_TRUE(!pre.isPresent(UiNodeId{"palette"}));  // descendant seeded absent
    ASSERT_TRUE(pre.isPresent(UiNodeId{"bar"}));        // sibling still present
}

// A reconciled patch carries the application id the acknowledgment will name.
TEST(reconciledPatchCarriesItsApplicationId) {
    const MutationPatch p{Generation{1}, PresenceBasis{0}, ApplicationId{42},
                          {show("bar")}};
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
    RUN(corruptOperationKindIsRejected);
    RUN(generationMismatchIsRejected);
    RUN(presenceFromAnotherGenerationIsRejected);
    RUN(stalePresenceBasisIsRejected);
    RUN(replayIsDeterministicAndOrderIndependent);
    RUN(initialHidingHidesTheSubtree);
    RUN(reconciledPatchCarriesItsApplicationId);
    return failed == 0 ? 0 : 1;
}
