#include <ssg/LuaCommandHost.h>
#include <ssg/ChromeDecode.h>
#include <ssg/startup_audit.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ssg {
namespace {

constexpr char kHostRegistryKey[] = "ssg.command_host";

// Bounds for the Lua->ChromeValue walk. Depth also breaks a cyclic table (a
// self-referential table recurses until the depth cap trips), and the node cap
// bounds a maliciously huge table before the (per-side/total-capped) decoder
// ever sees it. Chrome is shallow (region->side->widget->value ~= depth 4) and
// small, so both are generous headroom, not real limits.
constexpr int kMaxChromeWalkDepth = 32;
constexpr int kMaxChromeWalkNodes = 100'000;

struct RegisteredCommand {
    std::string id;
    int functionReference;
};

struct HandleSlot {
    void* object{};
    std::uint32_t generation{1};
};

struct RegistrationTransaction {
    std::vector<RegisteredCommand> commands;
    std::unordered_set<std::string> ids;
    // The composition staged by this evaluation's LAST `ssg.chrome` call (last
    // wins); nullopt when the evaluation called `ssg.chrome` never. Published
    // wholesale on success, dropped on rollback.
    std::optional<ValidatedComposition> stagedChrome;
};

// Convert a Lua value at `index` into the decoder's Lua-agnostic `ChromeValue`.
// Throws std::runtime_error (fail-loud) on an unsupported type, a non-integer
// number, excessive depth (also catches cyclic tables), an over-large tree, or
// a malformed table. A Lua table is an ARRAY when its keys are exactly 1..rawlen
// (a `left`/`right` side) and a keyed TABLE when every key is a string (root,
// region, widget); a mixed, sparse, or non-string/non-index key is rejected, so
// no author key is ever silently dropped.
ChromeValue luaToChromeValue(lua_State* state, int index, int depth,
                             int& nodeBudget) {
    if (--nodeBudget < 0)
        throw std::runtime_error{"chrome table is too large"};
    if (depth > kMaxChromeWalkDepth)
        throw std::runtime_error{"chrome table nests too deeply"};
    // Each recursion level holds a value on the Lua stack while descending; past
    // LUA_MINSTACK that would overflow (memory corruption) without a reservation.
    if (lua_checkstack(state, 4) == 0)
        throw std::runtime_error{"chrome table exhausts the Lua stack"};
    if (index < 0) index = lua_gettop(state) + index + 1;

    switch (lua_type(state, index)) {
    case LUA_TBOOLEAN:
        return ChromeValue::ofBool(lua_toboolean(state, index) != 0);
    case LUA_TNUMBER:
        if (!lua_isinteger(state, index))
            throw std::runtime_error{"chrome numbers must be integers"};
        return ChromeValue::ofInt(
            static_cast<long long>(lua_tointeger(state, index)));
    case LUA_TSTRING: {
        std::size_t length = 0;
        char const* data = lua_tolstring(state, index, &length);
        return ChromeValue::ofString(std::string{data, length});
    }
    case LUA_TTABLE:
        break;  // handled below
    default:
        throw std::runtime_error{
            std::string{"chrome value has unsupported type "} +
            lua_typename(state, lua_type(state, index))};
    }

    // Classify the table in one pass. A CHROME table is either a pure keyed
    // table (root/region/widget: every key a string) or a pure array (a
    // left/right side: keys exactly 1..rawlen). Anything else -- a non-string,
    // non-index key, a mix of named and array entries, or a sparse/oversized
    // index set -- is fail-loud, never silently dropped (the decoder's
    // unknown-field contract would otherwise be bypassed by a stray key).
    int stringKeys = 0;
    int indexKeys = 0;
    int otherKeys = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        int const keyType = lua_type(state, -2);
        if (keyType == LUA_TSTRING)
            ++stringKeys;
        else if (keyType == LUA_TNUMBER && lua_isinteger(state, -2))
            ++indexKeys;
        else
            ++otherKeys;
        lua_pop(state, 1);
    }
    if (otherKeys > 0)
        throw std::runtime_error{
            "chrome table has a key that is neither a name nor an array index"};
    if (stringKeys > 0 && indexKeys > 0)
        throw std::runtime_error{
            "chrome table mixes named fields and array entries"};
    std::size_t const length = lua_rawlen(state, index);

    if (indexKeys > 0) {
        if (static_cast<std::size_t>(indexKeys) != length)
            throw std::runtime_error{
                "chrome array has holes or out-of-range indices"};
        std::vector<ChromeValue> items;
        items.reserve(length);
        for (std::size_t i = 1; i <= length; ++i) {
            lua_rawgeti(state, index, static_cast<lua_Integer>(i));
            items.push_back(
                luaToChromeValue(state, lua_gettop(state), depth + 1, nodeBudget));
            lua_pop(state, 1);
        }
        return ChromeValue::ofArray(std::move(items));
    }

    // Keyed table (or empty {} -- an empty keyed table, so an omitted-or-empty
    // side is a decoder-level "expected an array", not a silent success).
    std::vector<std::pair<std::string, ChromeValue>> entries;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        std::size_t keyLength = 0;
        char const* keyData = lua_tolstring(state, -2, &keyLength);
        entries.emplace_back(
            std::string{keyData, keyLength},
            luaToChromeValue(state, lua_gettop(state), depth + 1, nodeBudget));
        lua_pop(state, 1);
    }
    return ChromeValue::ofTable(std::move(entries));
}

struct BudgetFrame {
    LuaError pendingError;
    std::uint64_t instructionsRemaining;
    std::uint64_t hookInterval;
    std::chrono::steady_clock::time_point deadline;
};

}  // namespace

struct LuaCommandHost::Impl {
    Impl(LuaCommandHostOptions configuredOptions,
         LuaDispatcher configuredDispatcher)
        : options{std::move(configuredOptions)},
          dispatcher{std::move(configuredDispatcher)},
          principal{options.pluginId, InvocationOrigin::Lua,
                    options.capabilities} {
        noteOptionalConstruction(OptionalSubsystem::Lua);
        if (!dispatcher) {
            throw std::invalid_argument{"Lua dispatcher must not be empty"};
        }
        if (options.instructionBudget == 0) {
            throw std::invalid_argument{
                "Lua instruction budget must be greater than zero"};
        }
        if (options.timeBudget <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument{
                "Lua time budget must be greater than zero"};
        }
        for (auto& command : options.commands) {
            if (command.id.empty()) {
                throw std::invalid_argument{"Lua command ID must not be empty"};
            }
            if (!catalog.emplace(command.id, std::move(command)).second) {
                throw std::invalid_argument{"duplicate Lua command ID: " +
                                            command.id};
            }
        }

        state = luaL_newstate();
        if (state == nullptr) {
            throw std::runtime_error{"failed to create Lua state"};
        }
        try {
            openLibraries();
            installApi();
        } catch (...) {
            lua_close(state);
            state = nullptr;
            throw;
        }
    }

    ~Impl() {
        if (state != nullptr) {
            lua_close(state);
        }
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;

    void openLibraries() {
        luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
        lua_pop(state, 1);
        for (char const* name :
             {"dofile", "loadfile", "load", "print", "warn"}) {
            lua_pushnil(state);
            lua_setglobal(state, name);
        }
        luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_pop(state, 1);
    }

    void installApi() {
        lua_pushlightuserdata(state, this);
        lua_setfield(state, LUA_REGISTRYINDEX, kHostRegistryKey);

        lua_newtable(state);
        // Installed from the same list the documentation check reads, so a
        // function cannot be exposed without a place to describe it.
        static_assert(std::size(LuaCommandHost::kApiFunctions) == 3,
                      "add the new API function's installer below");
        lua_pushlightuserdata(state, this);
        lua_pushcclosure(state, &Impl::commandCallback, 1);
        lua_setfield(state, -2, LuaCommandHost::kApiFunctions[0].data());
        lua_pushlightuserdata(state, this);
        lua_pushcclosure(state, &Impl::registerCallback, 1);
        lua_setfield(state, -2, LuaCommandHost::kApiFunctions[1].data());
        lua_pushlightuserdata(state, this);
        lua_pushcclosure(state, &Impl::chromeCallback, 1);
        lua_setfield(state, -2, LuaCommandHost::kApiFunctions[2].data());
        lua_setglobal(state, "ssg");
    }

    static Impl& callbackHost(lua_State* callbackState) {
        return *static_cast<Impl*>(lua_touserdata(
            callbackState, lua_upvalueindex(1)));
    }

    static int commandCallback(lua_State* callbackState) noexcept {
        auto& host = callbackHost(callbackState);
        bool raiseError = false;
        {
            try {
                std::size_t length = 0;
                char const* idData =
                    luaL_checklstring(callbackState, 1, &length);
                std::string id{idData, length};

                // ssg.command(id, args): `args` is an OPTIONAL second Lua
                // table argument, decoded into a flat string->string map
                // BEFORE any command lookup or dispatch happens -- a
                // malformed second argument (not a table, or a table with a
                // non-string key/value) must never reach a command handler,
                // so it is rejected here, ahead of even the unknown-command
                // check below.
                std::optional<std::unordered_map<std::string, std::string>>
                    arguments;
                if (lua_gettop(callbackState) >= 2 &&
                    !lua_isnoneornil(callbackState, 2)) {
                    if (lua_type(callbackState, 2) != LUA_TTABLE) {
                        host.pendingError = LuaError::InvalidScript;
                        host.callbackMessage =
                            "ssg.command's second argument must be a table";
                        raiseError = true;
                    } else {
                        std::unordered_map<std::string, std::string> decoded;
                        lua_pushvalue(callbackState, 2);
                        lua_pushnil(callbackState);
                        while (lua_next(callbackState, -2) != 0) {
                            if (lua_type(callbackState, -2) != LUA_TSTRING ||
                                lua_type(callbackState, -1) != LUA_TSTRING) {
                                host.pendingError = LuaError::InvalidScript;
                                host.callbackMessage =
                                    "ssg.command's argument table keys and "
                                    "values must be strings";
                                raiseError = true;
                                lua_pop(callbackState, 2);
                                break;
                            }
                            std::size_t keyLength = 0;
                            char const* keyData = lua_tolstring(
                                callbackState, -2, &keyLength);
                            std::size_t valueLength = 0;
                            char const* valueData = lua_tolstring(
                                callbackState, -1, &valueLength);
                            decoded.emplace(std::string{keyData, keyLength},
                                            std::string{valueData, valueLength});
                            lua_pop(callbackState, 1);
                        }
                        lua_pop(callbackState, 1);
                        if (!raiseError) {
                            arguments = std::move(decoded);
                        }
                    }
                }

                if (!raiseError) {
                    auto const found = host.catalog.find(id);
                    if (found == host.catalog.end()) {
                        host.pendingError = LuaError::UnknownCommand;
                        host.callbackMessage = "unknown Lua command: " + id;
                        raiseError = true;
                    } else {
                        for (auto const& capability :
                             found->second.requiredCapabilities) {
                            if (!host.principal.hasCapability(capability)) {
                                host.pendingError = LuaError::CapabilityDenied;
                                host.callbackMessage =
                                    "Lua plugin lacks capability: " +
                                    std::string{capability.value()};
                                raiseError = true;
                                break;
                            }
                        }
                    }
                }

                if (!raiseError) {
                    try {
                        auto result = host.dispatcher(
                            LuaInvocation{id, host.principal, std::move(arguments)});
                        if (!result.accepted) {
                            host.pendingError = LuaError::DispatchFailed;
                            host.callbackMessage =
                                "Lua dispatch failed: " + result.message;
                            raiseError = true;
                        }
                    } catch (std::exception const& exception) {
                        host.pendingError = LuaError::DispatchFailed;
                        host.callbackMessage =
                            "Lua dispatch threw: " +
                            std::string{exception.what()};
                        raiseError = true;
                    } catch (...) {
                        host.pendingError = LuaError::DispatchFailed;
                        host.callbackMessage =
                            "Lua dispatch threw an unknown exception";
                        raiseError = true;
                    }
                }
            } catch (std::exception const& exception) {
                host.pendingError = LuaError::RuntimeFault;
                host.callbackMessage =
                    "Lua command fault: " + std::string{exception.what()};
                raiseError = true;
            } catch (...) {
                host.pendingError = LuaError::RuntimeFault;
                host.callbackMessage =
                    "Lua command fault: unknown exception";
                raiseError = true;
            }
        }
        if (!raiseError) {
            return 0;
        }
        lua_pushlstring(callbackState, host.callbackMessage.data(),
                        host.callbackMessage.size());
        return lua_error(callbackState);
    }

    static int registerCallback(lua_State* callbackState) noexcept {
        auto& host = callbackHost(callbackState);
        bool raiseError = false;
        bool storeFunction = false;
        {
            try {
                std::size_t length = 0;
                char const* idData =
                    luaL_checklstring(callbackState, 1, &length);
                luaL_checktype(callbackState, 2, LUA_TFUNCTION);
                std::string id{idData, length};
                if (id.empty()) {
                    host.pendingError = LuaError::RuntimeFault;
                    host.callbackMessage =
                        "plugin command ID must not be empty";
                    raiseError = true;
                } else if (host.registrationStack.empty()) {
                    host.pendingError = LuaError::RuntimeFault;
                    host.callbackMessage =
                        "plugin commands may only be registered while evaluating";
                    raiseError = true;
                } else {
                    auto& transaction = host.registrationStack.back();
                    // Only THIS evaluation's registrations are checked.  The
                    // previously published generation is retired wholesale when
                    // this one publishes, so re-registering its ids is exactly
                    // what reloading an unchanged script does; rejecting that
                    // would make the second load of any script fail.
                    if (!transaction.ids.emplace(id).second) {
                        host.pendingError = LuaError::DuplicateCommand;
                        host.callbackMessage =
                            "duplicate plugin command: " + id;
                        raiseError = true;
                    } else {
                        transaction.commands.push_back(
                            RegisteredCommand{std::move(id), LUA_NOREF});
                        storeFunction = true;
                    }
                }
            } catch (std::exception const& exception) {
                host.pendingError = LuaError::RuntimeFault;
                host.callbackMessage =
                    "registration fault: " +
                    std::string{exception.what()};
                raiseError = true;
            } catch (...) {
                host.pendingError = LuaError::RuntimeFault;
                host.callbackMessage =
                    "registration fault: unknown exception";
                raiseError = true;
            }
        }
        if (raiseError) {
            lua_pushlstring(callbackState, host.callbackMessage.data(),
                            host.callbackMessage.size());
            return lua_error(callbackState);
        }
        if (storeFunction) {
            lua_pushvalue(callbackState, 2);
            int const reference =
                luaL_ref(callbackState, LUA_REGISTRYINDEX);
            host.registrationStack.back().commands.back().functionReference =
                reference;
        }
        return 0;
    }

    static int chromeCallback(lua_State* callbackState) noexcept {
        auto& host = callbackHost(callbackState);
        bool raiseError = false;
        try {
            if (lua_gettop(callbackState) < 1 ||
                lua_type(callbackState, 1) != LUA_TTABLE) {
                host.pendingError = LuaError::InvalidScript;
                host.callbackMessage = "ssg.chrome requires a table argument";
                raiseError = true;
            } else if (host.registrationStack.empty()) {
                host.pendingError = LuaError::RuntimeFault;
                host.callbackMessage =
                    "ssg.chrome may only be called while evaluating";
                raiseError = true;
            } else {
                int nodeBudget = kMaxChromeWalkNodes;
                ChromeValue root =
                    luaToChromeValue(callbackState, 1, 0, nodeBudget);
                auto decoded = decodeChrome(
                    root, host.options.chromeProviders);
                if (!decoded.ok()) {
                    host.pendingError = LuaError::InvalidScript;
                    host.callbackMessage = *decoded.error;
                    raiseError = true;
                } else {
                    // Last `ssg.chrome` call in the evaluation wins.
                    host.registrationStack.back().stagedChrome =
                        std::move(decoded.composition);
                }
            }
        } catch (std::exception const& exception) {
            host.pendingError = LuaError::InvalidScript;
            host.callbackMessage =
                "ssg.chrome: " + std::string{exception.what()};
            raiseError = true;
        } catch (...) {
            host.pendingError = LuaError::InvalidScript;
            host.callbackMessage = "ssg.chrome: unknown fault";
            raiseError = true;
        }
        if (!raiseError) {
            return 0;
        }
        lua_pushlstring(callbackState, host.callbackMessage.data(),
                        host.callbackMessage.size());
        return lua_error(callbackState);
    }

    static void budgetHook(lua_State* callbackState,
                            lua_Debug*) noexcept {
        lua_getfield(callbackState, LUA_REGISTRYINDEX, kHostRegistryKey);
        auto* host =
            static_cast<Impl*>(lua_touserdata(callbackState, -1));
        lua_pop(callbackState, 1);
        if (host == nullptr) {
            luaL_error(callbackState, "Lua host is unavailable");
            return;
        }
        auto const consumed =
            std::min(host->hookInterval, host->instructionsRemaining);
        host->instructionsRemaining -= consumed;
        if (host->instructionsRemaining == 0 ||
            std::chrono::steady_clock::now() >= host->deadline) {
            host->pendingError = LuaError::BudgetExhausted;
            luaL_error(callbackState, "Lua execution budget exhausted");
        }
    }

    void beginCall() {
        if (callActive) {
            budgetStack.push_back(
                {pendingError, instructionsRemaining, hookInterval,
                 deadline});
        }
        callActive = true;
        pendingError = LuaError::None;
        instructionsRemaining = options.instructionBudget;
        hookInterval =
            std::min<std::uint64_t>(instructionsRemaining, 100);
        deadline = std::chrono::steady_clock::now() + options.timeBudget;
        lua_sethook(state, &Impl::budgetHook, LUA_MASKCOUNT,
                    static_cast<int>(hookInterval));
    }

    LuaResult finishCall(int status, bool started, int stackBase) {
        LuaError const callError = pendingError;
        if (started) {
            if (budgetStack.empty()) {
                callActive = false;
                lua_sethook(state, nullptr, 0, 0);
            } else {
                auto const frame = budgetStack.back();
                budgetStack.pop_back();
                pendingError = frame.pendingError;
                instructionsRemaining = frame.instructionsRemaining;
                hookInterval = frame.hookInterval;
                deadline = frame.deadline;
                lua_sethook(state, &Impl::budgetHook, LUA_MASKCOUNT,
                            static_cast<int>(hookInterval));
            }
        }
        if (status == LUA_OK) {
            lua_settop(state, stackBase);
            return {};
        }
        std::string message = "unknown Lua error";
        if (char const* error = lua_tostring(state, -1); error != nullptr) {
            message = error;
        }
        lua_settop(state, stackBase);
        return {callError == LuaError::None ? LuaError::RuntimeFault
                                            : callError,
                std::move(message)};
    }

    void rollbackStaged(RegistrationTransaction& transaction) {
        for (auto const& command : transaction.commands) {
            if (command.functionReference != LUA_NOREF) {
                luaL_unref(state, LUA_REGISTRYINDEX,
                           command.functionReference);
            }
        }
    }

    // Installs this evaluation's registrations AS the current generation,
    // releasing the previous one: what a script registers is replaced by what
    // the next successful evaluation registers, never merged with it, so a
    // function deleted from the script stops existing on reload.
    void publishStaged(RegistrationTransaction& transaction) {
        for (auto const& [id, reference] : pluginCommands) {
            if (reference != LUA_NOREF) {
                luaL_unref(state, LUA_REGISTRYINDEX, reference);
            }
        }
        pluginCommands.clear();
        for (auto& command : transaction.commands) {
            pluginCommands.emplace(command.id, command.functionReference);
        }
        // Chrome publishes on the SAME success point as commands: a script that
        // no longer calls ssg.chrome reverts to built-in (stagedChrome nullopt).
        publishedChrome = std::move(transaction.stagedChrome);
    }

    LuaCommandHostOptions options;
    LuaDispatcher dispatcher;
    InvocationPrincipal principal;
    lua_State* state{};
    std::unordered_map<std::string, LuaCommand> catalog;
    std::unordered_map<std::string, int> pluginCommands;
    std::optional<ValidatedComposition> publishedChrome;
    std::vector<RegistrationTransaction> registrationStack;
    std::vector<HandleSlot> handles;
    std::string callbackMessage;
    LuaError pendingError{LuaError::None};
    bool callActive{false};
    std::uint64_t instructionsRemaining{};
    std::uint64_t hookInterval{};
    std::chrono::steady_clock::time_point deadline;
    std::vector<BudgetFrame> budgetStack;
    bool gateActive{false};
};

LuaCommandHost::LuaCommandHost(LuaCommandHostOptions options,
                               LuaDispatcher dispatcher)
    : impl_{std::make_unique<Impl>(std::move(options),
                                  std::move(dispatcher))} {}

LuaCommandHost::~LuaCommandHost() = default;
LuaCommandHost::LuaCommandHost(LuaCommandHost&&) noexcept = default;
LuaCommandHost& LuaCommandHost::operator=(LuaCommandHost&&) noexcept = default;

std::vector<std::string> LuaCommandHost::registeredCommands() const {
    std::vector<std::string> ids;
    ids.reserve(impl_->pluginCommands.size());
    for (auto const& [id, reference] : impl_->pluginCommands) ids.push_back(id);
    // Ordered so a caller registering these downstream produces the same
    // handles for the same script, rather than depending on hash order.
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::optional<ValidatedComposition> const& LuaCommandHost::composedUi() const noexcept {
    return impl_->publishedChrome;
}

LuaResult LuaCommandHost::evaluate(std::string_view script) {
    if (impl_->gateActive) {
        return {LuaError::RuntimeFault,
                "a command registration gate may not evaluate on the host it "
                "is gating"};
    }
    int const stackBase = lua_gettop(impl_->state);
    impl_->registrationStack.emplace_back();
    impl_->pendingError = LuaError::None;
    bool started = false;
    int status = luaL_loadbuffer(impl_->state, script.data(), script.size(),
                                 "ssg-plugin");
    if (status == LUA_OK) {
        impl_->beginCall();
        started = true;
        status = lua_pcall(impl_->state, 0, 0, 0);
    }
    auto result = impl_->finishCall(status, started, stackBase);
    auto transaction = std::move(impl_->registrationStack.back());
    impl_->registrationStack.pop_back();
    if (result.accepted()) {
        // The gate may still refuse, and it is the LAST thing that can: once
        // publishStaged runs, the previous generation's functions are gone.
        if (impl_->options.publishGate) {
            std::vector<std::string> ids;
            ids.reserve(transaction.commands.size());
            for (auto const& command : transaction.commands) {
                ids.push_back(command.id);
            }
            std::sort(ids.begin(), ids.end());
            // The gate is caller-supplied, so it is contained the same way a
            // dispatcher is: an escaping exception must still roll the staged
            // registrations back, or their Lua references leak and the
            // evaluation neither publishes nor rolls back.
            LuaResult refusal;
            // The gate runs while this evaluation's registrations are staged
            // and the previous generation's are still installed.  A gate that
            // re-entered this host would evaluate or invoke against that
            // half-swapped state and could interleave two generations, so it is
            // refused for the duration of the call rather than left to
            // convention.
            impl_->gateActive = true;
            struct GateScope {
                bool& active;
                ~GateScope() { active = false; }
            } const gateScope{impl_->gateActive};
            try {
                refusal = impl_->options.publishGate(ids);
            } catch (std::exception const& thrown) {
                refusal = {LuaError::RuntimeFault,
                           "command registration gate threw: " +
                               std::string{thrown.what()}};
            } catch (...) {
                refusal = {LuaError::RuntimeFault,
                           "command registration gate threw"};
            }
            if (!refusal.accepted()) {
                impl_->rollbackStaged(transaction);
                return refusal;
            }
        }
        impl_->publishStaged(transaction);
    } else {
        impl_->rollbackStaged(transaction);
        if (status == LUA_ERRSYNTAX) {
            result.error = LuaError::InvalidScript;
        }
    }
    return result;
}

LuaResult LuaCommandHost::invoke(std::string_view pluginCommand) {
    if (impl_->gateActive) {
        return {LuaError::RuntimeFault,
                "a command registration gate may not invoke on the host it is "
                "gating"};
    }
    auto const found =
        impl_->pluginCommands.find(std::string{pluginCommand});
    if (found == impl_->pluginCommands.end()) {
        return {LuaError::UnknownCommand,
                "plugin command is not registered: " +
                    std::string{pluginCommand}};
    }
    int const stackBase = lua_gettop(impl_->state);
    lua_rawgeti(impl_->state, LUA_REGISTRYINDEX, found->second);
    impl_->beginCall();
    return impl_->finishCall(lua_pcall(impl_->state, 0, 0, 0), true,
                              stackBase);
}

bool LuaCommandHost::hasCommand(std::string_view pluginCommand) const {
    return impl_->pluginCommands.contains(std::string{pluginCommand});
}

LuaHandle LuaCommandHost::expose(void* object) {
    if (object == nullptr) {
        throw std::invalid_argument{"exposed Lua object must not be null"};
    }
    for (std::size_t i = 0; i < impl_->handles.size(); ++i) {
        auto& slot = impl_->handles[i];
        if (slot.object == nullptr &&
            slot.generation !=
                std::numeric_limits<std::uint32_t>::max()) {
            slot.object = object;
            return {static_cast<std::uint32_t>(i), slot.generation};
        }
    }
    if (impl_->handles.size() >=
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error{"Lua handle table is exhausted"};
    }
    impl_->handles.push_back({object, 1});
    return {static_cast<std::uint32_t>(impl_->handles.size() - 1), 1};
}

void LuaCommandHost::invalidate(LuaHandle handle) {
    if (handle.index >= impl_->handles.size()) {
        return;
    }
    auto& slot = impl_->handles[handle.index];
    if (slot.object == nullptr || slot.generation != handle.generation) {
        return;
    }
    slot.object = nullptr;
    if (slot.generation != std::numeric_limits<std::uint32_t>::max()) {
        ++slot.generation;
    }
}

LuaResult LuaCommandHost::resolve(LuaHandle handle, void*& object) const {
    if (handle.index >= impl_->handles.size()) {
        return {LuaError::StaleHandle, "Lua handle index is stale"};
    }
    auto const& slot = impl_->handles[handle.index];
    if (slot.object == nullptr || slot.generation != handle.generation) {
        return {LuaError::StaleHandle,
                "Lua handle generation is stale"};
    }
    object = slot.object;
    return {};
}

}  // namespace ssg
