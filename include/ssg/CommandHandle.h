#pragma once

// A command's identity as an integer, and how a caller names one.
//
// Deliberately a header of its own: the keystroke path, the registry and the
// session all need to NAME a command without depending on the catalog's data,
// which pulls in every command's documentation and argument shape.  Identity is
// small; the catalog is not.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace ssg {

// A command's position in the catalog that issued it.
//
// A command id string is one NAME for a row that already has a perfectly good
// index.  Names are what the palette, `init.lua` and the wire protocol need;
// the keystroke path needs identity, and pays for a name it never reads.  A
// handle is that identity -- trivially copyable and compared as an integer.
//
// PROCESS-LOCAL.  A handle means whatever the catalog that issued it says, and
// registration order varies with which components and plugins registered.  It
// is never persisted, never sent on the wire, and never compared across
// processes.  The index is not publicly
// readable, and there is deliberately no `toValue` overload for it, so encoding
// one does not compile.
class CommandHandle {
public:
    CommandHandle() = default;

    [[nodiscard]] bool valid() const noexcept { return index_ != kInvalid; }

    bool operator==(CommandHandle const&) const noexcept = default;

private:
    friend class CommandCatalog;

    static constexpr std::uint16_t kInvalid =
        std::numeric_limits<std::uint16_t>::max();

    // Only a catalog may say what a handle means, so minting one stays private to
    // the catalog machinery (its two friends); a public caller cannot fabricate a
    // handle that aliases a command.
    explicit CommandHandle(std::uint16_t index) noexcept : index_{index} {}

    [[nodiscard]] std::size_t index() const noexcept { return index_; }

    std::uint16_t index_ = kInvalid;
};

// How a caller names the command it wants to invoke.
//
// Always carries the NAME, because that is what a rejected dispatch must report
// and what every boundary outside the editor speaks.  It may additionally carry
// a handle, which a caller that already resolved the command supplies so
// dispatch costs an array index instead of a hash of a constructed string.
//
// The name is authoritative: a CommandName with an absent handle still names the
// right command.  It never resolves a name on its own, because there is no global
// catalog to resolve it against -- resolution belongs to whoever holds one.
class CommandName {
public:
    CommandName() = default;

    // Implicit: a command name is the ordinary way to name a command, and every
    // call site outside the keystroke path spells one as a literal.
    CommandName(std::string_view name)  // NOLINT(google-explicit-constructor)
        : name_{name} {}
    CommandName(char const* name)  // NOLINT(google-explicit-constructor)
        : name_{name} {}
    CommandName(std::string name)  // NOLINT(google-explicit-constructor)
        : name_{std::move(name)} {}

    // Both spellings, for a caller that already resolved the command against a
    // catalog -- the compiled keymap does this once per binding.
    CommandName(std::string name, CommandHandle handle)
        : name_{std::move(name)}, handle_{handle} {}

    [[nodiscard]] CommandHandle handle() const noexcept { return handle_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    [[nodiscard]] bool empty() const noexcept { return name_.empty(); }

    // Two names are equal when they name the same command.  A literal converts,
    // so a call site asking "is this file.open?" reads as it always did.
    [[nodiscard]] bool operator==(CommandName const& other) const noexcept {
        return name_ == other.name_;
    }

private:
    std::string name_;
    CommandHandle handle_;
};

}  // namespace ssg
