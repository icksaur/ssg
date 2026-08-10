#include "test_helpers.h"


#include "all_command_ids.h"
#include <ssg/LuaCommandHost.h>

#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
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
    result.chromeProviders = {"path", "branch", "status", "follow"};
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
    // Registering the same id TWICE IN ONE evaluation is a mistake in the
    // script, and is refused.
    auto duplicate = host.evaluate(
        "ssg.register_command('twice', function() end);"
        "ssg.register_command('twice', function() end)");
    ASSERT_EQ(duplicate.error, LuaError::DuplicateCommand);

    // Registering it again in a LATER evaluation is a reload, not a mistake:
    // each evaluation replaces the previous evaluation's registrations, so a
    // script that registers the same ids every time must keep working.
    ASSERT_TRUE(host.evaluate(
        "ssg.register_command('owned', function() end)").accepted());
    ASSERT_TRUE(host.hasCommand("owned"));

    // And a command the newest evaluation did NOT register is gone, rather
    // than accumulating across reloads.
    ASSERT_TRUE(host.evaluate(
        "ssg.register_command('replacement', function() end)").accepted());
    ASSERT_FALSE(host.hasCommand("owned"));
    ASSERT_TRUE(host.hasCommand("replacement"));
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

// --- ssg.chrome staging ---

LuaCommandHost chromeHost() {
    return LuaCommandHost{options(), [](LuaInvocation const&) {
                             return CommandHandlerResult::success();
                         }};
}

// A well-formed ssg.chrome call stages a composition the host then surfaces,
// decoded through the nested-table walker (providers resolved, regions kept).
TEST(chromeCallStagesTheComposition) {
    auto host = chromeHost();
    ASSERT_FALSE(host.composition().has_value());
    auto const result = host.evaluate(
        "ssg.chrome{ header = { left = { { kind = 'field', provider = 'path' } } },"
        " footer = { left = { { kind = 'label', text = 'RO' } } } }");
    ASSERT_TRUE(result.accepted());
    ASSERT_TRUE(host.composition().has_value());
    ASSERT_TRUE(host.composition()->header.has_value());
    ASSERT_TRUE(host.composition()->footer.has_value());
    ASSERT_EQ(host.composition()->header->left.size(), std::size_t{1});
    ASSERT_TRUE(host.composition()->header->left[0].value.has_value());
    ASSERT_TRUE(host.composition()->header->left[0].value->isProvider);
    ASSERT_EQ(host.composition()->header->left[0].value->provider,
              std::string{"path"});
}

// Second ssg.chrome call in one evaluation replaces the first (last wins).
TEST(chromeLastCallWins) {
    auto host = chromeHost();
    ASSERT_TRUE(host.evaluate(
        "ssg.chrome{ header = { left = { { kind = 'label', text = 'A' } } } }"
        "\nssg.chrome{ footer = { left = { { kind = 'label', text = 'B' } } } }")
                    .accepted());
    ASSERT_TRUE(host.composition().has_value());
    ASSERT_FALSE(host.composition()->header.has_value());
    ASSERT_TRUE(host.composition()->footer.has_value());
}

// A script that calls ssg.chrome then errors leaves the PRIOR composition
// intact (staged-then-rolled-back), and a later successful reload that drops the
// call reverts to built-in (nullopt).
TEST(chromeRollsBackOnLaterScriptError) {
    auto host = chromeHost();
    ASSERT_TRUE(host.evaluate(
        "ssg.chrome{ header = { left = { { kind = 'label', text = 'keep' } } } }")
                    .accepted());
    ASSERT_TRUE(host.composition().has_value());

    // A second evaluation composes different chrome then raises: the previous
    // composition must survive unchanged.
    auto const errored = host.evaluate(
        "ssg.chrome{ footer = { left = { { kind = 'label', text = 'gone' } } } }"
        "\nerror('boom')");
    ASSERT_FALSE(errored.accepted());
    ASSERT_TRUE(host.composition().has_value());
    ASSERT_TRUE(host.composition()->header.has_value());
    ASSERT_FALSE(host.composition()->footer.has_value());

    // A clean reload with no ssg.chrome reverts to built-in.
    ASSERT_TRUE(host.evaluate("local x = 1").accepted());
    ASSERT_FALSE(host.composition().has_value());
}

// An invalid descriptor fails the whole call loud (path-qualified) and stages
// nothing.
TEST(chromeInvalidDescriptorFailsLoud) {
    auto host = chromeHost();
    auto const unknownKind = host.evaluate(
        "ssg.chrome{ header = { left = { { kind = 'buton', text = 'x' } } } }");
    ASSERT_EQ(unknownKind.error, LuaError::InvalidScript);
    ASSERT_TRUE(unknownKind.message.find("unknown kind") != std::string::npos);
    ASSERT_FALSE(host.composition().has_value());

    auto const unknownProvider = host.evaluate(
        "ssg.chrome{ header = { left = { { kind = 'field', provider = 'nope' } } } }");
    ASSERT_EQ(unknownProvider.error, LuaError::InvalidScript);
    ASSERT_TRUE(unknownProvider.message.find("unknown provider") !=
                std::string::npos);

    ASSERT_EQ(host.evaluate("ssg.chrome('not a table')").error,
              LuaError::InvalidScript);
}

// A cyclic table cannot hang the walker: the depth guard fails it loud.
TEST(chromeCyclicTableIsRejected) {
    auto host = chromeHost();
    auto const result =
        host.evaluate("local t = {} t.left = t ssg.chrome{ header = t }");
    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.error, LuaError::InvalidScript);
    ASSERT_TRUE(result.message.find("nests too deeply") != std::string::npos);
    ASSERT_FALSE(host.composition().has_value());
}

// A non-string key inside a descriptor table is rejected, not dropped -- the
// fail-loud key contract must not regress to a silent loss. An explicitly empty
// side is likewise a loud type error (a side must be an array), while an empty
// root table composes nothing.
TEST(chromeMalformedTableShapesFailLoud) {
    auto host = chromeHost();
    // A stray array index beside named widget fields.
    auto const mixedWidget = host.evaluate(
        "ssg.chrome{ header = { left = { { kind = 'label', text = 'x', [1] = 'oops' } } } }");
    ASSERT_EQ(mixedWidget.error, LuaError::InvalidScript);
    ASSERT_FALSE(host.composition().has_value());

    // A boolean key is neither a name nor an index.
    auto const boolKey = host.evaluate(
        "local w = { kind = 'label', text = 'x' } w[true] = 1"
        " ssg.chrome{ header = { left = { w } } }");
    ASSERT_EQ(boolKey.error, LuaError::InvalidScript);

    // An explicitly empty side is a type error (a side is an array of widgets).
    auto const emptySide =
        host.evaluate("ssg.chrome{ header = { left = {} } }");
    ASSERT_EQ(emptySide.error, LuaError::InvalidScript);

    // An empty root composes nothing (valid, no override).
    ASSERT_TRUE(host.evaluate("ssg.chrome{}").accepted());
    ASSERT_TRUE(host.composition().has_value());
    ASSERT_FALSE(host.composition()->header.has_value());
    ASSERT_FALSE(host.composition()->footer.has_value());
}

}  // namespace

TEST(aGateThatThrowsRollsTheEvaluationBackLikeAnyOtherRefusal) {
    // The gate is caller-supplied. An escaping exception must not skip the
    // rollback: that would leak the staged Lua references and leave the
    // evaluation neither published nor undone.
    bool throwing = true;
    auto configured = options();
    configured.publishGate =
        [&throwing](std::vector<std::string> const&) -> LuaResult {
        if (throwing) throw std::runtime_error{"gate refused loudly"};
        return {};
    };
    LuaCommandHost host{std::move(configured), [](LuaInvocation const&) {
        return CommandHandlerResult::success();
    }};

    auto const thrown =
        host.evaluate("ssg.register_command('gated', function() end)");
    ASSERT_EQ(thrown.error, LuaError::RuntimeFault);
    ASSERT_FALSE(host.hasCommand("gated"));
    // And the host is still usable.
    throwing = false;
    ASSERT_TRUE(host.evaluate("return 1").accepted());
}

TEST(aGateThatRefusesLeavesThePreviousGenerationRegistered) {
    bool refuse = false;
    auto configured = options();
    configured.publishGate =
        [&refuse](std::vector<std::string> const&) -> LuaResult {
        return refuse ? LuaResult{LuaError::DuplicateCommand, "refused"}
                      : LuaResult{};
    };
    LuaCommandHost host{std::move(configured), [](LuaInvocation const&) {
        return CommandHandlerResult::success();
    }};

    ASSERT_TRUE(
        host.evaluate("ssg.register_command('first', function() end)")
            .accepted());
    refuse = true;
    ASSERT_FALSE(
        host.evaluate("ssg.register_command('second', function() end)")
            .accepted());
    ASSERT_TRUE(host.hasCommand("first"));
    ASSERT_FALSE(host.hasCommand("second"));
}

TEST(aGateMayNotReEnterTheHostItIsGating) {
    // The gate runs with this evaluation's registrations staged and the
    // previous generation's still installed. Re-entering the host would work
    // against that half-swapped state and could interleave two generations, so
    // it is refused rather than left to the gate author's discretion.
    LuaResult nested{};
    LuaCommandHost* self = nullptr;
    auto configured = options();
    configured.publishGate =
        [&nested, &self](std::vector<std::string> const&) -> LuaResult {
        if (self != nullptr) nested = self->evaluate("return 1");
        return {};
    };
    LuaCommandHost host{std::move(configured), [](LuaInvocation const&) {
        return CommandHandlerResult::success();
    }};
    self = &host;

    ASSERT_TRUE(
        host.evaluate("ssg.register_command('outer', function() end)")
            .accepted());
    ASSERT_EQ(nested.error, LuaError::RuntimeFault);
    // The outer evaluation still completed normally.
    ASSERT_TRUE(host.hasCommand("outer"));
}

int main() {
    RUN(requiredCatalogMinusExclusionsIsCallable);
    RUN(capabilitiesAreImmutableAndCheckedBeforeDispatch);
    RUN(generationalHandlesRejectStaleAccessAfterReuse);
    RUN(instructionAndWallClockBudgetsIsolateCallbacks);
    RUN(reentrantCallsRestoreTheEnclosingBudget);
    RUN(registrationIsAtomicAndDuplicateSafe);
    RUN(aGateThatThrowsRollsTheEvaluationBackLikeAnyOtherRefusal);
    RUN(aGateThatRefusesLeavesThePreviousGenerationRegistered);
    RUN(aGateMayNotReEnterTheHostItIsGating);
    RUN(dispatchAndPluginFaultsAreIsolated);
    RUN(commandTableArgumentReachesTheDispatcherDecodedAsAStringMap);
    RUN(commandWithoutSecondArgumentLeavesArgumentsEmpty);
    RUN(malformedCommandArgumentIsRejectedBeforeTheDispatcherIsCalled);
    RUN(unsafeStandardLibrariesAndNativeLoaderAreAbsent);
    RUN(chromeCallStagesTheComposition);
    RUN(chromeLastCallWins);
    RUN(chromeRollsBackOnLaterScriptError);
    RUN(chromeInvalidDescriptorFailsLoud);
    RUN(chromeCyclicTableIsRejected);
    RUN(chromeMalformedTableShapesFailLoud);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
