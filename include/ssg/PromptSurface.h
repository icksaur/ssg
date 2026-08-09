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

// The layout region a prompt of a given kind is anchored to and draws its input
// in. The palette's query lives in the HEADER input line (its results narrow to
// the top of the buffer just below it); every other prompt reserves rows over
// the FOOTER. This is the SINGLE place that "where does the focused prompt live"
// is expressed, so the two scattered `kind == Palette` checks cannot drift
// (doc/spec-chrome-stacks.md §Prompt as a focusable region mode).
enum class PromptRegion : std::uint8_t { Header, Footer };

// CONTRACT
// promptFocusRegion: because there is exactly one PromptSurface (one optional
//   request), at most one prompt is ever active, and FocusTarget::Prompt
//   resolves to this region for that one prompt — the focus model is unambiguous
//   by construction, not by convention.
[[nodiscard]] constexpr PromptRegion promptFocusRegion(PromptKind kind) noexcept {
    return kind == PromptKind::Palette ? PromptRegion::Header
                                       : PromptRegion::Footer;
}

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
