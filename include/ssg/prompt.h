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
    path,
    find,
    replace,
    settings,
    command_argument,
};

enum class PromptControlKind : std::uint8_t { input, toggle, count };

struct PromptInput {
    std::string id;
    std::string accessible_label;
    std::string value;
    friend bool operator==(const PromptInput&, const PromptInput&) = default;
};

struct PromptToggle {
    std::string id;
    std::string accessible_label;
    bool value = false;
    int width = 0;
    friend bool operator==(const PromptToggle&, const PromptToggle&) = default;
};

struct PromptMatchCount {
    std::string id;
    std::string accessible_label;
    std::string value;
    friend bool operator==(const PromptMatchCount&,
                           const PromptMatchCount&) = default;
};

struct PromptRequest {
    PromptKind kind = PromptKind::command_argument;
    std::string accessible_label;
    std::vector<PromptInput> inputs;
    std::vector<PromptToggle> toggles;
    std::optional<PromptMatchCount> match_count;
    friend bool operator==(const PromptRequest&, const PromptRequest&) = default;
};

struct PromptSubmission {
    PromptKind kind = PromptKind::command_argument;
    std::vector<std::string> values;
    std::vector<bool> toggles;
    friend bool operator==(const PromptSubmission&,
                           const PromptSubmission&) = default;
};

enum class PromptErrorCode : std::uint8_t {
    invalid_request,
    invalid_reservation,
    no_active_prompt,
};

struct PromptError {
    PromptErrorCode code = PromptErrorCode::invalid_request;
    std::string message;
    friend bool operator==(const PromptError&, const PromptError&) = default;
};

struct PromptCommandResult {
    std::optional<PromptError> error;
    std::optional<PromptSubmission> submission;

    [[nodiscard]] bool accepted() const noexcept { return !error.has_value(); }
};

struct PromptControlView {
    PromptControlKind kind = PromptControlKind::input;
    std::string id;
    std::string accessible_label;
    std::string value;
    bool checked = false;
    Rect rect;
    friend bool operator==(const PromptControlView&,
                           const PromptControlView&) = default;
};

struct PromptViewState {
    PromptKind kind = PromptKind::command_argument;
    std::string accessible_label;
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

[[nodiscard]] std::uint8_t prompt_row_count(PromptKind kind) noexcept;
[[nodiscard]] PromptLayoutResult compute_prompt_layout(
    const PromptSurface& surface, Rect reservation);

} // namespace ssg
