#include <ssg/GridPresenter.h>

#include <ssg/EditorSession.h>

#include <algorithm>
#include <type_traits>

namespace ssg {

std::optional<GridFrame> GridFrame::fromDeprecatedSnapshot(
    SessionSnapshot snapshot) {
    if (!snapshot.presentation()) return std::nullopt;
    const GridBasis basis{snapshot.client().viewId, snapshot.revision(), 0};
    return GridFrame{std::move(snapshot), basis};
}

std::optional<GridFrame> GridPresenter::project(
    EditorSession& session, ClientId client, GridPresentationRequest request) {
    auto projectCurrent = [&](PaletteReport palette, bool revealSelection) {
        return session.projectForBridgedPresenterDeprecated(
            client, request.dimensions, std::move(palette), viewId_,
            navigation_, treeFirstVisible_, revealSelection);
    };
    auto snapshot = projectCurrent(request.palette, false);
    if (!snapshot || !snapshot->presentation()) {
        return std::nullopt;
    }
    if (adoptedRevision_ && snapshot->revision() < *adoptedRevision_) {
        return std::nullopt;
    }
    auto const& sections = snapshot->sections();
    auto const* presentation = &*snapshot->presentation();
    const auto primary = sections.selection.primary().active;
    const bool documentChanged =
        !documentRevision_ || *documentRevision_ != sections.document.revision ||
        !findGeneration_ ||
        *findGeneration_ != sections.findReplace.generation ||
        activeTab_ != sections.tabs.active ||
        !primarySelection_ || *primarySelection_ != primary;
    bool navigationChanged = false;
    documentRevision_ = sections.document.revision;
    findGeneration_ = sections.findReplace.generation;
    activeTab_ = sections.tabs.active;
    primarySelection_ = primary;

    const auto selectedTree =
        sections.tree.providers.empty()
            ? std::optional<TreeNodeId>{}
            : sections.tree.providers.front().selected;
    if (selectedTree && selectedTree != treeSelection_ &&
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
            ScrollOffset offset{treeFirstVisible_};
            offset.revealSelection(selected, scrollbar.totalRows,
                                   scrollbar.viewportRows);
            navigationChanged =
                navigationChanged ||
                offset.firstVisible() != treeFirstVisible_;
            treeFirstVisible_ = offset.firstVisible();
        }
    }
    treeSelection_ = selectedTree;

    if (documentChanged || navigationChanged) {
        snapshot = projectCurrent(request.palette, documentChanged);
        if (!snapshot || !snapshot->presentation()) return std::nullopt;
        presentation = &*snapshot->presentation();
    }
    adoptedRevision_ = snapshot->revision();
    auto frame = GridFrame{
        std::move(*snapshot),
        GridBasis{viewId_, *adoptedRevision_, ++generation_}};
    presentation = frame.presentation();
    navigation_ = presentation->selectionNav;
    navigation_.firstVisualRow = presentation->viewport.firstVisualRow;
    navigation_.firstVisualColumn = presentation->viewport.firstVisualColumn;
    if (!presentation->treeWindows.empty()) {
        treeFirstVisible_ = presentation->treeWindows.front().firstVisible;
    }
    return frame;
}

GridActionResult GridPresenter::apply(ViewActionRequest const& request,
                                      GridFrame const& frame) {
    const auto basis = frame.basis();
    if (request.viewId != viewId_ || basis.viewId != viewId_ ||
        request.semanticRevision != basis.semanticRevision ||
        !adoptedRevision_ ||
        basis.semanticRevision != *adoptedRevision_ ||
        basis.presentationGeneration != generation_) {
        return {GridActionStatus::Rejected, std::nullopt,
                "view action basis is stale"};
    }
    const auto* presentation = frame.presentation();
    if (!presentation) {
        return {GridActionStatus::Rejected, std::nullopt,
                "view action requires a grid projection"};
    }

    bool supported = true;
    bool changed = false;
    bool pausesFollow = false;
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
                    ScrollOffset offset{navigation_.firstVisualRow};
                    offset.byLines(
                        action.rows, presentation->viewport.totalVisualRows,
                        presentation->viewport.scrollbar.viewportRows);
                    changed =
                        offset.firstVisible() != navigation_.firstVisualRow;
                    navigation_.firstVisualRow = offset.firstVisible();
                } else if (!presentation->treeWindows.empty()) {
                    auto const& tree = presentation->treeWindows.front();
                    ScrollOffset offset{treeFirstVisible_};
                    offset.byLines(action.rows, tree.scrollbar.totalRows,
                                   tree.scrollbar.viewportRows);
                    changed = offset.firstVisible() != treeFirstVisible_;
                    treeFirstVisible_ = offset.firstVisible();
                }
            } else if constexpr (std::same_as<Action, ViewScrollPages>) {
                pausesFollow = true;
                if (action.pages == 0) {
                    supported = false;
                    return;
                }
                ScrollOffset offset{navigation_.firstVisualRow};
                offset.byPages(
                    action.pages, presentation->viewport.totalVisualRows,
                    presentation->viewport.scrollbar.viewportRows);
                changed = offset.firstVisible() != navigation_.firstVisualRow;
                navigation_.firstVisualRow = offset.firstVisible();
            } else if constexpr (std::same_as<Action,
                                              ViewScrollFraction>) {
                if (action.denominator == 0 ||
                    action.numerator > action.denominator) {
                    supported = false;
                    return;
                }
                if (action.target == ViewScrollTarget::Document) {
                    pausesFollow = true;
                    ScrollOffset offset{navigation_.firstVisualRow};
                    offset.toFraction(
                        action.numerator, action.denominator,
                        presentation->viewport.totalVisualRows,
                        presentation->viewport.scrollbar.viewportRows);
                    changed =
                        offset.firstVisible() != navigation_.firstVisualRow;
                    navigation_.firstVisualRow = offset.firstVisible();
                } else if (!presentation->treeWindows.empty()) {
                    auto const& tree = presentation->treeWindows.front();
                    ScrollOffset offset{treeFirstVisible_};
                    offset.toFraction(action.numerator, action.denominator,
                                      tree.scrollbar.totalRows,
                                      tree.scrollbar.viewportRows);
                    changed = offset.firstVisible() != treeFirstVisible_;
                    treeFirstVisible_ = offset.firstVisible();
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
                auto next = navigation_.firstVisualRow;
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
                changed = next != navigation_.firstVisualRow;
                navigation_.firstVisualRow = next;
            } else {
                supported = false;
            }
        },
        request.action);

    if (!supported) {
        return {GridActionStatus::Rejected, std::nullopt,
                "view action is not supported by this presenter"};
    }
    if (changed) ++generation_;
    if (pausesFollow &&
        frame.sections().followEdits.mode == FollowMode::Following) {
        return {GridActionStatus::TransitionRequired,
                ClientInput{ViewNavigationInput{
                    SemanticInputBasis{basis.semanticRevision}}},
                {}};
    }
    return {GridActionStatus::Applied, std::nullopt, {}};
}

}  // namespace ssg
