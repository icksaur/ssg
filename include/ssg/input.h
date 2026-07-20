#pragma once

#include <ssg/focus.h>
#include <ssg/selection.h>
#include <ssg/text_input_commands.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ssg {

struct KeyStroke {
    std::string code;
    bool control = false;
    bool alt = false;
    bool meta = false;
    bool shift = false;

    bool operator==(const KeyStroke&) const = default;
};

using KeySequence = std::vector<KeyStroke>;

[[nodiscard]] std::optional<KeyStroke> parse_key_stroke(
    std::string_view encoded);
[[nodiscard]] std::string format_key_stroke(const KeyStroke& stroke);
[[nodiscard]] std::optional<KeySequence> parse_key_sequence(
    std::initializer_list<std::string_view> encoded);

// A compact human display form of a key sequence, e.g. {Escape, KeyS} -> "Esc S"
// and {ArrowDown} -> "Down".  Modifiers are prefixed (Ctrl+/Alt+/Shift+/Meta+);
// strokes are space-joined.  Used for palette key-sequence detail (K7).
[[nodiscard]] std::string format_key_sequence(const KeySequence& sequence);

struct KeyBinding {
    KeySequence sequence;
    std::string command_id;
    std::string context;

    bool operator==(const KeyBinding&) const = default;
};

struct KeymapViewState {
    std::string name;
    std::vector<KeyBinding> bindings;

    bool operator==(const KeymapViewState&) const = default;
};

struct KeymapDelta {
    bool changed;
    std::optional<KeymapViewState> replacement;

    bool operator==(const KeymapDelta&) const = default;
};

enum class KeymapErrorCode : std::uint8_t {
    EmptyName,
    EmptySequence,
    InvalidStroke,
    EmptyCommand,
    EmptyContext,
    DuplicateBinding,
    UnreachableBinding,
    ReservedBinding,
    UnknownContext,
    AmbiguousPrefix,
};

struct KeymapError {
    KeymapErrorCode code;
    std::size_t binding_index;
    std::string message;

    bool operator==(const KeymapError&) const = default;
};

[[nodiscard]] std::vector<KeymapError> validate_keymap(
    const KeymapViewState& keymap,
    std::span<const KeySequence> reserved_sequences);
[[nodiscard]] KeymapDelta derive_keymap_delta(const KeymapViewState& previous,
                                              const KeymapViewState& current);

// The canonical keymap contexts: "*" plus every FocusTarget name (see
// keymap_contexts() in <ssg/focus.h>).  A binding whose context is outside this
// set is rejected by validate_keymap with unknown_context.

// Whether a binding is eligible in the given resolution context: its context is
// "*" (global) or equals the context (a FocusTarget name).  See
// doc/spec-keymap.md.
enum class KeymapMatchKind : std::uint8_t { None, Pending, Resolved };

struct KeymapResolution {
    KeymapMatchKind kind = KeymapMatchKind::None;
    std::string command_id;  // Set iff kind == resolved.

    bool operator==(const KeymapResolution&) const = default;
};

// Resolve a pending key sequence against the published keymap in a focus
// context.  Pure: a function of (keymap, pending, context) with no state and no
// round-trip (doc/spec-keymap.md K3).  `context` is a FocusTarget name.
//   resolved - an eligible binding's sequence equals `pending`; a "*" binding
//              takes precedence over a same-sequence focus binding (K4).
//   pending  - some eligible binding's sequence has `pending` as a strict prefix
//              (the chord is mid-entry).
//   none     - neither; the caller clears the pending sequence.
// Prefix-freeness (validate_keymap ambiguous_prefix) makes resolved and pending
// mutually exclusive.
[[nodiscard]] KeymapResolution resolve_key_sequence(
    const KeymapViewState& keymap, const KeySequence& pending,
    std::string_view context);

// Where committed text (with no pending chord) is routed in a focus context
// (doc/spec-keymap.md).  Committed text is never a keymap binding.
enum class TextRouting : std::uint8_t { Insert, PromptQuery, Ignore };

[[nodiscard]] TextRouting text_routing(std::string_view context) noexcept;

// Whether the keymap has a usable global binding for `command_id`: some
// "*"-context binding names it, is not browser-reserved (checked against
// `reserved_sequences`), and is not shadowed by an earlier "*" binding of the
// same sequence.  Used to enforce the settings.open escape hatch (I24, K6).
[[nodiscard]] bool has_global_binding(
    const KeymapViewState& keymap, std::string_view command_id,
    std::span<const KeySequence> reserved_sequences);

// The preferred key sequence bound to `command_id` for display, chosen
// deterministically (independent of binding order): the shortest sequence, then
// the lexicographically least display form (K7).  Empty if the command is
// unbound.
[[nodiscard]] std::optional<KeySequence> preferred_binding(
    const KeymapViewState& keymap, std::string_view command_id);

class CommittedText {
public:
    [[nodiscard]] static std::optional<CommittedText> from_utf8(
        std::string text);
    [[nodiscard]] const std::string& utf8() const noexcept { return text_; }
    bool operator==(const CommittedText&) const = default;

private:
    explicit CommittedText(std::string text) : text_{std::move(text)} {}
    std::string text_;
};

struct ScrollLinesArguments {
    std::int64_t rows;
    bool operator==(const ScrollLinesArguments&) const = default;
};

struct ScrollPagesArguments {
    std::int64_t pages;
    bool operator==(const ScrollPagesArguments&) const = default;
};

struct ScrollFractionArguments {
    std::uint32_t numerator;
    std::uint32_t denominator;

    ScrollFractionArguments(std::uint32_t numerator,
                            std::uint32_t denominator);
    bool operator==(const ScrollFractionArguments&) const = default;
};

using SemanticInputArguments =
    std::variant<std::monostate, TextInputArguments, SelectionCommandArguments,
                 ScrollLinesArguments, ScrollPagesArguments,
                 ScrollFractionArguments>;

struct SemanticCommand {
    std::string command_id;
    SemanticInputArguments arguments;

    bool operator==(const SemanticCommand& other) const;
};

[[nodiscard]] SemanticCommand semantic_input(const CommittedText& committed);

enum class HitTargetKind : std::uint8_t {
    EditorCell,
    Scrollbar,
    Tab,
    Splitter,
    PanelNode,
    StatusAction,
};

struct SemanticHitTarget {
    std::uint64_t id;
    HitTargetKind kind;
    std::string accessible_label;
    SemanticCommand command;

    bool operator==(const SemanticHitTarget& other) const = default;
};

[[nodiscard]] const SemanticCommand& activate_hit_target(
    const SemanticHitTarget& target) noexcept;

} // namespace ssg
