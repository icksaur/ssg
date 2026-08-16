// Algorithm oracle for the whole-screen interaction aggregate and its schema owner.
// Knowable answers: the schema owner advances its generation iff the assembled root
// changed structurally; presence is a pure function of semantic truth (panel presence,
// selected provider, finder-open); base focus never strands on an absent panel; and
// rebuilding from the same truth over a new schema generation (the migration) preserves
// presence-from-truth and focus and resets the presence basis.

#include "ssg/WholeScreenInteraction.h"
#include "ssg/WholeScreenSchema.h"

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

std::vector<StatusFieldCatalogEntry> catalog() {
    return {entry("path", StatusFieldRegion::Header),
            entry("mode", StatusFieldRegion::Footer)};
}

UiComposition assemble(const StyleDimensions& dims) {
    return assembleWholeScreen(catalog(), "help.open", dims,
                               Style{}.inputLineSigil, std::nullopt);
}

ValidatedSchema schemaOf(const StyleDimensions& dims) {
    auto result = ValidatedSchema::validate(
        UiSchema{Generation{0}, assemble(dims).root});
    ASSERT_TRUE(result.ok());
    return result.takeSchema();
}

ValidatedSchema schemaWithoutPromptInput() {
    auto result = ValidatedSchema::validate(UiSchema{Generation{0}, emptyUiRoot()});
    ASSERT_TRUE(result.ok());
    return result.takeSchema();
}

bool present(const UiInteractionState& s, std::string_view id) {
    return s.presence().isPresent(UiNodeId{std::string{id}});
}

// --- WholeScreenSchema: generation advances only on structural change ---------------

TEST(schemaGenerationHoldsWhenStructureIsUnchanged) {
    WholeScreenSchema owner{assemble(StyleDimensions{})};
    ASSERT_EQ(owner.generation().value(), std::uint64_t{0});
    // Re-assembling the same structure must NOT advance the generation.
    const bool advanced = owner.update(assemble(StyleDimensions{}));
    ASSERT_FALSE(advanced);
    ASSERT_EQ(owner.generation().value(), std::uint64_t{0});
}

TEST(schemaGenerationAdvancesOnAStructuralChange) {
    WholeScreenSchema owner{assemble(StyleDimensions{})};
    StyleDimensions wider;
    wider.panelTargetWidth = StyleDimensions{}.panelTargetWidth + 10;  // panel size
    const bool advanced = owner.update(assemble(wider));
    ASSERT_TRUE(advanced);
    ASSERT_EQ(owner.generation().value(), std::uint64_t{1});
}

// --- Presence is a pure function of semantic truth ----------------------------------

TEST(panelIsPresentOnlyWhenRequested) {
    WholeScreenTruth hidden;
    hidden.panelPresent = false;
    const auto a = buildWholeScreenInteraction(schemaOf({}), hidden);
    ASSERT_FALSE(present(a, kPanelNodeId));
    // No provider leaks its presence when the panel is absent (provider choice lives in
    // the last-active hint, not in presence).
    ASSERT_FALSE(present(a, kFileTreeNodeId));
    ASSERT_FALSE(present(a, kGitStatusNodeId));
    ASSERT_FALSE(present(a, kSymbolsNodeId));

    WholeScreenTruth shown;
    shown.panelPresent = true;
    const auto b = buildWholeScreenInteraction(schemaOf({}), shown);
    ASSERT_TRUE(present(b, kPanelNodeId));
}

TEST(corruptSelectedProviderIsRejected) {
    WholeScreenTruth truth;
    truth.panelPresent = true;
    truth.selectedProvider = static_cast<PanelProvider>(200);
    // Invalid truth must not produce plausible presence.
    ASSERT_THROWS(buildWholeScreenInteraction(schemaOf({}), truth), std::logic_error);
}

TEST(exactlyTheSelectedProviderIsPresent) {
    WholeScreenTruth truth;
    truth.panelPresent = true;
    truth.selectedProvider = PanelProvider::GitStatus;
    const auto s = buildWholeScreenInteraction(schemaOf({}), truth);
    ASSERT_TRUE(present(s, kGitStatusNodeId));
    ASSERT_FALSE(present(s, kFileTreeNodeId));
    ASSERT_FALSE(present(s, kSymbolsNodeId));
}

TEST(contentShowsTabViewXorFindResultsByFinderState) {
    WholeScreenTruth closed;
    closed.openPicker = std::nullopt;
    const auto a = buildWholeScreenInteraction(schemaOf({}), closed);
    ASSERT_TRUE(present(a, kTabViewNodeId));
    ASSERT_FALSE(present(a, kFindResultsNodeId));

    WholeScreenTruth open;
    open.openPicker = PickerKind::File;
    // A file finder is a Palette prompt -> header-region prompt focus.
    const auto b = buildWholeScreenInteraction(schemaOf({}), open, PromptRegion::Header);
    ASSERT_FALSE(present(b, kTabViewNodeId));
    ASSERT_TRUE(present(b, kFindResultsNodeId));
    // The prompt capture (anchored on the header input line) routes effective focus to the
    // prompt context; findresults is displayed content, not the focus anchor.
    ASSERT_TRUE(b.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(present(b, kHeaderNodeId));
}

TEST(promptFocusAnchorsOnTheRegionHost) {
    WholeScreenTruth truth;
    // A footer-region prompt (find/replace, save-path) routes focus to the prompt without
    // opening a picker: tabview stays, findresults stays hidden.
    const auto s = buildWholeScreenInteraction(schemaOf({}), truth, PromptRegion::Footer);
    ASSERT_TRUE(s.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(present(s, kTabViewNodeId));
    ASSERT_FALSE(present(s, kFindResultsNodeId));
}

TEST(headerPromptRequiresTheInputLineSchemaNode) {
    WholeScreenTruth truth;
    ASSERT_THROWS(buildWholeScreenInteraction(schemaWithoutPromptInput(), truth,
                                              PromptRegion::Header),
                  std::logic_error);
}

TEST(baseFocusNeverStrandsOnAnAbsentPanel) {
    WholeScreenTruth truth;
    truth.panelPresent = false;
    truth.baseFocus = BaseFocus::Panel;  // requested Panel, but panel is absent
    const auto s = buildWholeScreenInteraction(schemaOf({}), truth);
    ASSERT_TRUE(s.effectiveFocus() == FocusTarget::Editor);  // fell back to Editor

    truth.panelPresent = true;
    const auto t = buildWholeScreenInteraction(schemaOf({}), truth);
    ASSERT_TRUE(t.effectiveFocus() == FocusTarget::Panel);
}

// --- Migration: rebuild from the same truth over a new generation -------------------

TEST(rebuildOverANewGenerationPreservesTruthAndResetsBasis) {
    WholeScreenTruth truth;
    truth.panelPresent = true;
    truth.selectedProvider = PanelProvider::Symbols;
    truth.openPicker = PickerKind::Command;

    // Generation 0. A command palette is a Palette prompt -> header-region focus.
    const auto g0 = buildWholeScreenInteraction(schemaOf({}), truth, PromptRegion::Header);
    ASSERT_TRUE(present(g0, kSymbolsNodeId));
    ASSERT_TRUE(present(g0, kFindResultsNodeId));
    ASSERT_TRUE(g0.effectiveFocus() == FocusTarget::Prompt);

    // Generation 1 (a structural change: wider panel). Rebuilding from the SAME inputs
    // migrates: presence-from-truth is identical, the prompt capture is preserved, and
    // the presence basis is reset to its baseline for the new generation.
    StyleDimensions wider;
    wider.panelTargetWidth = StyleDimensions{}.panelTargetWidth + 10;
    WholeScreenSchema owner{assemble(StyleDimensions{})};
    ASSERT_TRUE(owner.update(assemble(wider)));
    ASSERT_EQ(owner.generation().value(), std::uint64_t{1});

    ValidatedSchema migrated = owner.validated();
    const auto g1 =
        buildWholeScreenInteraction(std::move(migrated), truth, PromptRegion::Header);
    ASSERT_TRUE(present(g1, kSymbolsNodeId));
    ASSERT_TRUE(present(g1, kFindResultsNodeId));
    ASSERT_FALSE(present(g1, kFileTreeNodeId));
    ASSERT_TRUE(g1.effectiveFocus() == FocusTarget::Prompt);
    // The basis is generation-scoped and reset on a fresh build to its baseline.
    ASSERT_EQ(g1.presence().basis().value(), std::uint64_t{0});
}

// --- The header prompt input line: presence gating + focus anchoring ----------------

TEST(theInputLineIsVisibleOnlyForAHeaderPromptAndCapturesTheInputNode) {
    // Closed: the prompt input is hidden (no picker on this client).
    WholeScreenTruth closed;
    const auto a = buildWholeScreenInteraction(schemaOf({}), closed);
    ASSERT_FALSE(present(a, kHeaderPromptInputNodeId));

    // A footer-region prompt does NOT reveal the header input line: only a
    // header-region prompt hosts its query there.
    const auto f =
        buildWholeScreenInteraction(schemaOf({}), closed, PromptRegion::Footer);
    ASSERT_FALSE(present(f, kHeaderPromptInputNodeId));
    // The footer prompt captures the footer host, not the input line.
    ASSERT_TRUE(f.focus().top() != nullptr);
    if (f.focus().top())
        ASSERT_EQ(f.focus().top()->node.value(), std::string{kFooterNodeId});

    // A header-region prompt reveals the input line AND anchors the capture on the
    // input_line NODE itself, so keystrokes route to the query node, not merely to
    // the header container.
    WholeScreenTruth open;
    open.openPicker = PickerKind::Command;
    const auto h =
        buildWholeScreenInteraction(schemaOf({}), open, PromptRegion::Header);
    ASSERT_TRUE(present(h, kHeaderPromptInputNodeId));
    ASSERT_TRUE(h.effectiveFocus() == FocusTarget::Prompt);
    ASSERT_TRUE(h.focus().top() != nullptr);
    if (h.focus().top())
        ASSERT_EQ(h.focus().top()->node.value(),
                  std::string{kHeaderPromptInputNodeId});
}

}  // namespace

int main() {
    RUN(schemaGenerationHoldsWhenStructureIsUnchanged);
    RUN(schemaGenerationAdvancesOnAStructuralChange);
    RUN(panelIsPresentOnlyWhenRequested);
    RUN(corruptSelectedProviderIsRejected);
    RUN(exactlyTheSelectedProviderIsPresent);
    RUN(contentShowsTabViewXorFindResultsByFinderState);
    RUN(promptFocusAnchorsOnTheRegionHost);
    RUN(headerPromptRequiresTheInputLineSchemaNode);
    RUN(theInputLineIsVisibleOnlyForAHeaderPromptAndCapturesTheInputNode);
    RUN(baseFocusNeverStrandsOnAnAbsentPanel);
    RUN(rebuildOverANewGenerationPreservesTruthAndResetsBasis);
    return failed;
}
