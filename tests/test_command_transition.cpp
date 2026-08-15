// Algorithm oracle for command transitions -- the writers of whole-screen truth.
// Knowable answers: each transition's next-truth derivation (panel show/hide with
// panel-return focus restoration, provider selection, reselect-toggle, picker identity),
// the tree-backing plan it prepares (activate-existing vs create-snapshot vs reject a
// missing Filesystem provider), replacing an already-active prompt without adding a new
// rejection, and the provider cycle order. The against-live grid-parity and
// rejection-mutates-nothing wiring oracles belong to the activation cutover, not here.

#include "ssg/CommandTransition.h"

#include "ssg/StatusFields.h"
#include "ssg/Style.h"
#include "ssg/UiTree.h"
#include "ssg/WholeScreenAssembly.h"
#include "test_helpers.h"

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
        "help.open", StyleDimensions{}, std::nullopt);
    auto result =
        ValidatedSchema::validate(UiSchema{Generation{0}, composition.root});
    ASSERT_TRUE(result.ok());
    return result.takeSchema();
}

TransitionInputs inputs(WholeScreenTruth truth,
                        std::vector<TreeProviderId> present = {},
                        PromptSurface prompt = {}) {
    TransitionInputs in{std::move(truth), schema(), std::move(prompt),
                        std::move(present), TreeRevision{7}};
    return in;
}

bool present(const PreparedTransition& p, std::string_view id) {
    return p.interaction.presence().isPresent(UiNodeId{std::string{id}});
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
    ASSERT_TRUE(prepared->truth.panelPresent);
    ASSERT_TRUE(prepared->truth.baseFocus == BaseFocus::Panel);
    ASSERT_TRUE(prepared->truth.panelReturnFocus == BaseFocus::Editor);
    ASSERT_TRUE(present(*prepared, kPanelNodeId));
    ASSERT_FALSE(prepared->tree.has_value());
}

TEST(togglePanelFromShownRestoresPanelReturnFocus) {
    WholeScreenTruth truth;
    truth.panelPresent = true;
    truth.baseFocus = BaseFocus::Panel;
    truth.panelReturnFocus = BaseFocus::Editor;
    const auto prepared = prepareTransition(TogglePanel{}, inputs(truth));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_FALSE(prepared->truth.panelPresent);
    ASSERT_TRUE(prepared->truth.baseFocus == BaseFocus::Editor);
    ASSERT_FALSE(present(*prepared, kPanelNodeId));
}

// --- ShowPanelProvider --------------------------------------------------------------

TEST(showProviderPreparesACreateSnapshotForAnAbsentGitProvider) {
    WholeScreenTruth truth;  // hidden, files selected
    const auto prepared = prepareTransition(
        ShowPanelProvider{PanelProvider::GitStatus},
        inputs(truth, {TreeProviderId{"filesystem"}}));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->truth.panelPresent);
    ASSERT_TRUE(prepared->truth.selectedProvider == PanelProvider::GitStatus);
    ASSERT_TRUE(prepared->tree.has_value());
    ASSERT_TRUE(prepared->tree->activate == TreeProviderId{"git"});
    ASSERT_TRUE(prepared->tree->create.has_value());
    ASSERT_TRUE(prepared->tree->create->kind() == TreeProviderKind::Git);
    ASSERT_TRUE(present(*prepared, kGitStatusNodeId));
    ASSERT_FALSE(present(*prepared, kFileTreeNodeId));
}

TEST(showProviderActivatesAnExistingProviderWithoutCreating) {
    WholeScreenTruth truth;
    const auto prepared = prepareTransition(
        ShowPanelProvider{PanelProvider::GitStatus},
        inputs(truth, {TreeProviderId{"filesystem"}, TreeProviderId{"git"}}));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->tree.has_value());
    ASSERT_FALSE(prepared->tree->create.has_value());
    ASSERT_TRUE(prepared->tree->activate == TreeProviderId{"git"});
}

TEST(showAMissingFilesystemProviderIsRejected) {
    WholeScreenTruth truth;
    // The Filesystem provider is seeded, never created; a missing one is a real failure.
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
        ShowPanelProvider{PanelProvider::FileTree},
        inputs(truth, {TreeProviderId{"filesystem"}}));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_FALSE(prepared->truth.panelPresent);
    ASSERT_TRUE(prepared->truth.baseFocus == BaseFocus::Editor);
    ASSERT_FALSE(prepared->tree.has_value());
}

// --- OpenFinder / CloseFinder -------------------------------------------------------

TEST(openFinderCarriesPickerIdentityAndOpensThePrompt) {
    WholeScreenTruth truth;
    const auto prepared =
        prepareTransition(OpenFinder{PickerKind::File}, inputs(truth));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->truth.openPicker.has_value());
    ASSERT_TRUE(*prepared->truth.openPicker == PickerKind::File);
    ASSERT_TRUE(prepared->rebuildFileCandidates);
    ASSERT_TRUE(prepared->prompt.active());
    ASSERT_TRUE(present(*prepared, kFindResultsNodeId));
    ASSERT_TRUE(prepared->interaction.effectiveFocus() == FocusTarget::Prompt);
}

TEST(openFinderReplacesAnAlreadyActivePromptWithoutNewRejection) {
    WholeScreenTruth truth;
    // Opening a finder while another prompt is active replaces it (matching the live
    // opener); introducing a conflicting-prompt rejection would be new behavior.
    const auto prepared = prepareTransition(
        OpenFinder{PickerKind::Command}, inputs(truth, {}, openPrompt()));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->truth.openPicker.has_value());
    ASSERT_TRUE(*prepared->truth.openPicker == PickerKind::Command);
    ASSERT_TRUE(prepared->prompt.active());
}

TEST(closeFinderClearsThePickerAndCancelsThePrompt) {
    WholeScreenTruth truth;
    truth.openPicker = PickerKind::File;
    const auto prepared =
        prepareTransition(CloseFinder{}, inputs(truth, {}, openPrompt()));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_FALSE(prepared->truth.openPicker.has_value());
    ASSERT_FALSE(prepared->prompt.active());
    ASSERT_TRUE(present(*prepared, kTabViewNodeId));
}

// --- Provider cycling ---------------------------------------------------------------

TEST(cyclePanelProviderWalksTheProviderOrder) {
    ASSERT_TRUE(cyclePanelProvider(PanelProvider::FileTree, CycleDirection::Next) ==
                PanelProvider::GitStatus);
    ASSERT_TRUE(cyclePanelProvider(PanelProvider::Symbols, CycleDirection::Next) ==
                PanelProvider::FileTree);
    ASSERT_TRUE(cyclePanelProvider(PanelProvider::FileTree, CycleDirection::Previous) ==
                PanelProvider::Symbols);
}

}  // namespace

int main() {
    RUN(togglePanelFromHiddenShowsPanelAndRetainsReturnFocus);
    RUN(togglePanelFromShownRestoresPanelReturnFocus);
    RUN(showProviderPreparesACreateSnapshotForAnAbsentGitProvider);
    RUN(showProviderActivatesAnExistingProviderWithoutCreating);
    RUN(showAMissingFilesystemProviderIsRejected);
    RUN(reselectingTheShownProviderHidesThePanel);
    RUN(openFinderCarriesPickerIdentityAndOpensThePrompt);
    RUN(openFinderReplacesAnAlreadyActivePromptWithoutNewRejection);
    RUN(closeFinderClearsThePickerAndCancelsThePrompt);
    RUN(cyclePanelProviderWalksTheProviderOrder);
    return 0;
}
