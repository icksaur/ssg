#pragma once

#include <ssg/CommandRegistry.h>

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ssg {

struct LuaHandle {
    std::uint32_t index{};
    std::uint32_t generation{};
    auto operator<=>(LuaHandle const&) const = default;
};

enum class LuaError : std::uint8_t {
    None,
    InvalidScript,
    RuntimeFault,
    BudgetExhausted,
    StaleHandle,
    DuplicateCommand,
    UnknownCommand,
    CapabilityDenied,
    DispatchFailed,
};

struct LuaResult {
    LuaError error{LuaError::None};
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LuaError::None;
    }
};

struct LuaCommand {
    std::string id;
    std::vector<CapabilityId> requiredCapabilities;
};

struct LuaInvocation {
    std::string_view commandId;
    InvocationPrincipal const& principal;
    // Present only when the Lua caller passed a SECOND table argument to
    // ssg.command(id, args) -- e.g. a future theme.define(colors) call
    // passing a table of hex color strings keyed by palette-slot name.
    // Absent (nullopt) for a zero-argument call, matching every
    // command reachable from Lua before this field existed. Only a flat
    // string->string table shape is supported: LuaCommandHost rejects a
    // non-table or a table with a non-string key/value BEFORE the
    // dispatcher is ever called (see commandCallback in LuaCommandHost.cpp),
    // so a handler can trust that when this is present, every key and value
    // is a plain string.
    std::optional<std::unordered_map<std::string, std::string>> arguments;
};

using LuaDispatcher = std::function<CommandHandlerResult(LuaInvocation const&)>;

struct LuaCommandHostOptions {
    ClientId pluginId;
    std::vector<CapabilityId> capabilities;
    std::vector<LuaCommand> commands;
    std::uint64_t instructionBudget{100'000};
    std::chrono::milliseconds timeBudget{50};
};

class LuaCommandHost {
public:
    LuaCommandHost(LuaCommandHostOptions options, LuaDispatcher dispatcher);
    ~LuaCommandHost();

    LuaCommandHost(LuaCommandHost const&) = delete;
    LuaCommandHost& operator=(LuaCommandHost const&) = delete;
    LuaCommandHost(LuaCommandHost&&) noexcept;
    LuaCommandHost& operator=(LuaCommandHost&&) noexcept;

    [[nodiscard]] LuaResult evaluate(std::string_view script);
    [[nodiscard]] LuaResult invoke(std::string_view pluginCommand);
    [[nodiscard]] bool hasCommand(std::string_view pluginCommand) const;

    [[nodiscard]] LuaHandle expose(void* object);
    void invalidate(LuaHandle handle);
    [[nodiscard]] LuaResult resolve(LuaHandle handle, void*& object) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
