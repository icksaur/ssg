#include <ssg/GridPresenter.h>

#include <ssg/Editor.h>

#include <algorithm>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace ssg {

struct GridPresenter::State {
    LineLayoutCache lineCache;
    SelectionNavigation navigation;
    std::uint32_t treeFirstVisible = 0;
    std::uint64_t generation = 0;
    std::optional<std::uint64_t> documentRevision;
    std::optional<std::uint64_t> findGeneration;
    std::optional<TabId> activeTab;
    std::optional<SelectionSet> selections;
    std::optional<TreeNodeId> treeSelection;
    bool panelVisible = false;
    std::optional<std::uint64_t> wrappedDocumentRevision;
    std::optional<TabId> wrappedTab;
    std::vector<CellRun> wrappedCellRuns;
    struct PendingSelection {
        TabId activeTab;
        std::uint64_t documentRevision;
        SelectionSet expected;
        SelectionNavigation navigation;
    };
    std::optional<PendingSelection> pendingSelection;
};

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

std::vector<GridIntrinsicSize> intrinsicSizes(
    const UiNode& root, const ExternalModificationViewState& external) {
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
            size.size = measureExternalModificationSurface(external);
            break;
        }
    }
    return sizes;
}

SolveUiFrameResult solveFrameLayout(
    const UiSchema& schema, const PromptStatusViewState& promptStatus,
    bool noticePresent, const ExternalModificationViewState& external,
    bool palettePresent, GridSize dimensions, const Style& style) {
    if (dimensions.columns <= 0 || dimensions.rows <= 0 ||
        dimensions.columns < static_cast<int>(style.dimensions.minimumColumns) ||
        dimensions.rows < static_cast<int>(style.dimensions.minimumRows)) {
        return {SolvedGridTree{}, {}};
    }
    const auto validation = validateUiSchema(schema);
    if (!validation.ok()) {
        return {std::nullopt, "invalid UI schema: " + *validation.error};
    }

    auto sizes = intrinsicSizes(schema.root, external);
    SolveUiFrameResult result;
    for (;;) {
        result = solveUiFrame(
            schema, sizes, {0, 0, dimensions.columns, dimensions.rows});
        const auto* content =
            result.tree
                ? (result.tree->find(UiNodeId{std::string{kEditorNodeId}})
                       ? result.tree->find(UiNodeId{std::string{kEditorNodeId}})
                       : result.tree->find(UiNodeId{
                             std::string{kFindResultsViewportNodeId}}))
                : nullptr;
        if (result.tree && (!content || content->rect.height > 0)) break;
        auto externalSize = std::find_if(
            sizes.begin(), sizes.end(), [](const GridIntrinsicSize& size) {
                return size.id == UiNodeId{std::string{kExternalModNodeId}};
            });
        if (externalSize == sizes.end() || externalSize->size.rows <= 1) break;
        --externalSize->size.rows;
    }
    if (!result.tree) {
        if (result.error == "UI frame does not fit grid bounds") {
            return {SolvedGridTree{}, {}};
        }
        return result;
    }

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
    const auto* promptSchema = findUiNode(schema.root, kFooterPromptNodeId);
    if (promptStatus.activeKind && promptSchema &&
        promptFocusRegion(*promptStatus.activeKind) == PromptRegion::Footer) {
        const auto retainPrompt = [&](const auto& self,
                                      const UiNode& node) -> void {
            retained.insert(node.id);
            if (const auto* container = std::get_if<UiContainer>(&node.content)) {
                for (const auto& child : container->children) self(self, child);
            }
        };
        retainPrompt(retainPrompt, *promptSchema);
    }
    std::erase_if(result.tree->nodes, [&](const SolvedGridNode& node) {
        return !retained.contains(node.id);
    });
    if (promptStatus.activeKind && promptSchema &&
        promptFocusRegion(*promptStatus.activeKind) == PromptRegion::Footer) {
        const auto validPrompt = [&](const auto& self,
                                     const UiNode& schemaNode) -> bool {
            const auto* node = result.tree->find(schemaNode.id);
            if (!node) return false;
            if (const auto* leaf = std::get_if<UiLeaf>(&schemaNode.content)) {
                return node->widget && node->widget->id == leaf->widget.id &&
                       node->widget->kind == leaf->widget.kind &&
                       (leaf->widget.kind != WidgetKind::Label ||
                        node->rect.width > 0);
            }
            const auto* container = std::get_if<UiContainer>(&schemaNode.content);
            if (!container) return true;
            return std::all_of(
                container->children.begin(), container->children.end(),
                [&](const UiNode& child) { return self(self, child); });
        };
        if (!validPrompt(validPrompt, *promptSchema)) {
            return {std::nullopt,
                    "prompt backing does not correspond to UI nodes"};
        }
    }
    if (noticePresent &&
        !result.tree->find(UiNodeId{std::string{kNoticeNodeId}})) {
        return {std::nullopt, "notice backing has no solved UI node"};
    }
    if (!external.files.empty() &&
        !result.tree->find(UiNodeId{std::string{kExternalModNodeId}})) {
        return {std::nullopt,
                "external-modification backing has no solved UI node"};
    }
    if (palettePresent &&
        !result.tree->find(
            UiNodeId{std::string{kFindResultsViewportNodeId}})) {
        return {std::nullopt, "palette backing has no solved UI node"};
    }
    return result;
}

bool solveUiRegions(
    const SolvedGridTree& layout, const UiSchema& schema,
    const PromptStatusViewState& promptStatus,
    const PaletteViewState& paletteView, const PaletteReport& palette,
    const Style& style, std::optional<SolvedUiRegion>& header,
    std::optional<SolvedUiRegion>& footer) {
    const auto solve = [&](std::string_view id, SemanticRole role,
                           std::optional<SolvedUiRegion>& output,
                           const StatusViewState* status,
                           const PromptInputProjection* input) {
        const auto* solved = layout.find(UiNodeId{std::string{id}});
        if (!solved) {
            output.reset();
            return true;
        }
        const auto* subtree = findUiNode(schema.root, id);
        if (!subtree) return false;
        SolvedUiRegion surface;
        if (!projectUiRegion(*subtree, solved->rect, role, style, surface,
                             status, input)
                 .ok()) {
            return false;
        }
        output = std::move(surface);
        return true;
    };

    PromptInputProjection input;
    const PromptInputProjection* inputPtr = nullptr;
    if (paletteView.activePicker &&
        layout.find(UiNodeId{std::string{kHeaderPromptInputNodeId}})) {
        input = {true, palette.query, palette.ghost};
        inputPtr = &input;
    }
    return solve(kHeaderNodeId, SemanticRole::Header, header, nullptr, inputPtr) &&
           solve(kFooterNodeId, SemanticRole::Footer, footer,
                 &promptStatus.status, nullptr);
}

}  // namespace

GridPresenter::GridPresenter()
    : state_{std::make_unique<State>()} {}
GridPresenter::~GridPresenter() = default;
GridPresenter::GridPresenter(GridPresenter&&) noexcept = default;
GridPresenter& GridPresenter::operator=(GridPresenter&&) noexcept = default;

std::optional<GridPresentation> GridPresenter::project(
    Editor& runtime, GridPresentationRequest request) {
    if (runtime.commands.dispatchInProgress()) {
        throw std::logic_error{"a view cannot be presented during dispatch"};
    }
    std::lock_guard operationLock{runtime.operationMutex};
    const auto* activeDocument = runtime.activeDocument();
    std::string documentText = runtime.activeText();
    std::uint64_t documentRevision =
        activeDocument ? activeDocument->revision() : 0;
    std::optional<std::string> diffFileIdentity;
    if (const auto* tab = runtime.activeTabState();
        tab && tab->kind == TabKind::LiveDiff &&
        !tab->contentIdentity.empty()) {
        diffFileIdentity = tab->contentIdentity;
        documentRevision = runtime.diff.viewState().revision;
    }
    auto style = runtime.style;
    auto panes = runtime.paneTopology;
    auto selections = runtime.selection.selections;
    auto findReplace = runtime.findReplace.viewState();
    auto diff = runtime.diff.viewState();
    auto lspSync = runtime.lspSync;
    auto syntax = runtime.activeSyntaxView();
    auto theme = runtime.theme;
    auto uiTree = runtime.projectedUiTree();
    auto tabs = runtime.tabs.viewState();
    auto notice = runtime.noticeView();
    auto externalModification = runtime.external.viewState();
    auto followMode = runtime.follow.viewState().mode;
    auto promptStatus = runtime.promptStatusView();
    auto paletteView = runtime.paletteView();
    auto tree = runtime.treeView();
    auto clipboardWrite = runtime.clipboard.viewState().systemWrite;
    const bool wordWrap = runtime.wordWrap;
    const bool lineNumbers = runtime.lineNumbers;

    auto& state = *state_;
    auto proposedNavigation = state.navigation;
    bool confirmedSelection = false;
    if (state.pendingSelection &&
        tabs.active == state.pendingSelection->activeTab &&
        documentRevision == state.pendingSelection->documentRevision &&
        selections == state.pendingSelection->expected) {
        proposedNavigation = state.pendingSelection->navigation;
        confirmedSelection = true;
    }
    const bool documentChanged =
        !state.documentRevision || *state.documentRevision != documentRevision ||
        !state.findGeneration ||
        *state.findGeneration != findReplace.generation ||
        state.activeTab != tabs.active || !state.selections ||
        *state.selections != selections;
    const auto* treeProvider = activeTreeProvider(tree);
    const auto selectedTree = treeProvider == nullptr
                                  ? std::optional<TreeNodeId>{}
                                  : treeProvider->selected;

    const GridSize gridSize{
        static_cast<int>(request.dimensions.columns),
        static_cast<int>(request.dimensions.rows)};
    auto solved = solveFrameLayout(
        uiTree, promptStatus, notice.has_value(), externalModification,
        paletteView.activePicker.has_value(), gridSize, style);
    if (!solved.tree) return std::nullopt;
    auto layout = std::move(*solved.tree);

    auto palette = request.palette;
    if (paletteView.activePicker && palette.rows.empty() &&
        palette.query.empty()) {
        const auto* candidates =
            paletteView.candidatesFor(paletteView.activePicker->mode);
        const auto* pickerNode = layout.find(
            UiNodeId{std::string{kFindResultsViewportNodeId}});
        if (candidates && !candidates->empty() && pickerNode) {
            PaletteWindowState window;
            window.paneRows = static_cast<std::uint32_t>(
                std::max(pickerNode->rect.height, 1));
            palette = buildPaletteReport(*candidates, window);
        }
    }

    std::optional<SolvedUiRegion> header;
    std::optional<SolvedUiRegion> footer;
    if (!solveUiRegions(layout, uiTree, promptStatus, paletteView, palette,
                        style, header, footer)) {
        return std::nullopt;
    }
    std::optional<SolvedPanelSurface> panel;
    if (const auto* node =
            layout.find(UiNodeId{std::string{kPanelNodeId}})) {
        panel = solvePanelSurface(
            tree, *node, state.treeFirstVisible,
            selectedTree != state.treeSelection || !state.panelVisible, style);
    }
    std::optional<SolvedDocumentSurface> document;
    if (const auto* node =
            layout.find(UiNodeId{std::string{kDocumentViewportNodeId}})) {
        document = solveDocumentSurface(
            *node, panes, lineNumbers,
            static_cast<std::uint32_t>(syntax.indentation().size()),
            style.dimensions);
    }

    std::uint32_t paneRows = 1;
    std::uint32_t paneColumns = 1;
    if (document && !document->panes.empty()) {
        const auto& content = document->panes[document->activePaneIndex].content;
        paneRows = static_cast<std::uint32_t>(std::max(content.height, 1));
        paneColumns = static_cast<std::uint32_t>(std::max(content.width, 1));
    }
    auto navigation = proposedNavigation;
    const auto activeDiff = diff.fileForIdentity(diffFileIdentity);
    const DiffFileView* activeDiffFile =
        activeDiff ? &activeDiff->get() : nullptr;
    if (documentChanged && !confirmedSelection) {
        const ViewportDimensions revealViewport{
            std::max(paneColumns, std::uint32_t{1}),
            std::max(paneRows, std::uint32_t{1})};
        auto selectionView = SelectionViewState{
            selections, proposedNavigation.firstVisualRow,
            proposedNavigation.firstVisualColumn,
            proposedNavigation.desiredCell};
        auto revealed = navigateSelection(
            documentText, selectionView, SelectionCommand::ViewRevealCaret,
            revealViewport, {}, {}, 4, wordWrap, activeDiffFile);
        if (revealed.accepted() && revealed.delta.replacement) {
            navigation = {revealed.delta.replacement->firstVisualRow,
                          revealed.delta.replacement->firstVisualColumn,
                          revealed.delta.replacement->desiredCell};
        }
    }
    const ViewportDimensions content{
        std::max<std::uint32_t>(
            1, std::min(paneColumns, request.dimensions.columns)),
        std::max<std::uint32_t>(
            1, std::min(paneRows, request.dimensions.rows))};
    ViewportViewState viewport = [&] {
        if (!wordWrap) {
            return computeUnwrappedViewport(
                documentText, content, navigation.firstVisualRow,
                navigation.firstVisualColumn, 4, activeDiffFile,
                request.dimensions, &state.lineCache);
        }
        if (state.wrappedDocumentRevision != documentRevision ||
            state.wrappedTab != tabs.active) {
            state.wrappedCellRuns.clear();
            std::size_t start = 0;
            while (start <= documentText.size()) {
                const auto end = documentText.find('\n', start);
                state.wrappedCellRuns.push_back(computeCellRun(
                    documentText.substr(start, end == std::string::npos
                                                   ? end
                                                   : end - start),
                    4));
                if (end == std::string::npos) break;
                start = end + 1;
            }
            state.wrappedDocumentRevision = documentRevision;
            state.wrappedTab = tabs.active;
        }
        return computeViewport(
            state.wrappedCellRuns, content, navigation.firstVisualRow,
            activeDiffFile, request.dimensions);
    }();

    state.pendingSelection.reset();
    state.documentRevision = documentRevision;
    state.findGeneration = findReplace.generation;
    state.activeTab = tabs.active;
    state.selections = selections;
    ++state.generation;
    state.navigation = navigation;
    state.navigation.firstVisualRow = viewport.firstVisualRow;
    state.navigation.firstVisualColumn = viewport.firstVisualColumn;
    if (panel) {
        state.treeFirstVisible = panel->firstVisible;
        state.panelVisible = true;
    } else {
        state.panelVisible = false;
    }
    state.treeSelection = selectedTree;

    return GridPresentation{
        .presentationGeneration = state.generation,
        .viewport = std::move(viewport),
        .style = std::move(style),
        .layout = std::move(layout),
        .palette = std::move(palette),
        .header = std::move(header),
        .footer = std::move(footer),
        .panel = std::move(panel),
        .document = std::move(document),
        .documentText = std::move(documentText),
        .documentRevision = documentRevision,
        .diffFileIdentity = std::move(diffFileIdentity),
        .selections = std::move(selections),
        .findReplace = std::move(findReplace),
        .diff = std::move(diff),
        .lspSync = std::move(lspSync),
        .syntax = std::move(syntax),
        .theme = std::move(theme),
        .uiTree = std::move(uiTree),
        .tabs = std::move(tabs),
        .notice = std::move(notice),
        .externalModification = std::move(externalModification),
        .followMode = followMode,
        .promptStatus = std::move(promptStatus),
        .paletteView = std::move(paletteView),
        .clipboardWrite = std::move(clipboardWrite),
        .wordWrap = wordWrap,
    };
}

ViewActionResult GridPresenter::apply(ViewAction const& request,
                                      GridPresentation const& frame) {
    auto& state = *state_;
    if (frame.presentationGeneration != state.generation) {
        return {ViewActionStatus::Rejected, std::nullopt,
                "view action presentation is stale"};
    }
    const auto* presentation = &frame;
    const DiffFileView* activeDiff = nullptr;
    if (frame.diffFileIdentity) {
        const auto found = std::ranges::find(
            frame.diff.files, *frame.diffFileIdentity,
            [](const DiffFileView& file) { return file.id.value(); });
        if (found != frame.diff.files.end()) activeDiff = &*found;
    }
    const auto* activeDocumentPane =
        frame.document
            ? &frame.document->panes[frame.document->activePaneIndex]
            : nullptr;
    const auto paneColumns =
        activeDocumentPane
            ? static_cast<std::uint32_t>(
                  std::max(activeDocumentPane->content.width, 1))
            : presentation->viewport.dimensions.columns;
    const auto paneRows =
        activeDocumentPane
            ? static_cast<std::uint32_t>(
                  std::max(activeDocumentPane->content.height, 1))
            : std::max(presentation->viewport.scrollbar.viewportRows,
                       std::uint32_t{1});

    bool supported = true;
    bool changed = false;
    bool pausesFollow = false;
    std::optional<ViewTransitionInput> resolvedInput;
    auto resolveVisualSelection = [&](SelectionCommand command,
                                      bool primaryOnly) {
        auto inputSelections = frame.selections;
        if (primaryOnly) {
            inputSelections =
                SelectionSet{{frame.selections.primary()}};
        }
        auto before = SelectionViewState{
            std::move(inputSelections),
            state.navigation.firstVisualRow,
            state.navigation.firstVisualColumn,
            state.navigation.desiredCell};
        auto result = navigateSelection(
            frame.documentText, before, command,
            {paneColumns, paneRows}, {}, {}, 4,
            frame.wordWrap, activeDiff);
        if (!result.accepted()) return false;

        auto resolved = result.delta.replacement.value_or(before);
        auto resolvedSelections = resolved.selections;
        if (primaryOnly) {
            auto selections = frame.selections.items();
            selections.back() = resolved.selections.primary();
            resolvedSelections = SelectionSet{std::move(selections)};
        }
        std::vector<SelectionRangeTransition> ranges;
        ranges.reserve(resolvedSelections.items().size());
        for (const auto& selection : resolvedSelections.items()) {
            ranges.push_back(
                {selection.anchor.byteOffset, selection.active.byteOffset});
        }
        if (primaryOnly) {
            resolvedInput = ViewTransitionInput{
                PointerSelectionTransition{
                    resolvedSelections.primary().active.byteOffset}};
        } else {
            resolvedInput = ViewTransitionInput{
                SelectionTransition{*frame.tabs.active,
                                    frame.documentRevision,
                                    std::move(ranges)}};
        }
        if (resolvedSelections != frame.selections) {
            state.pendingSelection =
                State::PendingSelection{
                    *frame.tabs.active,
                    frame.documentRevision,
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
            if constexpr (std::same_as<Action, ScrollLines>) {
                if (action.rows == 0) {
                    supported = false;
                    return;
                }
                if (action.target == ScrollTarget::Document) {
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
                } else if (action.target == ScrollTarget::Tree &&
                           frame.panel) {
                    auto const& tree = *frame.panel;
                    ScrollOffset offset{state.treeFirstVisible};
                    offset.byLines(action.rows, tree.scrollbar.totalRows,
                                   tree.scrollbar.viewportRows);
                    changed =
                        offset.firstVisible() != state.treeFirstVisible;
                    state.treeFirstVisible = offset.firstVisible();
                } else if (action.target != ScrollTarget::Tree) {
                    supported = false;
                }
            } else if constexpr (std::same_as<Action, ScrollPages>) {
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
                                              ScrollFraction>) {
                if (action.denominator == 0 ||
                    action.numerator > action.denominator) {
                    supported = false;
                    return;
                }
                if (action.target == ScrollTarget::Document) {
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
                } else if (action.target == ScrollTarget::Tree &&
                           frame.panel) {
                    auto const& tree = *frame.panel;
                    ScrollOffset offset{state.treeFirstVisible};
                    offset.toFraction(action.numerator, action.denominator,
                                      tree.scrollbar.totalRows,
                                      tree.scrollbar.viewportRows);
                    changed =
                        offset.firstVisible() != state.treeFirstVisible;
                    state.treeFirstVisible = offset.firstVisible();
                } else if (action.target != ScrollTarget::Tree) {
                    supported = false;
                }
            } else if constexpr (std::same_as<Action, RevealSelection> ||
                                 std::same_as<Action, CenterSelection>) {
                pausesFollow = true;
                const auto before = SelectionViewState{
                    frame.selections, state.navigation.firstVisualRow,
                    state.navigation.firstVisualColumn,
                    state.navigation.desiredCell};
                const auto result = navigateSelection(
                    frame.documentText, before,
                    std::same_as<Action, RevealSelection>
                        ? SelectionCommand::ViewRevealCaret
                        : SelectionCommand::ViewCenterCaret,
                    {paneColumns, paneRows}, {}, {}, 4, frame.wordWrap,
                    activeDiff);
                if (!result.accepted()) {
                    supported = false;
                    return;
                }
                if (result.delta.replacement) {
                    const auto& next = *result.delta.replacement;
                    changed = true;
                    state.navigation = {
                        next.firstVisualRow, next.firstVisualColumn,
                        next.desiredCell};
                }
            } else if constexpr (std::same_as<Action,
                                              MoveVisualSelection>) {
                if (!frame.tabs.active) {
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
                if (action.direction == DocumentPointerEdge::None) {
                    supported = false;
                    return;
                }
                if (!frame.tabs.active ||
                    !resolveVisualSelection(
                        action.direction == DocumentPointerEdge::Before
                            ? SelectionCommand::SelectLineUp
                            : SelectionCommand::SelectLineDown,
                        true)) {
                    supported = false;
                }
                return;
            } else if constexpr (std::same_as<Action, ResolvePaneFocus>) {
                if (!frame.document) {
                    supported = false;
                    return;
                }
                if (const auto pane =
                        paneInDirection(*frame.document, action.direction)) {
                    resolvedInput = ViewTransitionInput{
                        PaneFocusTransition{*pane}};
                }
            } else {
                supported = false;
            }
        },
        request);

    if (!supported) {
        return {ViewActionStatus::Rejected, std::nullopt,
                "view action is not supported by this presenter"};
    }
    ++state.generation;
    if (resolvedInput) {
        return {ViewActionStatus::TransitionRequired,
                std::move(*resolvedInput), {}};
    }
    if (pausesFollow &&
        frame.followMode == FollowMode::Following) {
        return {ViewActionStatus::TransitionRequired,
                ViewTransitionInput{PauseFollowTransition{}},
                {}};
    }
    return {ViewActionStatus::Applied, std::nullopt, {}};
}

}  // namespace ssg
