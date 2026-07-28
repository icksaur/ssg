#pragma once

// A keymap compiled for the keystroke path.
//
// `KeymapViewState` is the authored, transportable form of a keymap: key codes,
// command ids and focus contexts are all strings, because that is what a config
// file, `keymap.bind` and the wire protocol speak.  Resolving a keystroke
// against it costs a string comparison per stroke per binding, plus a hashed
// string lookup to dispatch -- for a keypress, which is identity, not text.
//
// CompiledKeymap pays those string costs ONCE, when the keymap is published,
// and then answers keystrokes with integer comparisons only:
//
//   * every distinct key code in the keymap is interned to a `StrokeCode`,
//   * every context is interned to a `ContextId`,
//   * every command id is resolved to a `CommandHandle`.
//
// The authored keymap remains the source of truth; this is a derived index of
// it, rebuilt whenever it changes.  Nothing here decides policy: the precedence
// and prefix rules are the same ones `KeymapMatcher` applies, expressed over
// integers.

#include <ssg/Commands.h>
#include <ssg/Keymap.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ssg {

// A key code interned against one keymap.  `kUnknownStroke` is the code for
// every key the keymap does not mention -- notably ordinary printable
// characters, which therefore fail every binding on an integer compare.
using StrokeCode = std::uint16_t;
inline constexpr StrokeCode kUnknownStroke = 0;

// A focus context interned against one keymap.  `kAnyContext` is "*".
using ContextId = std::uint16_t;
inline constexpr ContextId kUnknownContext = 0;
inline constexpr ContextId kAnyContext = 1;

// One keystroke as a single integer: the interned code plus modifier bits.
// Comparing two strokes is comparing two `std::uint32_t`.
class CompiledStroke {
public:
    CompiledStroke() = default;
    CompiledStroke(StrokeCode code, bool control, bool alt, bool meta,
                   bool shift) noexcept
        : bits_{static_cast<std::uint32_t>(code) << 4U |
                (control ? 1U : 0U) | (alt ? 2U : 0U) | (meta ? 4U : 0U) |
                (shift ? 8U : 0U)} {}

    [[nodiscard]] bool known() const noexcept {
        return (bits_ >> 4U) != kUnknownStroke;
    }

    bool operator==(CompiledStroke const&) const noexcept = default;

private:
    std::uint32_t bits_ = 0;
};

using CompiledSequence = std::vector<CompiledStroke>;

struct CompiledResolution {
    KeymapMatchKind kind = KeymapMatchKind::None;
    // Valid only when `kind` is Resolved.
    CommandHandle command;
};

class CompiledKeymap {
public:
    explicit CompiledKeymap(KeymapViewState const& keymap);

    CompiledKeymap(CompiledKeymap const&) = delete;
    CompiledKeymap& operator=(CompiledKeymap const&) = delete;

    // Interning a stroke is the single string lookup left on the keystroke
    // path.  An unmentioned key interns to `kUnknownStroke` rather than
    // failing, so a printable is a normal (unmatched) stroke, not an error.
    [[nodiscard]] CompiledStroke intern(KeyStroke const& stroke) const;
    [[nodiscard]] ContextId contextFor(std::string_view name) const;

    // Integer-only resolution: the same first-eligible-match, "*"-upgrades and
    // strict-prefix-is-pending rules KeymapMatcher applies to strings.
    [[nodiscard]] CompiledResolution resolve(
        std::span<CompiledStroke const> pending, ContextId context) const;

private:
    struct Entry {
        CompiledSequence sequence;
        CommandHandle command;
        ContextId context = kUnknownContext;
    };

    [[nodiscard]] StrokeCode codeFor(std::string_view name) const;

    std::unordered_map<std::string, StrokeCode> strokeCodes_;
    std::unordered_map<std::string, ContextId> contexts_;
    std::vector<Entry> entries_;
};

}  // namespace ssg
