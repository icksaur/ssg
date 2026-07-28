#include <ssg/ScriptHost.h>

#include <ssg/CommandCatalog.h>
#include <ssg/EditorRuntime.h>
#include <ssg/Keymap.h>
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

// The synthetic client every script-originated command is dispatched as.  Being
// a real client means a script goes through the same capability gate and the
// same command boundary as the palette, rather than reaching past it.
ClientId const kScriptClientId{2};

// Capabilities granted to the script client, and to the commands it may call --
// one list, so there is exactly one place to update when a future
// capability-gated script command is added.  Keeping these as two
// independently maintained lists would let one drift out of sync with the
// other: passing one gate but silently denied at the other.  No command needs
// one today, so this is empty; see doc/spec-config.md's Risks -- never a
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

}  // namespace

struct ScriptHost::Impl {
    EditorRuntime& runtime;
    std::thread::id owningThread{std::this_thread::get_id()};
    LuaCommandHost host;

    Impl(EditorRuntime& editorRuntime, LuaCommandHostOptions options)
        : runtime{editorRuntime},
          host{std::move(options),
               [this](LuaInvocation const& invocation) {
                   return dispatch(invocation);
               }} {}

    // Dispatches one command from `runtime` and reports it the way Lua expects.
    CommandHandlerResult forward(std::string_view id, std::any payload) {
        auto result = runtime.dispatch(
            kScriptClientId,
            {std::string{id}, runtime.revision(), std::move(payload)});
        return result.accepted() ? CommandHandlerResult::success()
                                 : CommandHandlerResult::failure(result.message);
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

        if (invocation.commandId == "theme.define") {
            if (auto refused = requireArguments(
                    "theme.define requires a color-table argument")) {
                return *refused;
            }
            return forward("theme.define",
                           ThemeDefineArguments{*invocation.arguments});
        }
        if (invocation.commandId == "theme.background") {
            if (auto refused = requireArguments(
                    "theme.background requires a multiplier table argument")) {
                return *refused;
            }
            return forward("theme.background",
                           ThemeBackgroundArguments{*invocation.arguments});
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
        return CommandHandlerResult::failure("unknown script command: " +
                                             std::string{invocation.commandId});
    }
};

ScriptHost::ScriptHost(EditorRuntime& runtime) {
    if (!runtime
             .attach({kScriptClientId, InvocationOrigin::Lua,
                      scriptCapabilities()},
                     ViewId{0})
             .accepted()) {
        throw std::runtime_error{"the runtime refused the script client"};
    }

    LuaCommandHostOptions options;
    options.pluginId = kScriptClientId;
    options.capabilities = scriptCapabilities();
    options.commands = scriptCommandCatalog(*runtime.commandCatalog());
    impl_ = std::make_unique<Impl>(runtime, std::move(options));
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
    return impl_->host.evaluate(script);
}

}  // namespace ssg
