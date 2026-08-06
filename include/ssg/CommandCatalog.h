#pragma once

// Every command the running editor offers.
//
// The catalog is fully dynamic: commands are added at runtime, `add` issues the
// handle, and there is no point at which the catalog is declared complete.  It
// never will be -- plugins and Lua extensions may register commands at any time
// -- so completeness is not a property this design tries to have
// (doc/spec-command-registry.md).
//
// Consequences worth knowing before using it:
//
//   * Registration is APPEND-ONLY and a handle is never reused.  Commands live
//     in stable storage, so a `CommandSpec const*` or `CommandRegistration
//     const*` obtained from the catalog stays valid for the life of the
//     process, even across later registrations.  Callers therefore need no
//     lifetime discipline.
//   * `add` takes the catalog's lock exclusively; reads take it shared.  This
//     matters because protocol decode runs off the session lock, on connection
//     threads, and may be concurrent with a registration.
//   * The catalog carries a REVISION, bumped on every `add`.  In-process
//     consumers hold the catalog and so never go stale; a client holds a
//     projection (its compiled keymap) and refreshes it when the revision
//     changes.

#include <ssg/CommandHandle.h>
#include <ssg/CommandSpecBuilder.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ssg {

// A registered command: what it is, and what runs it.  Stored in a std::deque
// so that references remain valid as the catalog grows.
struct CommandEntry {
    std::string id;
    std::string owner;
    // Empty means "humanise the id" (see commandLabel).
    std::string label;
    std::string summary;
    CommandEffect effect = CommandEffect::Mutation;
    // Validated CapabilityId, not raw text: a capability is checked when the
    // command is registered, so dispatch cannot be handed one that throws.
    std::vector<CapabilityId> requiredCapabilities;
    bool luaApi = false;
    bool initScript = false;
    CommandArgumentType argument;
    CommandHandler handler;
    // A tombstoned command is no longer dispatchable but keeps its slot, so its
    // handle is never reissued to a different command.
    bool retired = false;

    // The palette's display text for this command: its authored `label`, or the
    // humanised id ("cursor.line_down" -> "Cursor Line Down") when `label` is
    // empty, so a candidate never shows a raw dotted id. Takes no id lookup --
    // the entry IS the command, so an author's label can never be lost to a
    // fallback the way an id-only lookup against one catalog could.
    [[nodiscard]] std::string displayLabel() const;
};

// Bumped on every registration.  A client compares it to decide whether the
// projection it holds -- its compiled keymap -- needs rebuilding.
using CatalogRevision = std::uint64_t;

class CommandCatalog {
public:
    // A handle is a 16-bit index into this catalog, so this is how many
    // commands may exist at once.  Registration past it throws rather than
    // wrapping a handle onto another command.
    static constexpr std::size_t kMaximumCommands = 65535;

    CommandCatalog();
    ~CommandCatalog();

    CommandCatalog(CommandCatalog const&) = delete;
    CommandCatalog& operator=(CommandCatalog const&) = delete;

    // Registers one command and returns its handle.
    //
    // Throws std::runtime_error when the id is already registered (naming both
    // owners) or when a required field is missing.  Every check lives here
    // because a builder passed to `add` is finished by definition.
    CommandHandle add(CommandSpecBuilder spec);

    // Swaps one set of commands for another, atomically.
    //
    // Retires every command in `retire` and registers everything in `add`,
    // under a single exclusive lock, so no reader can observe the catalog with
    // the old set gone and the new one not yet present -- which for a script
    // reload would be a palette momentarily listing none of the user's commands
    // (doc/spec-lua-commands.md).
    //
    // ALL OR NOTHING.  Every addition is validated, and the batch checked
    // against the handle space, BEFORE anything is retired or added.  A batch
    // containing one bad spec therefore leaves the previous set registered and
    // working, rather than removing it and then failing to install its
    // replacement.  (Allocation failure is out of scope, as everywhere else in
    // this codebase.)
    //
    // Retiring frees a command's NAME while keeping its slot: the id may be
    // registered again -- receiving a NEW handle -- while the retired handle
    // stays permanently dead, so a stale handle resolves to nothing rather than
    // to a different command (doc/spec-command-registry.md, R4).
    //
    // Returns the handles of the added commands, in order.
    std::vector<CommandHandle> replaceGeneration(
        std::span<CommandHandle const> retire,
        std::vector<CommandSpecBuilder> add);

    [[nodiscard]] CatalogRevision revision() const;

    // Stable for the life of the process; nullptr when unknown or retired.
    [[nodiscard]] CommandEntry const* find(std::string_view id) const;
    [[nodiscard]] CommandEntry const* find(CommandHandle command) const;
    [[nodiscard]] CommandHandle handleFor(std::string_view id) const;

    // Live commands, in registration order.
    [[nodiscard]] std::vector<CommandEntry const*> commands() const;
    [[nodiscard]] std::vector<CommandEntry const*> ownedBy(
        std::string_view owner) const;
    [[nodiscard]] std::size_t size() const;

private:
    struct ValidatedSpec;

    // Everything that can refuse a registration.  Shared by add and
    // replaceGeneration so a check cannot be added to one and forgotten in the
    // other.  Caller holds the lock.
    [[nodiscard]] ValidatedSpec validate(
        CommandSpecBuilder spec,
        std::unordered_set<std::string> const& alsoTaken,
        std::unordered_set<std::string> const& beingFreed) const;
    CommandHandle appendValidated(ValidatedSpec spec);
    void requireCapacity(std::size_t additional) const;

    // Stable storage: a deque never moves an element it already holds, so a
    // borrowed pointer survives every later add (see the header comment).
    std::deque<CommandEntry> entries_;
    std::unordered_map<std::string, std::size_t> byId_;
    CatalogRevision revision_ = 0;
    mutable std::shared_mutex mutex_;
};

}  // namespace ssg
