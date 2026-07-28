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

// A focus context interned against one keymap.  `kAnyContext` is "*".
using ContextId = std::uint16_t;
inline constexpr ContextId kUnknownContext = 0;
inline constexpr ContextId kAnyContext = 1;

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

    std::unordered_map<std::string, ContextId> contexts_;
    std::vector<Entry> entries_;
};

}  // namespace ssg
