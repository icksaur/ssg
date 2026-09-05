// Screen state-machine oracles: rejected operations mutate nothing, tree and
// focus projection agree, and structural changes preserve screen truth.

#include <ssg/ScreenState.h>

#include <ssg/Style.h>
#include <ssg/UiTree.h>
#include <ssg/ScreenLayout.h>
#include "test_helpers.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ssg;
UiComposition assemble(const StyleDimensions& dims) {
    return assembleScreen("help.open", dims, Style{}.inputLineSigil);
}

// A TreeModel seeded with the always-present filesystem provider (empty nodes suffice).
TreeModel seededTree() {
    TreeModel tree;
    tree.replaceProvider(TreeProviderSnapshot{
        TreeProviderId{"filesystem"}, TreeProviderKind::Filesystem, {}});
    (void)tree.activateProvider(TreeProviderId{"filesystem"});
    return tree;
}

bool present(const ScreenState& a, std::string_view id) {
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
    ScreenState authority{assemble(StyleDimensions{}), tree};
    ASSERT_FALSE(present(authority, kPanelNodeId));
    ASSERT_FALSE(authority.openPicker().has_value());
    ASSERT_TRUE(present(authority, kEditorNodeId));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
}

TEST(openFinderUpdatesPromptPresenceAndFocus) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.openFinder(PickerKind::File));
    ASSERT_TRUE(authority.openPicker().has_value());
    ASSERT_TRUE(*authority.openPicker() == PickerKind::File);
    ASSERT_TRUE(authority.prompt().active());
    ASSERT_TRUE(present(authority, kFindResultsNodeId));
    ASSERT_FALSE(present(authority, kEditorNodeId));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
}

TEST(eachSuccessfulFinderOpenMintsANewActivation) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.openFinder(PickerKind::File));
    const auto first = authority.openPickerActivation();
    ASSERT_TRUE(first.has_value());
    if (!first) return;
    ASSERT_TRUE(first->id.valid());
    ASSERT_TRUE(first->mode == SearchMode::File);

    ASSERT_TRUE(authority.openFinder(PickerKind::File));
    const auto second = authority.openPickerActivation();
    ASSERT_TRUE(second.has_value());
    if (!second) return;
    ASSERT_TRUE(second->mode == SearchMode::File);
    ASSERT_FALSE(second->id == first->id);
}

TEST(exhaustedPickerActivationSourceRejectsOpenAtomically) {
    TreeModel tree = seededTree();
    ScreenState authority{
        assemble(StyleDimensions{}), tree,
        PickerActivationId{std::numeric_limits<std::uint64_t>::max()}};
    ASSERT_FALSE(authority.openFinder(PickerKind::Command));
    ASSERT_FALSE(present(authority, kPanelNodeId));
    ASSERT_FALSE(authority.openPickerActivation().has_value());
    ASSERT_FALSE(authority.prompt().active());
}

TEST(invalidPickerActivationSourceIsRejected) {
    TreeModel tree = seededTree();
    ASSERT_THROWS(
        (ScreenState{assemble(StyleDimensions{}), tree, PickerActivationId{0}}),
        std::invalid_argument);
}

TEST(showProviderCreatesTreeBackingFromTheOwnedSource) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    ASSERT_FALSE(revisionOf(tree, "git").has_value());
    ASSERT_TRUE(authority.showPanelProvider(TreeProviderKind::Git));
    ASSERT_TRUE(present(authority, kPanelNodeId));
    ASSERT_EQ(tree.activeProviderBinding()->kind, TreeProviderKind::Git);
    const auto gitRevision = revisionOf(tree, "git");
    ASSERT_TRUE(gitRevision.has_value());
    ASSERT_TRUE(gitRevision->value() > 0);
}

TEST(rejectedShowProviderMutatesNothing) {
    TreeModel empty;
    ScreenState authority{assemble(StyleDimensions{}), empty};
    const FocusTarget focusBefore = authority.effectiveFocus();
    ASSERT_FALSE(authority.showPanelProvider(TreeProviderKind::Filesystem));
    ASSERT_FALSE(present(authority, kPanelNodeId));
    ASSERT_TRUE(authority.effectiveFocus() == focusBefore);
    ASSERT_FALSE(authority.prompt().active());
}

// --- Generic prompt lifecycle -------------------------------------------------------

TEST(genericOpenPromptFocusesFooterWithoutAPicker) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(openGenericPrompt(authority.prompt(), footerPrompt()).accepted());
    ASSERT_FALSE(authority.openPicker().has_value());
    ASSERT_FALSE(authority.openPickerActivation().has_value());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(present(authority, kEditorNodeId));
    ASSERT_FALSE(present(authority, kFindResultsNodeId));
}

TEST(genericPromptOverAPickerClearsTheStalePickerIdentity) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.openFinder(PickerKind::Command));
    ASSERT_TRUE(authority.openPicker().has_value());
    // Opening a generic (non-Palette) prompt replaces the picker prompt; its identity,
    // which is not derivable from the prompt, is reconciled away by the owner.
    ASSERT_TRUE(openGenericPrompt(authority.prompt(), footerPrompt()).accepted());
    ASSERT_FALSE(authority.openPicker().has_value());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(present(authority, kEditorNodeId));
}

TEST(cancelPromptReleasesFocus) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(openGenericPrompt(authority.prompt(), footerPrompt()).accepted());
    ASSERT_TRUE(authority.prompt().cancel().accepted());
    ASSERT_FALSE(authority.prompt().active());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
}

TEST(directPromptCancellationImmediatelyClearsPickerProjection) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.openFinder(PickerKind::Command));
    ASSERT_TRUE(authority.openPickerActivation().has_value());

    ASSERT_TRUE(authority.prompt().cancel().accepted());
    ASSERT_FALSE(authority.openPicker().has_value());
    ASSERT_FALSE(authority.openPickerActivation().has_value());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
    ASSERT_TRUE(present(authority, kEditorNodeId));
    ASSERT_FALSE(present(authority, kFindResultsNodeId));
}

TEST(openPromptRejectsAPalettePromptSoOnlyAFinderMakesAPicker) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    const auto result = openGenericPrompt(authority.prompt(), PromptRequest{
        PromptKind::Palette, "cmd", {{"query", "q", ""}}, {}, std::nullopt});
    ASSERT_FALSE(result.accepted());
    ASSERT_FALSE(authority.prompt().active());
    ASSERT_FALSE(authority.openPicker().has_value());
}

TEST(valueEditKeepsPromptFocusAndUpdatesTheInput) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(openGenericPrompt(authority.prompt(), footerPrompt()).accepted());
    ASSERT_TRUE(authority.prompt().updateValue(0, "src/main.cpp").accepted());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(authority.prompt().active());
    ASSERT_EQ(authority.prompt().request()->inputs[0].value,
              std::string{"src/main.cpp"});
}

TEST(promptFocusUsesControlIdentityAndRejectsNonInputs) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    PromptRequest find{
        PromptKind::Find,
        "Find",
        {{"find.query", "Find", ""}},
        {{"find.case", "Case", false, 8}},
        PromptMatchCount{"find.count", "Matches", "0"}};
    ASSERT_TRUE(openGenericPrompt(authority.prompt(), std::move(find)).accepted());
    ASSERT_TRUE(authority.prompt().focusInput("find.query").accepted());
    ASSERT_FALSE(authority.prompt().focusInput("find.case").accepted());
    ASSERT_FALSE(authority.prompt().focusInput("find.count").accepted());
    ASSERT_FALSE(authority.prompt().focusInput("missing").accepted());
}

TEST(switchPanelProviderPreservesPanelTruth) {
    TreeModel hiddenTree = seededTree();
    ScreenState hidden{assemble(StyleDimensions{}), hiddenTree};
    ASSERT_TRUE(hidden.switchPanelProvider(CycleDirection::Next));
    ASSERT_FALSE(present(hidden, kPanelNodeId));
    ASSERT_EQ(hidden.effectiveFocus(), FocusTarget::Editor);

    TreeModel shownTree = seededTree();
    ScreenState shown{assemble(StyleDimensions{}), shownTree};
    ASSERT_TRUE(shown.showPanelProvider(TreeProviderKind::Filesystem));
    ASSERT_TRUE(shown.switchPanelProvider(CycleDirection::Previous));
    ASSERT_TRUE(present(shown, kPanelNodeId));
    ASSERT_EQ(shown.effectiveFocus(), FocusTarget::Panel);
    ASSERT_TRUE(shown.switchPanelProvider(CycleDirection::Next));
    ASSERT_TRUE(present(shown, kPanelNodeId));

    TreeModel emptyTree;
    ScreenState empty{assemble(StyleDimensions{}), emptyTree};
    ASSERT_FALSE(empty.switchPanelProvider(CycleDirection::Next));
    ASSERT_TRUE(present(shown, kPanelNodeId));
}

// --- Live migration -----------------------------------------------------------------

TEST(updateCompositionMigratesPreservingPanelAndPromptTruth) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.showPanelProvider(TreeProviderKind::Filesystem));
    ASSERT_TRUE(authority.openFinder(PickerKind::Command));
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
    ScreenState authority{assemble(StyleDimensions{}), tree};
    ASSERT_FALSE(authority.updateComposition(assemble(StyleDimensions{})));
}

TEST(statusOverlaySurvivesPromptAndEquivalentRebuilds) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    const UiNodeId actionId{"footer.status_action/7/3/72756e"};
    ASSERT_TRUE(authority.refreshStatusActions(
        {{actionId, "Run", "build.run"}}));
    ASSERT_TRUE(uiSchemaNodeIds(authority.schema())
                    .contains(actionId));

    ASSERT_TRUE(openGenericPrompt(authority.prompt(), footerPrompt()).accepted());
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
    ScreenState authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.showPanelProvider(TreeProviderKind::Filesystem));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Panel);
    ASSERT_TRUE(authority.openFinder(PickerKind::Command));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(authority.closeFinder());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Panel);
}

TEST(panelHideWhilePromptCapturedRestoresBaseUnderThePrompt) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.showPanelProvider(TreeProviderKind::Filesystem));
    ASSERT_TRUE(authority.openFinder(PickerKind::Command));
    // Hide the panel while the prompt is captured: the prompt still routes focus, but the
    // base focus underneath is restored to the panel-return focus (Editor).
    ASSERT_TRUE(authority.togglePanel());
    ASSERT_FALSE(present(authority, kPanelNodeId));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(authority.closeFinder());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
}

TEST(providerCyclingWhileHiddenAndEditorFocusedPreservesBoth) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    // Panel hidden, editor-focused: switching provider changes only the selection.
    ASSERT_TRUE(authority.switchPanelProvider(CycleDirection::Next));
    ASSERT_FALSE(present(authority, kPanelNodeId));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
    ASSERT_EQ(tree.activeProviderBinding()->kind, TreeProviderKind::Git);
}

TEST(editorFocusWithThePanelVisibleKeepsThePanelPresent) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.showPanelProvider(TreeProviderKind::Filesystem));
    authority.focusEditor();
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
    ASSERT_TRUE(present(authority, kPanelNodeId));  // focus moved, panel stayed
}

TEST(focusPanelRequiresThePanelThenFocusEditorReturns) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    // No panel yet -> focusPanel is refused and focus stays Editor.
    ASSERT_FALSE(authority.focusPanel());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);

    ASSERT_TRUE(authority.showPanelProvider(TreeProviderKind::Filesystem));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Panel);
    authority.focusEditor();
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
    ASSERT_TRUE(authority.focusPanel());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Panel);
}

TEST(focusChangeUnderAnOpenPromptSurfacesWhenThePromptCloses) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.showPanelProvider(TreeProviderKind::Filesystem));
    ASSERT_TRUE(openGenericPrompt(authority.prompt(), footerPrompt()).accepted());
    // The prompt capture routes effective focus regardless of the base change.
    authority.focusEditor();
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    // Closing the prompt surfaces the base focus set underneath it.
    ASSERT_TRUE(authority.prompt().cancel().accepted());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
}

// --- External-modification focus capture --------------------------------------------

TEST(theExternalModNodeIsPresentHiddenUntilAFileIsPresentAndGoldensAreUnchanged) {
    TreeModel tree = seededTree();
    ScreenState authority{assemble(StyleDimensions{}), tree};
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
    ScreenState authority{assemble(StyleDimensions{}), tree};
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
    ScreenState authority{assemble(StyleDimensions{}), tree};
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
    ScreenState authority{assemble(StyleDimensions{}), tree};
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
    ScreenState authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.refreshExternalModificationPresence(true));
    ASSERT_TRUE(authority.captureExternalFocus());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::ExternalModification);
    // A footer prompt opens on top: both captures coexist (the prompt guard binds
    // only prompt captures), and the LIFO top -- the active context -- is the prompt.
    ASSERT_TRUE(openGenericPrompt(authority.prompt(), footerPrompt()).accepted());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    // Dismissing the prompt returns to the still-held external context.
    ASSERT_TRUE(authority.prompt().cancel().accepted());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::ExternalModification);
}

}  // namespace

SSG_TEST_SUITE(test_screen_state) {
    RUN(initiallyNoPanelNoPromptTabViewShown);
    RUN(openFinderUpdatesPromptPresenceAndFocus);
    RUN(eachSuccessfulFinderOpenMintsANewActivation);
    RUN(exhaustedPickerActivationSourceRejectsOpenAtomically);
    RUN(invalidPickerActivationSourceIsRejected);
    RUN(showProviderCreatesTreeBackingFromTheOwnedSource);
    RUN(rejectedShowProviderMutatesNothing);
    RUN(genericOpenPromptFocusesFooterWithoutAPicker);
    RUN(genericPromptOverAPickerClearsTheStalePickerIdentity);
    RUN(cancelPromptReleasesFocus);
    RUN(directPromptCancellationImmediatelyClearsPickerProjection);
    RUN(openPromptRejectsAPalettePromptSoOnlyAFinderMakesAPicker);
    RUN(valueEditKeepsPromptFocusAndUpdatesTheInput);
    RUN(promptFocusUsesControlIdentityAndRejectsNonInputs);
    RUN(switchPanelProviderPreservesPanelTruth);
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
