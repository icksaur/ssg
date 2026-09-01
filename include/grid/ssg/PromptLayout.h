#pragma once

#include <ssg/Geometry.h>
#include <ssg/PromptSurface.h>
#include <ssg/UiTree.h>

#include <optional>
#include <vector>

namespace ssg {

struct PromptControlView {
    PromptControlKind kind = PromptControlKind::Input;
    std::string id;
    std::string accessibleLabel;
    std::string value;
    bool checked = false;
    Rect rect;
    friend bool operator==(const PromptControlView&,
                           const PromptControlView&) = default;
};

struct PromptViewState {
    PromptKind kind = PromptKind::CommandArgument;
    std::string accessibleLabel;
    Rect rect;
    std::vector<PromptControlView> controls;
    std::size_t activeInput = 0;
    friend bool operator==(const PromptViewState&,
                           const PromptViewState&) = default;
};

struct PromptLayoutResult {
    std::optional<PromptError> error;
    std::optional<PromptViewState> view;

    [[nodiscard]] bool accepted() const noexcept {
        return view.has_value() && !error.has_value();
    }
};

[[nodiscard]] std::uint8_t promptRowCount(PromptKind kind) noexcept;
[[nodiscard]] PromptLayoutResult computePromptLayout(
    const PromptSurface& surface, Rect reservation);
[[nodiscard]] PromptLayoutResult computePromptLayout(
    const PromptSurface& surface, const UiNode& promptTree, Rect reservation);

}  // namespace ssg
