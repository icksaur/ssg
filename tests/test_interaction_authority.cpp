// Seam oracle for InteractionAuthority -- the single owner of the schema, prompt,
// truth, interaction projection, and the tree revision source. Proves: apply() routes a
// transition through one atomic prepare+install; a rejected transition mutates nothing;
// ShowPanelProvider creates and stamps tree backing from the owned revision source; every
// generic prompt lifecycle path (open/submit/cancel/update) keeps prompt and focus
// consistent and reconciles a stale picker identity; the revision source is monotonic and
// the sole minter; and a structural schema change migrates interaction+truth atomically
// while preserving valid panel and prompt truth.

#include "ssg/InteractionAuthority.h"

#include "ssg/StatusFields.h"
#include "ssg/Style.h"
#include "ssg/UiTree.h"
#include "ssg/WholeScreenAssembly.h"
#include "test_helpers.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ssg;

StatusFieldCatalogEntry entry(std::string id, StatusFieldRegion region) {
    StatusFieldCatalogEntry e;
    e.id = std::move(id);
    e.region = region;
    return e;
}

UiComposition assemble(const StyleDimensions& dims) {
    return assembleWholeScreen({entry("path", StatusFieldRegion::Header),
                                entry("mode", StatusFieldRegion::Footer)},
                               "help.open", dims, Style{}.inputLineSigil);
}

// A TreeModel seeded with the always-present filesystem provider (empty nodes suffice).
TreeModel seededTree() {
    TreeModel tree;
    tree.replaceProvider(TreeProviderSnapshot{
        TreeProviderId{"filesystem"}, TreeProviderKind::Filesystem, TreeRevision{0}, {}});
    (void)tree.activateProvider(TreeProviderId{"filesystem"});
    return tree;
}

bool present(const InteractionAuthority& a, std::string_view id) {
    return isUiNodeVisible(a.schema(),
                           UiNodeId{std::string{id}});
}

PromptRequest footerPrompt() {
    return PromptRequest{PromptKind::CommandArgument, "save as",
                         {{"path", "path", ""}}, {}, std::nullopt};
}

// The revision of a present provider by id, via the node-free identity enumeration.
std::optional<TreeRevision> revisionOf(const TreeModel& tree, std::string_view id) {
    for (const auto& identity : tree.providerIdentities()) {
        if (identity.binding.id == TreeProviderId{std::string{id}}) {
            return identity.revision;
        }
    }
    return std::nullopt;
}

// --- Initial state ------------------------------------------------------------------

TEST(initiallyNoPanelNoPromptTabViewShown) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_FALSE(present(authority, kPanelNodeId));
    ASSERT_FALSE(authority.openPicker().has_value());
    ASSERT_TRUE(present(authority, kEditorNodeId));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
}

// --- apply(): transition chokepoint -------------------------------------------------

TEST(applyOpenFinderRoutesThroughOneAtomicInstall) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.apply(OpenFinder{PickerKind::File}));
    ASSERT_TRUE(authority.openPicker().has_value());
    ASSERT_TRUE(*authority.openPicker() == PickerKind::File);
    ASSERT_TRUE(authority.prompt().active());
    ASSERT_TRUE(present(authority, kFindResultsNodeId));
    ASSERT_FALSE(present(authority, kEditorNodeId));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
}

TEST(eachSuccessfulFinderOpenMintsANewActivation) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.apply(OpenFinder{PickerKind::File}));
    const auto first = authority.openPickerActivation();
    ASSERT_TRUE(first.has_value());
    if (!first) return;
    ASSERT_TRUE(first->id.valid());
    ASSERT_TRUE(first->mode == SearchMode::File);

    ASSERT_TRUE(authority.apply(OpenFinder{PickerKind::File}));
    const auto second = authority.openPickerActivation();
    ASSERT_TRUE(second.has_value());
    if (!second) return;
    ASSERT_TRUE(second->mode == SearchMode::File);
    ASSERT_FALSE(second->id == first->id);
}

TEST(exhaustedPickerActivationSourceRejectsOpenAtomically) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{
        assemble(StyleDimensions{}), tree, 1,
        PickerActivationId{std::numeric_limits<std::uint64_t>::max()}};
    ASSERT_FALSE(authority.apply(OpenFinder{PickerKind::Command}));
    ASSERT_FALSE(present(authority, kPanelNodeId));
    ASSERT_FALSE(authority.openPickerActivation().has_value());
    ASSERT_FALSE(authority.prompt().active());
}

TEST(applyShowProviderCreatesTreeBackingFromTheOwnedSource) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree, 5};
    ASSERT_FALSE(revisionOf(tree, "git").has_value());
    ASSERT_TRUE(authority.apply(ShowPanelProvider{
        builtInPanelTreeProvider(TreeProviderKind::Git)}));
    ASSERT_TRUE(present(authority, kPanelNodeId));
    ASSERT_TRUE(tree.activeProviderBinding() ==
                builtInPanelTreeProvider(TreeProviderKind::Git));
    // The git provider was created and stamped from the authority's revision source (5).
    const auto gitRevision = revisionOf(tree, "git");
    ASSERT_TRUE(gitRevision.has_value());
    ASSERT_EQ(gitRevision->value(), std::uint64_t{5});
}

TEST(applyRejectionMutatesNothing) {
    TreeModel empty;  // no filesystem provider -> ShowPanelProvider{FileTree} rejects
    InteractionAuthority authority{assemble(StyleDimensions{}), empty};
    const FocusTarget focusBefore = authority.effectiveFocus();
    ASSERT_FALSE(authority.apply(ShowPanelProvider{
        builtInPanelTreeProvider(TreeProviderKind::Filesystem)}));
    ASSERT_FALSE(present(authority, kPanelNodeId));
    ASSERT_TRUE(authority.effectiveFocus() == focusBefore);
    ASSERT_FALSE(authority.prompt().active());
}

// --- Generic prompt lifecycle -------------------------------------------------------

TEST(genericOpenPromptFocusesFooterWithoutAPicker) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.openPrompt(footerPrompt()).accepted());
    ASSERT_FALSE(authority.openPicker().has_value());
    ASSERT_FALSE(authority.openPickerActivation().has_value());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(present(authority, kEditorNodeId));
    ASSERT_FALSE(present(authority, kFindResultsNodeId));
}

TEST(genericPromptOverAPickerClearsTheStalePickerIdentity) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.apply(OpenFinder{PickerKind::Command}));
    ASSERT_TRUE(authority.openPicker().has_value());
    // Opening a generic (non-Palette) prompt replaces the picker prompt; its identity,
    // which is not derivable from the prompt, is reconciled away by the owner.
    ASSERT_TRUE(authority.openPrompt(footerPrompt()).accepted());
    ASSERT_FALSE(authority.openPicker().has_value());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(present(authority, kEditorNodeId));
}

TEST(cancelPromptReleasesFocus) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.openPrompt(footerPrompt()).accepted());
    ASSERT_TRUE(authority.cancelPrompt().accepted());
    ASSERT_FALSE(authority.prompt().active());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
}

TEST(openPromptRejectsAPalettePromptSoOnlyAFinderMakesAPicker) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    const auto result = authority.openPrompt(PromptRequest{
        PromptKind::Palette, "cmd", {{"query", "q", ""}}, {}, std::nullopt});
    ASSERT_FALSE(result.accepted());
    ASSERT_FALSE(authority.prompt().active());
    ASSERT_FALSE(authority.openPicker().has_value());
}

TEST(valueEditKeepsPromptFocusAndUpdatesTheInput) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.openPrompt(footerPrompt()).accepted());
    ASSERT_TRUE(authority.updatePromptValue(0, "src/main.cpp").accepted());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(authority.prompt().active());
    ASSERT_EQ(authority.prompt().request()->inputs[0].value,
              std::string{"src/main.cpp"});
}

TEST(promptFocusUsesControlIdentityAndRejectsNonInputs) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    PromptRequest find{
        PromptKind::Find,
        "Find",
        {{"find.query", "Find", ""}},
        {{"find.case", "Case", false, 8}},
        PromptMatchCount{"find.count", "Matches", "0"}};
    ASSERT_TRUE(authority.openPrompt(std::move(find)).accepted());
    ASSERT_TRUE(authority.focusPromptControl("find.query").accepted());
    ASSERT_FALSE(authority.focusPromptControl("find.case").accepted());
    ASSERT_FALSE(authority.focusPromptControl("find.count").accepted());
    ASSERT_FALSE(authority.focusPromptControl("missing").accepted());
}

// --- Revision source ----------------------------------------------------------------

TEST(allocateTreeRevisionIsMonotonic) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree, 10};
    const auto first = authority.allocateTreeRevision();
    const auto second = authority.allocateTreeRevision();
    ASSERT_EQ(first.value(), std::uint64_t{10});
    ASSERT_EQ(second.value(), std::uint64_t{11});
}

TEST(allocateTreeRevisionRejectsExhaustion) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree,
                                   std::numeric_limits<std::uint64_t>::max()};
    ASSERT_THROWS(authority.allocateTreeRevision(), std::logic_error);
}

TEST(panelProviderCycleUsesOnlyTheBuiltInCatalog) {
    const auto providers = builtInPanelTreeProviders();
    ASSERT_TRUE(!providers.empty());
    for (const auto& provider : providers) {
        const auto next =
            cyclePanelTreeProvider(provider, CycleDirection::Next);
        const auto previous =
            cyclePanelTreeProvider(provider, CycleDirection::Previous);
        ASSERT_TRUE(std::ranges::find(providers, next) != providers.end());
        ASSERT_TRUE(std::ranges::find(providers, previous) != providers.end());
    }
    const TreeProviderBinding invalid{TreeProviderId{"invalid"},
                                      TreeProviderKind::Filesystem};
    ASSERT_THROWS(cyclePanelTreeProvider(invalid, CycleDirection::Next),
                  std::logic_error);
}

TEST(switchPanelProviderPreservesPanelTruthAndRejectsInvalidRequests) {
    TreeModel hiddenTree = seededTree();
    InteractionAuthority hidden{assemble(StyleDimensions{}), hiddenTree};
    ASSERT_TRUE(hidden.apply(SwitchPanelProvider{
        builtInPanelTreeProvider(TreeProviderKind::Git)}));
    ASSERT_FALSE(present(hidden, kPanelNodeId));
    ASSERT_EQ(hidden.effectiveFocus(), FocusTarget::Editor);

    TreeModel shownTree = seededTree();
    InteractionAuthority shown{assemble(StyleDimensions{}), shownTree};
    ASSERT_TRUE(shown.apply(ShowPanelProvider{
        builtInPanelTreeProvider(TreeProviderKind::Filesystem)}));
    ASSERT_TRUE(shown.apply(SwitchPanelProvider{
        builtInPanelTreeProvider(TreeProviderKind::Symbols)}));
    ASSERT_TRUE(present(shown, kPanelNodeId));
    ASSERT_EQ(shown.effectiveFocus(), FocusTarget::Panel);
    ASSERT_TRUE(shown.apply(SwitchPanelProvider{
        builtInPanelTreeProvider(TreeProviderKind::Filesystem)}));
    ASSERT_TRUE(present(shown, kPanelNodeId));

    TreeModel emptyTree;
    InteractionAuthority empty{assemble(StyleDimensions{}), emptyTree};
    ASSERT_FALSE(empty.apply(SwitchPanelProvider{
        builtInPanelTreeProvider(TreeProviderKind::Filesystem)}));

    const TreeProviderBinding unknown{TreeProviderId{"other"},
                                      TreeProviderKind::Git};
    ASSERT_FALSE(shown.apply(ShowPanelProvider{unknown}));
    const TreeProviderBinding wrongKind{TreeProviderId{"git"},
                                        TreeProviderKind::Symbols};
    ASSERT_FALSE(shown.apply(ShowPanelProvider{wrongKind}));
    ASSERT_TRUE(present(shown, kPanelNodeId));
}

TEST(constructionRejectsARevisionSourceBehindAProvider) {
    TreeModel tree;
    tree.replaceProvider(TreeProviderSnapshot{TreeProviderId{"filesystem"},
                                              TreeProviderKind::Filesystem,
                                              TreeRevision{100}, {}});
    // A source not ahead of every provider would let a replacement fail to increase.
    ASSERT_THROWS((InteractionAuthority{assemble(StyleDimensions{}), tree, 50}),
                  std::logic_error);
}

// --- Live migration -----------------------------------------------------------------

TEST(updateCompositionMigratesPreservingPanelAndPromptTruth) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.apply(ShowPanelProvider{
        builtInPanelTreeProvider(TreeProviderKind::Filesystem)}));
    ASSERT_TRUE(authority.apply(OpenFinder{PickerKind::Command}));
    ASSERT_TRUE(present(authority, kPanelNodeId));
    ASSERT_TRUE(authority.openPicker().has_value());

    // A structural change (wider panel) migrates the schema and rebuilds.
    StyleDimensions wider;
    wider.panelTargetWidth = StyleDimensions{}.panelTargetWidth + 10;
    ASSERT_TRUE(authority.updateComposition(assemble(wider)));

    // Truth and prompt survive; the projection is rebuilt over the new schema.
    ASSERT_TRUE(present(authority, kPanelNodeId));
    ASSERT_TRUE(authority.openPicker().has_value());
    ASSERT_TRUE(*authority.openPicker() == PickerKind::Command);
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(present(authority, kFindResultsNodeId));
}

TEST(updateCompositionWithoutStructuralChangeDoesNotAdvance) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_FALSE(authority.updateComposition(assemble(StyleDimensions{})));
}

TEST(statusOverlaySurvivesPromptAndEquivalentRebuilds) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    const UiNodeId actionId{"footer.status_action/7/3/72756e"};
    ASSERT_TRUE(authority.refreshStatusActions(
        {{actionId, "Run", "build.run"}}));
    ASSERT_TRUE(uiSchemaNodeIds(authority.schema())
                    .contains(actionId));

    ASSERT_TRUE(authority.openPrompt(footerPrompt()).accepted());
    ASSERT_TRUE(authority.refreshStatusActions(
        {{actionId, "Run now", "build.run"}}));
    ASSERT_TRUE(authority.prompt().active());
    ASSERT_EQ(authority.effectiveFocus(), FocusTarget::Prompt);
    ASSERT_EQ(authority.statusActions()[0].accessibleLabel,
              std::string{"Run now"});

    ASSERT_FALSE(
        authority.updateComposition(assemble(StyleDimensions{})));
    ASSERT_TRUE(uiSchemaNodeIds(authority.schema())
                    .contains(actionId));
    ASSERT_EQ(authority.statusActions()[0].id, actionId);
    ASSERT_TRUE(authority.prompt().active());

    ASSERT_FALSE(authority.updateComposition(assemble(StyleDimensions{})));
    ASSERT_TRUE(uiSchemaNodeIds(authority.schema())
                    .contains(actionId));
    ASSERT_EQ(authority.statusActions()[0].id, actionId);
    ASSERT_TRUE(authority.prompt().active());
    ASSERT_EQ(authority.effectiveFocus(), FocusTarget::Prompt);
}

// --- Editor/panel focus -------------------------------------------------------------

TEST(promptOverPanelClosesBackToPanelFocus) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.apply(ShowPanelProvider{
        builtInPanelTreeProvider(TreeProviderKind::Filesystem)}));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Panel);
    ASSERT_TRUE(authority.apply(OpenFinder{PickerKind::Command}));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(authority.apply(CloseFinder{}));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Panel);
}

TEST(panelHideWhilePromptCapturedRestoresBaseUnderThePrompt) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.apply(ShowPanelProvider{
        builtInPanelTreeProvider(TreeProviderKind::Filesystem)}));
    ASSERT_TRUE(authority.apply(OpenFinder{PickerKind::Command}));
    // Hide the panel while the prompt is captured: the prompt still routes focus, but the
    // base focus underneath is restored to the panel-return focus (Editor).
    ASSERT_TRUE(authority.apply(TogglePanel{}));
    ASSERT_FALSE(present(authority, kPanelNodeId));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(authority.apply(CloseFinder{}));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
}

TEST(providerCyclingWhileHiddenAndEditorFocusedPreservesBoth) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    // Panel hidden, editor-focused: switching provider changes only the selection.
    ASSERT_TRUE(authority.apply(SwitchPanelProvider{
        builtInPanelTreeProvider(TreeProviderKind::Git)}));
    ASSERT_FALSE(present(authority, kPanelNodeId));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
    ASSERT_TRUE(tree.activeProviderBinding() ==
                builtInPanelTreeProvider(TreeProviderKind::Git));
}

TEST(editorFocusWithThePanelVisibleKeepsThePanelPresent) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.apply(ShowPanelProvider{
        builtInPanelTreeProvider(TreeProviderKind::Filesystem)}));
    authority.focusEditor();
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
    ASSERT_TRUE(present(authority, kPanelNodeId));  // focus moved, panel stayed
}

TEST(focusPanelRequiresThePanelThenFocusEditorReturns) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    // No panel yet -> focusPanel is refused and focus stays Editor.
    ASSERT_FALSE(authority.focusPanel());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);

    ASSERT_TRUE(authority.apply(ShowPanelProvider{
        builtInPanelTreeProvider(TreeProviderKind::Filesystem)}));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Panel);
    authority.focusEditor();
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
    ASSERT_TRUE(authority.focusPanel());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Panel);
}

TEST(focusChangeUnderAnOpenPromptSurfacesWhenThePromptCloses) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.apply(ShowPanelProvider{
        builtInPanelTreeProvider(TreeProviderKind::Filesystem)}));
    ASSERT_TRUE(authority.openPrompt(footerPrompt()).accepted());
    // The prompt capture routes effective focus regardless of the base change.
    authority.focusEditor();
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    // Closing the prompt surfaces the base focus set underneath it.
    ASSERT_TRUE(authority.cancelPrompt().accepted());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
}

// --- External-modification focus capture --------------------------------------------

TEST(theExternalModNodeIsPresentHiddenUntilAFileIsPresentAndGoldensAreUnchanged) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    // The node is always assembled but present-hidden by default -- nothing renders
    // it, which is why the grid goldens stay byte-identical.
    ASSERT_FALSE(present(authority, kExternalModNodeId));
    ASSERT_TRUE(authority.refreshExternalModificationPresence(true));
    ASSERT_TRUE(present(authority, kExternalModNodeId));
    ASSERT_TRUE(authority.refreshExternalModificationPresence(false));
    ASSERT_FALSE(present(authority, kExternalModNodeId));
}

TEST(theExternalContextIsActiveOnlyWhileTheCaptureHoldsAndFocusReturnPopsIt) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    // Present-gated: focus is refused while no file is present.
    ASSERT_FALSE(authority.captureExternalFocus());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);

    ASSERT_TRUE(authority.refreshExternalModificationPresence(true));
    ASSERT_TRUE(authority.captureExternalFocus());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::ExternalModification);
    // focus_return pops the capture, back to the base context.
    ASSERT_TRUE(authority.releaseExternalFocus());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
}

TEST(repeatedExternalFocusIsIdempotentSoOneReturnPops) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.refreshExternalModificationPresence(true));
    ASSERT_TRUE(authority.captureExternalFocus());
    // A repeated focus press is a no-op: the capture is derived from truth, never
    // stacked, so a SINGLE return pops it.
    ASSERT_FALSE(authority.captureExternalFocus());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::ExternalModification);
    ASSERT_TRUE(authority.releaseExternalFocus());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
    ASSERT_FALSE(authority.releaseExternalFocus());
}

TEST(theExternalCaptureAutoPopsWhenTheLastFileResolves) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.refreshExternalModificationPresence(true));
    ASSERT_TRUE(authority.captureExternalFocus());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::ExternalModification);
    // The last file resolves away: presence drops, the capture auto-pops, and the
    // focus flag is cleared so a later disk event never reactively re-steals focus.
    ASSERT_TRUE(authority.refreshExternalModificationPresence(false));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
    ASSERT_FALSE(authority.effectiveFocus() == FocusTarget::ExternalModification);
    ASSERT_TRUE(authority.refreshExternalModificationPresence(true));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
}

TEST(theExternalCaptureAndAPromptCoexistWithLifoActiveContext) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.refreshExternalModificationPresence(true));
    ASSERT_TRUE(authority.captureExternalFocus());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::ExternalModification);
    // A footer prompt opens on top: both captures coexist (the prompt guard binds
    // only prompt captures), and the LIFO top -- the active context -- is the prompt.
    ASSERT_TRUE(authority.openPrompt(footerPrompt()).accepted());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    // Dismissing the prompt returns to the still-held external context.
    ASSERT_TRUE(authority.cancelPrompt().accepted());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::ExternalModification);
}

}  // namespace

SSG_TEST_SUITE(test_interaction_authority) {
    RUN(initiallyNoPanelNoPromptTabViewShown);
    RUN(applyOpenFinderRoutesThroughOneAtomicInstall);
    RUN(eachSuccessfulFinderOpenMintsANewActivation);
    RUN(exhaustedPickerActivationSourceRejectsOpenAtomically);
    RUN(applyShowProviderCreatesTreeBackingFromTheOwnedSource);
    RUN(applyRejectionMutatesNothing);
    RUN(genericOpenPromptFocusesFooterWithoutAPicker);
    RUN(genericPromptOverAPickerClearsTheStalePickerIdentity);
    RUN(cancelPromptReleasesFocus);
    RUN(openPromptRejectsAPalettePromptSoOnlyAFinderMakesAPicker);
    RUN(valueEditKeepsPromptFocusAndUpdatesTheInput);
    RUN(promptFocusUsesControlIdentityAndRejectsNonInputs);
    RUN(allocateTreeRevisionIsMonotonic);
    RUN(allocateTreeRevisionRejectsExhaustion);
    RUN(panelProviderCycleUsesOnlyTheBuiltInCatalog);
    RUN(switchPanelProviderPreservesPanelTruthAndRejectsInvalidRequests);
    RUN(constructionRejectsARevisionSourceBehindAProvider);
    RUN(updateCompositionMigratesPreservingPanelAndPromptTruth);
    RUN(updateCompositionWithoutStructuralChangeDoesNotAdvance);
    RUN(statusOverlaySurvivesPromptAndEquivalentRebuilds);
    RUN(focusPanelRequiresThePanelThenFocusEditorReturns);
    RUN(focusChangeUnderAnOpenPromptSurfacesWhenThePromptCloses);
    RUN(promptOverPanelClosesBackToPanelFocus);
    RUN(panelHideWhilePromptCapturedRestoresBaseUnderThePrompt);
    RUN(providerCyclingWhileHiddenAndEditorFocusedPreservesBoth);
    RUN(editorFocusWithThePanelVisibleKeepsThePanelPresent);
    RUN(theExternalModNodeIsPresentHiddenUntilAFileIsPresentAndGoldensAreUnchanged);
    RUN(theExternalContextIsActiveOnlyWhileTheCaptureHoldsAndFocusReturnPopsIt);
    RUN(repeatedExternalFocusIsIdempotentSoOneReturnPops);
    RUN(theExternalCaptureAutoPopsWhenTheLastFileResolves);
    RUN(theExternalCaptureAndAPromptCoexistWithLifoActiveContext);
    return failed;
}
