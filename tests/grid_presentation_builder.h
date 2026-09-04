#pragma once

#include <ssg/GraphemeLayout.h>
#include <ssg/StatusFields.h>
#include <ssg/StatusQueue.h>
#include <ssg/whole_screen_schema.h>
#include <ssg/GridPresenter.h>

#include <ssg/interaction.h>
#include <ssg/ui_tree_population.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ssg::test {

class GridPresentationBuilder {
public:
    struct TabSpec {
        std::string title;
        std::string accessibleLabel;
        bool active = false;
        bool dirty = false;
    };

    GridPresentationBuilder& document(std::string text) {
        text_ = std::move(text);
        return *this;
    }
    GridPresentationBuilder& caret(std::size_t offset) {
        caret_ = offset;
        return *this;
    }
    GridPresentationBuilder& viewport(int columns, int rows) {
        columns_ = columns;
        rows_ = rows;
        return *this;
    }
    GridPresentationBuilder& firstVisualRow(std::uint32_t row) {
        firstRow_ = row;
        return *this;
    }
    GridPresentationBuilder& revision(std::uint64_t value) {
        revision_ = value;
        return *this;
    }
    GridPresentationBuilder& panel(bool shown) {
        panel_ = shown;
        return *this;
    }
    GridPresentationBuilder& panelFocused(bool focused) {
        panelFocused_ = focused;
        return *this;
    }
    GridPresentationBuilder& tabs(std::vector<TabSpec> labels) {
        tabs_ = std::move(labels);
        return *this;
    }
    GridPresentationBuilder& fields(
        std::function<void(GridPresentation&)> mutate) {
        mutators_.push_back(std::move(mutate));
        return *this;
    }
    GridPresentationBuilder& tree(
        std::function<void(TreeViewState&)> mutate) {
        treeMutators_.push_back(std::move(mutate));
        return *this;
    }
    GridPresentationBuilder& lineNumbers(bool enabled = true) {
        lineNumbers_ = enabled;
        return *this;
    }
    GridPresentationBuilder& noticePresent(bool present = true) {
        noticePresent_ = present;
        return *this;
    }
    GridPresentationBuilder& externalModificationPresent(bool present = true) {
        externalModificationPresent_ = present;
        return *this;
    }
    GridPresentationBuilder& statusFields(StatusFieldProjection status) {
        statusFields_ = std::move(status);
        return *this;
    }
    GridPresentationBuilder& helpHint(std::string label) {
        helpHintLabel_ = std::move(label);
        return *this;
    }
    GridPresentationBuilder& style(Style style) {
        style_ = std::move(style);
        return *this;
    }
    GridPresentationBuilder& paletteReport(PaletteReport palette) {
        palette_ = std::move(palette);
        return *this;
    }
    GridPresentationBuilder& promptInput(bool visible, std::string query,
                                         std::string ghost = {}) {
        if (visible) {
            promptInput_ = PromptInput{std::move(query), std::move(ghost)};
        }
        return *this;
    }
    GridPresentationBuilder& schema(UiSchema schema) {
        schema_ = std::move(schema);
        return *this;
    }
    GridPresentationBuilder& status(StatusViewState status) {
        status_ = std::move(status);
        return *this;
    }

    [[nodiscard]] GridPresentation build() const {
        const auto caret = std::min(caret_, text_.size());
        UiComposition composition;
        composition.root =
            schema_ ? schema_->root
                    : assembleWholeScreen("help.open", style_.dimensions,
                                          style_.inputLineSigil)
                          .root;
        TreeModel treeModel;
        InteractionState interaction{
            withStatusActions(std::move(composition),
                              projectStatusActionNodes(status_)),
            treeModel};
        if (panel_) {
            (void)interaction.togglePanel();
            if (!panelFocused_) interaction.focusEditor();
        }
        if (noticePresent_) (void)interaction.refreshNoticePresence(true);
        if (externalModificationPresent_) {
            (void)interaction.refreshExternalModificationPresence(true);
        }
        if (promptInput_) (void)interaction.openFinder(PickerKind::Command);

        UiSchema uiTree = interaction.schema();
        detail::populateUiTree(
            uiTree, detail::UiTreeValues{statusFields_, helpHintLabel_,
                                         projectStatusActionNodes(status_),
                                         std::nullopt});
        uiTree.focusPath = interaction.focusPath();
        uiTree = requirePublishedUiTree(std::move(uiTree));

        TabViewState tabs;
        for (std::size_t index = 0; index < tabs_.size(); ++index) {
            TabState tab;
            tab.id = TabId{index + 1};
            tab.label = tabs_[index].title;
            tab.dirty = tabs_[index].dirty;
            tabs.tabs.push_back(std::move(tab));
            if (tabs_[index].active) tabs.active = TabId{index + 1};
        }

        const ViewportDimensions dimensions{
            static_cast<std::uint32_t>(columns_),
            static_cast<std::uint32_t>(rows_)};
        auto presentation = GridPresentation{
            .viewport = Viewport{}.computeUnwrapped(
                text_, dimensions, firstRow_, 0, 4),
            .style = style_,
            .palette = palette_,
            .documentText = text_,
            .documentRevision = revision_,
            .selections = SelectionSet{{
                Selection{caretPosition(caret), caretPosition(caret)}}},
            .syntax = SyntaxViewState::plainText(
                revision_, LanguageId{"plain"}, text_, 4),
            .theme = defaultTheme(),
            .uiTree = std::move(uiTree),
            .tabs = std::move(tabs),
            .notice = noticePresent_
                          ? std::optional<NoticeView>{NoticeView{
                                "Draft conflict",
                                {{"draft.notice.dismiss", "dismiss",
                                  "draft.dismiss"}}}}
                          : std::nullopt,
            .externalModification = {},
            .followMode = FollowMode::Following,
            .promptStatus = PromptStatusViewState{
                status_, promptInput_
                             ? std::optional<PromptKind>{PromptKind::Palette}
                             : std::nullopt},
            .paletteView = {},
        };
        if (promptInput_) {
            presentation.paletteView.activePicker =
                PickerActivation{SearchMode::Command, PickerActivationId{1}};
            presentation.palette.query = promptInput_->query;
            presentation.palette.ghost = promptInput_->ghost;
        }
        for (const auto& mutate : mutators_) mutate(presentation);

        TreeViewState tree;
        for (const auto& mutate : treeMutators_) mutate(tree);
        std::vector<GridIntrinsicSize> sizes;
        const auto collect = [&](const auto& self, const UiNode& node) -> void {
            if (node.size.kind() == SizeKind::Auto && node.isLeaf()) {
                auto size = GridSize{1, 1};
                if (node.id == UiNodeId{std::string{kExternalModNodeId}}) {
                    size = measureExternalModificationSurface(
                        presentation.externalModification);
                }
                sizes.push_back({node.id, size});
            }
            if (const auto* container =
                    std::get_if<UiContainer>(&node.content)) {
                for (const auto& child : container->children) self(self, child);
            }
        };
        collect(collect, presentation.uiTree.root);
        SolveUiFrameResult solved;
        if (dimensions.columns < style_.dimensions.minimumColumns ||
            dimensions.rows < style_.dimensions.minimumRows) {
            solved.tree = SolvedGridTree{};
        } else {
            for (;;) {
                solved = solveUiFrame(
                    presentation.uiTree, sizes,
                    {0, 0, static_cast<int>(dimensions.columns),
                     static_cast<int>(dimensions.rows)});
                const auto* content =
                    solved.tree
                        ? (solved.tree->find(
                               UiNodeId{std::string{kEditorNodeId}})
                               ? solved.tree->find(
                                     UiNodeId{std::string{kEditorNodeId}})
                               : solved.tree->find(UiNodeId{std::string{
                                     kFindResultsViewportNodeId}}))
                        : nullptr;
                if (solved.tree && (!content || content->rect.height > 0)) {
                    break;
                }
                auto external = std::find_if(
                    sizes.begin(), sizes.end(),
                    [](const GridIntrinsicSize& size) {
                        return size.id ==
                               UiNodeId{std::string{kExternalModNodeId}};
                    });
                if (external == sizes.end() || external->size.rows <= 1) {
                    break;
                }
                --external->size.rows;
            }
        }
        if (!solved.tree) throw std::logic_error{solved.error};
        presentation.layout = std::move(*solved.tree);

        const auto solveRegion =
            [&](std::string_view id, SemanticRole role,
                std::optional<SolvedUiRegion>& out,
                const StatusViewState* status,
                const PromptInputProjection* input) {
                const auto* node =
                    presentation.layout.find(UiNodeId{std::string{id}});
                if (!node) return;
                const auto* schemaNode = findNode(presentation.uiTree.root, id);
                if (!schemaNode) throw std::logic_error{"missing UI node"};
                SolvedUiRegion region;
                auto result = projectUiRegion(
                    *schemaNode, node->rect, role, presentation.style, region,
                    status, input);
                if (!result.ok()) throw std::logic_error{*result.error};
                out = std::move(region);
            };
        PromptInputProjection promptInput;
        const PromptInputProjection* promptInputPointer = nullptr;
        if (promptInput_) {
            promptInput = {true, promptInput_->query, promptInput_->ghost};
            promptInputPointer = &promptInput;
        }
        solveRegion(kHeaderNodeId, SemanticRole::Header,
                    presentation.header, nullptr, promptInputPointer);
        solveRegion(kFooterNodeId, SemanticRole::Footer,
                    presentation.footer, &presentation.promptStatus.status,
                    nullptr);
        if (const auto* node = presentation.layout.find(
                UiNodeId{std::string{kPanelNodeId}})) {
            presentation.panel =
                solvePanelSurface(tree, *node, 0, true, presentation.style);
        }
        if (const auto* node = presentation.layout.find(
                UiNodeId{std::string{kDocumentViewportNodeId}})) {
            presentation.document = solveDocumentSurface(
                *node, PaneTopology::initial(), lineNumbers_,
                static_cast<std::uint32_t>(
                    presentation.syntax.indentation().size()),
                presentation.style.dimensions);
            if (!presentation.document->panes.empty()) {
                const auto& content =
                    presentation.document
                        ->panes[presentation.document->activePaneIndex]
                        .content;
                const ViewportDimensions pane{
                    static_cast<std::uint32_t>(std::max(content.width, 1)),
                    static_cast<std::uint32_t>(std::max(content.height, 1))};
                const auto activeDiff = presentation.diff.fileForIdentity(
                    presentation.diffFileIdentity);
                presentation.viewport = Viewport{}.computeUnwrapped(
                    presentation.documentText, pane, firstRow_, 0, 4,
                    activeDiff ? &activeDiff->get() : nullptr, dimensions);
            }
        }
        return presentation;
    }

private:
    struct PromptInput {
        std::string query;
        std::string ghost;
    };

    [[nodiscard]] static const UiNode* findNode(
        const UiNode& node, std::string_view id) {
        if (node.id.value() == id) return &node;
        if (const auto* container = std::get_if<UiContainer>(&node.content)) {
            for (const auto& child : container->children) {
                if (const auto* found = findNode(child, id)) return found;
            }
        }
        return nullptr;
    }

    [[nodiscard]] DocumentPosition caretPosition(std::size_t offset) const {
        std::uint32_t line = 0;
        std::size_t lineStart = 0;
        for (std::size_t i = 0; i < offset && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                lineStart = i + 1;
            }
        }
        const auto cells =
            GraphemeLayout{}
                .computeRun(
                    std::string_view{text_}.substr(lineStart, offset - lineStart))
                .totalCells;
        return {ByteOffset{offset}, LineIndex{line}, CellIndex{cells}};
    }

    std::string text_;
    std::size_t caret_ = 0;
    int columns_ = 80;
    int rows_ = 24;
    std::uint32_t firstRow_ = 0;
    std::uint64_t revision_ = 1;
    bool panel_ = false;
    bool panelFocused_ = true;
    bool lineNumbers_ = false;
    std::vector<TabSpec> tabs_;
    std::vector<std::function<void(GridPresentation&)>> mutators_;
    std::vector<std::function<void(TreeViewState&)>> treeMutators_;
    Style style_{};
    PaletteReport palette_;
    std::optional<UiSchema> schema_;
    std::optional<PromptInput> promptInput_;
    bool noticePresent_ = false;
    bool externalModificationPresent_ = false;
    StatusFieldProjection statusFields_;
    std::string helpHintLabel_;
    StatusViewState status_;
};

}  // namespace ssg::test
