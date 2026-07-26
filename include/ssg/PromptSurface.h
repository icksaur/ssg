#pragma once

#include "ssg/ShellState.h"

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
    // The command this prompt is collecting arguments for, re-dispatched with
    // the typed value on submit. Carrying the id -- rather than a kind enum the
    // submit path switches on -- is what lets a new path-taking command be
    // added without touching submission at all.
    //
    // Runtime-internal: PromptViewState is the protocol type, and it does not
    // carry this. The client never needs to know which command a prompt serves.
    std::string commandId;
    friend bool operator==(const PromptRequest&, const PromptRequest&) = default;
};

struct PromptSubmission {
    PromptKind kind = PromptKind::CommandArgument;
    std::vector<std::string> values;
    std::vector<bool> toggles;
    std::string commandId;
    friend bool operator==(const PromptSubmission&,
                           const PromptSubmission&) = default;
};

// Payload for `prompt.update_value`: which input of the active prompt receives
// the text. Prompts with several inputs (replace) address them by index.
struct PromptValueArguments {
    std::size_t index = 0;
    std::string value;
    friend bool operator==(const PromptValueArguments&,
                           const PromptValueArguments&) = default;
};

enum class PromptErrorCode : std::uint8_t {
    InvalidRequest,
    InvalidReservation,
    NoActivePrompt,
    UnknownInput,
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
    std::array<PromptStatusCommandDescriptor, 9> descriptors{{
        {"prompt.submit"},
        {"prompt.cancel"},
        {"prompt.next"},
        {"prompt.previous"},
        {"prompt.update_value"},
        {"status.next"},
        {"status.previous"},
        {"status.dismiss"},
        {"status.invoke_action"},
    }};
};

class PromptSurface {
public:
    [[nodiscard]] PromptCommandResult open(PromptRequest request);
    [[nodiscard]] PromptCommandResult updateValue(std::size_t index,
                                                  std::string value);
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
