#pragma once

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
    empty_name,
    empty_sequence,
    invalid_stroke,
    empty_command,
    empty_context,
    duplicate_binding,
    unreachable_binding,
    reserved_binding,
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
    editor_cell,
    scrollbar,
    tab,
    splitter,
    panel_node,
    status_action,
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
