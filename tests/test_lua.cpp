#include "test_helpers.h"

#include <ssg/Commands.h>

#include "all_command_ids.h"
#include <ssg/LuaCommandHost.h>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using namespace ssg;

// Every catalog command with its Lua-API eligibility.
std::vector<std::pair<std::string, bool>> catalogWithLuaEligibility() {
    std::vector<std::pair<std::string, bool>> entries;
    for (auto const& facts : ssg::testing::allCommandFacts()) {
        entries.emplace_back(facts.id, facts.luaApi);
    }
    return entries;
}

LuaCommandHostOptions options(std::vector<LuaCommand> commands = {}) {
    LuaCommandHostOptions result;
    result.pluginId = ClientId{81};
    result.commands = std::move(commands);
    return result;
}

TEST(requiredCatalogMinusExclusionsIsCallable) {
    auto const catalog = catalogWithLuaEligibility();
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
            called.emplace(invocation.commandId);
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

TEST(capabilitiesAreImmutableAndCheckedBeforeDispatch) {
    auto configured = options({{"safe", {}}, {"privileged", {CapabilityId{"fs"}}}});
    configured.capabilities.emplace_back("network");
    bool dispatched = false;
    LuaCommandHost host{std::move(configured),
        [&](LuaInvocation const& invocation) {
            dispatched = true;
            ASSERT_TRUE(invocation.principal.hasCapability(
                CapabilityId{"network"}));
            return CommandHandlerResult::success();
        }};
    auto denied = host.evaluate("ssg.command('privileged')");
    ASSERT_EQ(denied.error, LuaError::CapabilityDenied);
    ASSERT_FALSE(dispatched);
    ASSERT_TRUE(host.evaluate("ssg.command('safe')").accepted());
    ASSERT_TRUE(dispatched);
}

TEST(generationalHandlesRejectStaleAccessAfterReuse) {
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

TEST(instructionAndWallClockBudgetsIsolateCallbacks) {
    auto configured = options();
    configured.instructionBudget = 2'000;
    configured.timeBudget = std::chrono::milliseconds{5};
    LuaCommandHost host{std::move(configured), [](LuaInvocation const&) {
        return CommandHandlerResult::success();
    }};
    ASSERT_EQ(host.evaluate("while true do end").error,
              LuaError::BudgetExhausted);
    ASSERT_TRUE(host.evaluate("return 7").accepted());
}

TEST(reentrantCallsRestoreTheEnclosingBudget) {
    LuaCommandHost* reentrant = nullptr;
    auto configured = options({{"reenter", {}}});
    configured.instructionBudget = 2'000;
    configured.timeBudget = std::chrono::milliseconds{5};
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

TEST(registrationIsAtomicAndDuplicateSafe) {
    LuaCommandHost host{options(), [](LuaInvocation const&) {
        return CommandHandlerResult::success();
    }};
    auto evaluation = host.evaluate(
        "ssg.register_command('half', function() end); error('rollback')");
    ASSERT_EQ(evaluation.error, LuaError::RuntimeFault);
    ASSERT_FALSE(host.hasCommand("half"));
    ASSERT_TRUE(host.evaluate(
        "ssg.register_command('owned', function() ssg.command('missing') end)")
                    .accepted());
    auto duplicate = host.evaluate(
        "ssg.register_command('owned', function() end)");
    ASSERT_EQ(duplicate.error, LuaError::DuplicateCommand);
    ASSERT_TRUE(host.hasCommand("owned"));
}

TEST(dispatchAndPluginFaultsAreIsolated) {
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

TEST(commandTableArgumentReachesTheDispatcherDecodedAsAStringMap) {
    std::optional<std::unordered_map<std::string, std::string>> received;
    LuaCommandHost host{options({{"configure", {}}}),
        [&](LuaInvocation const& invocation) {
            received = invocation.arguments;
            return CommandHandlerResult::success();
        }};

    ASSERT_TRUE(host.evaluate(
        "ssg.command('configure', {red = 'crimson', name = 'dark'})")
                    .accepted());
    ASSERT_TRUE(received.has_value());
    ASSERT_EQ(received->size(), std::size_t{2});
    ASSERT_EQ(received->at("red"), "crimson");
    ASSERT_EQ(received->at("name"), "dark");
}

TEST(commandWithoutSecondArgumentLeavesArgumentsEmpty) {
    std::optional<std::unordered_map<std::string, std::string>> received{
        std::unordered_map<std::string, std::string>{{"stale", "value"}}};
    LuaCommandHost host{options({{"noop", {}}}),
        [&](LuaInvocation const& invocation) {
            received = invocation.arguments;
            return CommandHandlerResult::success();
        }};
    ASSERT_TRUE(host.evaluate("ssg.command('noop')").accepted());
    ASSERT_FALSE(received.has_value());
}

TEST(malformedCommandArgumentIsRejectedBeforeTheDispatcherIsCalled) {
    bool dispatched = false;
    LuaCommandHost host{options({{"configure", {}}}),
        [&](LuaInvocation const&) {
            dispatched = true;
            return CommandHandlerResult::success();
        }};

    // A non-table second argument.
    auto nonTable = host.evaluate("ssg.command('configure', 'oops')");
    ASSERT_EQ(nonTable.error, LuaError::InvalidScript);
    ASSERT_FALSE(dispatched);

    // A table with a non-string VALUE.
    auto nonStringValue =
        host.evaluate("ssg.command('configure', {red = 42})");
    ASSERT_EQ(nonStringValue.error, LuaError::InvalidScript);
    ASSERT_FALSE(dispatched);

    // A table with a non-string KEY.
    auto nonStringKey =
        host.evaluate("ssg.command('configure', {[1] = 'x'})");
    ASSERT_EQ(nonStringKey.error, LuaError::InvalidScript);
    ASSERT_FALSE(dispatched);

    // Confirm the host still works normally afterward (a rejected call
    // leaves no residual state).
    ASSERT_TRUE(host.evaluate("ssg.command('configure', {ok = 'yes'})")
                    .accepted());
    ASSERT_TRUE(dispatched);
}

TEST(unsafeStandardLibrariesAndNativeLoaderAreAbsent) {
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
    RUN(requiredCatalogMinusExclusionsIsCallable);
    RUN(capabilitiesAreImmutableAndCheckedBeforeDispatch);
    RUN(generationalHandlesRejectStaleAccessAfterReuse);
    RUN(instructionAndWallClockBudgetsIsolateCallbacks);
    RUN(reentrantCallsRestoreTheEnclosingBudget);
    RUN(registrationIsAtomicAndDuplicateSafe);
    RUN(dispatchAndPluginFaultsAreIsolated);
    RUN(commandTableArgumentReachesTheDispatcherDecodedAsAStringMap);
    RUN(commandWithoutSecondArgumentLeavesArgumentsEmpty);
    RUN(malformedCommandArgumentIsRejectedBeforeTheDispatcherIsCalled);
    RUN(unsafeStandardLibrariesAndNativeLoaderAreAbsent);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
