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

StatusField field(std::string id, std::uint8_t rank = 0,
                  std::optional<std::string> command = {}, std::string value = "v") {
    StatusField f;
    f.id = std::move(id);
    f.value = std::move(value);
    f.collapseRank = rank;
    f.commandId = std::move(command);
    return f;
}

StyleDimensions dims() {
    StyleDimensions d;  // defaults: panelTargetWidth 24, headerHeight 1, footerHeight 1
    return d;
}

constexpr std::string_view kHintCommand = "help.open";

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
    ASSERT_EQ(child(comp.root, 2).id.value(), std::string{kFooterNodeId});
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
    const std::vector<StatusField> headerFields{field("path", 3), field("branch")};
    const std::vector<StatusField> footerFields{field("mode")};
    const UiComposition comp =
        assembleWholeScreen(headerFields, footerFields, kHintCommand, dims(),
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

TEST(structureIsStableUnderValueCommandAndProviderChange) {
    // Same field IDS, different values/commands/ranks-unchanged: the assembled tree must
    // be byte-identical, because value/command are uiState, not structure. This is the
    // generation-stability guarantee: a branch value appearing/changing shapes no tree.
    const std::vector<StatusField> a{field("path", 3, std::optional<std::string>{"c1"}, "one"),
                                     field("branch", 0, std::nullopt, "main")};
    const std::vector<StatusField> b{field("path", 3, std::optional<std::string>{"c2"}, "two"),
                                     field("branch", 0, std::optional<std::string>{"x"}, "")};
    const UiComposition ca =
        assembleWholeScreen(a, {field("mode")}, kHintCommand, dims(), std::nullopt);
    const UiComposition cb =
        assembleWholeScreen(b, {field("mode")}, kHintCommand, dims(), std::nullopt);
    ASSERT_TRUE(ca == cb);
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
        {field("path")}, {field("mode")}, kHintCommand, dims(),
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
        {field("path")}, {field("mode")}, kHintCommand, dims(),
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
        {field("path")}, {field("mode")}, kHintCommand, dims(), std::nullopt);
    const UiComposition empty =
        assembleWholeScreen({}, {}, kHintCommand, dims(), std::nullopt);
    WidgetDescriptor composed;
    composed.kind = WidgetKind::Label;
    composed.id = "title";
    composed.value = ValueSource{false, "SSG", ""};
    const UiComposition overridden = assembleWholeScreen(
        {}, {}, kHintCommand, dims(),
        std::optional<ValidatedComposition>{
            ssgtest::composeHeaderAndFooterValidated({composed}, {composed})});
    for (const UiComposition* comp : {&fallback, &empty, &overridden}) {
        const UiSchema schema{Generation{1}, comp->root};
        ASSERT_TRUE(validateUiSchema(schema).ok());
        ASSERT_TRUE(validateWellKnownAreas(schema).ok());
    }
}

}  // namespace

int main() {
    RUN(builtinHeaderFooterAreProviderBackedAndStable);
    RUN(structureIsStableUnderValueCommandAndProviderChange);
    RUN(composedHeaderAndFooterOverrideTheBuiltins);
    RUN(aComposedHeaderKeepsTheBuiltinFooterWhenFooterIsOmitted);
    RUN(theAssembledTreeAlwaysValidates);
    return failed;
}
