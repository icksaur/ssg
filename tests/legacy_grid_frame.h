#pragma once

#include <ssg/GridPresenter.h>

#include <optional>

namespace ssg::test {

inline std::optional<GridFrame> gridFrameFromLegacy(
    LegacyPresentationSnapshot const& legacy) {
    auto const& semantic = legacy.semantic();
    auto sections = semantic.sections();
    if (sections.uiState.nodes.empty()) {
        auto schema = ValidatedSchema::validate(sections.ui);
        if (schema.ok()) {
            sections.uiState.generation = schema.schema().generation();
            for (const auto& id : schema.schema().nodeIds()) {
                sections.uiState.nodes.push_back({id, {}});
            }
        }
    }
    return GridFrame{
        SessionSnapshot{semantic.revision(), semantic.topology(),
                        semantic.client(), std::move(sections)},
        legacy.presentation(),
        GridBasis{semantic.client().viewId, semantic.revision(), 0}};
}

}  // namespace ssg::test
