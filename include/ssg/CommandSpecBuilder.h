#pragma once

// A command, described where it is implemented.
//
// `CommandSpecBuilder` is a free-standing value: it is not obtained from the
// catalog and the catalog does not own it.  A component describes one command
// by chaining on the builder and hands the finished description to
// `CommandCatalog::add`.  Because a builder passed to `add` is finished by
// definition, passing it IS the completion signal -- which is why the catalog
// needs no seal, finalise or build step.
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
#include <ssg/CommandInvocation.h>

#include <any>
#include <functional>
#include <optional>
#include <stdexcept>
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
        setEffect(CommandEffect::Mutation, CommandRevisionPolicy::Exact);
        return *this;
    }
    CommandSpecBuilder& observes() {
        setEffect(CommandEffect::Observation, CommandRevisionPolicy::Exact);
        return *this;
    }
    CommandSpecBuilder& stateValidatedMutation() {
        setEffect(CommandEffect::Mutation,
                  CommandRevisionPolicy::StateValidated);
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

    // The implementation, taking a typed argument that crosses the wire and may
    // be absent.
    //
    // Completes the pair with `handler`: some wire-carried commands have a
    // meaningful default -- `replace.workspace_apply` applies the preview it
    // already holds when given none -- and demanding a payload would refuse a
    // call that used to work.
    template <typename Arguments, typename Fn>
    CommandSpecBuilder& optionalHandler(Fn&& fn) {
        argument_.type = std::type_index{typeid(Arguments)};
        argument_.wire = true;
        handler_ = optionalTypedHandler<Arguments>(std::forward<Fn>(fn));
        return *this;
    }

    // The implementation, taking a typed argument that may be absent.
    //
    // Some in-process commands act on a specific thing when told which and on
    // the current one otherwise: `tab.close` closes the tab you name, or the
    // active tab.  The handler receives an engaged optional only when a payload
    // of the right type was supplied, so "no payload" and "wrong payload" stay
    // distinguishable from a real value.
    template <typename Arguments, typename Fn>
    CommandSpecBuilder& optionalInProcessHandler(Fn&& fn) {
        argument_.type = std::type_index{typeid(Arguments)};
        argument_.wire = false;
        handler_ = optionalTypedHandler<Arguments>(std::forward<Fn>(fn));
        return *this;
    }


    // The implementation, taking the payload untouched.
    //
    // For a handler that stands in for many commands at once and must accept
    // whatever each of them carries -- a test fixture mirroring the real
    // catalog, or a forwarding wrapper.  `wireType` states what the protocol
    // should carry, so a stand-in encodes as the command it stands for.
    //
    // A command that knows its own argument uses the typed forms instead: they
    // are what make a handler and its codec impossible to disagree, and this
    // gives that up.
    CommandSpecBuilder& untypedHandler(
        CommandHandler handler, std::optional<std::type_index> wireType) {
        argument_.type = wireType;
        argument_.wire = wireType.has_value();
        handler_ = std::move(handler);
        return *this;
    }

private:
    void setEffect(CommandEffect effect, CommandRevisionPolicy policy) {
        if (effect_ &&
            (*effect_ != effect || *revisionPolicy_ != policy)) {
            throw std::logic_error(
                "command effect and revision policy conflict");
        }
        effect_ = effect;
        revisionPolicy_ = policy;
    }

    friend class CommandCatalog;

    // Refuses a payload of the wrong type, and a missing one: the command said
    // it needs an argument.
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

    // Absent means absent, and a payload of the wrong type is still an error.
    //
    // Testing only whether the cast succeeded would collapse the two, so a
    // caller that sent the wrong type would be served the default instead of
    // being told.  The presence of ANY payload is what separates them.
    template <typename Arguments, typename Fn>
    static CommandHandler optionalTypedHandler(Fn&& fn) {
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

    std::string id_;
    std::string owner_;
    std::string label_;
    std::string summary_;
    std::optional<CommandEffect> effect_;
    std::optional<CommandRevisionPolicy> revisionPolicy_;
    std::vector<std::string> capabilities_;
    bool luaApi_ = false;
    bool initScript_ = false;
    CommandArgumentType argument_;
    CommandHandler handler_;
};

}  // namespace ssg
