#include "test_helpers.h"

#include <ssg/lua.h>

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using namespace ssg;

std::string read_required_catalog() {
    std::ifstream input{std::string{SSG_TEST_SOURCE_DIR} +
                        "/data/required-commands.json"};
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

std::vector<std::pair<std::string, bool>> parse_catalog(std::string const& json) {
    std::vector<std::pair<std::string, bool>> entries;
    std::size_t position = 0;
    while ((position = json.find("\"id\"", position)) != std::string::npos) {
        auto const value_begin = json.find('"', json.find(':', position) + 1) + 1;
        auto const value_end = json.find('"', value_begin);
        auto const object_end = json.find('}', value_end);
        auto const lua_key = json.find("\"lua\"", value_end);
        ASSERT_TRUE(value_begin != std::string::npos);
        ASSERT_TRUE(value_end != std::string::npos);
        ASSERT_TRUE(lua_key < object_end);
        auto const lua_value = json.find_first_not_of(" \t\r\n:",
            lua_key + std::string_view{"\"lua\""}.size());
        entries.emplace_back(json.substr(value_begin, value_end - value_begin),
                             json.compare(lua_value, 4, "true") == 0);
        position = object_end;
    }
    return entries;
}

LuaCommandHostOptions options(std::vector<LuaCommand> commands = {}) {
    LuaCommandHostOptions result;
    result.plugin_id = ClientId{81};
    result.commands = std::move(commands);
    return result;
}

TEST(required_catalog_minus_exclusions_is_callable) {
    auto const catalog = parse_catalog(read_required_catalog());
    std::vector<LuaCommand> commands;
    std::unordered_set<std::string> called;
    std::string excluded;
    for (auto const& [id, lua] : catalog) {
        if (lua) {
            commands.push_back({id, {}});
        } else {
            excluded = id;
        }
    }
    LuaCommandHost host{options(std::move(commands)),
        [&](LuaInvocation const& invocation) {
            ASSERT_EQ(invocation.principal.origin(), InvocationOrigin::Lua);
            called.emplace(invocation.command_id);
            return CommandHandlerResult::success();
        }};

    for (auto const& [id, lua] : catalog) {
        auto const result = host.evaluate("ssg.command(\"" + id + "\")");
        ASSERT_EQ(result.accepted(), lua);
        ASSERT_EQ(result.error, lua ? LuaError::None : LuaError::UnknownCommand);
    }
    ASSERT_EQ(called.size(), catalog.size() - 1);
    ASSERT_FALSE(excluded.empty());
}

TEST(capabilities_are_immutable_and_checked_before_dispatch) {
    auto configured = options({{"safe", {}}, {"privileged", {CapabilityId{"fs"}}}});
    configured.capabilities.emplace_back("network");
    bool dispatched = false;
    LuaCommandHost host{std::move(configured),
        [&](LuaInvocation const& invocation) {
            dispatched = true;
            ASSERT_TRUE(invocation.principal.has_capability(
                CapabilityId{"network"}));
            return CommandHandlerResult::success();
        }};
    auto denied = host.evaluate("ssg.command('privileged')");
    ASSERT_EQ(denied.error, LuaError::CapabilityDenied);
    ASSERT_FALSE(dispatched);
    ASSERT_TRUE(host.evaluate("ssg.command('safe')").accepted());
    ASSERT_TRUE(dispatched);
}

TEST(generational_handles_reject_stale_access_after_reuse) {
    LuaCommandHost host{options(), [](LuaInvocation const&) {
        return CommandHandlerResult::success();
    }};
    int first = 1;
    int second = 2;
    auto const stale = host.expose(&first);
    void* resolved = nullptr;
    ASSERT_TRUE(host.resolve(stale, resolved).accepted());
    ASSERT_EQ(resolved, static_cast<void*>(&first));
    host.invalidate(stale);
    auto const current = host.expose(&second);
    ASSERT_EQ(current.index, stale.index);
    ASSERT_NE(current.generation, stale.generation);
    ASSERT_EQ(host.resolve(stale, resolved).error, LuaError::StaleHandle);
    ASSERT_TRUE(host.resolve(current, resolved).accepted());
    ASSERT_EQ(resolved, static_cast<void*>(&second));
}

TEST(instruction_and_wall_clock_budgets_isolate_callbacks) {
    auto configured = options();
    configured.instruction_budget = 2'000;
    configured.time_budget = std::chrono::milliseconds{5};
    LuaCommandHost host{std::move(configured), [](LuaInvocation const&) {
        return CommandHandlerResult::success();
    }};
    ASSERT_EQ(host.evaluate("while true do end").error,
              LuaError::BudgetExhausted);
    ASSERT_TRUE(host.evaluate("return 7").accepted());
}

TEST(reentrant_calls_restore_the_enclosing_budget) {
    LuaCommandHost* reentrant = nullptr;
    auto configured = options({{"reenter", {}}});
    configured.instruction_budget = 2'000;
    configured.time_budget = std::chrono::milliseconds{5};
    LuaCommandHost host{std::move(configured),
        [&](LuaInvocation const&) {
            ASSERT_TRUE(reentrant->evaluate("return 1").accepted());
            return CommandHandlerResult::success();
        }};
    reentrant = &host;
    ASSERT_EQ(host.evaluate(
        "ssg.command('reenter'); while true do end").error,
        LuaError::BudgetExhausted);
    ASSERT_TRUE(host.evaluate("return 7").accepted());
}

TEST(registration_is_atomic_and_duplicate_safe) {
    LuaCommandHost host{options(), [](LuaInvocation const&) {
        return CommandHandlerResult::success();
    }};
    auto evaluation = host.evaluate(
        "ssg.register_command('half', function() end); error('rollback')");
    ASSERT_EQ(evaluation.error, LuaError::RuntimeFault);
    ASSERT_FALSE(host.has_command("half"));
    ASSERT_TRUE(host.evaluate(
        "ssg.register_command('owned', function() ssg.command('missing') end)")
                    .accepted());
    auto duplicate = host.evaluate(
        "ssg.register_command('owned', function() end)");
    ASSERT_EQ(duplicate.error, LuaError::DuplicateCommand);
    ASSERT_TRUE(host.has_command("owned"));
}

TEST(dispatch_and_plugin_faults_are_isolated) {
    LuaCommandHost denied{options({{"edit", {}}}), [](LuaInvocation const&) {
        return CommandHandlerResult::failure("atomic edit rejected");
    }};
    ASSERT_EQ(denied.evaluate("ssg.command('edit')").error,
              LuaError::DispatchFailed);
    ASSERT_TRUE(denied.evaluate("return 1").accepted());

    LuaCommandHost callbacks{options(), [](LuaInvocation const&) {
        return CommandHandlerResult::success();
    }};
    ASSERT_TRUE(callbacks.evaluate(
        "ssg.register_command('broken', function() error('bad') end)")
                    .accepted());
    ASSERT_EQ(callbacks.invoke("broken").error, LuaError::RuntimeFault);
    ASSERT_EQ(callbacks.invoke("absent").error, LuaError::UnknownCommand);
}

TEST(unsafe_standard_libraries_and_native_loader_are_absent) {
    LuaCommandHost host{options(), [](LuaInvocation const&) {
        return CommandHandlerResult::success();
    }};
    ASSERT_TRUE(host.evaluate(
        "assert(package == nil and io == nil and os == nil and debug == nil "
        "and dofile == nil and loadfile == nil and load == nil "
        "and print == nil and warn == nil)")
                    .accepted());
}

}  // namespace

int main() {
    RUN(required_catalog_minus_exclusions_is_callable);
    RUN(capabilities_are_immutable_and_checked_before_dispatch);
    RUN(generational_handles_reject_stale_access_after_reuse);
    RUN(instruction_and_wall_clock_budgets_isolate_callbacks);
    RUN(reentrant_calls_restore_the_enclosing_budget);
    RUN(registration_is_atomic_and_duplicate_safe);
    RUN(dispatch_and_plugin_faults_are_isolated);
    RUN(unsafe_standard_libraries_and_native_loader_are_absent);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
