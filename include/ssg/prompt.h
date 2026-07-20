#pragma once

#include "ssg/ui_layout.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

enum class PromptKind : std::uint8_t {
    Path,
    Find,
    Replace,
    Settings,
    CommandArgument,
    Palette,
};

enum class PromptControlKind : std::uint8_t { Input, Toggle, Count };

struct PromptInput {
    std::string id;
    std::string accessibleLabel;
    std::string value;
    friend bool operator==(const PromptInput&, const PromptInput&) = default;
};

struct PromptToggle {
    std::string id;
    std::string accessibleLabel;
    bool value = false;
    int width = 0;
    friend bool operator==(const PromptToggle&, const PromptToggle&) = default;
};

struct PromptMatchCount {
    std::string id;
    std::string accessibleLabel;
    std::string value;
    friend bool operator==(const PromptMatchCount&,
                           const PromptMatchCount&) = default;
};

struct PromptRequest {
    PromptKind kind = PromptKind::CommandArgument;
    std::string accessibleLabel;
    std::vector<PromptInput> inputs;
    std::vector<PromptToggle> toggles;
    std::optional<PromptMatchCount> matchCount;
    friend bool operator==(const PromptRequest&, const PromptRequest&) = default;
};

struct PromptSubmission {
    PromptKind kind = PromptKind::CommandArgument;
    std::vector<std::string> values;
    std::vector<bool> toggles;
    friend bool operator==(const PromptSubmission&,
                           const PromptSubmission&) = default;
};

enum class PromptErrorCode : std::uint8_t {
    InvalidRequest,
    InvalidReservation,
    NoActivePrompt,
};

struct PromptError {
    PromptErrorCode code = PromptErrorCode::InvalidRequest;
    std::string message;
    friend bool operator==(const PromptError&, const PromptError&) = default;
};

struct PromptCommandResult {
    std::optional<PromptError> error;
    std::optional<PromptSubmission> submission;

    [[nodiscard]] bool accepted() const noexcept { return !error.has_value(); }
};

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

struct PromptStatusCommandDescriptor {
    std::string_view id;
};

struct PromptStatusCommandSet {
    std::array<PromptStatusCommandDescriptor, 6> descriptors{{
        {"prompt.submit"},
        {"prompt.cancel"},
        {"status.next"},
        {"status.previous"},
        {"status.dismiss"},
        {"status.invoke_action"},
    }};
};

class PromptSurface {
public:
    [[nodiscard]] PromptCommandResult open(PromptRequest request);
    [[nodiscard]] PromptCommandResult submit();
    [[nodiscard]] PromptCommandResult cancel();
    [[nodiscard]] bool active() const noexcept { return request_.has_value(); }
    [[nodiscard]] const std::optional<PromptRequest>& request() const noexcept {
        return request_;
    }

private:
    std::optional<PromptRequest> request_;
};

[[nodiscard]] std::uint8_t promptRowCount(PromptKind kind) noexcept;
[[nodiscard]] PromptLayoutResult computePromptLayout(
    const PromptSurface& surface, Rect reservation);

} // namespace ssg
