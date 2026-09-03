#include <ssg/EditorSession.h>

#include <ssg/CommandCatalog.h>
#include <ssg/StatusQueue.h>
#include "../src/runtime/command_executor.h"

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
#include <typeindex>
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

std::unique_ptr<ssg::EditorSession> makeRuntime(fs::path const& root) {
    auto created = ssg::EditorSession::create(
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

class TestServices final : public ssg::CommandServices {
public:
    ssg::CommandHandlerResult runTransaction(
        std::function<ssg::CommandHandlerResult()> operation) override {
        return operation();
    }

private:
    std::any state_{std::uint32_t{0}};

    std::any& featureStateValue(std::type_index) override { return state_; }
    void publishStatusValue(std::type_index, std::any) override {}
    void publishDeltaValue(std::type_index, std::any) override {}
};

TEST(executorThreadsServicesThroughTheCommonDispatchPath) {
    TestServices services;
    auto catalog = std::make_shared<ssg::CommandCatalog>();
    catalog->add(ssg::CommandSpecBuilder{"probe.services"}
                     .owner("test-owner")
                     .summary("Observes dispatch services")
                     .observes()
                     .handler([&services](ssg::CommandContext& context) {
                         ASSERT_TRUE(context.services() == &services);
                         return ssg::CommandHandlerResult::success();
                     }));
    ssg::CommandExecutor executor{catalog, &services};
    ASSERT_TRUE(executor
                    .dispatch({"probe.services", ssg::Revision{1}, {}})
                    .accepted());
}

TEST(viewActionResultsRemainExplicitAcrossTheAggregateBoundary) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;

    (void)runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.view_action"}
            .owner("test-oracle")
            .summary("returns one typed view action")
            .viewAction()
            .handler([](ssg::CommandContext&) {
                return ssg::CommandHandlerResult::requireView(
                    ssg::ScrollPages{-2});
            }));

    ASSERT_TRUE(
        runtime
            ->dispatch({"keymap.bind", runtime->revision(),
                 ssg::KeymapBindArguments{"Ctrl+KeyG",
                                          "oracle.view_action", "editor"}})
            .accepted());
    const auto revision = runtime->revision();
    auto result = runtime->dispatch({"oracle.view_action", revision, {}});
    ASSERT_TRUE(result.accepted());
    ASSERT_FALSE(result.completed());
    ASSERT_EQ(result.outcome(),
              ssg::CommandResult::Outcome::ViewActionRequired);
    ASSERT_EQ(runtime->revision(), revision);
    ASSERT_TRUE(result.viewAction.has_value());
    if (result.viewAction) {
        ASSERT_EQ(result.viewAction->viewId, ssg::ViewId{1});
        ASSERT_EQ(result.viewAction->semanticRevision, revision);
        ASSERT_EQ(result.viewAction->action,
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
    ASSERT_EQ(runtime->revision(), revision);
}

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
    ASSERT_TRUE(
        runtime
            ->registerCommand(ssg::CommandSpecBuilder{"oracle.leaf"}
                                  .owner("test-oracle")
                                  .summary("counts itself")
                                  .mutates()
                                  .handler([&acceptedMutations](
                                               ssg::CommandContext&) {
                                      ++acceptedMutations;
                                      return ssg::CommandHandlerResult::success();
                                  }))
            .valid());

    // Asks for two more commands, so one dispatch becomes a chain of three
    // accepted mutations.
    ASSERT_TRUE(
        runtime
            ->registerCommand(
                ssg::CommandSpecBuilder{"oracle.chain"}
                    .owner("test-oracle")
                    .summary("defers two leaves")
                    .mutates()
                    .handler([&acceptedMutations,
                              &runtime](ssg::CommandContext& ctx) {
                        ++acceptedMutations;
                        for (int queued = 0; queued < 2; ++queued) {
                            if (!runtime->deferDispatch({"oracle.leaf", ctx.revision(), {}})) {
                                return ssg::CommandHandlerResult::failure(
                                    "could not queue");
                            }
                        }
                        return ssg::CommandHandlerResult::success();
                    }))
            .valid());

    auto const before = runtime->revision().value();
    auto const result = runtime->dispatch({"oracle.chain", runtime->revision(), {}});
    ASSERT_TRUE(result.accepted());
    auto const after = runtime->revision().value();

    ASSERT_EQ(acceptedMutations, 3);
    ASSERT_EQ(after - before, static_cast<std::uint64_t>(acceptedMutations));

    fs::remove_all(root);
}

TEST(stateValidatedMutationUsesCurrentRevisionWhenClientBasisIsStale) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);

    ssg::Revision handledAt;
    ASSERT_TRUE(
        runtime
            ->registerCommand(
                ssg::CommandSpecBuilder{"oracle.state_validated"}
                    .owner("test-oracle")
                    .summary("validates current domain state")
                    .stateValidatedMutation()
                    .handler([&handledAt](ssg::CommandContext& context) {
                        handledAt = context.revision();
                        return ssg::CommandHandlerResult::success();
                    }))
            .valid());

    const auto stale = runtime->revision();
    ASSERT_TRUE(
        runtime
            ->dispatch({"panel.toggle", runtime->revision(), {}})
            .accepted());
    const auto current = runtime->revision();
    ASSERT_TRUE(current != stale);

    const auto result = runtime->dispatch({"oracle.state_validated", stale, {}});
    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(handledAt, current);
    ASSERT_EQ(result.revision.value(), current.value() + 1);
    fs::remove_all(root);
}

// The same property across a NESTED chain: a deferred command that itself
// queues more.  Depth must not collapse revision steps either.
TEST(revisionAdvancesOncePerMutationAcrossANestedChain) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);

    int acceptedMutations = 0;
    ASSERT_TRUE(
        runtime
            ->registerCommand(ssg::CommandSpecBuilder{"oracle.deep_leaf"}
                                  .owner("test-oracle")
                                  .summary("counts itself")
                                  .mutates()
                                  .handler([&acceptedMutations](
                                               ssg::CommandContext&) {
                                      ++acceptedMutations;
                                      return ssg::CommandHandlerResult::success();
                                  }))
            .valid());
    ASSERT_TRUE(
        runtime
            ->registerCommand(
                ssg::CommandSpecBuilder{"oracle.deep_middle"}
                    .owner("test-oracle")
                    .summary("defers a leaf")
                    .mutates()
                    .handler([&acceptedMutations,
                              &runtime](ssg::CommandContext& ctx) {
                        ++acceptedMutations;
                        if (!runtime->deferDispatch({"oracle.deep_leaf", ctx.revision(), {}})) {
                            return ssg::CommandHandlerResult::failure(
                                "could not queue");
                        }
                        return ssg::CommandHandlerResult::success();
                    }))
            .valid());
    ASSERT_TRUE(
        runtime
            ->registerCommand(
                ssg::CommandSpecBuilder{"oracle.deep_outer"}
                    .owner("test-oracle")
                    .summary("defers a middle")
                    .mutates()
                    .handler([&acceptedMutations,
                              &runtime](ssg::CommandContext& ctx) {
                        ++acceptedMutations;
                        if (!runtime->deferDispatch({"oracle.deep_middle", ctx.revision(), {}})) {
                            return ssg::CommandHandlerResult::failure(
                                "could not queue");
                        }
                        return ssg::CommandHandlerResult::success();
                    }))
            .valid());

    auto const before = runtime->revision().value();
    ASSERT_TRUE(runtime
                    ->dispatch({"oracle.deep_outer", runtime->revision(), {}})
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

    ASSERT_TRUE(runtime
                    ->registerCommand(
                        ssg::CommandSpecBuilder{"oracle.observe"}
                            .owner("test-oracle")
                            .summary("changes nothing")
                            .observes()
                            .handler([](ssg::CommandContext&) {
                                return ssg::CommandHandlerResult::success();
                            }))
                    .valid());

    auto const before = runtime->revision().value();
    ASSERT_TRUE(runtime
                    ->dispatch({"oracle.observe", runtime->revision(), {}})
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
    ASSERT_TRUE(
        runtime
            ->registerCommand(ssg::CommandSpecBuilder{"oracle.refuses"}
                                  .owner("test-oracle")
                                  .summary("always fails")
                                  .mutates()
                                  .handler([](ssg::CommandContext&) {
                                      return ssg::CommandHandlerResult::failure(
                                          "no");
                                  }))
            .valid());
    ASSERT_TRUE(
        runtime
            ->registerCommand(
                ssg::CommandSpecBuilder{"oracle.queues_a_failure"}
                    .owner("test-oracle")
                    .summary("defers a command that fails")
                    .mutates()
                    .handler([&acceptedMutations,
                              &runtime](ssg::CommandContext& ctx) {
                        ++acceptedMutations;
                        if (!runtime->deferDispatch({"oracle.refuses", ctx.revision(), {}})) {
                            return ssg::CommandHandlerResult::failure(
                                "could not queue");
                        }
                        return ssg::CommandHandlerResult::success();
                    }))
            .valid());

    auto const before = runtime->revision().value();
    auto const result =
        runtime->dispatch({"oracle.queues_a_failure", runtime->revision(), {}});
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
    ASSERT_TRUE(
        runtime
            ->registerCommand(
                ssg::CommandSpecBuilder{"oracle.dispatches"}
                    .owner("test-oracle")
                    .summary("dispatches from its handler")
                    .mutates()
                    .handler([&nested, &runtime](ssg::CommandContext& ctx) {
                        nested = runtime->dispatch({"oracle.dispatches", ctx.revision(), {}});
                        return ssg::CommandHandlerResult::success();
                    }))
            .valid());

    ASSERT_TRUE(runtime
                    ->dispatch({"oracle.dispatches", runtime->revision(), {}})
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

TEST(routingCommandsQueueExactlyOneDirectOrdinaryTarget) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;

    int mutations = 0;
    ASSERT_TRUE(runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.route_target"}
            .owner("test-oracle")
            .summary("target")
            .mutates()
            .handler([&](ssg::CommandContext&) {
                ++mutations;
                return ssg::CommandHandlerResult::success();
            }))
                    .valid());
    ASSERT_TRUE(runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.route_once"}
            .owner("test-oracle")
            .summary("route once")
            .routes()
            .handler([&](ssg::CommandContext& context) {
                return runtime->deferDispatch({"oracle.route_target", context.revision(), {}})
                           ? ssg::CommandHandlerResult::success()
                           : ssg::CommandHandlerResult::failure("queue failed");
            }))
                    .valid());
    ASSERT_TRUE(runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.route_none"}
            .owner("test-oracle")
            .summary("route none")
            .routes()
            .handler([](ssg::CommandContext&) {
                return ssg::CommandHandlerResult::success();
            }))
                    .valid());
    ASSERT_TRUE(runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.route_twice"}
            .owner("test-oracle")
            .summary("route twice")
            .routes()
            .handler([&](ssg::CommandContext& context) {
                const bool first = runtime->deferDispatch({"oracle.route_target", context.revision(), {}});
                const bool second = runtime->deferDispatch({"oracle.route_target", context.revision(), {}});
                return first && second
                           ? ssg::CommandHandlerResult::success()
                           : ssg::CommandHandlerResult::failure("queue failed");
            }))
                    .valid());
    ASSERT_TRUE(runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.route_nested"}
            .owner("test-oracle")
            .summary("route nested")
            .routes()
            .handler([&](ssg::CommandContext& context) {
                return runtime->deferDispatch({"oracle.route_once", context.revision(), {}})
                           ? ssg::CommandHandlerResult::success()
                           : ssg::CommandHandlerResult::failure("queue failed");
            }))
                    .valid());

    const auto before = runtime->revision();
    const auto accepted = runtime->dispatch({"oracle.route_once", before, {}});
    ASSERT_TRUE(accepted.accepted());
    ASSERT_EQ(mutations, 1);
    ASSERT_EQ(runtime->revision().value(), before.value() + 1);

    for (const std::string_view id :
         {"oracle.route_none", "oracle.route_twice", "oracle.route_nested"}) {
        const auto revision = runtime->revision();
        const auto rejected = runtime->dispatch({std::string{id}, revision, {}});
        ASSERT_FALSE(rejected.accepted());
        ASSERT_EQ(rejected.error, ssg::CommandError::HandlerFailed);
        ASSERT_EQ(runtime->revision(), revision);
        ASSERT_EQ(mutations, 1);
    }
    fs::remove_all(root);
}

TEST(publishedStatusActionActivatesItsCurrentTargetCommand) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;

    int targetRuns = 0;
    ASSERT_TRUE(runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.status_target"}
            .owner("test-oracle")
            .summary("status target")
            .mutates()
            .handler([&](ssg::CommandContext&) {
                ++targetRuns;
                return ssg::CommandHandlerResult::success();
            }))
                    .valid());
    ASSERT_TRUE(runtime
                    ->registerCommand(
                        ssg::CommandSpecBuilder{"oracle.status_payload"}
                            .owner("test-oracle")
                            .summary("payload status target")
                            .mutates()
                            .inProcessHandler<std::string>(
                                [](ssg::CommandContext&,
                                   const std::string&) {
                                    return ssg::CommandHandlerResult::success();
                                }))
                    .valid());
    ASSERT_TRUE(runtime->registerCommand(
        ssg::CommandSpecBuilder{"oracle.publish_status"}
            .owner("test-oracle")
            .summary("publish status")
            .mutates()
            .handler([](ssg::CommandContext& context) {
                context.services()->publishStatus(ssg::StatusItem{
                    ssg::StatusId{41}, ssg::StatusPriority::Information,
                    "Ready",
                    {ssg::UiAction{"run", "Run action",
                                       "oracle.status_target"},
                     ssg::UiAction{"payload", "Payload action",
                                       "oracle.status_payload"}}});
                return ssg::CommandHandlerResult::success();
            }))
                    .valid());

    ASSERT_TRUE(runtime
                    ->dispatch({"oracle.publish_status",
                                runtime->revision(), {}})
                    .accepted());
    const auto snapshot = runtime->snapshot();
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto& tree = snapshot->sections().uiTree;
    const auto findByCommand =
        [&](std::string_view command) -> const ssg::UiNode* {
        const ssg::UiNode* found = nullptr;
        const auto walk = [&](const auto& self,
                              const ssg::UiNode& node) -> void {
            if (found) return;
            if (node.resolved &&
                node.resolved->command ==
                    std::optional<std::string>{std::string{command}}) {
                found = &node;
                return;
            }
            if (const auto* container =
                    std::get_if<ssg::UiContainer>(&node.content)) {
                for (const auto& child : container->children) self(self, child);
            }
        };
        walk(walk, tree.root);
        return found;
    };
    const auto* action = findByCommand("oracle.status_target");
    ASSERT_TRUE(action != nullptr);
    if (!action) return;
    const auto* payloadAction = findByCommand("oracle.status_payload");
    ASSERT_TRUE(payloadAction != nullptr);
    if (!payloadAction) return;

    const auto before = runtime->revision();
    const auto missingRejected = runtime->dispatch({"ui.activate", before,
         ssg::UiNodeActivationArguments{ssg::UiNodeId{"missing.action"}}});
    ASSERT_FALSE(missingRejected.accepted());
    ASSERT_EQ(missingRejected.error, ssg::CommandError::HandlerFailed);
    ASSERT_EQ(runtime->revision(), before);
    ASSERT_EQ(targetRuns, 0);
    const auto payloadRejected = runtime->dispatch({"ui.activate", before,
         ssg::UiNodeActivationArguments{payloadAction->id}});
    ASSERT_FALSE(payloadRejected.accepted());
    ASSERT_EQ(payloadRejected.error, ssg::CommandError::HandlerFailed);
    ASSERT_EQ(runtime->revision(), before);
    const auto activated = runtime->dispatch({"ui.activate", before,
         ssg::UiNodeActivationArguments{action->id}});
    ASSERT_TRUE(activated.accepted());
    ASSERT_EQ(targetRuns, 1);
    ASSERT_EQ(runtime->revision().value(), before.value() + 1);
    const auto stale = runtime->dispatch({"ui.activate", before,
         ssg::UiNodeActivationArguments{action->id}});
    ASSERT_FALSE(stale.accepted());
    ASSERT_EQ(stale.error, ssg::CommandError::StaleRevision);
    ASSERT_EQ(targetRuns, 1);

    fs::remove_all(root);
}

TEST(aHandlerThatSnapshotsIsRefusedBeforeTakingTheOperationLock) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);

    bool refused = false;
    ASSERT_TRUE(
        runtime
            ->registerCommand(
                ssg::CommandSpecBuilder{"oracle.presents"}
                    .owner("test-oracle")
                    .summary("attempts snapshot capture from its handler")
                    .observes()
                    .handler([&](ssg::CommandContext&) {
                        try {
                            (void)runtime->snapshot();
                        } catch (const std::logic_error&) {
                            refused = true;
                        }
                        return ssg::CommandHandlerResult::success();
                    }))
            .valid());

    ASSERT_TRUE(runtime
                    ->dispatch({"oracle.presents", runtime->revision(), {}})
                    .accepted());
    ASSERT_TRUE(refused);

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
            ->registerCommand(
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
                                        return ssg::CommandHandlerResult::
                                            success();
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
                    }))
            .valid());

    ASSERT_TRUE(runtime
                    ->dispatch({"oracle.registers", runtime->revision(), {}})
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

    ASSERT_TRUE(
        runtime
            ->registerCommand(
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
                    }))
            .valid());
    ASSERT_TRUE(
        runtime
            ->registerCommand(
                ssg::CommandSpecBuilder{"oracle.primary"}
                    .owner("test-oracle")
                    .summary("queues the blocking command")
                    .mutates()
                    .handler([&](ssg::CommandContext& context) {
                        stage.store(1);
                        if (!runtime->deferDispatch({"oracle.blocking_deferred", context.revision(),
                                 {}})) {
                            return ssg::CommandHandlerResult::failure(
                                "could not queue");
                        }
                        return ssg::CommandHandlerResult::success();
                    }))
            .valid());
    ASSERT_TRUE(
        runtime
            ->registerCommand(
                ssg::CommandSpecBuilder{"oracle.second"}
                    .owner("test-oracle")
                    .summary("records the revision it observes")
                    .observes()
                    .handler([&](ssg::CommandContext& context) {
                        observedRevision.store(context.revision().value());
                        return ssg::CommandHandlerResult::success();
                    }))
            .valid());

    auto const before = runtime->revision();
    auto first = std::async(std::launch::async, [&] {
        return runtime->dispatch({"oracle.primary", before, {}});
    });
    deferredStarted.wait();
    ASSERT_EQ(stage.load(), 2);

    std::promise<void> snapshotEnteringPromise;
    auto snapshotEntering = snapshotEnteringPromise.get_future();
    auto snapshot = std::async(std::launch::async, [&] {
        snapshotEnteringPromise.set_value();
        return runtime->snapshot();
    });
    std::promise<void> secondEnteringPromise;
    auto secondEntering = secondEnteringPromise.get_future();
    auto second = std::async(std::launch::async, [&] {
        secondEnteringPromise.set_value();
        return runtime->dispatch({"oracle.second", before, {}});
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

SSG_TEST_SUITE(test_command_dispatch) {
    RUN(executorThreadsServicesThroughTheCommonDispatchPath);
    RUN(viewActionResultsRemainExplicitAcrossTheAggregateBoundary);
    RUN(revisionAdvancesExactlyOncePerAcceptedMutation);
    RUN(stateValidatedMutationUsesCurrentRevisionWhenClientBasisIsStale);
    RUN(revisionAdvancesOncePerMutationAcrossANestedChain);
    RUN(anObservingCommandLeavesTheRevisionAlone);
    RUN(aFailedChainAdvancesTheRevisionOnlyForCommandsThatRan);
    RUN(aHandlerThatDispatchesIsToldToDeferInstead);
    RUN(routingCommandsQueueExactlyOneDirectOrdinaryTarget);
    RUN(publishedStatusActionActivatesItsCurrentTargetCommand);
    RUN(aHandlerThatSnapshotsIsRefusedBeforeTakingTheOperationLock);
    RUN(aHandlerCannotMutateTheCommandCatalogReentrantly);
    RUN(aggregateOperationHidesIntermediateDeferredRevisions);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
