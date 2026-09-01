#include <ssg/GridPresenter.h>

#include <ssg/EditorSession.h>

#include "grid_projection_state.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace ssg {
namespace {

const UiNode* findUiNode(const UiNode& node, std::string_view id) {
    if (node.id.value() == id) return &node;
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children) {
            if (const auto* found = findUiNode(child, id)) return found;
        }
    }
    return nullptr;
}

std::vector<GridIntrinsicSize> semanticIntrinsicSizes(
    const UiNode& root, const SessionSnapshotSections& sections) {
    std::vector<GridIntrinsicSize> sizes;
    const auto collect = [&](const auto& self, const UiNode& node) -> void {
        if (node.size.kind() == SizeKind::Auto && node.isLeaf()) {
            sizes.push_back({node.id, {1, 1}});
        }
        if (const auto* container = std::get_if<UiContainer>(&node.content)) {
            for (const auto& child : container->children) self(self, child);
        }
    };
    collect(collect, root);
    for (auto& size : sizes) {
        if (size.id == UiNodeId{std::string{kExternalModNodeId}}) {
            size.size =
                measureExternalModificationSurface(
                    sections.externalModification);
            break;
        }
    }
    return sizes;
}

SolveUiFrameResult trySolveFrameLayout(
    const SessionSnapshot& semantic,
    GridSize dimensions, const Style& style) {
    if (dimensions.columns <= 0 || dimensions.rows <= 0) {
        return {SolvedGridTree{}, {}};
    }
    if (dimensions.columns <
            static_cast<int>(style.dimensions.minimumColumns) ||
        dimensions.rows < static_cast<int>(style.dimensions.minimumRows)) {
        return {SolvedGridTree{}, {}};
    }

    auto validated =
        ValidatedSchema::validate(semantic.sections().uiFrame.schema());
    if (!validated.ok()) {
        return {std::nullopt,
                "invalid UI schema: " + validated.error()};
    }

    auto presence = semantic.sections().uiFrame.presence();

    const auto& root = validated.schema().schema().root;
    auto intrinsicSizes =
        semanticIntrinsicSizes(root, semantic.sections());
    SolveUiFrameResult result;
    for (;;) {
        result = solveUiFrame(
            validated.schema(), semantic.sections().uiFrame.state(), presence,
            ClientUiProfile::full(), intrinsicSizes,
            {0, 0, dimensions.columns, dimensions.rows});
        // WholeScreenAssembly's exhaustive replaceable content branches are
        // editor and find-results. Extend this check with that topology.
        const auto* content =
            result.tree
                ? (result.tree->find(UiNodeId{std::string{kEditorNodeId}})
                       ? result.tree->find(
                             UiNodeId{std::string{kEditorNodeId}})
                       : result.tree->find(UiNodeId{
                             std::string{kFindResultsViewportNodeId}}))
                : nullptr;
        if (result.tree && (!content || content->rect.height > 0)) break;
        auto external = std::find_if(
            intrinsicSizes.begin(), intrinsicSizes.end(),
            [](const GridIntrinsicSize& size) {
                return size.id ==
                       UiNodeId{std::string{kExternalModNodeId}};
            });
        if (external == intrinsicSizes.end() || external->size.rows <= 1) {
            break;
        }
        --external->size.rows;
    }
    if (!result.tree) {
        if (result.error == "UI frame does not fit grid bounds") {
            return {SolvedGridTree{}, {}};
        }
        return result;
    }

    // Only migrated placements are exposed. Unit intrinsic sizes let the outer
    // tree solve while unmigrated surfaces still own their legacy projections;
    // erasing those provisional nodes prevents a later consumer from treating
    // placeholder geometry as authoritative.
    std::set<UiNodeId> retained{
        UiNodeId{std::string{kRootNodeId}},
        UiNodeId{std::string{kHeaderNodeId}},
        UiNodeId{std::string{kFooterNodeId}},
        UiNodeId{std::string{kNoticeNodeId}},
        UiNodeId{std::string{kExternalModNodeId}},
        UiNodeId{std::string{kTabBarNodeId}},
        UiNodeId{std::string{kPanelNodeId}},
        UiNodeId{std::string{kDocumentViewportNodeId}},
        UiNodeId{std::string{kDocumentNodeId}},
        UiNodeId{std::string{kFindResultsViewportNodeId}},
        UiNodeId{std::string{kHeaderPromptInputNodeId}},
    };
    const auto* promptSchema = findUiNode(
        semantic.sections().uiFrame.schema().root, kFooterPromptNodeId);
    if (semantic.sections().promptStatus.activeKind && promptSchema &&
        promptFocusRegion(*semantic.sections().promptStatus.activeKind) ==
            PromptRegion::Footer) {
        const auto retainPrompt = [&](const auto& self,
                                      const UiNode& node) -> void {
            retained.insert(node.id);
            if (const auto* container =
                    std::get_if<UiContainer>(&node.content)) {
                for (const auto& child : container->children) {
                    self(self, child);
                }
            }
        };
        retainPrompt(retainPrompt, *promptSchema);
    }
    std::erase_if(result.tree->nodes, [&](const SolvedGridNode& node) {
        return !retained.contains(node.id);
    });
    if (semantic.sections().promptStatus.activeKind && promptSchema &&
        promptFocusRegion(*semantic.sections().promptStatus.activeKind) ==
            PromptRegion::Footer) {
        const auto validatePrompt = [&](const auto& self,
                                        const UiNode& schemaNode) -> bool {
            const auto* node = result.tree->find(schemaNode.id);
            if (!node) return false;
            if (const auto* leaf =
                    std::get_if<UiLeaf>(&schemaNode.content)) {
                return node->widget &&
                       node->widget->id == leaf->widget.id &&
                       node->widget->kind == leaf->widget.kind &&
                       (leaf->widget.kind != WidgetKind::Label ||
                        node->rect.width > 0);
            }
            const auto* container =
                std::get_if<UiContainer>(&schemaNode.content);
            if (!container) return true;
            return std::all_of(
                container->children.begin(), container->children.end(),
                [&](const UiNode& child) { return self(self, child); });
        };
        if (!validatePrompt(validatePrompt, *promptSchema)) {
            return {std::nullopt,
                    "prompt backing does not correspond to UI nodes"};
        }
    }
    if (semantic.sections().noticeView &&
        !result.tree->find(UiNodeId{std::string{kNoticeNodeId}})) {
        return {std::nullopt,
                "notice backing has no solved UI node"};
    }
    if (!semantic.sections().externalModification.files.empty() &&
        !result.tree->find(UiNodeId{std::string{kExternalModNodeId}})) {
        return {std::nullopt,
                "external-modification backing has no solved UI node"};
    }
    if (semantic.sections().palette.activePicker &&
        !result.tree->find(
            UiNodeId{std::string{kFindResultsViewportNodeId}})) {
        return {std::nullopt,
                "palette backing has no solved UI node"};
    }
    return result;
}

SolvedGridTree requireFrameLayout(
    const SessionSnapshot& semantic,
    GridSize dimensions, const Style& style) {
    auto result = trySolveFrameLayout(semantic, dimensions, style);
    if (!result.tree) {
        throw std::logic_error(
            "GridFrame: semantic UI frame cannot be solved: " + result.error);
    }
    return std::move(*result.tree);
}

}  // namespace

GridFrame::GridFrame(SessionSnapshot semantic,
                     GridProjection projection, GridBasis basis,
                     PaletteReport palette)
    : semantic_{std::move(semantic)},
      projection_{std::move(projection)},
      layout_{requireFrameLayout(
          semantic_,
          {static_cast<int>(projection_.viewport.dimensions.columns),
           static_cast<int>(projection_.viewport.dimensions.rows)},
          projection_.style)},
      palette_{std::move(palette)},
      basis_{basis} {
    if (auto error = solveChrome()) {
        throw std::logic_error("GridFrame: " + *error);
    }
    solvePanel(0, false);
    solveDocument(nullptr);
}

GridFrame::GridFrame(SessionSnapshot semantic, GridProjection projection,
                     SolvedGridTree layout, GridBasis basis,
                     PaletteReport palette)
    : semantic_{std::move(semantic)},
      projection_{std::move(projection)},
      layout_{std::move(layout)},
      palette_{std::move(palette)},
      basis_{basis} {}

std::optional<std::string> GridFrame::solveChrome() {
    const auto& schema = semantic_.sections().uiFrame.schema();
    const auto& state = semantic_.sections().uiFrame.state();
    if (schema.generation != state.generation) {
        return "chrome schema and state generations differ";
    }
    const auto solve = [&](std::string_view id, SemanticRole role,
                           std::optional<SolvedChromeSurface>& output,
                           const StatusViewState* status,
                           const PromptInputProjection* input)
        -> std::optional<std::string> {
        const auto* solved = layout_.find(UiNodeId{std::string{id}});
        if (!solved) {
            output.reset();
            return std::nullopt;
        }
        const auto* subtree = findUiNode(schema.root, id);
        if (!subtree) {
            return "solved " + std::string{id} +
                   " band has no schema subtree";
        }
        SolvedChromeSurface surface;
        const auto lowered =
            solveUiChromeRegion(*subtree, solved->rect, role,
                                projection_.style, schema.generation,
                                state, surface,
                                status, input);
        if (!lowered.ok()) {
            return std::string{id} + " chrome lowering failed: " +
                   *lowered.error;
        }
        output = std::move(surface);
        return std::nullopt;
    };

    PromptInputProjection input;
    const PromptInputProjection* inputPtr = nullptr;
    if (semantic_.sections().palette.activePicker &&
        layout_.find(
            UiNodeId{std::string{kHeaderPromptInputNodeId}})) {
        input = {true, palette_.query, palette_.ghost};
        inputPtr = &input;
    }
    if (auto error =
            solve(kHeaderNodeId, SemanticRole::Header, header_, nullptr,
                  inputPtr)) {
        return error;
    }
    return solve(kFooterNodeId, SemanticRole::Footer, footer_,
                 &semantic_.sections().promptStatus.status, nullptr);
}

void GridFrame::solvePanel(std::uint32_t treeFirstVisible,
                           bool revealTreeSelection) {
    const auto* node =
        layout_.find(UiNodeId{std::string{kPanelNodeId}});
    if (!node) {
        panel_.reset();
        return;
    }
    panel_ = solvePanelSurface(semantic_.sections().tree, *node,
                               treeFirstVisible, revealTreeSelection,
                               projection_.style);
}

void GridFrame::solveDocument(const ShellState* shell) {
    const auto* node =
        layout_.find(UiNodeId{std::string{kDocumentViewportNodeId}});
    if (!node) {
        document_.reset();
        return;
    }
    bool lineNumbers = false;
    if (const auto* setting =
            semantic_.sections().settings.find(SettingKey::LineNumbers)) {
        if (const auto* enabled =
                std::get_if<bool>(&setting->effective.value)) {
            lineNumbers = *enabled;
        }
    }
    const auto lines = static_cast<std::uint32_t>(
        semantic_.sections().syntax.indentation().size());
    const auto paneFrames =
        shell ? shell->paneFrames(node->rect)
              : std::vector<PaneFrame>{{PaneId{0}, node->rect}};
    const auto activePane = shell ? shell->activePane() : PaneId{0};
    document_ = solveDocumentSurface(
        *node, paneFrames, activePane, lineNumbers, lines,
        projection_.style.dimensions);
}

std::optional<GridFrame> GridFrame::fromSemantic(
    SessionSnapshot semantic, Style style, ViewportDimensions dimensions,
    GridBasis basis, PaletteReport palette,
    std::uint32_t treeFirstVisible, bool revealTreeSelection,
    const ShellState& shell, SelectionNavigation navigation) {
    const GridSize gridSize{static_cast<int>(dimensions.columns),
                            static_cast<int>(dimensions.rows)};
    auto result = trySolveFrameLayout(semantic, gridSize, style);
    if (!result.tree) return std::nullopt;
    GridFrame frame{
        std::move(semantic),
        GridProjection{ViewportViewState{dimensions}, std::move(style),
                       navigation},
        std::move(*result.tree), basis, std::move(palette)};
    if (frame.solveChrome()) return std::nullopt;
    frame.solvePanel(treeFirstVisible, revealTreeSelection);
    frame.solveDocument(&shell);
    return frame;
}

void GridFrame::finalizeViewport(ViewportViewState viewport,
                                 SelectionNavigation navigation) {
    projection_.viewport = std::move(viewport);
    projection_.selectionNav = navigation;
}

GridPresenter::GridPresenter(ViewId viewId)
    : viewId_{viewId},
      state_{std::make_unique<detail::GridProjectionState>()} {}
GridPresenter::~GridPresenter() = default;
GridPresenter::GridPresenter(GridPresenter&&) noexcept = default;
GridPresenter& GridPresenter::operator=(GridPresenter&&) noexcept = default;

std::optional<GridFrame> GridPresenter::project(
    EditorSession& session, ClientId client, GridPresentationRequest request) {
    auto& state = *state_;
    constexpr int kProjectionAttempts = 3;
    for (int attempt = 0; attempt < kProjectionAttempts; ++attempt) {
        auto captured = session.capturePresentation(
            client, viewId_, request.palette);
        if (!captured) return std::nullopt;
        if (state.adoptedRevision &&
            captured->semantic.revision() < *state.adoptedRevision) {
            return std::nullopt;
        }
        const auto& sections = captured->semantic.sections();
        auto proposedNavigation = state.navigation;
        bool confirmedSelection = false;
        if (state.pendingSelection &&
            sections.tabs.active == state.pendingSelection->activeTab &&
            sections.document.revision ==
                state.pendingSelection->documentRevision &&
            sections.selection == state.pendingSelection->expected) {
            proposedNavigation = state.pendingSelection->navigation;
            confirmedSelection = true;
        }
        const bool documentChanged =
            !state.documentRevision ||
            *state.documentRevision != sections.document.revision ||
            !state.findGeneration ||
            *state.findGeneration != sections.findReplace.generation ||
            state.activeTab != sections.tabs.active ||
            !state.selections || *state.selections != sections.selection;
        const auto* treeProvider = activeTreeProvider(sections.tree);
        const auto selectedTree =
            treeProvider == nullptr ? std::optional<TreeNodeId>{}
                                    : treeProvider->selected;
        const auto documentRevision = sections.document.revision;
        const auto findGeneration = sections.findReplace.generation;
        const auto activeTab = sections.tabs.active;
        const auto selections = sections.selection;
        const auto revision = captured->semantic.revision();
        const auto nextGeneration = state.generation + 1;
        auto frame = GridFrame::fromSemantic(
            std::move(captured->semantic), std::move(captured->style),
            request.dimensions,
            GridBasis{viewId_, revision, nextGeneration}, request.palette,
            state.treeFirstVisible,
            selectedTree != state.treeSelection || !state.panelVisible,
            state.shell, proposedNavigation);
        if (!frame) return std::nullopt;
        const auto& frameSections = frame->sections();
        if (frameSections.palette.activePicker && frame->palette_.rows.empty() &&
            frame->palette_.query.empty()) {
            const auto* candidates = frameSections.palette.candidatesFor(
               frameSections.palette.activePicker->mode);
            const auto* pickerNode = frame->layout().find(
               UiNodeId{std::string{kFindResultsViewportNodeId}});
            if (candidates && !candidates->empty() && pickerNode) {
               PaletteWindowState window;
               window.paneRows = static_cast<std::uint32_t>(
                   std::max(pickerNode->rect.height, 1));
               frame->palette_ =
                   PaletteSearcher{}.report(*candidates, window);
            }
        }

        std::uint32_t paneRows = 1;
        std::uint32_t paneColumns = 1;
        if (frame->document() && !frame->document()->panes.empty()) {
            const auto& pane =
                frame->document()
                    ->panes[frame->document()->activePaneIndex]
                    .content;
            paneRows = static_cast<std::uint32_t>(std::max(pane.height, 1));
            paneColumns =
                static_cast<std::uint32_t>(std::max(pane.width, 1));
        }
        auto viewport = session.projectViewport(
            {client, viewId_, revision, request.dimensions, paneRows,
             paneColumns, proposedNavigation,
             documentChanged && !confirmedSelection},
            state.viewport);
        if (!viewport) continue;

        frame->finalizeViewport(std::move(viewport->viewport),
                                viewport->navigation);
        state.pendingSelection.reset();
        state.documentRevision = documentRevision;
        state.findGeneration = findGeneration;
        state.activeTab = activeTab;
        state.selections = selections;
        state.adoptedRevision = revision;
        state.generation = nextGeneration;
        state.navigation = viewport->navigation;
        state.navigation.firstVisualRow =
            frame->presentation().viewport.firstVisualRow;
        state.navigation.firstVisualColumn =
            frame->presentation().viewport.firstVisualColumn;
        if (frame->panel()) {
            state.treeFirstVisible = frame->panel()->firstVisible;
            state.panelVisible = true;
        } else {
            state.panelVisible = false;
        }
        state.treeSelection = selectedTree;
        return frame;
    }
    return std::nullopt;
}

GridActionResult GridPresenter::apply(ViewActionRequest const& request,
                                      GridFrame const& frame) {
    auto& state = *state_;
    const auto basis = frame.basis();
    if (request.viewId != viewId_ || basis.viewId != viewId_ ||
        request.semanticRevision != basis.semanticRevision ||
        !state.adoptedRevision ||
        basis.semanticRevision != *state.adoptedRevision ||
        basis.presentationGeneration != state.generation) {
        return {GridActionStatus::Rejected, std::nullopt,
                "view action basis is stale"};
    }
    const auto* presentation = &frame.presentation();

    bool supported = true;
    bool changed = false;
    bool pausesFollow = false;
    bool focusesEditor = false;
    bool reportsNavigation = false;
    std::optional<ClientInput> resolvedInput;
    auto resolveVisualSelection = [&](SelectionCommand command,
                                      bool primaryOnly) {
        auto inputSelections = frame.sections().selection;
        if (primaryOnly) {
            inputSelections =
                SelectionSet{{frame.sections().selection.primary()}};
        }
        auto before = SelectionViewState{
            std::move(inputSelections),
            state.navigation.firstVisualRow,
            state.navigation.firstVisualColumn,
            state.navigation.desiredCell};
        const DiffFileView* activeDiff = nullptr;
        if (frame.sections().document.diffFileIdentity) {
            const auto found = std::ranges::find(
                frame.sections().diff.files,
                *frame.sections().document.diffFileIdentity,
                [](const DiffFileView& file) {
                    return file.id.value();
                });
            if (found != frame.sections().diff.files.end()) {
                activeDiff = &*found;
            }
        }
        const auto* activeDocumentPane = frame.document()
            ? &frame.document()->panes[frame.document()->activePaneIndex]
            : nullptr;
        const auto paneColumns = activeDocumentPane
            ? static_cast<std::uint32_t>(
                  std::max(activeDocumentPane->content.width, 1))
            : presentation->viewport.dimensions.columns;
        const auto paneRows = activeDocumentPane
            ? static_cast<std::uint32_t>(
                  std::max(activeDocumentPane->content.height, 1))
            : std::max(presentation->viewport.scrollbar.viewportRows,
                       std::uint32_t{1});
        const auto* wordWrapSetting =
            frame.sections().settings.find(SettingKey::WordWrap);
        const auto* wordWrap =
            wordWrapSetting
                ? std::get_if<bool>(&wordWrapSetting->effective.value)
                : nullptr;
        auto result = SelectionNavigator{}.apply(
            frame.sections().document.text, before, command,
            {paneColumns, paneRows}, {}, {}, 4,
            wordWrap != nullptr && *wordWrap, activeDiff);
        if (!result.accepted()) return false;

        auto resolved = result.delta.replacement.value_or(before);
        auto resolvedSelections = resolved.selections;
        if (primaryOnly) {
            auto selections = frame.sections().selection.items();
            selections.back() = resolved.selections.primary();
            resolvedSelections = SelectionSet{std::move(selections)};
        }
        std::vector<ResolvedSelectionRange> ranges;
        ranges.reserve(resolvedSelections.items().size());
        for (const auto& selection : resolvedSelections.items()) {
            ranges.push_back(
                {selection.anchor.byteOffset, selection.active.byteOffset});
        }
        if (primaryOnly) {
            resolvedInput = DocumentPointerInput{
                SemanticInputBasis{basis.semanticRevision},
                resolvedSelections.primary().active.byteOffset,
                false,
                false,
                InputPointerButton::Primary,
                InputPointerPhase::Move};
        } else {
            resolvedInput = ResolvedSelectionInput{
                SemanticInputBasis{basis.semanticRevision},
                *frame.sections().tabs.active,
                frame.sections().document.revision, std::move(ranges)};
        }
        if (resolvedSelections != frame.sections().selection) {
            state.pendingSelection =
                detail::GridProjectionState::PendingSelection{
                    *frame.sections().tabs.active,
                    frame.sections().document.revision,
                    std::move(resolvedSelections),
                    {resolved.firstVisualRow,
                     resolved.firstVisualColumn,
                     resolved.desiredCell}};
        }
        return true;
    };
    std::visit(
        [&](auto const& action) {
            using Action = std::decay_t<decltype(action)>;
            if constexpr (std::same_as<Action, ViewScrollLines>) {
                if (action.rows == 0) {
                    supported = false;
                    return;
                }
                if (action.target == ViewScrollTarget::Document) {
                    pausesFollow = true;
                    ScrollOffset offset{state.navigation.firstVisualRow};
                    offset.byLines(
                        action.rows, presentation->viewport.totalVisualRows,
                        presentation->viewport.scrollbar.viewportRows);
                    changed =
                        offset.firstVisible() !=
                        state.navigation.firstVisualRow;
                    state.navigation.firstVisualRow =
                        offset.firstVisible();
                } else if (frame.panel()) {
                    auto const& tree = *frame.panel();
                    ScrollOffset offset{state.treeFirstVisible};
                    offset.byLines(action.rows, tree.scrollbar.totalRows,
                                   tree.scrollbar.viewportRows);
                    changed =
                        offset.firstVisible() != state.treeFirstVisible;
                    state.treeFirstVisible = offset.firstVisible();
                }
            } else if constexpr (std::same_as<Action, ViewScrollPages>) {
                pausesFollow = true;
                if (action.pages == 0) {
                    supported = false;
                    return;
                }
                ScrollOffset offset{state.navigation.firstVisualRow};
                offset.byPages(
                    action.pages, presentation->viewport.totalVisualRows,
                    presentation->viewport.scrollbar.viewportRows);
                changed = offset.firstVisible() !=
                          state.navigation.firstVisualRow;
                state.navigation.firstVisualRow = offset.firstVisible();
            } else if constexpr (std::same_as<Action,
                                              ViewScrollFraction>) {
                if (action.denominator == 0 ||
                    action.numerator > action.denominator) {
                    supported = false;
                    return;
                }
                if (action.target == ViewScrollTarget::Document) {
                    pausesFollow = true;
                    ScrollOffset offset{state.navigation.firstVisualRow};
                    offset.toFraction(
                        action.numerator, action.denominator,
                        presentation->viewport.totalVisualRows,
                        presentation->viewport.scrollbar.viewportRows);
                    changed =
                        offset.firstVisible() !=
                        state.navigation.firstVisualRow;
                    state.navigation.firstVisualRow =
                        offset.firstVisible();
                } else if (frame.panel()) {
                    auto const& tree = *frame.panel();
                    ScrollOffset offset{state.treeFirstVisible};
                    offset.toFraction(action.numerator, action.denominator,
                                      tree.scrollbar.totalRows,
                                      tree.scrollbar.viewportRows);
                    changed =
                        offset.firstVisible() != state.treeFirstVisible;
                    state.treeFirstVisible = offset.firstVisible();
                }
            } else if constexpr (std::same_as<Action, RevealSelection> ||
                                 std::same_as<Action, CenterSelection>) {
                pausesFollow = true;
                auto const& selections = frame.sections().selection;
                RowProjection rows{presentation->viewport.rowProjection};
                const auto selected =
                    rows.visualRowForPosition(selections.primary().active);
                const auto viewportRows =
                    presentation->viewport.scrollbar.viewportRows;
                auto next = state.navigation.firstVisualRow;
                if constexpr (std::same_as<Action, RevealSelection>) {
                    ScrollOffset offset{next};
                    offset.revealSelection(
                        selected, presentation->viewport.totalVisualRows,
                        viewportRows);
                    next = offset.firstVisible();
                } else {
                    const auto centered =
                        selected > viewportRows / 2
                            ? selected - viewportRows / 2
                            : 0;
                    const auto maximum =
                        presentation->viewport.totalVisualRows > viewportRows
                            ? presentation->viewport.totalVisualRows -
                                  viewportRows
                            : 0;
                    next = std::min(centered, maximum);
                }
                changed = next != state.navigation.firstVisualRow;
                state.navigation.firstVisualRow = next;
            } else if constexpr (std::same_as<Action,
                                              MoveVisualSelection>) {
                if (!frame.sections().tabs.active) {
                    supported = false;
                    return;
                }
                SelectionCommand command = SelectionCommand::CursorLineDown;
                switch (action.direction) {
                    case VisualSelectionDirection::LineUp:
                        command = action.extend
                                      ? SelectionCommand::SelectLineUp
                                      : SelectionCommand::CursorLineUp;
                        break;
                    case VisualSelectionDirection::LineDown:
                        command = action.extend
                                      ? SelectionCommand::SelectLineDown
                                      : SelectionCommand::CursorLineDown;
                        break;
                    case VisualSelectionDirection::PageUp:
                        command = action.extend
                                      ? SelectionCommand::SelectPageUp
                                      : SelectionCommand::CursorPageUp;
                        break;
                    case VisualSelectionDirection::PageDown:
                        command = action.extend
                                      ? SelectionCommand::SelectPageDown
                                      : SelectionCommand::CursorPageDown;
                        break;
                }
                if (!resolveVisualSelection(command, false)) {
                    supported = false;
                }
                return;
            } else if constexpr (std::same_as<Action,
                                               ContinuePointerEdge>) {
                if (!frame.sections().tabs.active ||
                    !resolveVisualSelection(
                        action.direction == PointerEdgeDirection::Before
                            ? SelectionCommand::SelectLineUp
                            : SelectionCommand::SelectLineDown,
                        true)) {
                    supported = false;
                }
                return;
            } else if constexpr (std::same_as<Action, SplitPane>) {
                (void)state.shell.splitActive(action.axis);
            } else if constexpr (std::same_as<Action, ClosePane>) {
                (void)state.shell.closeActivePane();
            } else if constexpr (std::same_as<Action, CyclePane>) {
                pausesFollow = true;
                reportsNavigation = true;
                if (action.direction == PaneCycleDirection::Next) {
                    state.shell.nextPane();
                } else {
                    state.shell.previousPane();
                }
            } else if constexpr (std::same_as<Action, FocusPane>) {
                pausesFollow = true;
                std::vector<PaneFrame> panes;
                if (frame.document()) {
                    panes.reserve(frame.document()->panes.size());
                    for (const auto& pane : frame.document()->panes) {
                        panes.push_back({pane.id, pane.frame});
                    }
                }
                focusesEditor =
                    state.shell.focusPane(action.direction, panes);
                reportsNavigation = !focusesEditor;
            } else {
                supported = false;
            }
        },
        request.action);

    if (!supported) {
        return {GridActionStatus::Rejected, std::nullopt,
                "view action is not supported by this presenter"};
    }
    ++state.generation;
    if (resolvedInput) {
        return {GridActionStatus::TransitionRequired,
                std::move(*resolvedInput), {}};
    }
    if (focusesEditor) {
        return {GridActionStatus::TransitionRequired,
                ClientInput{ResolvedPaneFocusInput{
                    SemanticInputBasis{basis.semanticRevision}}},
                {}};
    }
    if (reportsNavigation ||
        (pausesFollow &&
         frame.sections().followEdits.mode == FollowMode::Following)) {
        return {GridActionStatus::TransitionRequired,
                ClientInput{ViewNavigationInput{
                    SemanticInputBasis{basis.semanticRevision}}},
                {}};
    }
    return {GridActionStatus::Applied, std::nullopt, {}};
}

}  // namespace ssg
