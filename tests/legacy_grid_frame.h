#pragma once

#include <ssg/GridPresenter.h>

#include <optional>

namespace ssg::test {

inline std::optional<GridFrame> gridFrameFromLegacy(
    LegacyPresentationSnapshot const& legacy) {
    auto const& semantic = legacy.semantic();
    return GridFrame{
        SessionSnapshot{semantic.revision(), semantic.topology(),
                        semantic.client(), semantic.sections()},
        legacy.presentation(),
        GridBasis{semantic.client().viewId, semantic.revision(), 0}};
}

}  // namespace ssg::test
