#pragma once

#include <ssg/UiNodeState.h>
#include <ssg/UiTree.h>
#include <ssg/UiWidget.h>

namespace ssg {

[[nodiscard]] std::optional<UiLeafState> resolveUiLeafState(
    const WidgetDescriptor& widget,
    const ChromeProviderResolver& resolveProvider,
    SemanticRole defaultRole);

[[nodiscard]] UiStateSection resolveUiState(
    const ValidatedSchema& schema,
    const ChromeProviderResolver& resolveProvider);

}  // namespace ssg
