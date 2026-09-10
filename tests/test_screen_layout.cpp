#include <ssg/ScreenLayout.h>

#include <ssg/StatusFields.h>
#include <ssg/Style.h>
#include <ssg/UiTree.h>
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

TEST(fixedHeaderAndFooterAreSemanticUiNodes) {
    const auto composition = assembleScreen(
        "help.open", StyleDimensions{}, Style{}.inputLineSigil);
    const UiSchema schema{composition.root};
    ASSERT_TRUE(validateUiSchema(schema).ok());

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

    // The header/footer left groups are the fixed status fields (StatusFields.h),
    // in their stable collapse-rank order: path/branch, then status/follow.
    const auto& headerFields =
        std::get<UiContainer>(headerLeft->content).children;
    const auto& footerFields =
        std::get<UiContainer>(footerLeft->content).children;
    ASSERT_EQ(headerFields.size(), std::size_t{2});
    ASSERT_EQ(footerFields.size(), std::size_t{2});

    const auto& pathField = std::get<UiLeaf>(headerFields.at(0).content).widget;
    const auto& branchField = std::get<UiLeaf>(headerFields.at(1).content).widget;
    const auto& statusField = std::get<UiLeaf>(footerFields.at(0).content).widget;
    const auto& followField = std::get<UiLeaf>(footerFields.at(1).content).widget;
    ASSERT_EQ(pathField.id, std::string{kPathStatusFieldId});
    ASSERT_EQ(pathField.rank, 0);
    ASSERT_EQ(branchField.id, std::string{kBranchStatusFieldId});
    ASSERT_EQ(branchField.rank, 1);
    ASSERT_EQ(statusField.id, std::string{kStatusValueFieldId});
    ASSERT_EQ(statusField.rank, 0);
    ASSERT_EQ(followField.id, std::string{kFollowStatusFieldId});
    ASSERT_EQ(followField.rank, 1);

    const auto& right = std::get<UiContainer>(footerRight->content).children;
    ASSERT_EQ(right.at(0).id.value(), std::string{kFooterHintNodeId});
    const auto& hint = std::get<UiLeaf>(right.at(0).content).widget;
    ASSERT_EQ(hint.id, std::string{"footer.hint"});
    ASSERT_TRUE(hint.command && *hint.command == "help.open");
    ASSERT_EQ(right.size(), std::size_t{1});

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
    const auto composition = assembleScreen(
        "help.open", StyleDimensions{}, Style{}.inputLineSigil);
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

SSG_TEST_SUITE(test_screen_layout) {
    RUN(fixedHeaderAndFooterAreSemanticUiNodes);
    RUN(viewportsRemainSemanticTreeProperties);
    return failed == 0 ? 0 : 1;
}
