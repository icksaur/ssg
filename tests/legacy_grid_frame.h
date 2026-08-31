#pragma once

#include <ssg/GridPresenter.h>

#include <optional>

namespace ssg::test {

inline std::optional<GridFrame> gridFrameFromLegacy(
    LegacyPresentationSnapshot const& legacy, PaletteReport palette = {}) {
    auto const& semantic = legacy.semantic();
    auto sections = semantic.sections();
    return GridFrame{
        SessionSnapshot{semantic.revision(), semantic.topology(),
                        semantic.client(), std::move(sections)},
        legacy.presentation(),
        GridBasis{semantic.client().viewId, semantic.revision(), 0},
        std::move(palette)};
}

}  // namespace ssg::test
