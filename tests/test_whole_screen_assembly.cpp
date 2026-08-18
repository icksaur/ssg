// Algorithm oracle for the whole-screen tree assembly. The knowable answers: the
// assembled tree is the canonical root>[header, body>[panel>[filetree,gitstatus],
// content>[tabview,findresults]], footer] with extents drawn from StyleDimensions; the
// STRUCTURE is stable under value/command/provider-presence change (only the STABLE
// catalog superset shapes it); a composed ssg.chrome header/footer REPLACES the built-in
// (override); and the whole tree passes validateUiSchema.

#include "ssg/WholeScreenAssembly.h"

#include "ssg/ChromeRegionShape.h"
#include "ssg/Style.h"
#include "ssg/UiTree.h"
#include "chrome_authoring.h"
#include "test_helpers.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace ssg;

const UiNode& child(const UiNode& node, std::size_t index) {
    return std::get<UiContainer>(node.content).children.at(index);
}

const UiNode* childById(const UiNode& node, std::string_view id) {
    if (const auto* c = std::get_if<UiContainer>(&node.content)) {
        for (const auto& ch : c->children)
            if (ch.id.value() == id) return &ch;
    }
    return nullptr;
}

StatusFieldCatalogEntry entry(std::string id, StatusFieldRegion region,
                              std::uint8_t rank = 0) {
    StatusFieldCatalogEntry e;
    e.id = std::move(id);
    e.region = region;
    e.collapseRank = rank;
    return e;
}

StyleDimensions dims() {
    StyleDimensions d;  // defaults: panelTargetWidth 24, headerHeight 1, footerHeight 1
    return d;
}

constexpr std::string_view kHintCommand = "help.open";
constexpr std::string_view kPromptSigil = "> ";

// The canonical skeleton (root/body/panel/content + view leaves + sizes) that must
// hold regardless of how header/footer were sourced.
void assertCanonicalSkeleton(const UiComposition& comp, const StyleDimensions& d) {
    ASSERT_EQ(comp.root.id.value(), std::string{kRootNodeId});
    ASSERT_TRUE(comp.root.size.kind() == SizeKind::Flex);
    const UiNode* header = childById(comp.root, kHeaderNodeId);
    const UiNode* body = childById(comp.root, kBodyNodeId);
    const UiNode* footer = childById(comp.root, kFooterNodeId);
    ASSERT_TRUE(header != nullptr);
    ASSERT_TRUE(body != nullptr);
    ASSERT_TRUE(footer != nullptr);
    ASSERT_EQ(child(comp.root, 0).id.value(), std::string{kHeaderNodeId});
    ASSERT_EQ(child(comp.root, 1).id.value(), std::string{kNoticeNodeId});
    ASSERT_EQ(child(comp.root, 2).id.value(), std::string{kExternalModNodeId});
    ASSERT_EQ(child(comp.root, 3).id.value(), std::string{kBodyNodeId});
    ASSERT_EQ(child(comp.root, 4).id.value(), std::string{kFooterPromptNodeId});
    ASSERT_EQ(child(comp.root, 5).id.value(), std::string{kFooterNodeId});
    // The draft-conflict notice is an always-assembled Auto-sized View naming Notice,
    // between the header and the body; presence (not assembly) hides it.
    const UiNode* notice = childById(comp.root, kNoticeNodeId);
    ASSERT_TRUE(notice != nullptr);
    if (notice) {
        ASSERT_TRUE(notice->size.kind() == SizeKind::Auto);
        const auto* nLeaf = std::get_if<UiLeaf>(&notice->content);
        ASSERT_TRUE(nLeaf != nullptr);
        if (nLeaf) {
            ASSERT_TRUE(nLeaf->widget.kind == WidgetKind::View);
            ASSERT_TRUE(nLeaf->widget.surface.has_value());
            if (nLeaf->widget.surface)
                ASSERT_TRUE(*nLeaf->widget.surface == ViewSurface::Notice);
        }
    }
    // The footer prompt is an always-assembled Auto-sized View naming FooterPrompt,
    // between the body and the footer; presence (not assembly) hides it.
    const UiNode* footerPrompt = childById(comp.root, kFooterPromptNodeId);
    ASSERT_TRUE(footerPrompt != nullptr);
    if (footerPrompt) {
        ASSERT_TRUE(footerPrompt->size.kind() == SizeKind::Auto);
        const auto* fpLeaf = std::get_if<UiLeaf>(&footerPrompt->content);
        ASSERT_TRUE(fpLeaf != nullptr);
        if (fpLeaf) {
            ASSERT_TRUE(fpLeaf->widget.kind == WidgetKind::View);
            ASSERT_TRUE(fpLeaf->widget.surface.has_value());
            if (fpLeaf->widget.surface)
                ASSERT_TRUE(*fpLeaf->widget.surface == ViewSurface::FooterPrompt);
        }
    }
    ASSERT_TRUE(header->size.kind() == SizeKind::Exact);
    ASSERT_EQ(header->size.extent(), d.headerHeight);
    ASSERT_TRUE(footer->size.kind() == SizeKind::Exact);
    ASSERT_EQ(footer->size.extent(), d.footerHeight);
    ASSERT_TRUE(body->size.kind() == SizeKind::Flex);
    const UiNode* panel = childById(*body, kPanelNodeId);
    const UiNode* content = childById(*body, kContentNodeId);
    ASSERT_TRUE(panel != nullptr);
    ASSERT_TRUE(content != nullptr);
    ASSERT_TRUE(panel->size.kind() == SizeKind::Exact);
    ASSERT_EQ(panel->size.extent(), d.panelTargetWidth);
    ASSERT_TRUE(content->size.kind() == SizeKind::Flex);
    struct Leaf { std::string_view id; const UiNode* parent; ViewSurface surface; };
    const Leaf leaves[] = {
        {kFileTreeNodeId, panel, ViewSurface::FileTree},
        {kGitStatusNodeId, panel, ViewSurface::GitStatus},
        {kSymbolsNodeId, panel, ViewSurface::Symbols},
        {kTabViewNodeId, content, ViewSurface::TabView},
        {kFindResultsNodeId, content, ViewSurface::FindResults},
    };
    for (const Leaf& leaf : leaves) {
        const UiNode* node = childById(*leaf.parent, leaf.id);
        ASSERT_TRUE(node != nullptr);
        if (!node) continue;
        ASSERT_TRUE(node->size.kind() == SizeKind::Flex);
        const auto* uiLeaf = std::get_if<UiLeaf>(&node->content);
        ASSERT_TRUE(uiLeaf != nullptr);
        if (!uiLeaf) continue;
        ASSERT_TRUE(uiLeaf->widget.kind == WidgetKind::View);
        ASSERT_TRUE(uiLeaf->widget.surface.has_value());
        if (uiLeaf->widget.surface) ASSERT_TRUE(*uiLeaf->widget.surface == leaf.surface);
    }
}

TEST(builtinHeaderFooterAreProviderBackedAndStable) {
    const std::vector<StatusFieldCatalogEntry> catalog{
        entry("path", StatusFieldRegion::Header, 3),
        entry("branch", StatusFieldRegion::Header),
        entry("mode", StatusFieldRegion::Footer)};
    const UiComposition comp =
        assembleWholeScreen(catalog, kHintCommand, dims(), kPromptSigil,
                            std::nullopt);
    assertCanonicalSkeleton(comp, dims());

    // Each built-in header field is a provider-backed Field keyed by its id, carrying
    // ONLY id + rank; value/label/command ride uiState (resolved per frame), never the
    // structure.
    const ssgtest::RowView headerRow =
        ssgtest::rowOf(*childById(comp.root, kHeaderNodeId));
    ASSERT_EQ(headerRow.left.size(), std::size_t{2});
    ASSERT_TRUE(headerRow.left[0].kind == WidgetKind::Field);
    ASSERT_TRUE(headerRow.left[0].value.has_value());
    if (headerRow.left[0].value) {
        ASSERT_TRUE(headerRow.left[0].value->isProvider);
        ASSERT_EQ(headerRow.left[0].value->provider, std::string{"path"});
    }
    ASSERT_EQ(headerRow.left[0].rank, 3);
    ASSERT_TRUE(!headerRow.left[0].command.has_value());  // command rides uiState

    // The footer right group carries the provider-backed hint (label rides uiState,
    // command stable) and the stable status-actions widget.
    const ssgtest::RowView footerRow =
        ssgtest::rowOf(*childById(comp.root, kFooterNodeId));
    ASSERT_EQ(footerRow.right.size(), std::size_t{2});
    ASSERT_TRUE(footerRow.right[0].value.has_value());
    if (footerRow.right[0].value) {
        ASSERT_TRUE(footerRow.right[0].value->isProvider);
        ASSERT_EQ(footerRow.right[0].value->provider, std::string{"footer.hint"});
    }
    ASSERT_TRUE(footerRow.right[0].command.has_value());
    if (footerRow.right[0].command)
        ASSERT_EQ(*footerRow.right[0].command, std::string{kHintCommand});
    ASSERT_TRUE(footerRow.right[1].kind == WidgetKind::StatusActions);
}

TEST(theCatalogSplitsByRegionDeterministically) {
    // The catalog is the ONLY structural input (the signature admits no dynamic value/
    // command/provider state), and it splits by each entry's own region. Assembling the
    // same catalog twice yields a byte-identical tree -- the generation-stability
    // guarantee, now a type fact: there is no per-frame value input to perturb it.
    const std::vector<StatusFieldCatalogEntry> catalog{
        entry("path", StatusFieldRegion::Header, 3),
        entry("mode", StatusFieldRegion::Footer)};
    const UiComposition a =
        assembleWholeScreen(catalog, kHintCommand, dims(), kPromptSigil,
                            std::nullopt);
    const UiComposition b =
        assembleWholeScreen(catalog, kHintCommand, dims(), kPromptSigil,
                            std::nullopt);
    ASSERT_TRUE(a == b);
    // A Header entry lands in the header left group; a Footer entry in the footer left.
    ASSERT_EQ(ssgtest::rowOf(*childById(a.root, kHeaderNodeId)).left.size(),
              std::size_t{1});
    ASSERT_EQ(ssgtest::rowOf(*childById(a.root, kFooterNodeId)).left.size(),
              std::size_t{1});
}

TEST(composedHeaderAndFooterOverrideTheBuiltins) {
    WidgetDescriptor composedHeaderField;
    composedHeaderField.kind = WidgetKind::Label;
    composedHeaderField.id = "title";
    composedHeaderField.value = ValueSource{false, "SSG", ""};
    WidgetDescriptor composedFooterField;
    composedFooterField.kind = WidgetKind::Label;
    composedFooterField.id = "hintlabel";
    composedFooterField.value = ValueSource{false, "ready", ""};
    const ValidatedComposition override = ssgtest::composeHeaderAndFooterValidated(
        {composedHeaderField}, {composedFooterField});

    const UiComposition comp = assembleWholeScreen(
        {entry("path", StatusFieldRegion::Header), entry("mode", StatusFieldRegion::Footer)},
        kHintCommand, dims(), kPromptSigil,
        std::optional<ValidatedComposition>{override});
    assertCanonicalSkeleton(comp, dims());

    const ssgtest::RowView headerRow =
        ssgtest::rowOf(*childById(comp.root, kHeaderNodeId));
    ASSERT_EQ(headerRow.left.size(), std::size_t{1});
    ASSERT_EQ(headerRow.left[0].value->literal, std::string{"SSG"});  // composed
    const ssgtest::RowView footerRow =
        ssgtest::rowOf(*childById(comp.root, kFooterNodeId));
    ASSERT_EQ(footerRow.left.size(), std::size_t{1});
    ASSERT_EQ(footerRow.left[0].value->literal, std::string{"ready"});  // composed
    // The composed footer replaces the whole built-in footer -- no hint, no actions.
    ASSERT_TRUE(footerRow.right.empty());
}

TEST(aComposedHeaderKeepsTheBuiltinFooterWhenFooterIsOmitted) {
    WidgetDescriptor composedHeaderField;
    composedHeaderField.kind = WidgetKind::Label;
    composedHeaderField.id = "title";
    composedHeaderField.value = ValueSource{false, "SSG", ""};
    const ValidatedComposition override =
        ssgtest::composeHeaderValidated({composedHeaderField});

    const UiComposition comp = assembleWholeScreen(
        {entry("path", StatusFieldRegion::Header), entry("mode", StatusFieldRegion::Footer)},
        kHintCommand, dims(), kPromptSigil,
        std::optional<ValidatedComposition>{override});
    assertCanonicalSkeleton(comp, dims());

    const ssgtest::RowView headerRow =
        ssgtest::rowOf(*childById(comp.root, kHeaderNodeId));
    ASSERT_EQ(headerRow.left[0].value->literal, std::string{"SSG"});  // composed
    const ssgtest::RowView footerRow =
        ssgtest::rowOf(*childById(comp.root, kFooterNodeId));
    ASSERT_TRUE(footerRow.left[0].value->isProvider);  // built-in fallback
    ASSERT_EQ(footerRow.left[0].value->provider, std::string{"mode"});
}

TEST(theAssembledTreeAlwaysValidates) {
    const UiComposition fallback = assembleWholeScreen(
        {entry("path", StatusFieldRegion::Header), entry("mode", StatusFieldRegion::Footer)},
        kHintCommand, dims(), kPromptSigil, std::nullopt);
    const UiComposition empty =
        assembleWholeScreen({}, kHintCommand, dims(), kPromptSigil,
                            std::nullopt);
    WidgetDescriptor composed;
    composed.kind = WidgetKind::Label;
    composed.id = "title";
    composed.value = ValueSource{false, "SSG", ""};
    const UiComposition overridden = assembleWholeScreen(
        {}, kHintCommand, dims(), kPromptSigil,
        std::optional<ValidatedComposition>{
            ssgtest::composeHeaderAndFooterValidated({composed}, {composed})});
    for (const UiComposition* comp : {&fallback, &empty, &overridden}) {
        const UiSchema schema{Generation{1}, comp->root};
        ASSERT_TRUE(validateUiSchema(schema).ok());
        ASSERT_TRUE(validateWellKnownAreas(schema).ok());
    }
}

TEST(theHeaderCarriesThePromptInputRightAfterTheLeftGroup) {
    // The prompt input is a stable TextInput leaf placed right after the header's
    // left (status-fields) group, present whether the header is built-in or
    // composed, so presence gating and chrome lowering always find input_line under
    // the header AND tree order matches the visual order (a tree-order client
    // renders it after the fields, not past the flex middle). It is never added to
    // the footer.
    const auto assertPromptInput = [](const UiComposition& comp) {
        const UiNode* header = childById(comp.root, kHeaderNodeId);
        ASSERT_TRUE(header != nullptr);
        if (!header) return;
        const UiNode* input = childById(*header, kHeaderPromptInputNodeId);
        ASSERT_TRUE(input != nullptr);
        if (!input) return;
        // It is the SECOND child -- immediately after the left group and before the
        // flex middle -- so a tree-order client renders it right after the fields.
        const auto& children = std::get<UiContainer>(header->content).children;
        ASSERT_EQ(children.at(1).id.value(),
                  std::string{kHeaderPromptInputNodeId});
        const auto* leaf = std::get_if<UiLeaf>(&input->content);
        ASSERT_TRUE(leaf != nullptr);
        if (leaf) {
            ASSERT_TRUE(leaf->widget.kind == WidgetKind::TextInput);
            ASSERT_EQ(leaf->widget.sigil, std::string{kPromptSigil});
        }
        // The footer never carries it.
        const UiNode* footer = childById(comp.root, kFooterNodeId);
        ASSERT_TRUE(footer != nullptr);
        if (footer)
            ASSERT_TRUE(childById(*footer, kHeaderPromptInputNodeId) == nullptr);
    };

    const UiComposition builtin = assembleWholeScreen(
        {entry("path", StatusFieldRegion::Header),
         entry("mode", StatusFieldRegion::Footer)},
        kHintCommand, dims(), kPromptSigil, std::nullopt);
    assertPromptInput(builtin);

    WidgetDescriptor composed;
    composed.kind = WidgetKind::Label;
    composed.id = "title";
    composed.value = ValueSource{false, "SSG", ""};
    const UiComposition composedHeader = assembleWholeScreen(
        {entry("mode", StatusFieldRegion::Footer)}, kHintCommand, dims(),
        kPromptSigil,
        std::optional<ValidatedComposition>{
            ssgtest::composeHeaderValidated({composed})});
    assertPromptInput(composedHeader);

    // The whole tree with the trailing input still validates.
    const UiSchema schema{Generation{1}, builtin.root};
    ASSERT_TRUE(validateUiSchema(schema).ok());
    ASSERT_TRUE(validateWellKnownAreas(schema).ok());
}

}  // namespace

int main() {
    RUN(builtinHeaderFooterAreProviderBackedAndStable);
    RUN(theCatalogSplitsByRegionDeterministically);
    RUN(composedHeaderAndFooterOverrideTheBuiltins);
    RUN(aComposedHeaderKeepsTheBuiltinFooterWhenFooterIsOmitted);
    RUN(theAssembledTreeAlwaysValidates);
    RUN(theHeaderCarriesThePromptInputRightAfterTheLeftGroup);
    return failed;
}
