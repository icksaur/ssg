#pragma once

#include <ssg/CommandCatalog.h>

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

enum class LuaError : std::uint8_t {
    None,
    InvalidScript,
    RuntimeFault,
    BudgetExhausted,
    DuplicateCommand,
    UnknownCommand,
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
};

struct LuaInvocation {
    std::string_view commandId;
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

// Asked to accept an evaluation's registrations BEFORE they replace the
// previous ones.  Returning a failure abandons the evaluation: the new
// registrations are discarded and the previous ones stay in place, so whatever
// the gate protects and the host's own state cannot disagree.
//
// This exists so the step that can REFUSE runs before the step that cannot be
// undone.  Publishing first and reconciling afterwards leaves the previous
// generation's Lua functions already released, with nothing able to restore
// them.
using LuaGenerationGate =
    std::function<LuaResult(std::vector<std::string> const& commandIds)>;

struct LuaCommandHostOptions {
    std::vector<LuaCommand> commands;
    std::uint64_t instructionBudget{100'000};
    std::chrono::milliseconds timeBudget{50};
    LuaGenerationGate publishGate;
};

class LuaCommandHost {
public:
    // Every function the `ssg` table exposes to a script, in one place, so
    // installing the API and checking that it is documented read the same list
    // rather than two hand-maintained ones.
    static constexpr std::string_view kApiFunctions[]{"command",
                                                      "register_command"};

    LuaCommandHost(LuaCommandHostOptions options, LuaDispatcher dispatcher);
    ~LuaCommandHost();

    LuaCommandHost(LuaCommandHost const&) = delete;
    LuaCommandHost& operator=(LuaCommandHost const&) = delete;
    LuaCommandHost(LuaCommandHost&&) noexcept;
    LuaCommandHost& operator=(LuaCommandHost&&) noexcept;

    [[nodiscard]] LuaResult evaluate(std::string_view script);
    [[nodiscard]] LuaResult invoke(std::string_view pluginCommand);
    [[nodiscard]] bool hasCommand(std::string_view pluginCommand) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
