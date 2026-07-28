#pragma once

// A command's identity as an integer.
//
// Deliberately a header of its own: the keystroke path, the registry and the
// session all need to NAME a command without depending on the catalog's data
// (Commands.h), which pulls in every command's documentation and argument
// shape.  Identity is small; the catalog is not.

#include <cstdint>
#include <limits>
#include <string_view>

namespace ssg {

struct CommandSpec;

// The catalog is compiled, fixed and ordered, so a command id string is only
// one NAME for a row that already has a perfectly good index.  Names are what
// the palette, `init.lua` and the wire protocol need; the keystroke path needs
// identity, and pays for a name it never reads.  A handle is that identity --
// trivially copyable, compared as an integer, and resolvable back to its spec
// (and so its id) whenever a boundary does want the string.
//
// Handles are minted by resolving a name ONCE, at build or keymap-compile time,
// and are then carried by value down the hot path.
class CommandHandle {
public:
    CommandHandle() = default;

    [[nodiscard]] bool valid() const noexcept { return index_ != kInvalid; }
    [[nodiscard]] std::size_t index() const noexcept { return index_; }
    // The catalog row, or nullptr when invalid.
    [[nodiscard]] CommandSpec const* spec() const noexcept;
    // The name this handle stands for; empty when invalid.
    [[nodiscard]] std::string_view id() const noexcept;

    bool operator==(CommandHandle const&) const noexcept = default;

private:
    friend CommandHandle commandHandle(std::string_view id) noexcept;

    static constexpr std::uint16_t kInvalid =
        std::numeric_limits<std::uint16_t>::max();

    explicit CommandHandle(std::uint16_t index) noexcept : index_{index} {}

    std::uint16_t index_ = kInvalid;
};

// Resolve a command name to its handle.  This is the one place a command id
// string is matched; call it at construction time, not per keystroke.
[[nodiscard]] CommandHandle commandHandle(std::string_view id) noexcept;

}  // namespace ssg
