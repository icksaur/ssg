#pragma once

// Every command the running editor offers.
//
// The catalog is fully dynamic: commands are added at runtime, `add` issues the
// handle, and there is no point at which the catalog is declared complete.  It
// never will be -- plugins and Lua extensions may register commands at any time
// -- so completeness is not a property this design tries to have
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
#include <ssg/ViewAction.h>

#include <ssg/types.h>

#include <any>
#include <compare>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ssg {

struct WorkspaceId {
    explicit constexpr WorkspaceId(std::uint64_t value = 0) noexcept
        : value_{value} {}
    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }
    constexpr auto operator<=>(WorkspaceId const&) const noexcept = default;

private:
    std::uint64_t value_;
};

class CommandCatalog;
struct CommandHandlerResult;

class CommandContext {
public:
    void setActiveWorkspace(WorkspaceId workspace) noexcept;

private:
    friend class CommandCatalog;

    CommandContext() = default;

    bool workspaceChanged_{false};
    WorkspaceId activeWorkspace_;
};

enum class CommandEffect : std::uint8_t {
    Observation,
    Mutation,
    ViewAction,
    Routing,
};

struct CommandHandlerResult {
    bool accepted;
    std::string message;
    std::optional<ViewAction> viewAction;

    [[nodiscard]] static CommandHandlerResult success();
    [[nodiscard]] static CommandHandlerResult failure(std::string message);
    [[nodiscard]] static CommandHandlerResult requireView(ViewAction action);
};

using CommandHandler =
    std::function<CommandHandlerResult(CommandContext&, std::any const&)>;

struct ClientCommand {
    // The command to invoke, named however the caller most cheaply can: a name
    // at the protocol, Lua and palette boundaries, a handle on the keystroke
    // path.  One field, so a dispatch cannot carry two different commands.
    CommandName id;
    std::any payload;
};

enum class CommandError : std::uint8_t {
    None,
    UnknownCommand,
    HandlerFailed,
};

struct CommandResult {
    CommandError error;
    std::string message;
    std::optional<ViewAction> viewAction;

    enum class Outcome : std::uint8_t {
        Completed,
        ViewActionRequired,
        Rejected,
    };

    [[nodiscard]] bool accepted() const noexcept {
        return error == CommandError::None;
    }
    [[nodiscard]] bool completed() const noexcept {
        return outcome() == Outcome::Completed;
    }
    [[nodiscard]] Outcome outcome() const noexcept {
        if (error != CommandError::None) return Outcome::Rejected;
        return viewAction ? Outcome::ViewActionRequired : Outcome::Completed;
    }
};

struct CatalogDispatchResult {
    CommandError error;
    std::string message;
    std::optional<ViewAction> viewAction;
    std::optional<WorkspaceId> activeWorkspace;

    [[nodiscard]] bool accepted() const noexcept {
        return error == CommandError::None;
    }
};

inline constexpr std::string_view kNestedDispatchRefusal =
    "a command handler may not dispatch another command directly; ask for "
    "it instead, so commands remain serialized";

// How a command's argument is carried across the wire.  Registered with the
// argument type rather than named by an enum, so a handler cannot disagree with
// its codec: there is one type, written once, at the handler.
struct CommandArgumentType {
    // The payload's type, for the codec registry to key on.  Absent for a
    // command that takes no arguments -- which is a declaration, not a default.
    std::optional<std::type_index> type;
    // Whether that payload crosses the wire.  A typed payload that does not is
    // in-process only: the handler unwraps it, the protocol never sees it, and
    // no codec is required for its type.
    bool wire = false;
    // Required payloads cannot be invoked from a command palette, which has no
    // argument value to supply.
    bool required = false;
};

// A command's argument type and the handler that consumes it, always produced
// together by one of the `bind*Handler` functions below so a handler and its
// argument metadata cannot disagree.
struct CommandHandlerBinding {
    CommandArgumentType argument;
    CommandHandler handler;
};

namespace detail {

// Refuses a payload of the wrong type, and a missing one: the command said it
// needs an argument.
template <typename Arguments, typename Fn>
CommandHandler makeTypedHandler(Fn&& fn) {
    return [call = std::forward<Fn>(fn)](
               CommandContext& context,
               std::any const& payload) -> CommandHandlerResult {
        auto const* typed = std::any_cast<Arguments>(&payload);
        if (typed == nullptr) {
            return CommandHandlerResult::failure(
                "command payload has the wrong type");
        }
        return call(context, *typed);
    };
}

// Absent means absent, and a payload of the wrong type is still an error.
//
// Testing only whether the cast succeeded would collapse the two, so a caller
// that sent the wrong type would be served the default instead of being told.
// The presence of ANY payload is what separates them.
template <typename Arguments, typename Fn>
CommandHandler makeOptionalTypedHandler(Fn&& fn) {
    return [call = std::forward<Fn>(fn)](
               CommandContext& context,
               std::any const& payload) -> CommandHandlerResult {
        auto const* typed = std::any_cast<Arguments>(&payload);
        if (typed == nullptr && payload.has_value()) {
            return CommandHandlerResult::failure(
                "command payload has the wrong type");
        }
        return call(context, typed == nullptr
                                 ? std::optional<Arguments>{}
                                 : std::optional<Arguments>{*typed});
    };
}

}  // namespace detail

// The implementation, taking a typed argument that crosses the wire.
//
// The argument type is recorded here and nowhere else: the codec and the
// unwrap are both derived from it, so a handler and its codec cannot disagree.
// The type must have a wire codec; if it does not, the command wanted
// `bindInProcessHandler` instead.
template <typename Arguments, typename Fn>
CommandHandlerBinding bindWireHandler(Fn&& fn) {
    return {{std::type_index{typeid(Arguments)}, true, true},
            detail::makeTypedHandler<Arguments>(std::forward<Fn>(fn))};
}

// The implementation, taking a typed argument that does NOT cross the wire.
//
// Some commands are only ever invoked in-process -- `theme.define` carries a
// whole colour table from `init.lua`, `diff.open_file` a live document id --
// and their payloads have no wire representation.  Binding them with
// `bindWireHandler` would demand a codec that should not exist; binding them
// with `bindNoArgumentHandler` would silently drop the payload.  This states
// the third case: typed for the handler, absent from the protocol.
template <typename Arguments, typename Fn>
CommandHandlerBinding bindInProcessHandler(Fn&& fn) {
    return {{std::type_index{typeid(Arguments)}, false, true},
            detail::makeTypedHandler<Arguments>(std::forward<Fn>(fn))};
}

// The implementation, taking a typed argument that crosses the wire and may be
// absent.
//
// Completes the pair with `bindWireHandler`: some wire-carried commands have a
// meaningful default -- `replace.workspace_apply` applies the preview it
// already holds when given none -- and demanding a payload would refuse a call
// that used to work.
template <typename Arguments, typename Fn>
CommandHandlerBinding bindOptionalWireHandler(Fn&& fn) {
    return {{std::type_index{typeid(Arguments)}, true, false},
            detail::makeOptionalTypedHandler<Arguments>(std::forward<Fn>(fn))};
}

// The implementation, taking a typed argument that may be absent and never
// crosses the wire.
//
// Some in-process commands act on a specific thing when told which and on the
// current one otherwise: `tab.close` closes the tab you name, or the active
// tab.  The handler receives an engaged optional only when a payload of the
// right type was supplied, so "no payload" and "wrong payload" stay
// distinguishable from a real value.
template <typename Arguments, typename Fn>
CommandHandlerBinding bindOptionalInProcessHandler(Fn&& fn) {
    return {{std::type_index{typeid(Arguments)}, false, false},
            detail::makeOptionalTypedHandler<Arguments>(std::forward<Fn>(fn))};
}

// The implementation of a command that takes no arguments.
template <typename Fn>
CommandHandlerBinding bindNoArgumentHandler(Fn&& fn) {
    return {{std::nullopt, false, false},
            [call = std::forward<Fn>(fn)](
                CommandContext& context,
                std::any const&) -> CommandHandlerResult {
                return call(context);
            }};
}

// The implementation, taking the payload untouched.
//
// For a handler that stands in for many commands at once and must accept
// whatever each of them carries -- a test fixture mirroring the real catalog,
// or a forwarding wrapper.  `wireType` states what the protocol should carry,
// so a stand-in encodes as the command it stands for.
//
// A command that knows its own argument uses the typed forms instead: they are
// what make a handler and its codec impossible to disagree, and this gives
// that up.
inline CommandHandlerBinding bindUntypedHandler(
    CommandHandler handler, std::optional<std::type_index> wireType,
    bool required) {
    return {{wireType, wireType.has_value(), required}, std::move(handler)};
}

// A command, described where it is implemented, and handed to
// `CommandCatalog::add` or `replaceGeneration`.  Aggregate-initialized, most
// often with designated initializers:
//
//     catalog.add(CommandSpec{
//         .id = "text.insert",
//         .owner = "text-input-commands",
//         .summary = "Insert text at every cursor",
//         .effect = CommandEffect::Mutation,
//         .luaApi = true,
//         .binding = bindWireHandler<TextInputArguments>(
//             [&impl](CommandContext& context, TextInputArguments const& text) {
//                 return impl.insertText(context, text);
//             }),
//     });
struct CommandSpec {
    std::string id;
    std::string owner;
    std::string label;
    std::string summary;
    // No default: "nobody decided" must not be shippable, so `add` rejects a
    // spec that leaves this unset.
    std::optional<CommandEffect> effect;
    // Eligible for the versioned Lua API (I20).
    bool luaApi = false;
    // `init.lua` at startup may call this command.  A separate axis from
    // `luaApi`: this is a grant to one host, that is API eligibility.  Implies
    // `luaApi`; `CommandCatalog::add` treats the two as never disagreeing, so
    // a spec need not set `luaApi` itself to also set this.
    bool initScript = false;
    CommandHandlerBinding binding;
};

// A registered command: what it is, and what runs it.  Stored in a std::deque
// so that references remain valid as the catalog grows.
struct CommandEntry {
    std::string id;
    std::string owner;
    // Empty means "humanise the id" (see commandLabel).
    std::string label;
    std::string summary;
    CommandEffect effect = CommandEffect::Mutation;
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
    // owners) or when a required field is missing.
    CommandHandle add(CommandSpec spec);

    // Swaps one set of commands for another as one all-or-nothing operation.
    //
    // Validates everything in `add` before retiring any command, so a script
    // reload cannot leave the old set gone and the new one partly installed.
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
    // to a different command.
    //
    // Returns the handles of the added commands, in order.
    std::vector<CommandHandle> replaceGeneration(
        std::span<CommandHandle const> retire,
        std::vector<CommandSpec> add);

    [[nodiscard]] CatalogRevision revision() const;
    [[nodiscard]] bool dispatchInProgress() const noexcept;
    [[nodiscard]] CatalogDispatchResult dispatch(ClientCommand const& command);

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
    // other.
    [[nodiscard]] ValidatedSpec validate(
        CommandSpec spec,
        std::unordered_set<std::string> const& alsoTaken,
        std::unordered_set<std::string> const& beingFreed) const;
    CommandHandle appendValidated(ValidatedSpec spec);
    void requireCapacity(std::size_t additional) const;

    // Stable storage: a deque never moves an element it already holds, so a
    // borrowed pointer survives every later add (see the header comment).
    std::deque<CommandEntry> entries_;
    std::unordered_map<std::string, std::size_t> byId_;
    CatalogRevision revision_ = 0;
    bool dispatching_ = false;
};

}  // namespace ssg
