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
#include <optional>
#include <span>
#include <vector>

namespace ssg {

// A binding's context, compiled.  A keymap context is already a closed set --
// `"*"` plus the FocusTarget names (focus.h) -- so a compiled binding stores the
// focus it applies to, the global context, or `Never` for a name outside that
// set.  Resolution then takes the FocusTarget the client already holds, and no
// context name is built, hashed or compared on the keystroke path.
//
// `Never` matters: an unrecognised context must match no keystroke, exactly as
// KeymapMatcher's name comparison does.  Folding it into the global context
// would turn a rejected binding into a binding active everywhere.
class CompiledContext {
public:
    static constexpr CompiledContext any() noexcept {
        return CompiledContext{kAny};
    }
    static constexpr CompiledContext never() noexcept {
        return CompiledContext{kNever};
    }
    static constexpr CompiledContext of(FocusTarget focus) noexcept {
        return CompiledContext{static_cast<std::int8_t>(focus)};
    }

    [[nodiscard]] constexpr bool isAny() const noexcept {
        return value_ == kAny;
    }
    [[nodiscard]] constexpr bool eligibleIn(FocusTarget focus) const noexcept {
        return value_ == kAny || value_ == static_cast<std::int8_t>(focus);
    }

    bool operator==(CompiledContext const&) const noexcept = default;

private:
    static constexpr std::int8_t kAny = -1;
    static constexpr std::int8_t kNever = -2;

    explicit constexpr CompiledContext(std::int8_t value) noexcept
        : value_{value} {}

    std::int8_t value_ = kNever;
};

// One keystroke as a single integer: the decoder's KeyCode plus modifier bits.
// Comparing two strokes is comparing two `std::uint32_t`.
class CompiledStroke {
public:
    CompiledStroke() = default;
    explicit CompiledStroke(KeyStroke const& stroke) noexcept
        : bits_{static_cast<std::uint32_t>(stroke.code) << 4U |
                (stroke.control ? 1U : 0U) | (stroke.alt ? 2U : 0U) |
                (stroke.meta ? 4U : 0U) | (stroke.shift ? 8U : 0U)} {}

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

    // Compiling a stroke is now pure arithmetic: the decoder already produced
    // the key's identity, so there is nothing left to look up.
    [[nodiscard]] static CompiledStroke compile(KeyStroke const& stroke) noexcept {
        return CompiledStroke{stroke};
    }

    // Integer-only resolution: the same first-eligible-match, "*"-upgrades and
    // strict-prefix-is-pending rules KeymapMatcher applies to strings.
    [[nodiscard]] CompiledResolution resolve(
        std::span<CompiledStroke const> pending, FocusTarget focus) const;

private:
    struct Entry {
        CompiledSequence sequence;
        CommandHandle command;
        CompiledContext context = CompiledContext::never();
    };

    std::vector<Entry> entries_;
};

}  // namespace ssg
