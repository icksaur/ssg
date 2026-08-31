// Algorithm oracle for the whole-screen tree assembly. The knowable answers: the
// assembled tree is the canonical root>[header, body>[panel>[filetree,gitstatus],
// content>[editor>[tabbar,document viewport],find-results viewport]], footer]
// with extents drawn from StyleDimensions; the
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

const UiNode* findById(const UiNode& node, std::string_view id) {
    if (node.id.value() == id) return &node;
    if (const auto* c = std::get_if<UiContainer>(&node.content)) {
        for (const auto& childNode : c->children) {
            if (const auto* found = findById(childNode, id)) return found;
        }
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
    ASSERT_EQ(child(comp.root, 1).id.value(), std::string{kBodyNodeId});
    ASSERT_EQ(child(comp.root, 2).id.value(), std::string{kFooterPromptNodeId});
    ASSERT_EQ(child(comp.root, 3).id.value(), std::string{kFooterNodeId});
    // The footer prompt is an always-assembled Auto-sized container between the
    // body and footer; presence hides it while inactive.
    const UiNode* footerPrompt = childById(comp.root, kFooterPromptNodeId);
    ASSERT_TRUE(footerPrompt != nullptr);
    if (footerPrompt) {
        ASSERT_TRUE(footerPrompt->size.kind() == SizeKind::Auto);
        const auto* fp = std::get_if<UiContainer>(&footerPrompt->content);
        ASSERT_TRUE(fp != nullptr);
        if (fp) {
            ASSERT_TRUE(fp->axis == Axis::Column);
            ASSERT_TRUE(fp->children.empty());
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
    ASSERT_TRUE(panel->size.kind() == SizeKind::Responsive);
    ASSERT_EQ(panel->size.extent(), d.panelTargetWidth);
    ASSERT_EQ(panel->size.minimum(), d.panelMinimumWidth);
    ASSERT_TRUE(panel->size.optional());
    ASSERT_TRUE(content->size.kind() == SizeKind::Responsive);
    ASSERT_EQ(content->size.minimum(), d.editorMinimumWidth);
    ASSERT_TRUE(content->size.grows());
    ASSERT_FALSE(content->size.optional());
    const UiNode* editor = childById(*content, kEditorNodeId);
    const UiNode* notice = childById(*content, kNoticeNodeId);
    const UiNode* external = childById(*content, kExternalModNodeId);
    const UiNode* documentViewport =
        editor ? childById(*editor, kDocumentViewportNodeId) : nullptr;
    const UiNode* findResultsViewport =
        childById(*content, kFindResultsViewportNodeId);
    ASSERT_TRUE(editor != nullptr);
    ASSERT_TRUE(notice != nullptr);
    ASSERT_TRUE(external != nullptr);
    ASSERT_TRUE(documentViewport != nullptr);
    ASSERT_TRUE(findResultsViewport != nullptr);
    struct Leaf { std::string_view id; const UiNode* parent; ViewSurface surface; };
    const Leaf leaves[] = {
        {kTreeNodeId, panel, ViewSurface::Tree},
        {kTabBarNodeId, content, ViewSurface::TabBar},
        {kNoticeNodeId, content, ViewSurface::Notice},
        {kExternalModNodeId, content, ViewSurface::ExternalModification},
        {kDocumentNodeId, documentViewport, ViewSurface::Document},
        {kFindResultsNodeId, findResultsViewport, ViewSurface::FindResults},
    };
    for (const Leaf& leaf : leaves) {
        const UiNode* node = childById(*leaf.parent, leaf.id);
        ASSERT_TRUE(node != nullptr);
        if (!node) continue;
        if (leaf.surface == ViewSurface::TabBar) {
            ASSERT_TRUE(node->size.kind() == SizeKind::Exact);
            ASSERT_EQ(node->size.extent(), dims().tabBarHeight);
        } else if (leaf.surface == ViewSurface::Notice ||
                   leaf.surface == ViewSurface::ExternalModification) {
            ASSERT_TRUE(node->size.kind() == SizeKind::Auto);
        } else {
            ASSERT_TRUE(node->size.kind() == SizeKind::Flex);
        }
        const auto* uiLeaf = std::get_if<UiLeaf>(&node->content);
        ASSERT_TRUE(uiLeaf != nullptr);
        if (!uiLeaf) continue;
        ASSERT_TRUE(uiLeaf->widget.kind == WidgetKind::View);
        ASSERT_TRUE(uiLeaf->widget.surface.has_value());
        if (uiLeaf->widget.surface) ASSERT_TRUE(*uiLeaf->widget.surface == leaf.surface);
    }
}

TEST(assembledTreeNamesTheIndependentVerticalScrollViewports) {
    const UiComposition comp =
        assembleWholeScreen({}, kHintCommand, dims(), kPromptSigil, std::nullopt);
    const UiNode* body = childById(comp.root, kBodyNodeId);
    ASSERT_TRUE(body != nullptr);
    if (!body) return;
    const UiNode* panel = childById(*body, kPanelNodeId);
    const UiNode* content = childById(*body, kContentNodeId);
    ASSERT_TRUE(panel != nullptr);
    ASSERT_TRUE(content != nullptr);
    // The scroll viewport property lives on the container; a leaf can never be a
    // viewport. `scrollOf` reads the container's axis (None for a leaf).
    const auto scrollOf = [](const UiNode* node) {
        if (!node) return ScrollAxis::None;
        const auto* c = std::get_if<UiContainer>(&node->content);
        return c ? c->scroll : ScrollAxis::None;
    };
    const UiNode* editor = content ? childById(*content, kEditorNodeId) : nullptr;
    const UiNode* documentViewport =
        editor ? childById(*editor, kDocumentViewportNodeId) : nullptr;
    const UiNode* findResultsViewport =
        content ? childById(*content, kFindResultsViewportNodeId) : nullptr;
    ASSERT_TRUE(scrollOf(panel) == ScrollAxis::Vertical);
    ASSERT_TRUE(scrollOf(content) == ScrollAxis::None);
    ASSERT_TRUE(scrollOf(editor) == ScrollAxis::None);
    ASSERT_TRUE(scrollOf(documentViewport) == ScrollAxis::Vertical);
    ASSERT_TRUE(scrollOf(findResultsViewport) == ScrollAxis::Vertical);
    if (panel) ASSERT_TRUE(scrollOf(childById(*panel, kTreeNodeId)) == ScrollAxis::None);
    if (content)
        ASSERT_TRUE(scrollOf(childById(*content, kTabBarNodeId)) ==
                    ScrollAxis::None);
    if (documentViewport)
        ASSERT_TRUE(scrollOf(childById(*documentViewport, kDocumentNodeId)) ==
                    ScrollAxis::None);
    if (findResultsViewport)
        ASSERT_TRUE(scrollOf(childById(*findResultsViewport,
                                      kFindResultsNodeId)) == ScrollAxis::None);
    // Chrome/notice regions never scroll.
    for (const auto id : {kHeaderNodeId, kFooterNodeId, kNoticeNodeId,
                          kFooterPromptNodeId, kExternalModNodeId, kBodyNodeId}) {
        ASSERT_TRUE(scrollOf(findById(comp.root, id)) == ScrollAxis::None);
    }
}

TEST(assembledTreeAssignsCanonicalSemanticStyles) {
    const UiComposition comp =
        assembleWholeScreen({}, kHintCommand, dims(), kPromptSigil, std::nullopt);
    struct Expected {
        std::string_view id;
        SemanticRole foreground;
        SemanticRole background;
    };
    const Expected expected[] = {
        {kRootNodeId, SemanticRole::Text, SemanticRole::Canvas},
        {kHeaderNodeId, SemanticRole::Header, SemanticRole::HeaderBackground},
        {kFooterNodeId, SemanticRole::Footer, SemanticRole::FooterBackground},
        {kPanelNodeId, SemanticRole::PanelInactive,
         SemanticRole::TreeBackground},
        {kTabBarNodeId, SemanticRole::TabInactive,
         SemanticRole::TabInactiveBackground},
        {kDocumentNodeId, SemanticRole::Text, SemanticRole::Canvas},
        {kFindResultsNodeId, SemanticRole::Text, SemanticRole::Canvas},
        {kFooterPromptNodeId, SemanticRole::Prompt, SemanticRole::Canvas},
        {kNoticeNodeId, SemanticRole::Canvas, SemanticRole::StatusWarning},
        {kExternalModNodeId, SemanticRole::Canvas,
         SemanticRole::StatusWarning},
    };
    for (const auto& item : expected) {
        const UiNode* node = findById(comp.root, item.id);
        ASSERT_TRUE(node != nullptr);
        if (!node) continue;
        ASSERT_TRUE(node->style.foreground == item.foreground);
        ASSERT_TRUE(node->style.background == item.background);
    }
}

TEST(footerPromptAssemblyPublishesInputRowsAndAHorizontalOptionsRow) {
        PromptRequest request;
        request.kind = PromptKind::Replace;
        request.accessibleLabel = "Replace";
        request.inputs = {{"find.query", "Find text", "needle"},
                          {"replace.replacement", "Replacement text", "value"}};
        request.toggles = {{"find.case", "Case", true, 8},
                           {"find.regex", "Regex", false, 9}};
        request.matchCount = PromptMatchCount{"find.count", "Matches", "3"};

        const auto base =
            assembleWholeScreen({}, kHintCommand, dims(), kPromptSigil, std::nullopt);
        PromptSurface surface;
        ASSERT_TRUE(surface.open(request).accepted());
        const auto composed = withFooterPrompt(base, surface);
        const UiNode* prompt = childById(composed.root, kFooterPromptNodeId);
        ASSERT_TRUE(prompt != nullptr);
        if (!prompt) return;
        const auto* column = std::get_if<UiContainer>(&prompt->content);
        ASSERT_TRUE(column != nullptr);
        if (!column) return;
        ASSERT_TRUE(column->axis == Axis::Column);
        ASSERT_EQ(column->children.size(), std::size_t{3});
        for (std::size_t index = 0; index < request.inputs.size(); ++index) {
            const auto& node = column->children[index];
            const auto& leaf = std::get<UiLeaf>(node.content);
            ASSERT_TRUE(node.size.kind() == SizeKind::Exact);
            ASSERT_EQ(node.size.extent(), 1);
            ASSERT_TRUE(leaf.widget.kind == WidgetKind::TextInput);
            ASSERT_EQ(leaf.widget.id, request.inputs[index].id);
            ASSERT_TRUE(leaf.widget.value.has_value());
            ASSERT_TRUE(leaf.widget.value->isProvider);
            ASSERT_EQ(
                leaf.widget.value->provider,
                column->children[index].id.value());
        }
        const auto& optionsNode = column->children.back();
        ASSERT_EQ(optionsNode.id.value(), std::string{kFooterPromptOptionsNodeId});
        const auto& options = std::get<UiContainer>(optionsNode.content);
        ASSERT_TRUE(options.axis == Axis::Row);
        ASSERT_EQ(options.children.size(), request.toggles.size() + 1);
        for (std::size_t index = 0; index < request.toggles.size(); ++index) {
            const auto& node = options.children[index];
            const auto& leaf = std::get<UiLeaf>(node.content);
            ASSERT_TRUE(leaf.widget.kind == WidgetKind::Checkbox);
            ASSERT_TRUE(node.size.kind() == SizeKind::Exact);
            ASSERT_EQ(node.size.extent(), request.toggles[index].width);
            ASSERT_EQ(leaf.widget.id, request.toggles[index].id);
        }
        const auto& count = options.children.back();
        ASSERT_TRUE(count.size.kind() == SizeKind::Flex);
        ASSERT_TRUE(std::get<UiLeaf>(count.content).widget.kind == WidgetKind::Label);
        ASSERT_TRUE(validateWellKnownAreas(UiSchema{Generation{1}, composed.root}).ok());
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

    const auto* footer = childById(comp.root, kFooterNodeId);
    ASSERT_TRUE(footer != nullptr);
    if (!footer) return;
    const auto& footerRegion = std::get<ssg::UiContainer>(footer->content);
    const auto& footerRight =
        std::get<ssg::UiContainer>(footerRegion.children[2].content);
    ASSERT_EQ(footerRight.children.size(), std::size_t{2});
    const auto& hint =
        std::get<ssg::UiLeaf>(footerRight.children[0].content).widget;
    ASSERT_TRUE(hint.value.has_value());
    if (hint.value) {
        ASSERT_TRUE(hint.value->isProvider);
        ASSERT_EQ(hint.value->provider, std::string{"footer.hint"});
    }
    ASSERT_TRUE(hint.command.has_value());
    if (hint.command)
        ASSERT_EQ(*hint.command, std::string{kHintCommand});
    ASSERT_EQ(footerRight.children[1].id,
              ssg::UiNodeId{"footer.status_actions"});
    ASSERT_TRUE(std::holds_alternative<ssg::UiContainer>(
        footerRight.children[1].content));
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
    RUN(assembledTreeNamesTheIndependentVerticalScrollViewports);
    RUN(assembledTreeAssignsCanonicalSemanticStyles);
    RUN(footerPromptAssemblyPublishesInputRowsAndAHorizontalOptionsRow);
    RUN(theCatalogSplitsByRegionDeterministically);
    RUN(composedHeaderAndFooterOverrideTheBuiltins);
    RUN(aComposedHeaderKeepsTheBuiltinFooterWhenFooterIsOmitted);
    RUN(theAssembledTreeAlwaysValidates);
    RUN(theHeaderCarriesThePromptInputRightAfterTheLeftGroup);
    return failed;
}
