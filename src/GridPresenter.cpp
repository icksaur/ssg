#include <ssg/GridPresenter.h>

#include <ssg/EditorSession.h>

#include "grid_projection_state.h"

#include <algorithm>
#include <type_traits>

namespace ssg {

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
    auto frame = GridFrame{
        std::move(*snapshot),
        GridBasis{viewId_, *state.adoptedRevision, ++state.generation}};
    presentation = &frame.presentation();
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
