#include <ssg/EditorSession.h>

#include <ssg/CommandCatalog.h>
#include "../src/runtime/command_executor.h"

#include "test_helpers.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// Pid-unique so parallel ctest runs cannot remove a directory another test is
// still using.
fs::path uniqueRoot() {
    auto root = fs::current_path() /
                ("dispatch_root_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "scratch");
    fs::create_directories(root / "recovery");
    return root;
}

std::unique_ptr<ssg::EditorSession> makeRuntime(fs::path const& root) {
    auto created = ssg::EditorSession::create(
        {.cwd = root,
         .scratchRoot = root / "scratch",
         .recoveryRoot = root / "recovery",
         .enableGitDiffWorker = false,
         .enableFilesystemWatcher = false});
    auto runtime = std::move(created.session);
    if (runtime) {
        (void)runtime->attach(
            {ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
            ssg::ViewId{1});
    }
    return runtime;
}

// ---------------------------------------------------------------------------

// THE ORACLE for reentrant dispatch.
//
// Counts accepted mutating dispatches independently of the revision counter --
// each handler increments a plain integer when it runs -- and asserts the
// session revision advanced by exactly that many steps.
//
// This is the property that decides whether a handler may dispatch
// synchronously. It cannot: CommandExecutor::dispatch computes the new revision
// from a value captured BEFORE the handler ran, so a nested mutation advances
// the revision and the outer then writes its own value over it.  Two accepted
// mutations, one revision step -- and a client replaying deltas against a base
// revision silently misses an edit (I3).
//
// Deferral satisfies the property because each deferred command is its own
// dispatch with its own revision step.
//
// To perturb: make CommandExecutor::Impl::mutex a std::recursive_mutex, delete
// the nested-dispatch guards in CommandExecutor::dispatch and
// EditorSession::dispatch, have the outer handler dispatch instead of defer,
// and REBUILD THE LIBRARY (a probe linked against a stale libssg.a still
// contains the guards and reports a false pass).  The counts then diverge.
TEST(revisionAdvancesExactlyOncePerAcceptedMutation) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);

    int acceptedMutations = 0;
    runtime->registerCommand(ssg::CommandSpecBuilder{"oracle.leaf"}
                     .owner("test-oracle")
                     .summary("counts itself")
                     .mutates()
                     .handler([&acceptedMutations](ssg::CommandContext&) {
                         ++acceptedMutations;
                         return ssg::CommandHandlerResult::success();
                     }));

    // Asks for two more commands, so one dispatch becomes a chain of three
    // accepted mutations.
    runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.chain"}
            .owner("test-oracle")
            .summary("defers two leaves")
            .mutates()
            .handler([&acceptedMutations, &runtime](ssg::CommandContext& ctx) {
                ++acceptedMutations;
                for (int queued = 0; queued < 2; ++queued) {
                    if (!runtime->deferDispatch(
                            ssg::ClientId{1},
                            {"oracle.leaf", ctx.revision(), {}})) {
                        return ssg::CommandHandlerResult::failure(
                            "could not queue");
                    }
                }
                return ssg::CommandHandlerResult::success();
            }));

    auto const before = runtime->revision().value();
    auto const result = runtime->dispatch(
        ssg::ClientId{1}, {"oracle.chain", runtime->revision(), {}});
    ASSERT_TRUE(result.accepted());
    auto const after = runtime->revision().value();

    ASSERT_EQ(acceptedMutations, 3);
    ASSERT_EQ(after - before, static_cast<std::uint64_t>(acceptedMutations));

    fs::remove_all(root);
}

// The same property across a NESTED chain: a deferred command that itself
// queues more.  Depth must not collapse revision steps either.
TEST(revisionAdvancesOncePerMutationAcrossANestedChain) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);

    int acceptedMutations = 0;
    runtime->registerCommand(ssg::CommandSpecBuilder{"oracle.deep_leaf"}
                     .owner("test-oracle")
                     .summary("counts itself")
                     .mutates()
                     .handler([&acceptedMutations](ssg::CommandContext&) {
                         ++acceptedMutations;
                         return ssg::CommandHandlerResult::success();
                     }));
    runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.deep_middle"}
            .owner("test-oracle")
            .summary("defers a leaf")
            .mutates()
            .handler([&acceptedMutations, &runtime](ssg::CommandContext& ctx) {
                ++acceptedMutations;
                if (!runtime->deferDispatch(
                        ssg::ClientId{1},
                        {"oracle.deep_leaf", ctx.revision(), {}})) {
                    return ssg::CommandHandlerResult::failure("could not queue");
                }
                return ssg::CommandHandlerResult::success();
            }));
    runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.deep_outer"}
            .owner("test-oracle")
            .summary("defers a middle")
            .mutates()
            .handler([&acceptedMutations, &runtime](ssg::CommandContext& ctx) {
                ++acceptedMutations;
                if (!runtime->deferDispatch(
                        ssg::ClientId{1},
                        {"oracle.deep_middle", ctx.revision(), {}})) {
                    return ssg::CommandHandlerResult::failure("could not queue");
                }
                return ssg::CommandHandlerResult::success();
            }));

    auto const before = runtime->revision().value();
    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"oracle.deep_outer", runtime->revision(), {}})
                    .accepted());
    auto const after = runtime->revision().value();

    ASSERT_EQ(acceptedMutations, 3);
    ASSERT_EQ(after - before, static_cast<std::uint64_t>(acceptedMutations));

    fs::remove_all(root);
}

// An observing command must not advance the revision at all, so the oracle
// above is counting mutations rather than dispatches.
TEST(anObservingCommandLeavesTheRevisionAlone) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);

    runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.observe"}
            .owner("test-oracle")
            .summary("changes nothing")
            .observes()
            .handler([](ssg::CommandContext&) {
                return ssg::CommandHandlerResult::success();
            }));

    auto const before = runtime->revision().value();
    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"oracle.observe", runtime->revision(), {}})
                    .accepted());
    ASSERT_EQ(runtime->revision().value(), before);

    fs::remove_all(root);
}

// A failed handler performs none of its requests, so a refused chain must not
// advance the revision for commands that never ran.
TEST(aFailedChainAdvancesTheRevisionOnlyForCommandsThatRan) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);

    int acceptedMutations = 0;
    runtime->registerCommand(ssg::CommandSpecBuilder{"oracle.refuses"}
                     .owner("test-oracle")
                     .summary("always fails")
                     .mutates()
                     .handler([](ssg::CommandContext&) {
                         return ssg::CommandHandlerResult::failure("no");
                     }));
    runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.queues_a_failure"}
            .owner("test-oracle")
            .summary("defers a command that fails")
            .mutates()
            .handler([&acceptedMutations, &runtime](ssg::CommandContext& ctx) {
                ++acceptedMutations;
                if (!runtime->deferDispatch(
                        ssg::ClientId{1},
                        {"oracle.refuses", ctx.revision(), {}})) {
                    return ssg::CommandHandlerResult::failure("could not queue");
                }
                return ssg::CommandHandlerResult::success();
            }));

    auto const before = runtime->revision().value();
    auto const result =
        runtime->dispatch(ssg::ClientId{1},
                          {"oracle.queues_a_failure", runtime->revision(), {}});
    ASSERT_TRUE(!result.accepted());
    // The outer ran and was accepted; the queued one was refused by its own
    // handler and changed nothing.
    ASSERT_EQ(acceptedMutations, 1);
    ASSERT_EQ(runtime->revision().value() - before,
              static_cast<std::uint64_t>(acceptedMutations));

    fs::remove_all(root);
}


// A handler that dispatches is refused -- and told what to do instead.  The
// prohibition alone leaves a script author stuck: composing commands is a
// supported thing to want, so the message has to name the alternative.
TEST(aHandlerThatDispatchesIsToldToDeferInstead) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);

    ssg::CommandResult nested{};
    runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.dispatches"}
            .owner("test-oracle")
            .summary("dispatches from its handler")
            .mutates()
            .handler([&nested, &runtime](ssg::CommandContext& ctx) {
                nested = runtime->dispatch(
                    ssg::ClientId{1}, {"oracle.dispatches", ctx.revision(), {}});
                return ssg::CommandHandlerResult::success();
            }));

    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"oracle.dispatches", runtime->revision(), {}})
                    .accepted());

    ASSERT_TRUE(!nested.accepted());
    ASSERT_EQ(nested.error, ssg::CommandError::HandlerFailed);
    // Names the alternative, not only the prohibition.
    ASSERT_TRUE(nested.message.find("instead") != std::string::npos);
    // And says why it matters, so the rule is not mistaken for an arbitrary
    // limitation by whoever reads it next.
    ASSERT_TRUE(nested.message.find("revision") != std::string::npos);
    ASSERT_EQ(std::string{ssg::kNestedDispatchRefusal},
              nested.message);

    fs::remove_all(root);
}

TEST(aHandlerCannotMutateTheCommandCatalogReentrantly) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);

    bool registrationRefused = false;
    bool replacementRefused = false;
    runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.registers"}
            .owner("test-oracle")
            .summary("attempts catalog mutation")
            .observes()
            .handler([&](ssg::CommandContext&) {
                try {
                    (void)runtime->registerCommand(
                        ssg::CommandSpecBuilder{"oracle.illegal"}
                            .owner("test-oracle")
                            .summary("must not be registered")
                            .observes()
                            .handler([](ssg::CommandContext&) {
                                return ssg::CommandHandlerResult::success();
                            }));
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
            }));

    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"oracle.registers", runtime->revision(), {}})
                    .accepted());
    ASSERT_TRUE(registrationRefused);
    ASSERT_TRUE(replacementRefused);
    ASSERT_TRUE(runtime->commandCatalog()->find("oracle.illegal") == nullptr);

    fs::remove_all(root);
}

TEST(aggregateOperationHidesIntermediateDeferredRevisions) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);

    std::promise<void> deferredStartedPromise;
    auto deferredStarted = deferredStartedPromise.get_future();
    std::promise<void> releaseDeferredPromise;
    auto releaseDeferred = releaseDeferredPromise.get_future().share();
    std::atomic<int> stage{0};
    std::atomic<std::uint64_t> observedRevision{0};

    runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.blocking_deferred"}
            .owner("test-oracle")
            .summary("blocks the aggregate operation")
            .mutates()
            .handler([&](ssg::CommandContext&) {
                stage.store(2);
                deferredStartedPromise.set_value();
                releaseDeferred.wait();
                stage.store(3);
                return ssg::CommandHandlerResult::success();
            }));
    runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.primary"}
            .owner("test-oracle")
            .summary("queues the blocking command")
            .mutates()
            .handler([&](ssg::CommandContext& context) {
                stage.store(1);
                if (!runtime->deferDispatch(
                        ssg::ClientId{1},
                        {"oracle.blocking_deferred", context.revision(), {}})) {
                    return ssg::CommandHandlerResult::failure(
                        "could not queue");
                }
                return ssg::CommandHandlerResult::success();
            }));
    runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.second"}
            .owner("test-oracle")
            .summary("records the revision it observes")
            .observes()
            .handler([&](ssg::CommandContext& context) {
                observedRevision.store(context.revision().value());
                return ssg::CommandHandlerResult::success();
            }));

    auto const before = runtime->revision();
    auto first = std::async(std::launch::async, [&] {
        return runtime->dispatch(
            ssg::ClientId{1}, {"oracle.primary", before, {}});
    });
    deferredStarted.wait();
    ASSERT_EQ(stage.load(), 2);

    std::promise<void> snapshotEnteringPromise;
    auto snapshotEntering = snapshotEnteringPromise.get_future();
    auto snapshot = std::async(std::launch::async, [&] {
        snapshotEnteringPromise.set_value();
        return runtime->snapshot(ssg::ClientId{1});
    });
    std::promise<void> secondEnteringPromise;
    auto secondEntering = secondEnteringPromise.get_future();
    auto second = std::async(std::launch::async, [&] {
        secondEnteringPromise.set_value();
        return runtime->dispatch(
            ssg::ClientId{1}, {"oracle.second", before, {}});
    });
    snapshotEntering.wait();
    secondEntering.wait();

    ASSERT_TRUE(snapshot.wait_for(20ms) == std::future_status::timeout);
    ASSERT_TRUE(second.wait_for(20ms) == std::future_status::timeout);

    releaseDeferredPromise.set_value();
    auto const firstResult = first.get();
    auto const captured = snapshot.get();
    auto const secondResult = second.get();

    ASSERT_TRUE(firstResult.accepted());
    ASSERT_TRUE(captured.has_value());
    ASSERT_TRUE(secondResult.accepted());
    ASSERT_EQ(stage.load(), 3);
    ASSERT_EQ(firstResult.revision.value(), before.value() + 2);
    if (captured) {
        ASSERT_EQ(captured->revision().value(), before.value() + 2);
    }
    ASSERT_EQ(observedRevision.load(), before.value() + 2);

    fs::remove_all(root);
}

}  // namespace

int main() {
    RUN(revisionAdvancesExactlyOncePerAcceptedMutation);
    RUN(revisionAdvancesOncePerMutationAcrossANestedChain);
    RUN(anObservingCommandLeavesTheRevisionAlone);
    RUN(aFailedChainAdvancesTheRevisionOnlyForCommandsThatRan);
    RUN(aHandlerThatDispatchesIsToldToDeferInstead);
    RUN(aHandlerCannotMutateTheCommandCatalogReentrantly);
    RUN(aggregateOperationHidesIntermediateDeferredRevisions);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
