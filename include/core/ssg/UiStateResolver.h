#pragma once

#include <ssg/UiNodeState.h>
#include <ssg/UiTree.h>
#include <ssg/UiWidget.h>

namespace ssg {

[[nodiscard]] std::optional<UiLeafState> resolveUiLeafState(
    const WidgetDescriptor& widget,
    const WidgetProviderResolver& resolveProvider,
    SemanticRole defaultRole);

[[nodiscard]] UiStateSection resolveUiState(
    const ValidatedSchema& schema,
    const WidgetProviderResolver& resolveProvider);

}  // namespace ssg
