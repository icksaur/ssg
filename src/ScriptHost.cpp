#include <ssg/ScriptHost.h>

#include <ssg/CommandCatalog.h>
#include <ssg/EditorSession.h>
#include <ssg/Keymap.h>
#include <ssg/StatusFields.h>
#include <ssg/Style.h>
#include <ssg/Theme.h>

#include <any>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace ssg {

namespace {

// Capabilities granted to the script client, and to the commands it may call --
// one list, so there is exactly one place to update when a future
// capability-gated script command is added.  Keeping these as two
// independently maintained lists would let one drift out of sync with the
// other: passing one gate but silently denied at the other.  No command needs
// one today, so this is empty -- never a
// wildcard grant.
std::vector<CapabilityId> scriptCapabilities() { return {}; }

// The commands a script may call via ssg.command(id, args).  Asked of the
// running catalog rather than listed here, because whether a command is
// available at startup is a fact about the component that implements it.
std::vector<LuaCommand> scriptCommandCatalog(CommandCatalog const& catalog) {
    std::vector<LuaCommand> result;
    for (auto const* command : catalog.commands()) {
        if (command->initScript) result.push_back({command->id, {}});
    }
    return result;
}

// Reads a string field from a Lua flat-map argument table, or an empty string
// when absent -- keymap.bind/unbind treat an absent "context" as "*" (see
// KeymapBindArguments in Keymap.h), so the caller distinguishes "absent" from
// "empty" only for required fields.
std::string argumentField(
    std::unordered_map<std::string, std::string> const& arguments,
    std::string_view key) {
    auto const it = arguments.find(std::string{key});
    return it == arguments.end() ? std::string{} : it->second;
}

// The live provider ids a composed `ssg.chrome` widget may reference, taken from
// the built-in status-field providers so the two cannot drift: a provider that
// exists at runtime is exactly one a composition may name.
std::vector<std::string> scriptChromeProviders() {
    std::vector<std::string> ids;
    for (auto const& binding : defaultStatusFieldProviders())
        ids.push_back(binding.id);
    return ids;
}

}  // namespace

struct ScriptHost::Impl {
    EditorSession& runtime;
    std::thread::id owningThread{std::this_thread::get_id()};
    LuaCommandHost host;
    ViewActionSink viewActionSink;
    // What the last successful evaluation put in the catalog, retired by the
    // next one.
    std::vector<CommandHandle> generation;

    Impl(EditorSession& editorRuntime, LuaCommandHostOptions options,
         ViewActionSink sink)
        : runtime{editorRuntime},
          host{std::move(options),
               [this](LuaInvocation const& invocation) {
                   return dispatch(invocation);
               }},
          viewActionSink{std::move(sink)} {}

    // Sends one command from `runtime` and reports it the way Lua expects.
    //
    // A script called from a KEYSTROKE is running inside a dispatch, whose
    // session lock its own call holds, so the command is queued to run when
    // that dispatch finishes.  A script evaluated at startup or reload is not
    // inside one, and its commands run immediately -- which is what makes an
    // init.lua of bare ssg.command calls behave as it always has.
    CommandHandlerResult forward(std::string_view id, std::any payload) {
        ClientCommand command{std::string{id}, runtime.revision(),
                              std::move(payload)};
        if (runtime.dispatchInProgress()) {
            if (!runtime.deferDispatch(kScriptClientId, std::move(command))) {
                return CommandHandlerResult::failure(
                    "too many commands queued from one script command");
            }
            // Queued, not yet run: see doc/config.md on what a script can and
            // cannot conclude from this.
            return CommandHandlerResult::success();
        }
        auto result = runtime.dispatch(kScriptClientId, std::move(command));
        if (!result.accepted()) {
            return CommandHandlerResult::failure(result.message);
        }
        if (!result.viewAction) return CommandHandlerResult::success();
        if (!viewActionSink) {
            return CommandHandlerResult::failure(
                "view_action_unavailable");
        }
        auto applied = viewActionSink(*result.viewAction);
        if (!applied.accepted()) {
            return CommandHandlerResult::failure(
                applied.message.empty() ? "view action was rejected"
                                        : applied.message);
        }
        if ((applied.status == ViewActionStatus::TransitionRequired) !=
            applied.transition.has_value()) {
            return CommandHandlerResult::failure(
                "view action returned an invalid transition");
        }
        if (applied.transition) {
            auto transition =
                runtime.input(kScriptClientId, *applied.transition);
            if (transition.outcome == ClientInputOutcome::Rejected) {
                return CommandHandlerResult::failure(
                    transition.command &&
                            !transition.command->message.empty()
                        ? transition.command->message
                        : "view action transition was rejected");
            }
        }
        return CommandHandlerResult::success();
    }

    // Translates one ssg.command(id, args) call into the matching payload and
    // sends it through the SAME command boundary every other caller uses.
    CommandHandlerResult dispatch(LuaInvocation const& invocation) {
        // Each of these commands always requires its table: a bare call with no
        // table is a caller mistake and must fail loudly, not silently apply an
        // empty no-op.
        auto const requireArguments = [&](std::string_view what)
            -> std::optional<CommandHandlerResult> {
            if (invocation.arguments) return std::nullopt;
            return CommandHandlerResult::failure(std::string{what});
        };

        if (invocation.commandId == "theme.set") {
            if (auto refused = requireArguments(
                    "theme.set requires a color-table argument")) {
                return *refused;
            }
            return forward("theme.set",
                           ThemeSetArguments{*invocation.arguments});
        }
        if (invocation.commandId == "style.define") {
            if (auto refused = requireArguments(
                    "style.define requires a style-table argument")) {
                return *refused;
            }
            return forward("style.define",
                           StyleDefineArguments{*invocation.arguments});
        }
        if (invocation.commandId == "keymap.bind") {
            if (auto refused = requireArguments(
                    "keymap.bind requires a sequence/command argument table")) {
                return *refused;
            }
            return forward(
                "keymap.bind",
                KeymapBindArguments{
                    argumentField(*invocation.arguments, "sequence"),
                    argumentField(*invocation.arguments, "command"),
                    argumentField(*invocation.arguments, "context")});
        }
        if (invocation.commandId == "keymap.unbind") {
            if (auto refused = requireArguments(
                    "keymap.unbind requires a sequence argument table")) {
                return *refused;
            }
            return forward(
                "keymap.unbind",
                KeymapUnbindArguments{
                    argumentField(*invocation.arguments, "sequence"),
                    argumentField(*invocation.arguments, "context")});
        }
        if (!invocation.arguments) {
            return forward(invocation.commandId, {});
        }
        return CommandHandlerResult::failure("unknown script command: " +
                                             std::string{invocation.commandId});
    }
};

ScriptHost::ScriptHost(EditorSession& runtime)
    : ScriptHost(runtime, ViewId{0}, {}) {}

ScriptHost::ScriptHost(EditorSession& runtime, ViewId viewId,
                      ViewActionSink viewActionSink) {
    if (!runtime
             .attach({kScriptClientId, InvocationOrigin::Lua,
                      scriptCapabilities()},
                     viewId)
             .accepted()) {
        throw std::runtime_error{"the runtime refused the script client"};
    }

    LuaCommandHostOptions options;
    options.pluginId = kScriptClientId;
    options.capabilities = scriptCapabilities();
    options.commands = scriptCommandCatalog(*runtime.commandCatalog());
    options.chromeProviders = scriptChromeProviders();
    options.publishGate = [this](std::vector<std::string> const& ids) {
        return offerGeneration(ids);
    };
    impl_ = std::make_unique<Impl>(
        runtime, std::move(options), std::move(viewActionSink));
}

ScriptHost::~ScriptHost() {
    if (impl_) (void)impl_->runtime.detach(kScriptClientId);
}

LuaResult ScriptHost::evaluate(std::string_view script) {
    // keymap.bind/unbind's reset-then-reapply model: the script's current
    // content is the WHOLE keymap customization on every evaluation, never
    // additive, so a line removed from it reverts that binding on the next
    // reload -- matching theme.define's replace-on-reload behavior.
    impl_->runtime.resetKeymapToDefault();
    // The catalog swap happens inside this call, through the publish gate.  A
    // failed script -- or a batch the catalog refuses -- leaves the previous
    // evaluation's commands registered and callable.  Neither can undo effects
    // the script already caused before failing: a theme it applied stays
    // applied.
    return impl_->host.evaluate(script);
}

std::optional<ValidatedComposition> const& ScriptHost::composedUi() const {
    return impl_->host.composedUi();
}

// Offers what an evaluation registered to the catalog, before the host makes
// those registrations its own.
//
// A script's command becomes an ORDINARY catalog command: the palette lists it,
// a keybinding resolves it, and dispatch reaches it through the same path as
// every built-in.  Nothing downstream knows it came from Lua.  If this needed a
// second lookup path, the catalog would not have earned its keep.
//
// Called as the host's publish gate, so a batch the catalog refuses abandons
// the whole evaluation: the previous generation keeps both its catalog entries
// and the Lua functions behind them.  Doing this AFTER the host published would
// leave the catalog listing commands whose functions had already been released.
LuaResult ScriptHost::offerGeneration(std::vector<std::string> const& ids) {
    std::vector<CommandSpecBuilder> specs;
    specs.reserve(ids.size());
    for (auto const& id : ids) {
        specs.push_back(
            CommandSpecBuilder{id}
                .owner("lua")
                .summary("Registered by init.lua")
                // A script's function may do anything the commands it calls can
                // do, so it is always treated as a mutation.  It carries no
                // capability of its own: everything it invokes is gated
                // individually at dispatch, as any other Lua caller is.
                .mutates()
                .handler([impl = impl_.get(), id](CommandContext&) {
                    // The Lua state belongs to one thread.  A call from another
                    // is REFUSED rather than serialised: serialising would run
                    // script code at a moment the caller cannot reason about,
                    // and the editor has one thread that dispatches commands.
                    if (std::this_thread::get_id() != impl->owningThread) {
                        return CommandHandlerResult::failure(
                            "script commands run only on the editor thread");
                    }
                    auto const result = impl->host.invoke(id);
                    return result.accepted()
                               ? CommandHandlerResult::success()
                               : CommandHandlerResult::failure(result.message);
                }));
    }

    try {
        impl_->generation = impl_->runtime.replaceCommandGeneration(
            impl_->generation, std::move(specs));
    } catch (std::exception const& refused) {
        // The catalog validated the whole batch before applying any of it, so
        // the previous generation is still installed -- and because this ran
        // before the host published, its Lua functions are still there too.
        return {LuaError::DuplicateCommand, refused.what()};
    }
    return {};
}

}  // namespace ssg
