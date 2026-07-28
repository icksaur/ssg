#pragma once

// A command, described where it is implemented.
//
// `CommandSpecBuilder` is a free-standing value: it is not obtained from the
// catalog and the catalog does not own it.  A component describes one command
// by chaining on the builder and hands the finished description to
// `CommandCatalog::add`.  Because a builder passed to `add` is finished by
// definition, passing it IS the completion signal -- which is why the catalog
// needs no seal, finalise or build step (doc/spec-command-registry.md).
//
//     catalog.add(CommandSpecBuilder{"text.insert"}
//                     .owner("text-input-commands")
//                     .summary("Insert text at every cursor")
//                     .mutates()
//                     .lua()
//                     .handler<TextInputArguments>(
//                         [&impl](CommandContext& context,
//                                 TextInputArguments const& text) {
//                             return impl.insertText(context, text);
//                         }));
//
// The builder is always valid and possibly incomplete.  It tracks no state and
// needs no terminator; completeness is `add`'s concern, not its own.

#include <ssg/CommandHandle.h>
#include <ssg/CommandRegistry.h>

#include <any>
#include <functional>
#include <optional>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace ssg {

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
};

class CommandSpecBuilder {
public:
    explicit CommandSpecBuilder(std::string id) : id_{std::move(id)} {}

    CommandSpecBuilder& owner(std::string owner) {
        owner_ = std::move(owner);
        return *this;
    }
    CommandSpecBuilder& label(std::string label) {
        label_ = std::move(label);
        return *this;
    }
    CommandSpecBuilder& summary(std::string summary) {
        summary_ = std::move(summary);
        return *this;
    }
    CommandSpecBuilder& mutates() {
        effect_ = CommandEffect::Mutation;
        return *this;
    }
    CommandSpecBuilder& observes() {
        effect_ = CommandEffect::Observation;
        return *this;
    }
    CommandSpecBuilder& capability(std::string capability) {
        capabilities_.push_back(std::move(capability));
        return *this;
    }
    // Eligible for the versioned Lua API (I20).
    CommandSpecBuilder& lua() {
        luaApi_ = true;
        return *this;
    }
    // `init.lua` at startup may call this command.  A separate axis from `lua`:
    // this is a grant to one host, that is API eligibility.  Implies `lua`.
    CommandSpecBuilder& initScript() {
        luaApi_ = true;
        initScript_ = true;
        return *this;
    }

    // The implementation, taking a typed argument that crosses the wire.
    //
    // The argument type is recorded here and nowhere else: the codec and the
    // unwrap are both derived from it, so a handler and its codec cannot
    // disagree.  The type must have a wire codec; if it does not, the command
    // wanted `inProcessHandler` instead.
    template <typename Arguments, typename Fn>
    CommandSpecBuilder& handler(Fn&& fn) {
        argument_.type = std::type_index{typeid(Arguments)};
        argument_.wire = true;
        handler_ = typedHandler<Arguments>(std::forward<Fn>(fn));
        return *this;
    }

    // The implementation, taking a typed argument that does NOT cross the wire.
    //
    // Some commands are only ever invoked in-process -- `theme.define` carries
    // a whole colour table from `init.lua`, `diff.open_file` a live document id
    // -- and their payloads have no wire representation.  Declaring them with
    // `handler` would demand a codec that should not exist; declaring them with
    // the no-argument overload would silently drop the payload.  This states
    // the third case: typed for the handler, absent from the protocol.
    template <typename Arguments, typename Fn>
    CommandSpecBuilder& inProcessHandler(Fn&& fn) {
        argument_.type = std::type_index{typeid(Arguments)};
        argument_.wire = false;
        handler_ = typedHandler<Arguments>(std::forward<Fn>(fn));
        return *this;
    }

    // The implementation of a command that takes no arguments.
    template <typename Fn>
    CommandSpecBuilder& handler(Fn&& fn) {
        argument_ = {};
        handler_ = [call = std::forward<Fn>(fn)](
                       CommandContext& context,
                       std::any const&) -> CommandHandlerResult {
            return call(context);
        };
        return *this;
    }

    // Migration only: adopt an already type-erased handler together with the
    // argument type it expects.  Used while commands still come from the static
    // table in Commands.cpp, where the handler was bound separately from the
    // declaration.  Deleted with that table (doc/spec-command-registry.md, D5);
    // a component that has migrated calls `handler<Args>` instead, which is the
    // whole point -- there the type is deduced rather than asserted.
    CommandSpecBuilder& adoptBoundHandler(
        CommandHandler handler, std::optional<std::type_index> argumentType) {
        argument_.type = argumentType;
        argument_.wire = argumentType.has_value();
        handler_ = std::move(handler);
        return *this;
    }

private:
    friend class CommandCatalog;

    template <typename Arguments, typename Fn>
    static CommandHandler typedHandler(Fn&& fn) {
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

    std::string id_;
    std::string owner_;
    std::string label_;
    std::string summary_;
    std::optional<CommandEffect> effect_;
    std::vector<std::string> capabilities_;
    bool luaApi_ = false;
    bool initScript_ = false;
    CommandArgumentType argument_;
    CommandHandler handler_;
};

}  // namespace ssg
