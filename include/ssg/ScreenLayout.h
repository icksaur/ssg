#pragma once

#include <ssg/UiTree.h>
#include <ssg/PromptSurface.h>
#include <ssg/PromptSurface.h>
#include <ssg/Style.h>

#include <string_view>
#include <vector>

namespace ssg {

[[nodiscard]] UiComposition assembleScreen(
    std::string_view hintCommandId, const StyleDimensions& dimensions,
    std::string_view promptSigil);

[[nodiscard]] UiComposition withFooterPrompt(UiComposition base,
                                             const PromptSurface& prompt);
[[nodiscard]] UiNode assembleFooterPrompt(const PromptSurface& prompt);

}  // namespace ssg
