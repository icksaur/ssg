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

std::vector<GridIntrinsicSize> transitionalIntrinsicSizes(
    const UiNode& root, const PresentationSnapshot& presentation) {
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
    int externalTop = std::numeric_limits<int>::max();
    int externalBottom = 0;
    for (const auto& node : presentation.shell.accessibilityNodes) {
        if (node.kind != ShellNodeKind::ExternalModificationBar &&
            node.kind != ShellNodeKind::ExternalModificationRow) {
            continue;
        }
        externalTop = std::min(externalTop, node.rect.y);
        externalBottom = std::max(externalBottom, node.rect.bottom());
    }
    if (externalTop != std::numeric_limits<int>::max()) {
        for (auto& size : sizes) {
            if (size.id == UiNodeId{std::string{kExternalModNodeId}}) {
                size.size.rows = externalBottom - externalTop;
                break;
            }
        }
    }
    return sizes;
}

SolveUiFrameResult trySolveFrameLayout(
    const SessionSnapshot& semantic,
    const PresentationSnapshot& presentation) {
    const auto& shell = presentation.shell;
    if (shell.viewport.columns <= 0 || shell.viewport.rows <= 0) {
        return {SolvedGridTree{}, {}};
    }

    auto validated = ValidatedSchema::validate(semantic.sections().ui);
    if (!validated.ok()) {
        return {std::nullopt,
                "invalid UI schema: " + validated.error()};
    }

    auto presence = semantic.sections().uiPresence;
    if (!shell.panel) {
        // Removed with the panel surface migration. Until then, the legacy grid
        // projection owns the panel's responsive collapse decision.
        for (auto& node : presence.nodes) {
            if (node.id == UiNodeId{std::string{kPanelNodeId}}) {
                node.present = false;
            }
        }
    }

    const auto& root = validated.schema().schema().root;
    auto result = solveUiFrame(
        validated.schema(), semantic.sections().uiState, presence,
        ClientUiProfile::full(),
        transitionalIntrinsicSizes(root, presentation),
        {0, 0, shell.viewport.columns, shell.viewport.rows});
    if (!result.tree) return result;

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
    };
    if (semantic.sections().promptView) {
        retained.insert(UiNodeId{std::string{kFooterPromptNodeId}});
        retained.insert(UiNodeId{std::string{kFooterPromptOptionsNodeId}});
        for (const auto& control :
             semantic.sections().promptView->controls) {
            retained.insert(footerPromptControlNodeId(control.id));
        }
    }
    std::erase_if(result.tree->nodes, [&](const SolvedGridNode& node) {
        return !retained.contains(node.id);
    });
    if (semantic.sections().promptView) {
        for (const auto& control :
             semantic.sections().promptView->controls) {
            const auto* node =
                result.tree->find(footerPromptControlNodeId(control.id));
            const auto expectedKind = [&] {
                switch (control.kind) {
                case PromptControlKind::Input:
                    return WidgetKind::TextInput;
                case PromptControlKind::Toggle:
                    return WidgetKind::Checkbox;
                case PromptControlKind::Count:
                    return WidgetKind::Label;
                }
                throw std::logic_error(
                    "GridFrame: corrupt prompt control kind");
            }();
            if (!node || !node->widget ||
                node->widget->id != control.id ||
                node->widget->kind != expectedKind ||
                (control.kind == PromptControlKind::Count &&
                 node->rect.width <= 0)) {
                return {std::nullopt,
                        "prompt backing does not correspond to UI nodes"};
            }
        }
    }
    if (semantic.sections().noticeView &&
        !result.tree->find(UiNodeId{std::string{kNoticeNodeId}})) {
        return {std::nullopt,
                "notice backing has no solved UI node"};
    }
    return result;
}

SolvedGridTree requireFrameLayout(
    const SessionSnapshot& semantic,
    const PresentationSnapshot& presentation) {
    auto result = trySolveFrameLayout(semantic, presentation);
    if (!result.tree) {
        throw std::logic_error(
            "GridFrame: semantic UI frame cannot be solved: " + result.error);
    }
    return std::move(*result.tree);
}

}  // namespace

GridFrame::GridFrame(SessionSnapshot semantic,
                     PresentationSnapshot presentation, GridBasis basis)
    : semantic_{std::move(semantic)},
      presentation_{std::move(presentation)},
      layout_{requireFrameLayout(semantic_, presentation_)},
      basis_{basis} {}

GridFrame::GridFrame(SessionSnapshot semantic,
                     PresentationSnapshot presentation,
                     SolvedGridTree layout, GridBasis basis)
    : semantic_{std::move(semantic)},
      presentation_{std::move(presentation)},
      layout_{std::move(layout)},
      basis_{basis} {}

std::optional<GridFrame> GridFrame::fromLegacy(
    LegacyPresentationSnapshot legacy, GridBasis basis) {
    auto result =
        trySolveFrameLayout(legacy.semantic_, legacy.presentation_);
    if (!result.tree) return std::nullopt;
    return GridFrame{std::move(legacy.semantic_),
                     std::move(legacy.presentation_),
                     std::move(*result.tree),
                     basis};
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
    auto projectCurrent = [&](PaletteReport palette, bool revealSelection) {
        return session.projectForBridgedPresenterDeprecated(
            client, request.dimensions, std::move(palette), viewId_, state,
            revealSelection);
    };
    auto snapshot = projectCurrent(request.palette, false);
    if (!snapshot) return std::nullopt;
    if (state.adoptedRevision &&
        snapshot->semantic().revision() < *state.adoptedRevision) {
        return std::nullopt;
    }
    auto const& sections = snapshot->semantic().sections();
    auto const* presentation = &snapshot->presentation();
    bool navigationChanged = false;
    bool confirmedSelection = false;
    if (state.pendingSelection) {
        if (sections.tabs.active == state.pendingSelection->activeTab &&
            sections.document.revision ==
                state.pendingSelection->documentRevision &&
            sections.selection == state.pendingSelection->expected) {
            state.navigation = state.pendingSelection->navigation;
            navigationChanged = true;
            confirmedSelection = true;
        }
        state.pendingSelection.reset();
    }
    const bool documentChanged =
        !state.documentRevision ||
        *state.documentRevision != sections.document.revision ||
        !state.findGeneration ||
        *state.findGeneration != sections.findReplace.generation ||
        state.activeTab != sections.tabs.active ||
        !state.selections || *state.selections != sections.selection;
    state.documentRevision = sections.document.revision;
    state.findGeneration = sections.findReplace.generation;
    state.activeTab = sections.tabs.active;
    state.selections = sections.selection;

    const auto selectedTree =
        sections.tree.providers.empty()
            ? std::optional<TreeNodeId>{}
            : sections.tree.providers.front().selected;
    if (selectedTree && selectedTree != state.treeSelection &&
        !presentation->treeWindows.empty() &&
        !sections.tree.providers.empty()) {
        auto const& provider = sections.tree.providers.front();
        const auto found = std::find_if(
            provider.nodes.begin(), provider.nodes.end(),
            [&](auto const& row) { return row.node.id == *selectedTree; });
        if (found != provider.nodes.end()) {
            const auto selected = static_cast<std::uint32_t>(
                std::distance(provider.nodes.begin(), found));
            auto const& scrollbar =
                presentation->treeWindows.front().scrollbar;
            ScrollOffset offset{state.treeFirstVisible};
            offset.revealSelection(selected, scrollbar.totalRows,
                                   scrollbar.viewportRows);
            navigationChanged =
                navigationChanged ||
                offset.firstVisible() != state.treeFirstVisible;
            state.treeFirstVisible = offset.firstVisible();
        }
    }
    state.treeSelection = selectedTree;

    if (documentChanged || navigationChanged) {
        snapshot = projectCurrent(request.palette,
                                  documentChanged && !confirmedSelection);
        if (!snapshot) return std::nullopt;
        presentation = &snapshot->presentation();
    }
    state.adoptedRevision = snapshot->semantic().revision();
    const auto nextGeneration = state.generation + 1;
    auto frame = GridFrame::fromLegacy(
        std::move(*snapshot),
        GridBasis{viewId_, *state.adoptedRevision, nextGeneration});
    if (!frame) return std::nullopt;
    state.generation = nextGeneration;
    presentation = &frame->presentation();
    state.navigation = presentation->selectionNav;
    state.navigation.firstVisualRow =
        presentation->viewport.firstVisualRow;
    state.navigation.firstVisualColumn =
        presentation->viewport.firstVisualColumn;
    if (!presentation->treeWindows.empty()) {
        state.treeFirstVisible =
            presentation->treeWindows.front().firstVisible;
    }
    return frame;
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
        const auto activePane = std::ranges::find(
            presentation->shell.panes, state.shell.activePane(),
            &PaneGeometry::id);
        const auto paneColumns =
            activePane == presentation->shell.panes.end()
                ? presentation->viewport.dimensions.columns
                : static_cast<std::uint32_t>(
                      std::max(activePane->content.width, 1));
        const auto paneRows =
            activePane == presentation->shell.panes.end()
                ? std::max(
                      presentation->viewport.scrollbar.viewportRows,
                      std::uint32_t{1})
                : static_cast<std::uint32_t>(
                      std::max(activePane->content.height, 1));
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
                } else if (!presentation->treeWindows.empty()) {
                    auto const& tree = presentation->treeWindows.front();
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
                } else if (!presentation->treeWindows.empty()) {
                    auto const& tree = presentation->treeWindows.front();
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
                focusesEditor =
                    state.shell.focusPane(action.direction,
                                          presentation->shell);
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
