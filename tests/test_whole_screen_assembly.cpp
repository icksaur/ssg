// Algorithm oracle for the whole-screen tree assembly. The knowable answers: the
// assembled tree is the canonical root>[header, body>[panel>[filetree,gitstatus],
// content>[tabview,findresults]], footer] with the canonical sizes; an omitted
// ssg.chrome header/footer is synthesized from the status fields (fallback); a
// composed header/footer REPLACES the built-in (override); and the whole tree passes
// validateUiSchema.

#include "ssg/WholeScreenAssembly.h"

#include "ssg/ChromeRegionShape.h"
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

StatusField field(std::string id, std::optional<std::string> command = {}) {
    StatusField f;
    f.id = std::move(id);
    f.value = "v";
    f.commandId = std::move(command);
    return f;
}

// The canonical skeleton (root/body/panel/content + view leaves + sizes) that must
// hold regardless of how header/footer were sourced.
void assertCanonicalSkeleton(const UiComposition& comp) {
    ASSERT_EQ(comp.root.id.value(), std::string{kRootNodeId});
    ASSERT_TRUE(comp.root.size.kind() == SizeKind::Flex);
    const UiNode* header = childById(comp.root, kHeaderNodeId);
    const UiNode* body = childById(comp.root, kBodyNodeId);
    const UiNode* footer = childById(comp.root, kFooterNodeId);
    ASSERT_TRUE(header != nullptr);
    ASSERT_TRUE(body != nullptr);
    ASSERT_TRUE(footer != nullptr);
    // Order is header, body, footer.
    ASSERT_EQ(child(comp.root, 0).id.value(), std::string{kHeaderNodeId});
    ASSERT_EQ(child(comp.root, 1).id.value(), std::string{kBodyNodeId});
    ASSERT_EQ(child(comp.root, 2).id.value(), std::string{kFooterNodeId});
    // header/footer occupy fixed rows regardless of source.
    ASSERT_TRUE(header->size.kind() == SizeKind::Exact);
    ASSERT_EQ(header->size.extent(), kHeaderRows);
    ASSERT_TRUE(footer->size.kind() == SizeKind::Exact);
    ASSERT_EQ(footer->size.extent(), kFooterRows);
    // body Row Flex.
    ASSERT_TRUE(body->size.kind() == SizeKind::Flex);
    const UiNode* panel = childById(*body, kPanelNodeId);
    const UiNode* content = childById(*body, kContentNodeId);
    ASSERT_TRUE(panel != nullptr);
    ASSERT_TRUE(content != nullptr);
    // panel Exact(kPanelWidth), content Flex.
    ASSERT_TRUE(panel->size.kind() == SizeKind::Exact);
    ASSERT_EQ(panel->size.extent(), kPanelWidth);
    ASSERT_TRUE(content->size.kind() == SizeKind::Flex);
    // The four view leaves, each Flex, naming its surface.
    struct Leaf {
        std::string_view id;
        const UiNode* parent;
        ViewSurface surface;
    };
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

TEST(omittedBuiltinsSynthesizeHeaderAndFooterFromStatusFields) {
    const std::vector<StatusField> headerFields{field("path", "panel.show_files"),
                                                field("branch")};
    const std::vector<StatusField> footerFields{field("mode")};
    const UiComposition comp =
        assembleWholeScreen(headerFields, footerFields, std::nullopt);
    assertCanonicalSkeleton(comp);

    // The built-in header's left group carries one provider-backed Field per header
    // status field, keyed by the field id, with the inherited command.
    const UiNode& header = *childById(comp.root, kHeaderNodeId);
    const ssgtest::RowView headerRow = ssgtest::rowOf(header);
    ASSERT_EQ(headerRow.left.size(), std::size_t{2});
    ASSERT_TRUE(headerRow.left[0].kind == WidgetKind::Field);
    ASSERT_TRUE(headerRow.left[0].value.has_value());
    if (headerRow.left[0].value) {
        ASSERT_TRUE(headerRow.left[0].value->isProvider);
        ASSERT_EQ(headerRow.left[0].value->provider, std::string{"path"});
    }
    ASSERT_TRUE(headerRow.left[0].command.has_value());
    if (headerRow.left[0].command)
        ASSERT_EQ(*headerRow.left[0].command, std::string{"panel.show_files"});
    ASSERT_TRUE(headerRow.right.empty());

    const UiNode& footer = *childById(comp.root, kFooterNodeId);
    const ssgtest::RowView footerRow = ssgtest::rowOf(footer);
    ASSERT_EQ(footerRow.left.size(), std::size_t{1});
    ASSERT_EQ(footerRow.left[0].value->provider, std::string{"mode"});
}

TEST(composedHeaderAndFooterOverrideTheBuiltins) {
    // A ssg.chrome composition of both regions must REPLACE the built-in header and
    // footer (their composed content survives), not sit beside them.
    WidgetDescriptor composedHeaderField;
    composedHeaderField.kind = WidgetKind::Label;
    composedHeaderField.id = "title";
    composedHeaderField.value = ValueSource{false, "SSG", ""};
    WidgetDescriptor composedFooterField;
    composedFooterField.kind = WidgetKind::Label;
    composedFooterField.id = "hint";
    composedFooterField.value = ValueSource{false, "ready", ""};
    const UiComposition override = ssgtest::composeHeaderAndFooter(
        {composedHeaderField}, {composedFooterField});

    // Built-in status fields that must be IGNORED because the composition overrides.
    const std::vector<StatusField> headerFields{field("path")};
    const std::vector<StatusField> footerFields{field("mode")};
    const UiComposition comp =
        assembleWholeScreen(headerFields, footerFields, override);
    assertCanonicalSkeleton(comp);

    const UiNode& header = *childById(comp.root, kHeaderNodeId);
    const ssgtest::RowView headerRow = ssgtest::rowOf(header);
    ASSERT_EQ(headerRow.left.size(), std::size_t{1});
    ASSERT_EQ(headerRow.left[0].value->literal, std::string{"SSG"});  // composed, not "path"

    const UiNode& footer = *childById(comp.root, kFooterNodeId);
    const ssgtest::RowView footerRow = ssgtest::rowOf(footer);
    ASSERT_EQ(footerRow.left.size(), std::size_t{1});
    ASSERT_EQ(footerRow.left[0].value->literal, std::string{"ready"});
}

TEST(aComposedHeaderKeepsTheBuiltinFooterWhenFooterIsOmitted) {
    // header-only composition: header overridden, footer falls back to built-in.
    WidgetDescriptor composedHeaderField;
    composedHeaderField.kind = WidgetKind::Label;
    composedHeaderField.id = "title";
    composedHeaderField.value = ValueSource{false, "SSG", ""};
    const UiComposition override = ssgtest::composeHeader({composedHeaderField});

    const UiComposition comp =
        assembleWholeScreen({field("path")}, {field("mode")}, override);
    assertCanonicalSkeleton(comp);

    const ssgtest::RowView headerRow =
        ssgtest::rowOf(*childById(comp.root, kHeaderNodeId));
    ASSERT_EQ(headerRow.left[0].value->literal, std::string{"SSG"});  // composed
    const ssgtest::RowView footerRow =
        ssgtest::rowOf(*childById(comp.root, kFooterNodeId));
    ASSERT_TRUE(footerRow.left[0].value->isProvider);  // built-in fallback
    ASSERT_EQ(footerRow.left[0].value->provider, std::string{"mode"});
}

TEST(theAssembledTreeAlwaysValidates) {
    // Fallback, override, and empty-field cases all produce a schema-valid tree.
    const UiComposition fallback =
        assembleWholeScreen({field("path")}, {field("mode")}, std::nullopt);
    const UiComposition empty = assembleWholeScreen({}, {}, std::nullopt);
    WidgetDescriptor composed;
    composed.kind = WidgetKind::Label;
    composed.id = "title";
    composed.value = ValueSource{false, "SSG", ""};
    const UiComposition override = assembleWholeScreen(
        {}, {}, ssgtest::composeHeaderAndFooter({composed}, {composed}));
    for (const UiComposition* comp : {&fallback, &empty, &override}) {
        const UiSchema schema{Generation{1}, comp->root};
        ASSERT_TRUE(validateUiSchema(schema).ok());
        // And the assembled tree satisfies the well-known-area contract (root +
        // header/footer as direct-child containers) even before areas are promoted.
        ASSERT_TRUE(validateWellKnownAreas(schema).ok());
    }
}

}  // namespace

int main() {
    RUN(omittedBuiltinsSynthesizeHeaderAndFooterFromStatusFields);
    RUN(composedHeaderAndFooterOverrideTheBuiltins);
    RUN(aComposedHeaderKeepsTheBuiltinFooterWhenFooterIsOmitted);
    RUN(theAssembledTreeAlwaysValidates);
    return failed;
}
