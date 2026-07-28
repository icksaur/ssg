#pragma once

// A command's identity as an integer.
//
// Deliberately a header of its own: the keystroke path, the registry and the
// session all need to NAME a command without depending on the catalog's data
// (Commands.h), which pulls in every command's documentation and argument
// shape.  Identity is small; the catalog is not.

#include <cstdint>
#include <limits>
#include <string>
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
    friend CommandHandle commandHandleFromIndex(std::size_t index) noexcept;

    static constexpr std::uint16_t kInvalid =
        std::numeric_limits<std::uint16_t>::max();

    explicit CommandHandle(std::uint16_t index) noexcept : index_{index} {}

    std::uint16_t index_ = kInvalid;
};

// Resolve a command name to its handle.  This is the one place a command id
// string is matched; call it at construction time, not per keystroke.
[[nodiscard]] CommandHandle commandHandle(std::string_view id) noexcept;

// Mint a handle for a catalog position.  Only a catalog may say what a handle
// means, which is why this is not a public constructor.
[[nodiscard]] CommandHandle commandHandleFromIndex(std::size_t index) noexcept;

// How a caller names the command it wants to invoke.
//
// A command has two spellings -- a name and a handle -- and a dispatch target
// that carried BOTH as independent fields could carry two different commands,
// with no principled answer as to which wins.  CommandRef holds ONE identity
// and derives the other, so they cannot disagree:
//
//   * built from a name, it resolves the handle once, here;
//   * built from a handle, the name comes free from the catalog.
//
// Either way `name()` is always available, so a rejected dispatch can say which
// command it rejected.  A name outside the catalog keeps its text and yields an
// invalid handle, which is exactly the unknown-command case.
class CommandRef {
public:
    CommandRef() = default;

    // Implicit: a command name is the ordinary way to name a command, and every
    // existing call site spells one as a literal.  The name is retained only
    // when the catalog does not know it -- otherwise the handle is the identity
    // and the name is derived from it, so there is one fact, not two.
    CommandRef(std::string_view name)  // NOLINT(google-explicit-constructor)
        : handle_{commandHandle(name)} {
        if (!handle_.valid()) unknownName_ = std::string{name};
    }
    CommandRef(char const* name)  // NOLINT(google-explicit-constructor)
        : CommandRef{std::string_view{name}} {}
    CommandRef(std::string const& name)  // NOLINT(google-explicit-constructor)
        : CommandRef{std::string_view{name}} {}

    // The keystroke path's spelling: no name is constructed or compared.
    explicit CommandRef(CommandHandle handle) noexcept : handle_{handle} {}

    [[nodiscard]] CommandHandle handle() const noexcept { return handle_; }
    // The command's name, for lookup fallback and for diagnostics.  Never empty
    // for a ref that names any real command.
    [[nodiscard]] std::string_view name() const noexcept {
        return handle_.valid() ? handle_.id() : std::string_view{unknownName_};
    }
    [[nodiscard]] bool empty() const noexcept { return name().empty(); }

    // Two refs are equal when they name the same command.  A literal converts,
    // so a call site asking "is this file.open?" reads as it always did.
    //
    // A catalogued command's handle IS its identity, so the common case is an
    // integer compare and no name is examined; only two uncatalogued refs fall
    // back to comparing text.
    [[nodiscard]] bool operator==(CommandRef const& other) const noexcept {
        if (handle_.valid() || other.handle_.valid()) {
            return handle_ == other.handle_;
        }
        return unknownName_ == other.unknownName_;
    }

private:
    // The text of a name the catalog does not know; empty otherwise, because a
    // known command's name is derived from its handle.
    std::string unknownName_;
    CommandHandle handle_;
};

}  // namespace ssg
