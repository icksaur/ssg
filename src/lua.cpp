#include <ssg/lua.h>
#include <ssg/startup_audit.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ssg {
namespace {

constexpr char host_registry_key[] = "ssg.command_host";

struct RegisteredCommand {
    std::string id;
    int function_reference;
};

struct HandleSlot {
    void* object{};
    std::uint32_t generation{1};
};

struct RegistrationTransaction {
    std::vector<RegisteredCommand> commands;
    std::unordered_set<std::string> ids;
};

struct BudgetFrame {
    LuaError pending_error;
    std::uint64_t instructions_remaining;
    std::uint64_t hook_interval;
    std::chrono::steady_clock::time_point deadline;
};

}  // namespace

struct LuaCommandHost::Impl {
    Impl(LuaCommandHostOptions configured_options,
         LuaDispatcher configured_dispatcher)
        : options{std::move(configured_options)},
          dispatcher{std::move(configured_dispatcher)},
          principal{options.plugin_id, InvocationOrigin::Lua,
                    options.capabilities} {
        note_optional_construction(OptionalSubsystem::Lua);
        if (!dispatcher) {
            throw std::invalid_argument{"Lua dispatcher must not be empty"};
        }
        if (options.instruction_budget == 0) {
            throw std::invalid_argument{
                "Lua instruction budget must be greater than zero"};
        }
        if (options.time_budget <= std::chrono::milliseconds::zero()) {
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
            open_libraries();
            install_api();
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

    void open_libraries() {
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

    void install_api() {
        lua_pushlightuserdata(state, this);
        lua_setfield(state, LUA_REGISTRYINDEX, host_registry_key);

        lua_newtable(state);
        lua_pushlightuserdata(state, this);
        lua_pushcclosure(state, &Impl::command_callback, 1);
        lua_setfield(state, -2, "command");
        lua_pushlightuserdata(state, this);
        lua_pushcclosure(state, &Impl::register_callback, 1);
        lua_setfield(state, -2, "register_command");
        lua_setglobal(state, "ssg");
    }

    static Impl& callback_host(lua_State* callback_state) {
        return *static_cast<Impl*>(lua_touserdata(
            callback_state, lua_upvalueindex(1)));
    }

    static int command_callback(lua_State* callback_state) noexcept {
        auto& host = callback_host(callback_state);
        bool raise_error = false;
        {
            try {
                std::size_t length = 0;
                char const* id_data =
                    luaL_checklstring(callback_state, 1, &length);
                std::string id{id_data, length};
                auto const found = host.catalog.find(id);
                if (found == host.catalog.end()) {
                    host.pending_error = LuaError::UnknownCommand;
                    host.callback_message = "unknown Lua command: " + id;
                    raise_error = true;
                } else {
                    for (auto const& capability :
                         found->second.required_capabilities) {
                        if (!host.principal.has_capability(capability)) {
                            host.pending_error = LuaError::CapabilityDenied;
                            host.callback_message =
                                "Lua plugin lacks capability: " +
                                std::string{capability.value()};
                            raise_error = true;
                            break;
                        }
                    }
                }

                if (!raise_error) {
                    try {
                        auto result =
                            host.dispatcher(LuaInvocation{id, host.principal});
                        if (!result.accepted) {
                            host.pending_error = LuaError::DispatchFailed;
                            host.callback_message =
                                "Lua dispatch failed: " + result.message;
                            raise_error = true;
                        }
                    } catch (std::exception const& exception) {
                        host.pending_error = LuaError::DispatchFailed;
                        host.callback_message =
                            "Lua dispatch threw: " +
                            std::string{exception.what()};
                        raise_error = true;
                    } catch (...) {
                        host.pending_error = LuaError::DispatchFailed;
                        host.callback_message =
                            "Lua dispatch threw an unknown exception";
                        raise_error = true;
                    }
                }
            } catch (std::exception const& exception) {
                host.pending_error = LuaError::RuntimeFault;
                host.callback_message =
                    "Lua command fault: " + std::string{exception.what()};
                raise_error = true;
            } catch (...) {
                host.pending_error = LuaError::RuntimeFault;
                host.callback_message =
                    "Lua command fault: unknown exception";
                raise_error = true;
            }
        }
        if (!raise_error) {
            return 0;
        }
        lua_pushlstring(callback_state, host.callback_message.data(),
                        host.callback_message.size());
        return lua_error(callback_state);
    }

    static int register_callback(lua_State* callback_state) noexcept {
        auto& host = callback_host(callback_state);
        bool raise_error = false;
        bool store_function = false;
        {
            try {
                std::size_t length = 0;
                char const* id_data =
                    luaL_checklstring(callback_state, 1, &length);
                luaL_checktype(callback_state, 2, LUA_TFUNCTION);
                std::string id{id_data, length};
                if (id.empty()) {
                    host.pending_error = LuaError::RuntimeFault;
                    host.callback_message =
                        "plugin command ID must not be empty";
                    raise_error = true;
                } else if (host.registration_stack.empty()) {
                    host.pending_error = LuaError::RuntimeFault;
                    host.callback_message =
                        "plugin commands may only be registered while evaluating";
                    raise_error = true;
                } else {
                    auto& transaction = host.registration_stack.back();
                    if (host.plugin_commands.contains(id) ||
                        !transaction.ids.emplace(id).second) {
                        host.pending_error = LuaError::DuplicateCommand;
                        host.callback_message =
                            "duplicate plugin command: " + id;
                        raise_error = true;
                    } else {
                        transaction.commands.push_back(
                            RegisteredCommand{std::move(id), LUA_NOREF});
                        store_function = true;
                    }
                }
            } catch (std::exception const& exception) {
                host.pending_error = LuaError::RuntimeFault;
                host.callback_message =
                    "registration fault: " +
                    std::string{exception.what()};
                raise_error = true;
            } catch (...) {
                host.pending_error = LuaError::RuntimeFault;
                host.callback_message =
                    "registration fault: unknown exception";
                raise_error = true;
            }
        }
        if (raise_error) {
            lua_pushlstring(callback_state, host.callback_message.data(),
                            host.callback_message.size());
            return lua_error(callback_state);
        }
        if (store_function) {
            lua_pushvalue(callback_state, 2);
            int const reference =
                luaL_ref(callback_state, LUA_REGISTRYINDEX);
            host.registration_stack.back().commands.back().function_reference =
                reference;
        }
        return 0;
    }

    static void budget_hook(lua_State* callback_state,
                            lua_Debug*) noexcept {
        lua_getfield(callback_state, LUA_REGISTRYINDEX, host_registry_key);
        auto* host =
            static_cast<Impl*>(lua_touserdata(callback_state, -1));
        lua_pop(callback_state, 1);
        if (host == nullptr) {
            luaL_error(callback_state, "Lua host is unavailable");
            return;
        }
        auto const consumed =
            std::min(host->hook_interval, host->instructions_remaining);
        host->instructions_remaining -= consumed;
        if (host->instructions_remaining == 0 ||
            std::chrono::steady_clock::now() >= host->deadline) {
            host->pending_error = LuaError::BudgetExhausted;
            luaL_error(callback_state, "Lua execution budget exhausted");
        }
    }

    void begin_call() {
        if (call_active) {
            budget_stack.push_back(
                {pending_error, instructions_remaining, hook_interval,
                 deadline});
        }
        call_active = true;
        pending_error = LuaError::None;
        instructions_remaining = options.instruction_budget;
        hook_interval =
            std::min<std::uint64_t>(instructions_remaining, 100);
        deadline = std::chrono::steady_clock::now() + options.time_budget;
        lua_sethook(state, &Impl::budget_hook, LUA_MASKCOUNT,
                    static_cast<int>(hook_interval));
    }

    LuaResult finish_call(int status, bool started, int stack_base) {
        LuaError const call_error = pending_error;
        if (started) {
            if (budget_stack.empty()) {
                call_active = false;
                lua_sethook(state, nullptr, 0, 0);
            } else {
                auto const frame = budget_stack.back();
                budget_stack.pop_back();
                pending_error = frame.pending_error;
                instructions_remaining = frame.instructions_remaining;
                hook_interval = frame.hook_interval;
                deadline = frame.deadline;
                lua_sethook(state, &Impl::budget_hook, LUA_MASKCOUNT,
                            static_cast<int>(hook_interval));
            }
        }
        if (status == LUA_OK) {
            lua_settop(state, stack_base);
            return {};
        }
        std::string message = "unknown Lua error";
        if (char const* error = lua_tostring(state, -1); error != nullptr) {
            message = error;
        }
        lua_settop(state, stack_base);
        return {call_error == LuaError::None ? LuaError::RuntimeFault
                                            : call_error,
                std::move(message)};
    }

    void rollback_staged(RegistrationTransaction& transaction) {
        for (auto const& command : transaction.commands) {
            if (command.function_reference != LUA_NOREF) {
                luaL_unref(state, LUA_REGISTRYINDEX,
                           command.function_reference);
            }
        }
    }

    void publish_staged(RegistrationTransaction& transaction) {
        for (auto& command : transaction.commands) {
            plugin_commands.emplace(command.id,
                                    command.function_reference);
        }
    }

    LuaCommandHostOptions options;
    LuaDispatcher dispatcher;
    InvocationPrincipal principal;
    lua_State* state{};
    std::unordered_map<std::string, LuaCommand> catalog;
    std::unordered_map<std::string, int> plugin_commands;
    std::vector<RegistrationTransaction> registration_stack;
    std::vector<HandleSlot> handles;
    std::string callback_message;
    LuaError pending_error{LuaError::None};
    bool call_active{false};
    std::uint64_t instructions_remaining{};
    std::uint64_t hook_interval{};
    std::chrono::steady_clock::time_point deadline;
    std::vector<BudgetFrame> budget_stack;
};

LuaCommandHost::LuaCommandHost(LuaCommandHostOptions options,
                               LuaDispatcher dispatcher)
    : impl_{std::make_unique<Impl>(std::move(options),
                                  std::move(dispatcher))} {}

LuaCommandHost::~LuaCommandHost() = default;
LuaCommandHost::LuaCommandHost(LuaCommandHost&&) noexcept = default;
LuaCommandHost& LuaCommandHost::operator=(LuaCommandHost&&) noexcept = default;

LuaResult LuaCommandHost::evaluate(std::string_view script) {
    int const stack_base = lua_gettop(impl_->state);
    impl_->registration_stack.emplace_back();
    impl_->pending_error = LuaError::None;
    bool started = false;
    int status = luaL_loadbuffer(impl_->state, script.data(), script.size(),
                                 "ssg-plugin");
    if (status == LUA_OK) {
        impl_->begin_call();
        started = true;
        status = lua_pcall(impl_->state, 0, 0, 0);
    }
    auto result = impl_->finish_call(status, started, stack_base);
    auto transaction = std::move(impl_->registration_stack.back());
    impl_->registration_stack.pop_back();
    if (result.accepted()) {
        impl_->publish_staged(transaction);
    } else {
        impl_->rollback_staged(transaction);
        if (status == LUA_ERRSYNTAX) {
            result.error = LuaError::InvalidScript;
        }
    }
    return result;
}

LuaResult LuaCommandHost::invoke(std::string_view plugin_command) {
    auto const found =
        impl_->plugin_commands.find(std::string{plugin_command});
    if (found == impl_->plugin_commands.end()) {
        return {LuaError::UnknownCommand,
                "plugin command is not registered: " +
                    std::string{plugin_command}};
    }
    int const stack_base = lua_gettop(impl_->state);
    lua_rawgeti(impl_->state, LUA_REGISTRYINDEX, found->second);
    impl_->begin_call();
    return impl_->finish_call(lua_pcall(impl_->state, 0, 0, 0), true,
                              stack_base);
}

bool LuaCommandHost::has_command(std::string_view plugin_command) const {
    return impl_->plugin_commands.contains(std::string{plugin_command});
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
