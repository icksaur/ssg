#include <ssg/Editor.h>

#include <ssg/CommandCatalog.h>
#include "test_helpers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// Pid-unique so parallel ctest runs cannot remove a directory another test is
// still using.
fs::path uniqueRoot() {
    auto root = testRuntimePath("dispatch_root_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "scratch");
    fs::create_directories(root / "recovery");
    return root;
}

std::unique_ptr<ssg::Editor> makeRuntime(fs::path const& root) {
    auto created = ssg::createEditor(
        {.cwd = root,
         .scratchRoot = root / "scratch",
         .recoveryRoot = root / "recovery",
         .enableGitDiffWorker = false,
         .enableFilesystemWatcher = false});
    auto runtime = std::move(created.session);
    if (runtime) {
    }
    return runtime;
}

// ---------------------------------------------------------------------------

TEST(viewActionResultsRemainExplicitAcrossTheAggregateBoundary) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;

    (void)runtime->registerCommand(ssg::CommandSpec{
        .id = "oracle.view_action",
        .owner = "test-oracle",
        .summary = "returns one typed view action",
        .effect = ssg::CommandEffect::ViewAction,
        .binding = ssg::bindNoArgumentHandler([](ssg::CommandContext&) {
            return ssg::CommandHandlerResult::requireView(
                ssg::ScrollPages{-2});
        }),
    });

    ASSERT_TRUE(
        runtime
            ->dispatch({"keymap.bind",
                 ssg::KeymapBindArguments{"Ctrl+KeyG",
                                          "oracle.view_action", "editor"}})
            .accepted());
    auto result = runtime->dispatch({"oracle.view_action", {}});
    ASSERT_TRUE(result.accepted());
    ASSERT_FALSE(result.completed());
    ASSERT_EQ(result.outcome(),
              ssg::CommandResult::Outcome::ViewActionRequired);
    ASSERT_TRUE(result.viewAction.has_value());
    if (result.viewAction) {
        ASSERT_EQ(*result.viewAction,
                  ssg::ViewAction{ssg::ScrollPages{-2}});
    }

    ssg::KeyStroke stroke;
    stroke.code = ssg::KeyCode::KeyG;
    stroke.control = true;
    auto input = runtime->input(ssg::ClientKeyInput{stroke, {}});
    ASSERT_EQ(input.outcome, ssg::ClientInputOutcome::ViewOwned);
    ASSERT_TRUE(input.command.has_value());
    if (input.command) {
        ASSERT_EQ(input.command->outcome(),
                  ssg::CommandResult::Outcome::ViewActionRequired);
        ASSERT_EQ(input.command->viewAction, result.viewAction);
    }
}

TEST(inputKeymapRebuildsForKeymapAndCatalogChanges) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;

    ASSERT_TRUE(
        runtime
            ->dispatch({"keymap.bind",
                        ssg::KeymapBindArguments{
                            "Ctrl+KeyG", "oracle.late_command", "editor"}})
            .accepted());

    ssg::KeyStroke firstStroke;
    firstStroke.code = ssg::KeyCode::KeyG;
    firstStroke.control = true;
    auto missing = runtime->input(ssg::ClientKeyInput{firstStroke, {}});
    ASSERT_EQ(missing.outcome, ssg::ClientInputOutcome::Rejected);
    ASSERT_TRUE(missing.command.has_value());
    if (missing.command) {
        ASSERT_EQ(missing.command->error, ssg::CommandError::UnknownCommand);
    }

    int calls = 0;
    (void)runtime->registerCommand(ssg::CommandSpec{
        .id = "oracle.late_command",
        .owner = "test-oracle",
        .summary = "records keymap cache invalidation",
        .effect = ssg::CommandEffect::Mutation,
        .binding = ssg::bindNoArgumentHandler(
            [&](ssg::CommandContext&) {
                ++calls;
                return ssg::CommandHandlerResult::success();
            }),
    });

    auto registered = runtime->input(ssg::ClientKeyInput{firstStroke, {}});
    ASSERT_EQ(registered.outcome, ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(calls, 1);

    ASSERT_TRUE(
        runtime
            ->dispatch({"keymap.bind",
                        ssg::KeymapBindArguments{
                            "Ctrl+KeyH", "oracle.late_command", "editor"}})
            .accepted());
    ssg::KeyStroke secondStroke;
    secondStroke.code = ssg::KeyCode::KeyH;
    secondStroke.control = true;
    auto rebound = runtime->input(ssg::ClientKeyInput{secondStroke, {}});
    ASSERT_EQ(rebound.outcome, ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(calls, 2);
}

// THE ORACLE for reentrant dispatch.
//
// Counts accepted mutating dispatches independently of the revision counter --
// each handler increments a plain integer when it runs -- and asserts the
// session revision advanced by exactly that many steps.
//
// This is the property that decides whether a handler may dispatch
// synchronously. It cannot: the outer mutation may commit state computed before
// the nested mutation ran, overwriting its result. Two accepted
// mutations, one revision step -- and a client replaying deltas against a base
// revision silently misses an edit (I3).
//
// Deferral satisfies the property because each deferred command is its own
// dispatch with its own revision step.
//
// To perturb: delete the nested-dispatch guards in CommandCatalog::dispatch and
// Editor::dispatch, have the outer handler dispatch instead of defer,
// and REBUILD THE LIBRARY (a probe linked against a stale libssg.a still
// contains the guards and reports a false pass).  The counts then diverge.
// The same property across a NESTED chain: a deferred command that itself
// queues more.  Depth must not collapse revision steps either.
// An observing command must not advance the revision at all, so the oracle
// above is counting mutations rather than dispatches.
// A failed handler performs none of its requests, so a refused chain must not
// advance the revision for commands that never ran.
// A handler that dispatches is refused -- and told what to do instead.  The
// prohibition alone leaves a script author stuck: composing commands is a
// supported thing to want, so the message has to name the alternative.
TEST(aHandlerThatDispatchesIsToldToDeferInstead) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);

    ssg::CommandResult nested{};
    ASSERT_TRUE(
        runtime
            ->registerCommand(ssg::CommandSpec{
                .id = "oracle.dispatches",
                .owner = "test-oracle",
                .summary = "dispatches from its handler",
                .effect = ssg::CommandEffect::Mutation,
                .binding = ssg::bindNoArgumentHandler(
                    [&nested, &runtime](ssg::CommandContext& ctx) {
                        nested = runtime->dispatch({"oracle.dispatches",  {}});
                        return ssg::CommandHandlerResult::success();
                    }),
            })
            .valid());

    ASSERT_TRUE(runtime
                    ->dispatch({"oracle.dispatches",  {}})
                    .accepted());

    ASSERT_TRUE(!nested.accepted());
    ASSERT_EQ(nested.error, ssg::CommandError::HandlerFailed);
    // Names the alternative, not only the prohibition.
    ASSERT_TRUE(nested.message.find("instead") != std::string::npos);
    // And says why it matters, so the rule is not mistaken for an arbitrary
    // limitation by whoever reads it next.
    ASSERT_TRUE(nested.message.find("serialized") != std::string::npos);
    ASSERT_EQ(std::string{ssg::kNestedDispatchRefusal},
              nested.message);

    fs::remove_all(root);
}

TEST(routingCommandsQueueExactlyOneDirectOrdinaryTarget) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;

    int mutations = 0;
    ASSERT_TRUE(runtime->registerCommand(ssg::CommandSpec{
        .id = "oracle.route_target",
        .owner = "test-oracle",
        .summary = "target",
        .effect = ssg::CommandEffect::Mutation,
        .binding = ssg::bindNoArgumentHandler([&](ssg::CommandContext&) {
            ++mutations;
            return ssg::CommandHandlerResult::success();
        }),
    })
                    .valid());
    ASSERT_TRUE(runtime->registerCommand(ssg::CommandSpec{
        .id = "oracle.route_once",
        .owner = "test-oracle",
        .summary = "route once",
        .effect = ssg::CommandEffect::Routing,
        .binding = ssg::bindNoArgumentHandler(
            [&](ssg::CommandContext& context) {
                return runtime->deferDispatch({"oracle.route_target",  {}})
                           ? ssg::CommandHandlerResult::success()
                           : ssg::CommandHandlerResult::failure("queue failed");
            }),
    })
                    .valid());
    ASSERT_TRUE(runtime->registerCommand(ssg::CommandSpec{
        .id = "oracle.route_none",
        .owner = "test-oracle",
        .summary = "route none",
        .effect = ssg::CommandEffect::Routing,
        .binding = ssg::bindNoArgumentHandler([](ssg::CommandContext&) {
            return ssg::CommandHandlerResult::success();
        }),
    })
                    .valid());
    ASSERT_TRUE(runtime->registerCommand(ssg::CommandSpec{
        .id = "oracle.route_twice",
        .owner = "test-oracle",
        .summary = "route twice",
        .effect = ssg::CommandEffect::Routing,
        .binding = ssg::bindNoArgumentHandler(
            [&](ssg::CommandContext& context) {
                const bool first = runtime->deferDispatch({"oracle.route_target",  {}});
                const bool second = runtime->deferDispatch({"oracle.route_target",  {}});
                return first && second
                           ? ssg::CommandHandlerResult::success()
                           : ssg::CommandHandlerResult::failure("queue failed");
            }),
    })
                    .valid());
    ASSERT_TRUE(runtime->registerCommand(ssg::CommandSpec{
        .id = "oracle.route_nested",
        .owner = "test-oracle",
        .summary = "route nested",
        .effect = ssg::CommandEffect::Routing,
        .binding = ssg::bindNoArgumentHandler(
            [&](ssg::CommandContext& context) {
                return runtime->deferDispatch({"oracle.route_once",  {}})
                           ? ssg::CommandHandlerResult::success()
                           : ssg::CommandHandlerResult::failure("queue failed");
            }),
    })
                    .valid());

    const auto accepted = runtime->dispatch({"oracle.route_once", {}});
    ASSERT_TRUE(accepted.accepted());
    ASSERT_EQ(mutations, 1);

    for (const std::string_view id :
         {"oracle.route_none", "oracle.route_twice", "oracle.route_nested"}) {
        const auto rejected = runtime->dispatch({std::string{id}, {}});
        ASSERT_FALSE(rejected.accepted());
        ASSERT_EQ(rejected.error, ssg::CommandError::HandlerFailed);
            ASSERT_EQ(mutations, 1);
    }
    fs::remove_all(root);
}

TEST(aHandlerCannotMutateTheCommandCatalogReentrantly) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);

    bool registrationRefused = false;
    bool replacementRefused = false;
    ASSERT_TRUE(
        runtime
            ->registerCommand(ssg::CommandSpec{
                .id = "oracle.registers",
                .owner = "test-oracle",
                .summary = "attempts catalog mutation",
                .effect = ssg::CommandEffect::Observation,
                .binding = ssg::bindNoArgumentHandler(
                    [&](ssg::CommandContext&) {
                        try {
                            (void)runtime->registerCommand(ssg::CommandSpec{
                                .id = "oracle.illegal",
                                .owner = "test-oracle",
                                .summary = "must not be registered",
                                .effect = ssg::CommandEffect::Observation,
                                .binding = ssg::bindNoArgumentHandler(
                                    [](ssg::CommandContext&) {
                                        return ssg::CommandHandlerResult::
                                            success();
                                    }),
                            });
                        } catch (std::logic_error const&) {
                            registrationRefused = true;
                        }
                        try {
                            (void)runtime->replaceCommandGeneration(
                                std::span<ssg::CommandHandle const>{}, {});
                        } catch (std::logic_error const&) {
                            replacementRefused = true;
                        }
                        return ssg::CommandHandlerResult::success();
                    }),
            })
            .valid());

    ASSERT_TRUE(runtime
                    ->dispatch({"oracle.registers",  {}})
                    .accepted());
    ASSERT_TRUE(registrationRefused);
    ASSERT_TRUE(replacementRefused);
    ASSERT_TRUE(runtime->commandCatalog().find("oracle.illegal") == nullptr);

    fs::remove_all(root);
}

TEST(editorSessionAbsorbsSuccessfulWorkspaceChanges) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;

    (void)runtime->registerCommand(ssg::CommandSpec{
        .id = "oracle.workspace",
        .owner = "test-oracle",
        .summary = "changes the active workspace",
        .effect = ssg::CommandEffect::Mutation,
        .binding = ssg::bindNoArgumentHandler([](ssg::CommandContext& context) {
            context.setActiveWorkspace(ssg::WorkspaceId{7});
            return ssg::CommandHandlerResult::success();
        }),
    });

    ASSERT_TRUE(runtime->dispatch({"oracle.workspace", {}}).accepted());
    ASSERT_EQ(runtime->topology().activeWorkspace,
              std::optional<ssg::WorkspaceId>{ssg::WorkspaceId{7}});
    fs::remove_all(root);
}

}  // namespace

SSG_TEST_SUITE(test_command_dispatch) {
    RUN(viewActionResultsRemainExplicitAcrossTheAggregateBoundary);
    RUN(inputKeymapRebuildsForKeymapAndCatalogChanges);
    RUN(aHandlerThatDispatchesIsToldToDeferInstead);
    RUN(routingCommandsQueueExactlyOneDirectOrdinaryTarget);
    RUN(aHandlerCannotMutateTheCommandCatalogReentrantly);
    RUN(editorSessionAbsorbsSuccessfulWorkspaceChanges);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
