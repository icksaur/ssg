#pragma once

#include <ssg/FindReplace.h>
#include <ssg/PromptSurface.h>

#include <optional>
#include <string>
#include <vector>

namespace ssg::detail {

struct ResolvedPromptControls {
    std::vector<PromptControl> controls;
    std::size_t activeInput = 0;
};

inline void applyFindReplaceValues(
    std::vector<PromptControl>& controls, PromptKind kind,
    const FindReplaceViewState& findState) {
    if (kind != PromptKind::Find && kind != PromptKind::Replace) return;
    for (auto& control : controls) {
        switch (control.kind) {
        case PromptControlKind::Input:
            if (control.id == "find.query") {
                control.value = findState.query;
            } else if (control.id == "replace.replacement") {
                control.value = findState.replacement;
            }
            break;
        case PromptControlKind::Count: {
            const auto position =
                findState.activeMatch ? *findState.activeMatch + 1 : 0;
            control.value = std::to_string(position) + "/" +
                            std::to_string(findState.matches.size());
            break;
        }
        case PromptControlKind::Toggle:
            if (control.id == "find.toggle_case") {
                control.checked = findState.options.caseSensitive;
            } else if (control.id == "find.toggle_whole_word") {
                control.checked = findState.options.wholeWord;
            } else if (control.id == "find.toggle_regex") {
                control.checked = findState.options.regex;
            }
            break;
        }
    }
}

inline std::optional<ResolvedPromptControls> resolveRuntimePromptControls(
    const PromptSurface& prompt, const FindReplaceViewState& findState) {
    const auto& request = prompt.request();
    if (!request ||
        promptFocusRegion(request->kind) != PromptRegion::Footer) {
        return std::nullopt;
    }
    ResolvedPromptControls resolved{
        resolvePromptControls(*request), prompt.activeInput()};
    applyFindReplaceValues(resolved.controls, request->kind, findState);
    return resolved;
}

}  // namespace ssg::detail
