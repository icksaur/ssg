#include <ssg/ScriptHost.h>

#include <ssg/Editor.h>
#include <ssg/Keymap.h>
#include <ssg/StatusFields.h>
#include <ssg/Style.h>
#include <ssg/Theme.h>

#include <cctype>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace ssg {

namespace {

std::vector<LuaCommand> scriptCommandCatalog(Commands const& commands) {
    std::vector<LuaCommand> result;
    result.reserve(commands.all().size() + 4);
    for (auto const& [id, command] : commands.all()) {
        (void)command;
        result.push_back({id});
    }
    result.push_back({"theme.set"});
    result.push_back({"style.define"});
    result.push_back({"keymap.bind"});
    result.push_back({"keymap.unbind"});
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

std::string scriptCommandLabel(std::string_view id) {
    std::string label;
    bool wordStart = true;
    for (char raw : id) {
        if (raw == '.' || raw == '_') {
            label += ' ';
            wordStart = true;
            continue;
        }
        auto const ch = static_cast<unsigned char>(raw);
        label += wordStart ? static_cast<char>(std::toupper(ch)) : raw;
        wordStart = false;
    }
    return label;
}

LuaResult luaResult(OperationResult result) {
    return result.accepted ? LuaResult{}
                           : LuaResult{LuaError::DispatchFailed,
                                       std::move(result.message)};
}

}  // namespace

struct ScriptHost::Impl {
    Editor& runtime;
    std::thread::id owningThread{std::this_thread::get_id()};
    LuaCommandHost host;
    ViewActionSink viewActionSink;
    // What the last successful evaluation published, replaced by the next one.
    std::vector<std::string> generation;

    Impl(Editor& editorRuntime, LuaCommandHostOptions options,
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
    LuaResult forward(std::string_view id) {
        if (runtime.dispatchInProgress()) {
            if (!runtime.deferDispatch(std::string{id})) {
                return {LuaError::DispatchFailed,
                        "too many commands queued from one script command"};
            }
            // Queued, not yet run: see doc/config.md on what a script can and
            // cannot conclude from this.
            return {};
        }
        auto result = runtime.dispatch(id);
        if (!result.accepted()) {
            return {LuaError::DispatchFailed, std::move(result.message)};
        }
        if (!result.viewAction) return {};
        if (!viewActionSink) {
            return {LuaError::DispatchFailed, "view_action_unavailable"};
        }
        auto applied = viewActionSink(*result.viewAction);
        if (!applied.accepted()) {
            return {LuaError::DispatchFailed,
                applied.message.empty() ? "view action was rejected"
                                        : applied.message};
        }
        if ((applied.status == ViewActionStatus::TransitionRequired) !=
            applied.transition.has_value()) {
            return {LuaError::DispatchFailed,
                    "view action returned an invalid transition"};
        }
        if (applied.transition) {
            auto transition = runtime.input(*applied.transition);
            if (transition.outcome == ClientInputOutcome::Rejected) {
                return {LuaError::DispatchFailed,
                    transition.command &&
                            !transition.command->message.empty()
                        ? transition.command->message
                        : "view action transition was rejected"};
            }
        }
        return {};
    }

    LuaResult dispatchConfiguration(LuaInvocation const& invocation) {
        std::unique_lock operationLock{runtime.operationMutex, std::defer_lock};
        if (!runtime.dispatchInProgress()) operationLock.lock();
        auto const requireArguments = [&](std::string_view message)
            -> std::optional<LuaResult> {
            if (invocation.arguments) return std::nullopt;
            return LuaResult{LuaError::DispatchFailed, std::string{message}};
        };
        if (invocation.commandId == "theme.set") {
            if (auto refused = requireArguments(
                    "theme.set requires a color-table argument")) {
                return *refused;
            }
            return luaResult(applyThemeSet(
                runtime, ThemeSetArguments{*invocation.arguments}));
        }
        if (invocation.commandId == "style.define") {
            if (auto refused = requireArguments(
                    "style.define requires a style-table argument")) {
                return *refused;
            }
            return luaResult(applyStyleDefine(
                runtime, StyleDefineArguments{*invocation.arguments}));
        }
        if (invocation.commandId == "keymap.bind") {
            if (auto refused = requireArguments(
                    "keymap.bind requires a sequence/command argument table")) {
                return *refused;
            }
            return luaResult(applyKeymapBind(
                runtime, {argumentField(*invocation.arguments, "sequence"),
                          argumentField(*invocation.arguments, "command"),
                          argumentField(*invocation.arguments, "context")}));
        }
        if (invocation.commandId == "keymap.unbind") {
            if (auto refused = requireArguments(
                    "keymap.unbind requires a sequence argument table")) {
                return *refused;
            }
            return luaResult(applyKeymapUnbind(
                runtime, {argumentField(*invocation.arguments, "sequence"),
                          argumentField(*invocation.arguments, "context")}));
        }
        return {LuaError::UnknownCommand,
                "unknown script configuration: " +
                    std::string{invocation.commandId}};
    }

    LuaResult dispatch(LuaInvocation const& invocation) {
        if (invocation.commandId == "theme.set" ||
            invocation.commandId == "style.define" ||
            invocation.commandId == "keymap.bind" ||
            invocation.commandId == "keymap.unbind") {
            return dispatchConfiguration(invocation);
        }
        if (!invocation.arguments) {
            return forward(invocation.commandId);
        }
        return {LuaError::UnknownCommand, "unknown script command: " +
                                           std::string{invocation.commandId}};
    }
};

ScriptHost::ScriptHost(Editor& runtime) : ScriptHost(runtime, {}) {}

ScriptHost::ScriptHost(Editor& runtime, ViewActionSink viewActionSink) {
    LuaCommandHostOptions options;
    options.commands = scriptCommandCatalog(runtime.commandRegistry());
    options.commandAvailable = [editor = &runtime](std::string_view id) {
        return editor->commandRegistry().find(id) != nullptr;
    };
    options.publishGate = [this](std::vector<std::string> const& ids) {
        return offerGeneration(ids);
    };
    impl_ = std::make_unique<Impl>(
        runtime, std::move(options), std::move(viewActionSink));
}

ScriptHost::~ScriptHost() = default;

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

LuaResult ScriptHost::offerGeneration(std::vector<std::string> const& ids) {
    Commands::Replacements replacements;
    replacements.reserve(ids.size());
    for (auto const& id : ids) {
        replacements.emplace_back(
            id, Command{scriptCommandLabel(id), [impl = impl_.get(), id] {
                if (std::this_thread::get_id() != impl->owningThread) {
                    return CommandResult{
                        CommandError::HandlerFailed,
                        "script commands run only on the editor thread", {}};
                }
                auto const result = impl->host.invoke(id);
                return result.accepted()
                           ? CommandResult{}
                           : CommandResult{CommandError::HandlerFailed,
                                           result.message, {}};
            }});
    }

    try {
        impl_->runtime.replaceCommands(impl_->generation, std::move(replacements));
        impl_->generation = ids;
    } catch (std::exception const& refused) {
        // The catalog validated the whole batch before applying any of it, so
        // the previous generation is still installed -- and because this ran
        // before the host published, its Lua functions are still there too.
        return {LuaError::DuplicateCommand, refused.what()};
    }
    return {};
}

}  // namespace ssg
