// Seam oracle for InteractionAuthority -- the single owner of schema generation, prompt,
// truth, interaction projection, and the tree revision source. Proves: apply() routes a
// transition through one atomic prepare+install; a rejected transition mutates nothing;
// ShowPanelProvider creates and stamps tree backing from the owned revision source; every
// generic prompt lifecycle path (open/submit/cancel/update) keeps prompt and focus
// consistent and reconciles a stale picker identity; the revision source is monotonic and
// the sole minter; and a schema-generation change migrates interaction+truth atomically
// while preserving valid panel and prompt truth.

#include "ssg/InteractionAuthority.h"

#include "ssg/StatusFields.h"
#include "ssg/Style.h"
#include "ssg/UiTree.h"
#include "ssg/WholeScreenAssembly.h"
#include "chrome_authoring.h"
#include "test_helpers.h"

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
                               "help.open", dims, Style{}.inputLineSigil,
                               std::nullopt);
}

UiComposition assembleWithComposedFooter(const StyleDimensions& dims) {
    WidgetDescriptor field;
    field.kind = WidgetKind::Label;
    field.id = "composed";
    field.value = ValueSource{false, "composed", ""};
    const auto composed =
        ssgtest::composeHeaderAndFooterValidated({field}, {field});
    return assembleWholeScreen(
        {entry("path", StatusFieldRegion::Header),
         entry("mode", StatusFieldRegion::Footer)},
        "help.open", dims, Style{}.inputLineSigil, composed);
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
    return a.interaction().presence().isPresent(UiNodeId{std::string{id}});
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
    ASSERT_FALSE(authority.truth().panelPresent);
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
    const auto before = authority.truth();
    ASSERT_FALSE(authority.apply(OpenFinder{PickerKind::Command}));
    ASSERT_TRUE(authority.truth() == before);
    ASSERT_FALSE(authority.openPickerActivation().has_value());
    ASSERT_FALSE(authority.prompt().active());
}

TEST(applyShowProviderCreatesTreeBackingFromTheOwnedSource) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree, 5};
    ASSERT_FALSE(revisionOf(tree, "git").has_value());
    ASSERT_TRUE(authority.apply(ShowPanelProvider{PanelProvider::GitStatus}));
    ASSERT_TRUE(authority.truth().panelPresent);
    ASSERT_TRUE(tree.activeProviderBinding() ==
                panelProviderTreeBinding(PanelProvider::GitStatus));
    // The git provider was created and stamped from the authority's revision source (5).
    const auto gitRevision = revisionOf(tree, "git");
    ASSERT_TRUE(gitRevision.has_value());
    ASSERT_EQ(gitRevision->value(), std::uint64_t{5});
    // The source advanced past the consumed revision.
    ASSERT_EQ(authority.allocateTreeRevision().value(), std::uint64_t{6});
}

TEST(applyRejectionMutatesNothing) {
    TreeModel empty;  // no filesystem provider -> ShowPanelProvider{FileTree} rejects
    InteractionAuthority authority{assemble(StyleDimensions{}), empty};
    const WholeScreenTruth before = authority.truth();
    const FocusTarget focusBefore = authority.effectiveFocus();
    ASSERT_FALSE(authority.apply(ShowPanelProvider{PanelProvider::FileTree}));
    ASSERT_TRUE(authority.truth() == before);
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

TEST(footerPromptStructureAdvancesOnlyWhenItsShapeChanges) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    const auto initial = authority.interaction().schema().generation();
    ASSERT_TRUE(authority.openPrompt(footerPrompt()).accepted());
    const auto pathGeneration = authority.interaction().schema().generation();
    ASSERT_NE(pathGeneration, initial);

    ASSERT_TRUE(authority.updatePromptValue(0, "changed").accepted());
    ASSERT_TRUE(authority.focusPromptControl("path").accepted());
    ASSERT_EQ(authority.interaction().schema().generation(), pathGeneration);
    ASSERT_TRUE(authority.cancelPrompt().accepted());
    ASSERT_EQ(authority.interaction().schema().generation(), pathGeneration);

    auto sameShape = footerPrompt();
    sameShape.inputs[0].value = "different";
    ASSERT_TRUE(authority.openPrompt(std::move(sameShape)).accepted());
    ASSERT_EQ(authority.interaction().schema().generation(), pathGeneration);

    PromptRequest replace{
        PromptKind::Replace,
        "Replace",
        {{"find.query", "Find", ""}, {"replace.replacement", "Replace", ""}},
        {{"find.case", "Case", false, 8}},
        PromptMatchCount{"find.count", "Matches", "0"}};
    ASSERT_TRUE(authority.openPrompt(std::move(replace)).accepted());
    ASSERT_NE(authority.interaction().schema().generation(), pathGeneration);
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
    const auto a = authority.allocateTreeRevision();
    const auto b = authority.allocateTreeRevision();
    ASSERT_EQ(a.value(), std::uint64_t{10});
    ASSERT_EQ(b.value(), std::uint64_t{11});
}

TEST(allocateTreeRevisionRejectsExhaustion) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree,
                                   std::numeric_limits<std::uint64_t>::max()};
    ASSERT_THROWS(authority.allocateTreeRevision(), std::logic_error);
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
    ASSERT_TRUE(authority.apply(ShowPanelProvider{PanelProvider::FileTree}));
    ASSERT_TRUE(authority.apply(OpenFinder{PickerKind::Command}));
    ASSERT_TRUE(authority.truth().panelPresent);
    ASSERT_TRUE(authority.openPicker().has_value());

    // A structural change (wider panel) advances the generation and migrates.
    StyleDimensions wider;
    wider.panelTargetWidth = StyleDimensions{}.panelTargetWidth + 10;
    ASSERT_TRUE(authority.updateComposition(assemble(wider)));

    // Truth and prompt survive; the projection is rebuilt over the new schema.
    ASSERT_TRUE(authority.truth().panelPresent);
    ASSERT_TRUE(authority.openPicker().has_value());
    ASSERT_TRUE(*authority.openPicker() == PickerKind::Command);
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(present(authority, kFindResultsNodeId));
    // The presence basis is generation-scoped and reset on the migration rebuild.
    ASSERT_EQ(authority.interaction().presence().basis().value(), std::uint64_t{0});
}

TEST(updateCompositionWithoutStructuralChangeDoesNotAdvance) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_FALSE(authority.updateComposition(assemble(StyleDimensions{})));
}

TEST(statusOverlaySurvivesPromptAndComposedFooterRebuilds) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    const UiNodeId actionId{"footer.status_action/7/3/72756e"};
    ASSERT_TRUE(authority.refreshStatusActions(
        {{actionId, "Run", "build.run"}}));
    ASSERT_TRUE(uiSchemaNodeIds(authority.interaction().schema().schema())
                    .contains(actionId));

    ASSERT_TRUE(authority.openPrompt(footerPrompt()).accepted());
    ASSERT_TRUE(authority.refreshStatusActions(
        {{actionId, "Run now", "build.run"}}));
    ASSERT_TRUE(authority.prompt().active());
    ASSERT_EQ(authority.effectiveFocus(), FocusTarget::Prompt);
    ASSERT_EQ(authority.statusActions()[0].accessibleLabel,
              std::string{"Run now"});

    ASSERT_TRUE(
        authority.updateComposition(assembleWithComposedFooter(
            StyleDimensions{})));
    ASSERT_FALSE(uiSchemaNodeIds(authority.interaction().schema().schema())
                     .contains(actionId));
    ASSERT_EQ(authority.statusActions()[0].id, actionId);
    ASSERT_TRUE(authority.prompt().active());

    ASSERT_TRUE(authority.updateComposition(assemble(StyleDimensions{})));
    ASSERT_TRUE(uiSchemaNodeIds(authority.interaction().schema().schema())
                    .contains(actionId));
    ASSERT_EQ(authority.statusActions()[0].id, actionId);
    ASSERT_TRUE(authority.prompt().active());
    ASSERT_EQ(authority.effectiveFocus(), FocusTarget::Prompt);
}

// --- Editor/panel focus -------------------------------------------------------------

TEST(promptOverPanelClosesBackToPanelFocus) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.apply(ShowPanelProvider{PanelProvider::FileTree}));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Panel);
    ASSERT_TRUE(authority.apply(OpenFinder{PickerKind::Command}));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(authority.apply(CloseFinder{}));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Panel);
}

TEST(panelHideWhilePromptCapturedRestoresBaseUnderThePrompt) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.apply(ShowPanelProvider{PanelProvider::FileTree}));
    ASSERT_TRUE(authority.apply(OpenFinder{PickerKind::Command}));
    // Hide the panel while the prompt is captured: the prompt still routes focus, but the
    // base focus underneath is restored to the panel-return focus (Editor).
    ASSERT_TRUE(authority.apply(TogglePanel{}));
    ASSERT_FALSE(authority.truth().panelPresent);
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(authority.apply(CloseFinder{}));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
}

TEST(providerCyclingWhileHiddenAndEditorFocusedPreservesBoth) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    // Panel hidden, editor-focused: switching provider changes only the selection.
    ASSERT_TRUE(authority.apply(SwitchPanelProvider{PanelProvider::GitStatus}));
    ASSERT_FALSE(authority.truth().panelPresent);
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
    ASSERT_TRUE(tree.activeProviderBinding() ==
                panelProviderTreeBinding(PanelProvider::GitStatus));
}

TEST(editorFocusWithThePanelVisibleKeepsThePanelPresent) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.apply(ShowPanelProvider{PanelProvider::FileTree}));
    authority.focusEditor();
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
    ASSERT_TRUE(authority.truth().panelPresent);  // focus moved, panel stayed
}

TEST(focusPanelRequiresThePanelThenFocusEditorReturns) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    // No panel yet -> focusPanel is refused and focus stays Editor.
    ASSERT_FALSE(authority.focusPanel());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);

    ASSERT_TRUE(authority.apply(ShowPanelProvider{PanelProvider::FileTree}));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Panel);
    authority.focusEditor();
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Editor);
    ASSERT_TRUE(authority.focusPanel());
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Panel);
}

TEST(focusChangeUnderAnOpenPromptSurfacesWhenThePromptCloses) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    ASSERT_TRUE(authority.apply(ShowPanelProvider{PanelProvider::FileTree}));
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
    ASSERT_FALSE(authority.truth().externalFocusHeld);
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

int main() {
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
    RUN(footerPromptStructureAdvancesOnlyWhenItsShapeChanges);
    RUN(promptFocusUsesControlIdentityAndRejectsNonInputs);
    RUN(allocateTreeRevisionIsMonotonic);
    RUN(allocateTreeRevisionRejectsExhaustion);
    RUN(constructionRejectsARevisionSourceBehindAProvider);
    RUN(updateCompositionMigratesPreservingPanelAndPromptTruth);
    RUN(updateCompositionWithoutStructuralChangeDoesNotAdvance);
    RUN(statusOverlaySurvivesPromptAndComposedFooterRebuilds);
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
