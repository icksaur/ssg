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

class KeyCodec {
public:
    [[nodiscard]] std::optional<KeyStroke> parseStroke(
        std::string_view encoded) const;
    [[nodiscard]] std::string formatStroke(const KeyStroke& stroke) const;
    [[nodiscard]] std::optional<KeySequence> parseSequence(
        std::initializer_list<std::string_view> encoded) const;
    [[nodiscard]] std::string formatSequence(const KeySequence& sequence) const;
};

struct KeyBinding {
    KeySequence sequence;
    std::string commandId;
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
    std::size_t bindingIndex;
    std::string message;

    bool operator==(const KeymapError&) const = default;
};

enum class KeymapMatchKind : std::uint8_t { None, Pending, Resolved };

struct KeymapResolution {
    KeymapMatchKind kind = KeymapMatchKind::None;
    std::string commandId;

    bool operator==(const KeymapResolution&) const = default;
};

class KeymapMatcher {
public:
    explicit KeymapMatcher(const KeymapViewState& keymap) : keymap_{keymap} {}

    [[nodiscard]] std::vector<KeymapError> validate(
        std::span<const KeySequence> reservedSequences) const;
    [[nodiscard]] KeymapResolution resolveSequence(const KeySequence& pending,
                                                   std::string_view context) const;
    [[nodiscard]] bool hasGlobalBinding(
        std::string_view commandId,
        std::span<const KeySequence> reservedSequences) const;
    [[nodiscard]] std::optional<KeySequence> preferredBinding(
        std::string_view commandId) const;
    [[nodiscard]] static KeymapDelta deriveDelta(
        const KeymapViewState& previous, const KeymapViewState& current);

private:
    const KeymapViewState& keymap_;
};

enum class TextRouting : std::uint8_t { Insert, PromptQuery, Ignore };

class CommittedText {
public:
    [[nodiscard]] static std::optional<CommittedText> fromUtf8(
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
    std::string commandId;
    SemanticInputArguments arguments;

    bool operator==(const SemanticCommand& other) const;
};

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
    std::string accessibleLabel;
    SemanticCommand command;

    bool operator==(const SemanticHitTarget& other) const = default;
};

class SemanticInputRouter {
public:
    [[nodiscard]] TextRouting textRouting(std::string_view context) const noexcept;
    [[nodiscard]] SemanticCommand semanticInput(
        const CommittedText& committed) const;
    [[nodiscard]] const SemanticCommand& activateHitTarget(
        const SemanticHitTarget& target) const noexcept;
};

} // namespace ssg
