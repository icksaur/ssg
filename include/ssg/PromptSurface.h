#pragma once

#include <ssg/focus.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

enum class PromptKind : std::uint8_t {
    Path = 0,
    Find = 1,
    Replace = 2,
    Settings = 3,
    CommandArgument = 4,
    Palette = 5,
};

enum class PromptControlKind : std::uint8_t {
    Input = 0,
    Toggle = 1,
    Count = 2,
};

// The layout region a prompt of a given kind is anchored to and draws its input
// in. The palette's query lives in the HEADER input line (its results narrow to
// the top of the buffer just below it); every other prompt reserves rows over
// the FOOTER. This is the SINGLE place that "where does the focused prompt live"
// is expressed, so the two scattered `kind == Palette` checks cannot drift
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

enum class PromptCompletion : std::uint8_t {
    None = 0,
    WorkspaceOpenDirectory,
    FileOpen,
    FileSaveAs,
    FileRename,
    FileNewDirectory,
    GotoLine,
};

struct PromptRequest {
    PromptKind kind = PromptKind::CommandArgument;
    std::string accessibleLabel;
    std::vector<PromptInput> inputs;
    std::vector<PromptToggle> toggles;
    std::optional<PromptMatchCount> matchCount;
    PromptCompletion completion = PromptCompletion::None;
    friend bool operator==(const PromptRequest&, const PromptRequest&) = default;
};

struct PromptSubmission {
    PromptKind kind = PromptKind::CommandArgument;
    std::vector<std::string> values;
    std::vector<bool> toggles;
    PromptCompletion completion = PromptCompletion::None;
    friend bool operator==(const PromptSubmission&,
                           const PromptSubmission&) = default;
};

struct PromptValueArguments {
    std::size_t index = 0;
    std::string value;
    friend bool operator==(const PromptValueArguments&,
                           const PromptValueArguments&) = default;
};

enum class PromptErrorCode : std::uint8_t {
    InvalidRequest,
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

// A geometry-free prompt control. Grid rendering combines it with the matching
// solved UI node. `command` is the library command that
// OPERATES the control (an input's
// update-value command, a toggle's toggle command), so a client dispatches it
// generically without knowing find-vs-replace ids; empty for a Count.
struct PromptControl {
    PromptControlKind kind = PromptControlKind::Input;
    std::string id;
    std::string accessibleLabel;
    std::string value;
    bool checked = false;
    std::string command;
    friend bool operator==(const PromptControl&, const PromptControl&) = default;
};

struct PromptViewState {
    std::optional<PromptKind> activeKind;
    friend bool operator==(const PromptViewState&, const PromptViewState&) =
        default;
};

enum class ActivePrompt : std::uint8_t {
    None,
    Palette,
    Find,
    Replace,
    TextPrompt,
};

struct PromptRoutingState {
    FocusTarget focus = FocusTarget::Editor;
    ActivePrompt prompt = ActivePrompt::None;
    std::string currentValue;
    std::size_t activeInput = 0;
};

struct PromptTextEdit {
    enum class Kind : std::uint8_t {
        Append,
        DeleteGraphemeBack,
        DeleteWordBack,
    } kind = Kind::Append;
    std::string text;
};

struct PromptTextRoute {
    enum class Kind : std::uint8_t {
        UpdateFindQuery,
        UpdateReplacement,
        AppendPaletteQuery,
        UpdatePromptValue,
        Ignore,
    } kind = Kind::Ignore;
    std::string query;
    std::string appendText;
    PromptValueArguments promptValue;
};

[[nodiscard]] std::string applyPromptTextEdit(
    std::string_view value, const PromptTextEdit& edit);
[[nodiscard]] PromptTextRoute routePromptTextEdit(
    const PromptRoutingState& state, const PromptTextEdit& edit);

class PromptSurface {
public:
    [[nodiscard]] PromptCommandResult open(PromptRequest request);
    [[nodiscard]] PromptCommandResult updateValue(std::size_t index,
                                                  std::string value);
    // Focus the input named by `controlId`. Rejected with UnknownInput when the
    // id does not address an input, so a toggle or
    // the match count can never own the keyboard.
    [[nodiscard]] PromptCommandResult focusInput(std::string_view controlId);
    // Advance the active input to the next input, wrapping. A single-input prompt
    // stays on its one input.
    [[nodiscard]] PromptCommandResult focusNextInput();
    [[nodiscard]] PromptCommandResult submit();
    [[nodiscard]] PromptCommandResult cancel();
    [[nodiscard]] bool active() const noexcept { return request_.has_value(); }
    [[nodiscard]] const std::optional<PromptRequest>& request() const noexcept {
        return request_;
    }
    // Which input owns the keyboard. Deterministically reset on every open/kind
    // transition: Replace focuses its replacement input, Find and the single-input
    // prompts focus their sole input. Always a valid index into request_->inputs
    // while a prompt is active; 0 when none.
    [[nodiscard]] std::size_t activeInput() const noexcept { return activeInput_; }

private:
    std::optional<PromptRequest> request_;
    std::size_t activeInput_ = 0;
};

// The one geometry-free control resolver: the active prompt's ordered controls,
// each with the request's seeded value/checked and its operating command, WITHOUT
// any geometry. Presentation attaches geometry to these controls rather than
// resolving prompt behavior a second time.
[[nodiscard]] std::vector<PromptControl> resolvePromptControls(
    const PromptRequest& request);

[[nodiscard]] PromptCommandResult openGenericPrompt(
    PromptSurface& prompt, PromptRequest request);

}  // namespace ssg
