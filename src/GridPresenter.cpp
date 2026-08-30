#include <ssg/GridPresenter.h>

#include <ssg/EditorSession.h>

namespace ssg {

std::optional<GridFrame> GridFrame::fromDeprecatedSnapshot(
    SessionSnapshot snapshot) {
    if (!snapshot.presentation()) return std::nullopt;
    const GridBasis basis{snapshot.client().viewId, snapshot.revision(), 0};
    return GridFrame{std::move(snapshot), basis};
}

std::optional<GridFrame> GridPresenter::project(
    EditorSession& session, ClientId client, GridPresentationRequest request) {
    auto snapshot = session.projectForBridgedPresenterDeprecated(
        client, request.dimensions, std::move(request.palette), viewId_);
    if (!snapshot || !snapshot->presentation()) {
        return std::nullopt;
    }
    if (adoptedRevision_ && snapshot->revision() < *adoptedRevision_) {
        return std::nullopt;
    }
    adoptedRevision_ = snapshot->revision();
    return GridFrame{
        std::move(*snapshot),
        GridBasis{viewId_, *adoptedRevision_, ++generation_}};
}

}  // namespace ssg
