#pragma once

#include <ssg/focus.h>
#include <ssg/KeyCode.h>
#include <ssg/Selection.h>
#include <ssg/TextInputCommands.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

// `mod` is SSG's single chord modifier: Ctrl OR Alt, never both. The decoder
// collapses the two physical keys onto it and discards Ctrl+Alt entirely, so
// nothing above the decoder knows which key was pressed. Shift is orthogonal;
// meta is a distinct physical key only the Kitty protocol reports.
struct KeyStroke {
    KeyCode code = KeyCode::None;
    bool mod = false;
    bool meta = false;
    bool shift = false;

    bool operator==(const KeyStroke&) const = default;
};

using KeySequence = std::vector<KeyStroke>;

[[nodiscard]] std::optional<KeyStroke> parseKeyStroke(std::string_view encoded);
[[nodiscard]] std::optional<KeySequence> parseKeySequence(
    std::initializer_list<std::string_view> encoded);
// Runtime counterpart to parseKeySequence's compile-time initializer_list.
[[nodiscard]] std::optional<KeySequence> parseKeySequenceString(
    std::string_view encoded);
[[nodiscard]] std::string formatKeySequence(const KeySequence& sequence);

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

[[nodiscard]] KeymapViewState defaultTerminalKeymap();

enum class KeymapErrorCode : std::uint8_t {
    EmptyName,
    EmptySequence,
    InvalidStroke,
    EmptyCommand,
    EmptyContext,
    DuplicateBinding,
    UnreachableBinding,
    UnknownContext,
    MultiStrokeBinding,
    ModifiedEnterBinding,
};

struct KeymapError {
    KeymapErrorCode code;
    std::size_t bindingIndex;
    std::string message;

    bool operator==(const KeymapError&) const = default;
};

enum class KeymapMatchKind : std::uint8_t { None, Resolved };

struct KeymapResolution {
    KeymapMatchKind kind = KeymapMatchKind::None;
    std::string commandId;

    bool operator==(const KeymapResolution&) const = default;
};

class KeymapMatcher {
public:
    explicit KeymapMatcher(const KeymapViewState& keymap) : keymap_{keymap} {}

    [[nodiscard]] std::vector<KeymapError> validate() const;
    [[nodiscard]] KeymapResolution resolveSequence(const KeySequence& pending,
                                                   std::string_view context) const;
    [[nodiscard]] bool hasGlobalBinding(std::string_view commandId) const;
    [[nodiscard]] std::optional<KeySequence> preferredBinding(
        std::string_view commandId) const;
private:
    const KeymapViewState& keymap_;
};

// keymap.bind's argument: a single sequence string (space-separated
// key strokes, e.g. "Escape KeyF KeyT"), the command id it should
// invoke, and the context it applies in ("*"/"editor"/"panel"/"prompt";
// empty defaults to "*"). This is the ONLY argument shape keymap.bind
// accepts; one call binds exactly one sequence.
struct KeymapBindArguments {
    std::string sequence;
    std::string command;
    std::string context;

    friend bool operator==(const KeymapBindArguments&, const KeymapBindArguments&) = default;
};

// keymap.unbind's argument: the sequence string to remove and the context
// it was bound in (empty defaults to "*"). Unbinding a sequence that is
// not currently bound in that context is a no-op success, not an error --
// matching the reset-then-reapply model's "config always reflects exactly
// what the current config asked for" contract.
struct KeymapUnbindArguments {
    std::string sequence;
    std::string context;

    friend bool operator==(const KeymapUnbindArguments&, const KeymapUnbindArguments&) = default;
};

struct KeymapMutationError {
    std::string message;

    friend bool operator==(const KeymapMutationError&, const KeymapMutationError&) = default;
};

struct KeymapMutationResult {
    std::optional<KeymapMutationError> error;
    // The replacement keymap when accepted; left default-constructed
    // (unused) when rejected -- all-or-nothing, no partial apply on error.
    KeymapViewState keymap;

    [[nodiscard]] bool accepted() const noexcept { return !error.has_value(); }
};

// Applies keymap.bind's request to `current`: parses the sequence string,
// removes any existing binding for the SAME (context, sequence) pair (a
// rebind, not a duplicate), appends the new binding, then re-validates the
// WHOLE resulting keymap via KeymapMatcher (K1 context validity, K2
// prefix-freedom, K6 the settings.open global escape hatch survives).
// Rejects -- leaving `current` untouched -- on an unparseable sequence, an
// unknown context, an empty command id, or any KeymapMatcher validation
// failure; the whole binding is validated before it is applied, so a
// rejected call never partially mutates the result.
[[nodiscard]] KeymapMutationResult applyKeymapBind(
    KeymapViewState const& current,
    KeymapBindArguments const& arguments) noexcept;

// Applies keymap.unbind's request to `current`: parses the sequence
// string and removes any binding matching the (context, sequence) pair.
// Absence is a no-op success (see KeymapUnbindArguments above). Rejects
// on an unparseable sequence, an unknown context, or if removing the
// binding would eliminate the LAST settings.open global binding (K6's
// escape hatch): unlike K1/K2 (which removal can never violate), K6 is a
// property of the WHOLE keymap and a removal can be the one that breaks
// it, so it is re-checked the same way keymap.bind checks it.
[[nodiscard]] KeymapMutationResult applyKeymapUnbind(
    KeymapViewState const& current,
    KeymapUnbindArguments const& arguments) noexcept;

enum class TextRouting : std::uint8_t { Insert, PromptQuery, Ignore };

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

[[nodiscard]] TextRouting textRoutingForContext(std::string_view context) noexcept;

} // namespace ssg
