#include <ssg/EditorRuntime.h>

#include <ssg/CommandCatalog.h>
#include <ssg/EditorSession.h>

#include "test_helpers.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

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

std::unique_ptr<ssg::EditorRuntime> makeRuntime(fs::path const& root) {
    auto created =
        ssg::EditorRuntime::create({root, root / "scratch", root / "recovery"});
    auto runtime = std::move(created.runtime);
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
// synchronously.  It cannot: EditorSession::dispatch computes the new revision
// from a value captured BEFORE the handler ran, so a nested mutation advances
// the revision and the outer then writes its own value over it.  Two accepted
// mutations, one revision step -- and a client replaying deltas against a base
// revision silently misses an edit (I3).
//
// Deferral satisfies the property because each deferred command is its own
// dispatch with its own revision step.
//
// To perturb: make EditorSession::Impl::mutex a std::recursive_mutex, delete
// the nested-dispatch guards in EditorSession::dispatch and
// EditorRuntime::dispatch, have the outer handler dispatch instead of defer,
// and REBUILD THE LIBRARY (a probe linked against a stale libssg.a still
// contains the guards and reports a false pass).  The counts then diverge.
TEST(revisionAdvancesExactlyOncePerAcceptedMutation) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);

    int acceptedMutations = 0;
    auto catalog = runtime->commandCatalog();

    catalog->add(ssg::CommandSpecBuilder{"oracle.leaf"}
                     .owner("test-oracle")
                     .summary("counts itself")
                     .mutates()
                     .handler([&acceptedMutations](ssg::CommandContext&) {
                         ++acceptedMutations;
                         return ssg::CommandHandlerResult::success();
                     }));

    // Asks for two more commands, so one dispatch becomes a chain of three
    // accepted mutations.
    catalog->add(
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
    auto catalog = runtime->commandCatalog();

    catalog->add(ssg::CommandSpecBuilder{"oracle.deep_leaf"}
                     .owner("test-oracle")
                     .summary("counts itself")
                     .mutates()
                     .handler([&acceptedMutations](ssg::CommandContext&) {
                         ++acceptedMutations;
                         return ssg::CommandHandlerResult::success();
                     }));
    catalog->add(
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
    catalog->add(
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

    runtime->commandCatalog()->add(
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
    auto catalog = runtime->commandCatalog();

    catalog->add(ssg::CommandSpecBuilder{"oracle.refuses"}
                     .owner("test-oracle")
                     .summary("always fails")
                     .mutates()
                     .handler([](ssg::CommandContext&) {
                         return ssg::CommandHandlerResult::failure("no");
                     }));
    catalog->add(
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
    runtime->commandCatalog()->add(
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
    ASSERT_EQ(std::string{ssg::EditorSession::kNestedDispatchRefusal},
              nested.message);

    fs::remove_all(root);
}

}  // namespace

int main() {
    RUN(revisionAdvancesExactlyOncePerAcceptedMutation);
    RUN(revisionAdvancesOncePerMutationAcrossANestedChain);
    RUN(anObservingCommandLeavesTheRevisionAlone);
    RUN(aFailedChainAdvancesTheRevisionOnlyForCommandsThatRan);
    RUN(aHandlerThatDispatchesIsToldToDeferInstead);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
