#include "ssg/WholeScreenAssembly.h"

#include "ssg/Style.h"
#include "ssg/UiTree.h"
#include "test_helpers.h"

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace ssg;

const UiNode* child(const UiNode& parent, std::string_view id) {
    const auto* container = std::get_if<UiContainer>(&parent.content);
    if (!container) return nullptr;
    for (const auto& node : container->children) {
        if (node.id.value() == id) return &node;
    }
    return nullptr;
}

const UiNode* find(const UiNode& root, std::string_view id) {
    if (root.id.value() == id) return &root;
    const auto* container = std::get_if<UiContainer>(&root.content);
    if (!container) return nullptr;
    for (const auto& node : container->children) {
        if (const auto* match = find(node, id)) return match;
    }
    return nullptr;
}

StatusFieldCatalogEntry entry(std::string id, StatusFieldRegion region,
                              std::uint8_t rank = 0) {
    return {std::move(id), {}, region, rank};
}

TEST(fixedHeaderAndFooterAreSemanticUiNodes) {
    const auto composition = assembleWholeScreen(
        {entry("path", StatusFieldRegion::Header, 3),
         entry("mode", StatusFieldRegion::Footer),
         entry("branch", StatusFieldRegion::Header),
         entry("position", StatusFieldRegion::Footer)},
        "help.open", StyleDimensions{}, Style{}.inputLineSigil);
    const UiSchema schema{Generation{1}, composition.root};
    ASSERT_TRUE(validateUiSchema(schema).ok());
    ASSERT_TRUE(validateWellKnownAreas(schema).ok());

    const auto* header = child(composition.root, kHeaderNodeId);
    const auto* footer = child(composition.root, kFooterNodeId);
    ASSERT_TRUE(header != nullptr);
    ASSERT_TRUE(footer != nullptr);
    if (!header || !footer) return;

    const auto& headerChildren = std::get<UiContainer>(header->content).children;
    ASSERT_EQ(headerChildren.at(1).id.value(),
              std::string{kHeaderPromptInputNodeId});
    ASSERT_TRUE(std::get<UiLeaf>(headerChildren.at(1).content).widget.kind ==
                WidgetKind::TextInput);

    const auto* headerLeft = child(*header, "header.left");
    const auto* footerLeft = child(*footer, "footer.left");
    const auto* footerRight = child(*footer, "footer.right");
    ASSERT_TRUE(headerLeft != nullptr);
    ASSERT_TRUE(footerLeft != nullptr);
    ASSERT_TRUE(footerRight != nullptr);
    if (!headerLeft || !footerLeft || !footerRight) return;

    const auto& headerField =
        std::get<UiLeaf>(
            std::get<UiContainer>(headerLeft->content).children.at(0).content)
            .widget;
    const auto& footerField =
        std::get<UiLeaf>(
            std::get<UiContainer>(footerLeft->content).children.at(0).content)
            .widget;
    ASSERT_TRUE(headerField.value && headerField.value->isProvider);
    ASSERT_EQ(headerField.value->provider, std::string{"path"});
    ASSERT_EQ(headerField.rank, 3);
    ASSERT_TRUE(footerField.value && footerField.value->isProvider);
    ASSERT_EQ(footerField.value->provider, std::string{"mode"});
    const auto& headerFields =
        std::get<UiContainer>(headerLeft->content).children;
    const auto& footerFields =
        std::get<UiContainer>(footerLeft->content).children;
    ASSERT_EQ(std::get<UiLeaf>(headerFields.at(1).content).widget.id,
              std::string{"branch"});
    ASSERT_EQ(std::get<UiLeaf>(footerFields.at(1).content).widget.id,
              std::string{"position"});

    const auto& right = std::get<UiContainer>(footerRight->content).children;
    ASSERT_EQ(right.at(0).id.value(), std::string{"footer.right.0"});
    const auto& hint = std::get<UiLeaf>(right.at(0).content).widget;
    ASSERT_TRUE(hint.value && hint.value->isProvider);
    ASSERT_EQ(hint.value->provider, std::string{"footer.hint"});
    ASSERT_TRUE(hint.command && *hint.command == "help.open");
    ASSERT_EQ(right.at(1).id.value(), std::string{"footer.status_actions"});

    struct ExpectedStyle {
        std::string_view id;
        SemanticRole foreground;
        SemanticRole background;
    };
    constexpr ExpectedStyle expected[] = {
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
        const auto* node = find(composition.root, item.id);
        ASSERT_TRUE(node != nullptr);
        if (!node) continue;
        ASSERT_TRUE(node->style.foreground == item.foreground);
        ASSERT_TRUE(node->style.background == item.background);
    }
}

TEST(viewportsRemainSemanticTreeProperties) {
    const auto composition = assembleWholeScreen(
        {}, "help.open", StyleDimensions{}, Style{}.inputLineSigil);
    const auto* body = child(composition.root, kBodyNodeId);
    const auto* panel = body ? child(*body, kPanelNodeId) : nullptr;
    const auto* content = body ? child(*body, kContentNodeId) : nullptr;
    const auto* editor = content ? child(*content, kEditorNodeId) : nullptr;
    const auto* document =
        editor ? child(*editor, kDocumentViewportNodeId) : nullptr;
    const auto* results =
        content ? child(*content, kFindResultsViewportNodeId) : nullptr;
    ASSERT_TRUE(panel && content && editor && document && results);
    if (!panel || !content || !editor || !document || !results) return;
    ASSERT_TRUE(std::get<UiContainer>(panel->content).scroll ==
                ScrollAxis::Vertical);
    ASSERT_TRUE(std::get<UiContainer>(document->content).scroll ==
                ScrollAxis::Vertical);
    ASSERT_TRUE(std::get<UiContainer>(results->content).scroll ==
                ScrollAxis::Vertical);
    ASSERT_TRUE(std::get<UiContainer>(content->content).scroll ==
                ScrollAxis::None);
    ASSERT_TRUE(std::get<UiContainer>(editor->content).scroll ==
                ScrollAxis::None);
    for (const auto id :
         {kTreeNodeId, kTabBarNodeId, kNoticeNodeId, kExternalModNodeId,
          kDocumentNodeId, kFindResultsNodeId, kHeaderNodeId, kFooterNodeId,
          kFooterPromptNodeId}) {
        const auto* node = find(composition.root, id);
        ASSERT_TRUE(node != nullptr);
        if (!node) continue;
        if (const auto* container = std::get_if<UiContainer>(&node->content)) {
            ASSERT_TRUE(container->scroll == ScrollAxis::None);
        }
    }
}

}  // namespace

int main() {
    RUN(fixedHeaderAndFooterAreSemanticUiNodes);
    RUN(viewportsRemainSemanticTreeProperties);
    return failed == 0 ? 0 : 1;
}
