#pragma once

#include <ssg/EditorSession.h>
#include <tui/GridPresenter.h>

#include <optional>
#include <utility>

namespace ssg::test {

inline std::optional<GridPresentation> projectGridFrame(
    EditorSession& session, ViewId view,
    ViewportDimensions dimensions, PaletteReport palette = {}) {
    GridPresenter presenter{view};
    return presenter.project(session, {dimensions, std::move(palette)});
}

inline SessionSnapshot copySemantic(
    SessionSnapshot const& source, SessionSnapshotSections sections) {
    return {source.revision(), source.topology(), std::move(sections)};
}

inline GridPresentation copyGridFrame(
    GridPresentation const& source, ViewId view,
    SessionSnapshotSections sections,
    GridProjection projection, PaletteReport palette = {}) {
    auto const& semantic = source.semantic();
    auto copied = copySemantic(semantic, std::move(sections));
    GridFrame frame{
        copied, std::move(projection),
        GridBasis{view, semantic.revision(), 0},
        std::move(palette)};
    return {std::move(copied), std::move(frame)};
}

}  // namespace ssg::test
