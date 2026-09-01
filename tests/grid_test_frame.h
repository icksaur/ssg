#pragma once

#include <ssg/EditorSession.h>
#include <ssg/GridPresenter.h>

#include <optional>
#include <utility>

namespace ssg::test {

inline std::optional<GridFrame> projectGridFrame(
    EditorSession& session, ClientId client, ViewId view,
    ViewportDimensions dimensions, PaletteReport palette = {}) {
    GridPresenter presenter{view};
    return presenter.project(
        session, client, {dimensions, std::move(palette)});
}

inline SessionSnapshot copySemantic(
    SessionSnapshot const& source, SessionSnapshotSections sections) {
    return {source.revision(), source.topology(), source.client(),
            std::move(sections)};
}

inline GridFrame copyGridFrame(
    GridFrame const& source, SessionSnapshotSections sections,
    GridProjection projection, PaletteReport palette = {}) {
    auto const& semantic = source.semantic();
    return {
        copySemantic(semantic, std::move(sections)),
        std::move(projection),
        GridBasis{semantic.client().viewId, semantic.revision(), 0},
        std::move(palette)};
}

}  // namespace ssg::test
