// Algorithm oracle for command transitions -- the writers of whole-screen truth.
// Knowable answers: each transition's next-truth derivation (panel show/hide with
// panel-return focus restoration, provider selection, reselect-toggle, picker identity),
// the tree-backing plan it prepares (activate a matching id+kind provider, create-replace
// one that is absent OR present under the wrong kind, reject a missing Filesystem
// provider), replacing an already-active prompt without a new rejection, refusing a
// close whose cancel fails, rejecting corrupt provider enumerators, and the provider cycle
// order. The against-live grid-parity and rejection-mutates-nothing wiring oracles belong
// to the activation cutover, not here.

#include "ssg/CommandTransition.h"

#include "ssg/StatusFields.h"
#include "ssg/Style.h"
#include "ssg/UiTree.h"
#include "ssg/WholeScreenAssembly.h"
#include "test_helpers.h"

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

ValidatedSchema schema() {
    auto composition = assembleWholeScreen(
        {entry("path", StatusFieldRegion::Header),
         entry("mode", StatusFieldRegion::Footer)},
        "help.open", StyleDimensions{}, Style{}.inputLineSigil, std::nullopt);
    auto result =
        ValidatedSchema::validate(UiSchema{Generation{0}, composition.root});
    ASSERT_TRUE(result.ok());
    return result.takeSchema();
}

TreeProviderBinding filesystem() {
    return panelProviderTreeBinding(PanelProvider::FileTree);
}

TreeProviderPresence presence(TreeProviderBinding binding, std::uint64_t revision) {
    return TreeProviderPresence{std::move(binding), TreeRevision{revision}};
}

TransitionInputs inputs(WholeScreenTruth truth,
                        std::vector<TreeProviderPresence> present = {},
                        PromptSurface prompt = {}) {
    return TransitionInputs{std::move(truth), schema(), std::move(prompt),
                            std::move(present), TreeRevision{7}};
}

bool present(const PreparedTransition& p, std::string_view id) {
    return p.interaction().presence().isPresent(UiNodeId{std::string{id}});
}

PromptSurface openPrompt() {
    PromptSurface surface;
    (void)surface.open(PromptRequest{PromptKind::Palette, "busy",
                                     {{"query", "q", ""}}, {}, std::nullopt});
    return surface;
}

// --- TogglePanel --------------------------------------------------------------------

TEST(togglePanelFromHiddenShowsPanelAndRetainsReturnFocus) {
    WholeScreenTruth truth;  // hidden, base Editor
    const auto prepared = prepareTransition(TogglePanel{}, inputs(truth));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->truth().panelPresent);
    ASSERT_TRUE(prepared->truth().baseFocus == BaseFocus::Panel);
    ASSERT_TRUE(prepared->truth().panelReturnFocus == BaseFocus::Editor);
    ASSERT_TRUE(present(*prepared, kPanelNodeId));
    ASSERT_FALSE(prepared->tree().has_value());
}

TEST(togglePanelFromShownRestoresPanelReturnFocus) {
    WholeScreenTruth truth;
    truth.panelPresent = true;
    truth.baseFocus = BaseFocus::Panel;
    truth.panelReturnFocus = BaseFocus::Editor;
    const auto prepared = prepareTransition(TogglePanel{}, inputs(truth));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_FALSE(prepared->truth().panelPresent);
    ASSERT_TRUE(prepared->truth().baseFocus == BaseFocus::Editor);
    ASSERT_FALSE(present(*prepared, kPanelNodeId));
}

// --- ShowPanelProvider --------------------------------------------------------------

TEST(showProviderPreparesACreateSnapshotForAnAbsentGitProvider) {
    WholeScreenTruth truth;  // hidden, files selected
    const auto prepared = prepareTransition(
        ShowPanelProvider{PanelProvider::GitStatus}, inputs(truth, {presence(filesystem(), 1)}));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->truth().panelPresent);
    ASSERT_TRUE(prepared->truth().selectedProvider == PanelProvider::GitStatus);
    ASSERT_TRUE(prepared->tree().has_value());
    ASSERT_TRUE(prepared->tree()->activate == TreeProviderId{"git"});
    ASSERT_TRUE(prepared->tree()->create.has_value());
    ASSERT_TRUE(prepared->tree()->create->kind() == TreeProviderKind::Git);
    ASSERT_TRUE(present(*prepared, kGitStatusNodeId));
    ASSERT_FALSE(present(*prepared, kFileTreeNodeId));
}

TEST(showProviderActivatesAMatchingProviderWithoutCreating) {
    WholeScreenTruth truth;
    const auto prepared = prepareTransition(
        ShowPanelProvider{PanelProvider::GitStatus},
        inputs(truth, {presence(filesystem(), 1),
                       presence(TreeProviderBinding{TreeProviderId{"git"},
                                                    TreeProviderKind::Git}, 2)}));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->tree().has_value());
    ASSERT_FALSE(prepared->tree()->create.has_value());
    ASSERT_TRUE(prepared->tree()->activate == TreeProviderId{"git"});
}

TEST(showProviderRecreatesAnIdPresentUnderTheWrongKind) {
    WholeScreenTruth truth;
    // A "git" id backing a Symbols-kind tree is NOT the GitStatus provider: activating by
    // id alone would show GitStatus over a Symbols tree, so it must be recreated.
    const auto prepared = prepareTransition(
        ShowPanelProvider{PanelProvider::GitStatus},
        inputs(truth, {presence(filesystem(), 1),
                       presence(TreeProviderBinding{TreeProviderId{"git"},
                                                    TreeProviderKind::Symbols}, 5)}));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->tree().has_value());
    ASSERT_TRUE(prepared->tree()->create.has_value());
    ASSERT_TRUE(prepared->tree()->create->kind() == TreeProviderKind::Git);
    // Stamped from the runtime's revision source (nextTreeRevision == 7), which leads the
    // provider it replaces, so commit's replaceProvider cannot throw.
    ASSERT_EQ(prepared->tree()->create->revision().value(), std::uint64_t{7});
}

TEST(showProviderRejectsARecreateWhenTheRevisionSourceHasDesynced) {
    WholeScreenTruth truth;
    // The revision source (nextTreeRevision == 7) no longer leads the existing provider
    // (revision 9): stamping a replacement from it would be rejected by replaceProvider,
    // so preflight refuses rather than let commit throw.
    const auto prepared = prepareTransition(
        ShowPanelProvider{PanelProvider::GitStatus},
        inputs(truth, {presence(filesystem(), 1),
                       presence(TreeProviderBinding{TreeProviderId{"git"},
                                                    TreeProviderKind::Symbols}, 9)}));
    ASSERT_FALSE(prepared.has_value());
}

TEST(showAMissingFilesystemProviderIsRejected) {
    WholeScreenTruth truth;
    const auto prepared = prepareTransition(
        ShowPanelProvider{PanelProvider::FileTree}, inputs(truth, {}));
    ASSERT_FALSE(prepared.has_value());
}

TEST(reselectingTheShownProviderHidesThePanel) {
    WholeScreenTruth truth;
    truth.panelPresent = true;
    truth.selectedProvider = PanelProvider::FileTree;
    truth.baseFocus = BaseFocus::Panel;
    truth.panelReturnFocus = BaseFocus::Editor;
    const auto prepared = prepareTransition(
        ShowPanelProvider{PanelProvider::FileTree}, inputs(truth, {presence(filesystem(), 1)}));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_FALSE(prepared->truth().panelPresent);
    ASSERT_TRUE(prepared->truth().baseFocus == BaseFocus::Editor);
    ASSERT_FALSE(prepared->tree().has_value());
}

// --- SwitchPanelProvider (cycling: preserve visibility + focus) ---------------------

TEST(switchProviderWhileHiddenPreservesHiddenAndFocus) {
    WholeScreenTruth truth;  // hidden, Editor-focused, FileTree selected
    const auto prepared = prepareTransition(
        SwitchPanelProvider{PanelProvider::GitStatus},
        inputs(truth, {presence(filesystem(), 1),
                       presence(TreeProviderBinding{TreeProviderId{"git"},
                                                    TreeProviderKind::Git}, 2)}));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_FALSE(prepared->truth().panelPresent);            // stayed hidden
    ASSERT_TRUE(prepared->truth().baseFocus == BaseFocus::Editor);  // focus untouched
    ASSERT_TRUE(prepared->truth().selectedProvider == PanelProvider::GitStatus);
    ASSERT_FALSE(prepared->tree()->create.has_value());      // matching -> activate only
}

TEST(switchProviderWhileShownPreservesShownAndFocus) {
    WholeScreenTruth truth;
    truth.panelPresent = true;
    truth.baseFocus = BaseFocus::Panel;
    truth.selectedProvider = PanelProvider::FileTree;
    const auto prepared = prepareTransition(
        SwitchPanelProvider{PanelProvider::Symbols}, inputs(truth, {presence(filesystem(), 1)}));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->truth().panelPresent);             // stayed shown
    ASSERT_TRUE(prepared->truth().baseFocus == BaseFocus::Panel);   // focus untouched
    ASSERT_TRUE(prepared->truth().selectedProvider == PanelProvider::Symbols);
    ASSERT_TRUE(prepared->tree()->create.has_value());       // symbols absent -> create
}

TEST(switchProviderNeverTogglesOffOnReselect) {
    WholeScreenTruth truth;
    truth.panelPresent = true;
    truth.selectedProvider = PanelProvider::FileTree;
    // ShowPanelProvider would hide here; SwitchPanelProvider keeps it shown.
    const auto prepared = prepareTransition(
        SwitchPanelProvider{PanelProvider::FileTree}, inputs(truth, {presence(filesystem(), 1)}));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->truth().panelPresent);
}

TEST(switchProviderRejectsMissingFilesystem) {
    WholeScreenTruth truth;
    const auto prepared = prepareTransition(
        SwitchPanelProvider{PanelProvider::FileTree}, inputs(truth, {}));
    ASSERT_FALSE(prepared.has_value());
}

// --- OpenFinder / CloseFinder -------------------------------------------------------

TEST(openFinderCarriesPickerIdentityAndOpensThePrompt) {
    WholeScreenTruth truth;
    const auto prepared =
        prepareTransition(OpenFinder{PickerKind::File}, inputs(truth));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->truth().openPicker.has_value());
    ASSERT_TRUE(*prepared->truth().openPicker == PickerKind::File);
    ASSERT_TRUE(prepared->prompt().active());
    ASSERT_TRUE(present(*prepared, kFindResultsNodeId));
    ASSERT_TRUE(prepared->interaction().effectiveFocus() == FocusTarget::Prompt);
}

TEST(openFinderReplacesAnAlreadyActivePromptWithoutNewRejection) {
    WholeScreenTruth truth;
    const auto prepared = prepareTransition(
        OpenFinder{PickerKind::Command}, inputs(truth, {}, openPrompt()));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->truth().openPicker.has_value());
    ASSERT_TRUE(*prepared->truth().openPicker == PickerKind::Command);
    ASSERT_TRUE(prepared->prompt().active());
}

TEST(closeFinderClearsThePickerAndCancelsThePrompt) {
    WholeScreenTruth truth;
    truth.openPicker = PickerKind::File;
    const auto prepared =
        prepareTransition(CloseFinder{}, inputs(truth, {}, openPrompt()));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_FALSE(prepared->truth().openPicker.has_value());
    ASSERT_FALSE(prepared->prompt().active());
    ASSERT_TRUE(present(*prepared, kEditorNodeId));
}

TEST(closeFinderIsRejectedWhenCancelRefuses) {
    WholeScreenTruth truth;
    truth.openPicker = PickerKind::File;
    // No active prompt -> cancel refuses -> the transition refuses rather than fabricate
    // a cleared state.
    const auto prepared = prepareTransition(CloseFinder{}, inputs(truth));
    ASSERT_FALSE(prepared.has_value());
}

// --- Provider domain ----------------------------------------------------------------

TEST(cyclePanelProviderWalksTheProviderOrder) {
    ASSERT_TRUE(cyclePanelProvider(PanelProvider::FileTree, CycleDirection::Next) ==
                PanelProvider::GitStatus);
    ASSERT_TRUE(cyclePanelProvider(PanelProvider::Symbols, CycleDirection::Next) ==
                PanelProvider::FileTree);
    ASSERT_TRUE(cyclePanelProvider(PanelProvider::FileTree, CycleDirection::Previous) ==
                PanelProvider::Symbols);
}

TEST(corruptProviderEnumeratorsAreRejected) {
    const auto corrupt = static_cast<PanelProvider>(200);
    ASSERT_THROWS(panelProviderLabel(corrupt), std::logic_error);
    ASSERT_THROWS(panelProviderTreeBinding(corrupt), std::logic_error);
    ASSERT_THROWS(cyclePanelProvider(corrupt, CycleDirection::Next), std::logic_error);
    ASSERT_THROWS(
        cyclePanelProvider(PanelProvider::FileTree, static_cast<CycleDirection>(200)),
        std::logic_error);
}

}  // namespace

int main() {
    RUN(togglePanelFromHiddenShowsPanelAndRetainsReturnFocus);
    RUN(togglePanelFromShownRestoresPanelReturnFocus);
    RUN(showProviderPreparesACreateSnapshotForAnAbsentGitProvider);
    RUN(showProviderActivatesAMatchingProviderWithoutCreating);
    RUN(showProviderRecreatesAnIdPresentUnderTheWrongKind);
    RUN(showProviderRejectsARecreateWhenTheRevisionSourceHasDesynced);
    RUN(showAMissingFilesystemProviderIsRejected);
    RUN(reselectingTheShownProviderHidesThePanel);
    RUN(switchProviderWhileHiddenPreservesHiddenAndFocus);
    RUN(switchProviderWhileShownPreservesShownAndFocus);
    RUN(switchProviderNeverTogglesOffOnReselect);
    RUN(switchProviderRejectsMissingFilesystem);
    RUN(openFinderCarriesPickerIdentityAndOpensThePrompt);
    RUN(openFinderReplacesAnAlreadyActivePromptWithoutNewRejection);
    RUN(closeFinderClearsThePickerAndCancelsThePrompt);
    RUN(closeFinderIsRejectedWhenCancelRefuses);
    RUN(cyclePanelProviderWalksTheProviderOrder);
    RUN(corruptProviderEnumeratorsAreRejected);
    return failed;
}
