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
#include <string>
#include <string_view>
#include <unordered_map>
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
    std::vector<std::string> requiredCapabilities;
    bool luaApi = false;
    bool initScript = false;
    CommandArgumentType argument;
    CommandHandler handler;
    // A tombstoned command is no longer dispatchable but keeps its slot, so its
    // handle is never reissued to a different command.
    bool retired = false;
};

// Bumped on every registration.  A client compares it to decide whether the
// projection it holds -- its compiled keymap -- needs rebuilding.
using CatalogRevision = std::uint64_t;

class CommandCatalog {
public:
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
    // Stable storage: a deque never moves an element it already holds, so a
    // borrowed pointer survives every later add (see the header comment).
    std::deque<CommandEntry> entries_;
    std::unordered_map<std::string, std::size_t> byId_;
    CatalogRevision revision_ = 0;
    mutable std::shared_mutex mutex_;
};

}  // namespace ssg
