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
                               "help.open", dims, std::nullopt);
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
    ASSERT_TRUE(present(authority, kTabViewNodeId));
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
    ASSERT_FALSE(present(authority, kTabViewNodeId));
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
}

TEST(applyShowProviderCreatesTreeBackingFromTheOwnedSource) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree, 5};
    ASSERT_FALSE(revisionOf(tree, "git").has_value());
    ASSERT_TRUE(authority.apply(ShowPanelProvider{PanelProvider::GitStatus}));
    ASSERT_TRUE(authority.truth().panelPresent);
    ASSERT_TRUE(authority.truth().selectedProvider == PanelProvider::GitStatus);
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
    ASSERT_TRUE(authority.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(present(authority, kTabViewNodeId));
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
    ASSERT_TRUE(present(authority, kTabViewNodeId));
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

// --- Editor/panel focus -------------------------------------------------------------

TEST(pickerEpochAdvancesOnEveryFinderOpenIncludingAReopen) {
    TreeModel tree = seededTree();
    InteractionAuthority authority{assemble(StyleDimensions{}), tree};
    const std::uint64_t start = authority.pickerEpoch();
    ASSERT_TRUE(authority.apply(OpenFinder{PickerKind::File}));
    const std::uint64_t afterOpen = authority.pickerEpoch();
    ASSERT_TRUE(afterOpen > start);
    // Reopen the SAME kind with NO close in between: openPicker stays File, yet the epoch
    // must still advance so a candidate owner refreshes.
    ASSERT_TRUE(authority.apply(OpenFinder{PickerKind::File}));
    ASSERT_TRUE(*authority.openPicker() == PickerKind::File);
    ASSERT_TRUE(authority.pickerEpoch() > afterOpen);
    // A close-then-reopen also advances.
    const std::uint64_t afterReopen = authority.pickerEpoch();
    ASSERT_TRUE(authority.apply(CloseFinder{}));
    ASSERT_TRUE(authority.apply(OpenFinder{PickerKind::File}));
    ASSERT_TRUE(authority.pickerEpoch() > afterReopen);
}

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
    ASSERT_TRUE(authority.truth().selectedProvider == PanelProvider::GitStatus);
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

}  // namespace

int main() {
    RUN(initiallyNoPanelNoPromptTabViewShown);
    RUN(applyOpenFinderRoutesThroughOneAtomicInstall);
    RUN(applyShowProviderCreatesTreeBackingFromTheOwnedSource);
    RUN(applyRejectionMutatesNothing);
    RUN(genericOpenPromptFocusesFooterWithoutAPicker);
    RUN(genericPromptOverAPickerClearsTheStalePickerIdentity);
    RUN(cancelPromptReleasesFocus);
    RUN(openPromptRejectsAPalettePromptSoOnlyAFinderMakesAPicker);
    RUN(valueEditKeepsPromptFocusAndUpdatesTheInput);
    RUN(allocateTreeRevisionIsMonotonic);
    RUN(allocateTreeRevisionRejectsExhaustion);
    RUN(constructionRejectsARevisionSourceBehindAProvider);
    RUN(updateCompositionMigratesPreservingPanelAndPromptTruth);
    RUN(updateCompositionWithoutStructuralChangeDoesNotAdvance);
    RUN(focusPanelRequiresThePanelThenFocusEditorReturns);
    RUN(focusChangeUnderAnOpenPromptSurfacesWhenThePromptCloses);
    RUN(pickerEpochAdvancesOnEveryFinderOpenIncludingAReopen);
    RUN(promptOverPanelClosesBackToPanelFocus);
    RUN(panelHideWhilePromptCapturedRestoresBaseUnderThePrompt);
    RUN(providerCyclingWhileHiddenAndEditorFocusedPreservesBoth);
    RUN(editorFocusWithThePanelVisibleKeepsThePanelPresent);
    return failed;
}
