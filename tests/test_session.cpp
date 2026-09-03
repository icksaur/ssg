#include "test_helpers.h"

#include <ssg/CommandCatalog.h>
#include <ssg/CommandSpecBuilder.h>
#include <ssg/CommandInvocation.h>
#include "../src/runtime/command_executor.h"

#include <algorithm>
#include <any>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

// The session dispatches from a catalog, so these tests register into one.
// Kept as a tuple-shaped helper so each test still reads as "a command with
// this effect and this handler".
struct TestCommand {
    std::string id;
    ssg::CommandEffect effect;
    ssg::CommandHandler handler;
};

TestCommand command(
    std::string id,
    ssg::CommandEffect effect,
    ssg::CommandHandler handler) {
    return {std::move(id), effect, std::move(handler)};
}

std::shared_ptr<ssg::CommandCatalog> catalogOf(
    std::vector<TestCommand> commands) {
    auto catalog = std::make_shared<ssg::CommandCatalog>();
    for (auto& entry : commands) {
        ssg::CommandSpecBuilder spec{std::move(entry.id)};
        spec.owner("test-owner").summary("a command");
        if (entry.effect == ssg::CommandEffect::Mutation) {
            spec.mutates();
        } else if (entry.effect == ssg::CommandEffect::ViewAction) {
            spec.viewAction();
        } else {
            spec.observes();
        }
        spec.untypedHandler(std::move(entry.handler), std::nullopt);
        catalog->add(std::move(spec));
    }
    return catalog;
}

ssg::ClientCommand request(std::string id, std::uint64_t revision) {
    return {std::move(id), ssg::Revision{revision}, std::any{}};
}

TEST(totalOrderAndRegisteredDispatch) {
    int calls = 0;
    auto catalog = catalogOf({command(
        "state.advance", ssg::CommandEffect::Mutation,
        [&](ssg::CommandContext&, std::any const&) {
            ++calls;
            return ssg::CommandHandlerResult::success();
        })});
    ssg::CommandExecutor session{catalog};

    ASSERT_EQ(session.revision(), ssg::Revision{1});

    auto first = session.dispatch(request("state.advance", 1));
    auto second = session.dispatch(request("state.advance", 2));
    auto third = session.dispatch(request("state.advance", 3));

    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(second.accepted());
    ASSERT_TRUE(third.accepted());
    ASSERT_EQ(first.revision, ssg::Revision{2});
    ASSERT_EQ(second.revision, ssg::Revision{3});
    ASSERT_EQ(third.revision, ssg::Revision{4});
    ASSERT_EQ(calls, 3);

    auto unknown = session.dispatch(request("missing", 4));
    ASSERT_EQ(unknown.error, ssg::CommandError::UnknownCommand);
    ASSERT_EQ(session.revision(), ssg::Revision{4});
}

TEST(staleRejectionAppliesOnlyToMutations) {
    int observations = 0;
    auto catalog = catalogOf({
        command("state.advance", ssg::CommandEffect::Mutation,
                [](ssg::CommandContext&, std::any const&) {
                    return ssg::CommandHandlerResult::success();
                }),
        command("state.inspect", ssg::CommandEffect::Observation,
                [&](ssg::CommandContext&, std::any const&) {
                    ++observations;
                    return ssg::CommandHandlerResult::success();
                }),
    });
    ssg::CommandExecutor session{catalog};
    ASSERT_TRUE(
        session.dispatch(request("state.advance", 1)).accepted());

    auto staleMutation =
        session.dispatch(request("state.advance", 1));
    auto staleObservation =
        session.dispatch(request("state.inspect", 1));

    ASSERT_EQ(staleMutation.error, ssg::CommandError::StaleRevision);
    ASSERT_TRUE(staleObservation.accepted());
    ASSERT_EQ(observations, 1);
    ASSERT_EQ(session.revision(), ssg::Revision{2});
}

TEST(viewActionsAreStampedWithoutAdvancingSemanticState) {
    auto catalog = catalogOf({
        command("view.scroll", ssg::CommandEffect::ViewAction,
                [](ssg::CommandContext&, std::any const&) {
                    return ssg::CommandHandlerResult::requireView(
                        ssg::ScrollLines{ssg::ScrollTarget::Tree, -3});
                }),
        command("view.missing_action", ssg::CommandEffect::ViewAction,
                [](ssg::CommandContext&, std::any const&) {
                    return ssg::CommandHandlerResult::success();
                }),
        command("observe.invalid_action", ssg::CommandEffect::Observation,
                [](ssg::CommandContext&, std::any const&) {
                    return ssg::CommandHandlerResult::requireView(
                        ssg::CenterSelection{});
                }),
    });
    ssg::CommandExecutor session{catalog};

    auto result =
        session.dispatch(request("view.scroll", 1));
    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(result.revision, ssg::Revision{1});
    ASSERT_EQ(session.revision(), ssg::Revision{1});
    ASSERT_TRUE(result.viewAction.has_value());
    if (result.viewAction) {
        ASSERT_EQ(result.viewAction->viewId, ssg::ViewId{1});
        ASSERT_EQ(result.viewAction->semanticRevision, ssg::Revision{1});
        ASSERT_EQ(result.viewAction->action,
                  (ssg::ViewAction{ssg::ScrollLines{
                      ssg::ScrollTarget::Tree, -3}}));
    }

    auto stale =
        session.dispatch(request("view.scroll", 0));
    ASSERT_EQ(stale.error, ssg::CommandError::StaleRevision);
    ASSERT_FALSE(stale.viewAction.has_value());
    ASSERT_EQ(session.revision(), ssg::Revision{1});

    ASSERT_EQ(session
                  .dispatch(request("view.missing_action", 1))
                  .error,
              ssg::CommandError::HandlerFailed);
    ASSERT_EQ(session
                  .dispatch(request("observe.invalid_action", 1))
                  .error,
              ssg::CommandError::HandlerFailed);
}

TEST(handlerFailureIsAtomic) {
    auto catalog = catalogOf({
        command("topology.fail", ssg::CommandEffect::Mutation,
                [](ssg::CommandContext& context, std::any const&) {
                    context.setActiveWorkspace(ssg::WorkspaceId{5});
                    context.setActiveView(ssg::ViewId{6});
                    return ssg::CommandHandlerResult::failure("injected");
                }),
        command("topology.throw", ssg::CommandEffect::Mutation,
                [](ssg::CommandContext& context, std::any const&)
                    -> ssg::CommandHandlerResult {
                    context.setActiveWorkspace(ssg::WorkspaceId{8});
                    context.setActiveView(ssg::ViewId{9});
                    throw std::runtime_error{"injected"};
                }),
        command("topology.commit", ssg::CommandEffect::Mutation,
                [](ssg::CommandContext& context, std::any const&) {
                    context.setActiveWorkspace(ssg::WorkspaceId{2});
                    context.setActiveView(ssg::ViewId{3});
                    return ssg::CommandHandlerResult::success();
                }),
    });
    ssg::CommandExecutor session{catalog};

    auto rejectedResult =
        session.dispatch(request("topology.fail", 1));
    ASSERT_EQ(rejectedResult.error, ssg::CommandError::HandlerFailed);
    ASSERT_EQ(session.revision(), ssg::Revision{1});
    auto topology = session.topology();
    ASSERT_FALSE(topology.activeWorkspace.has_value());
    ASSERT_FALSE(topology.activeView.has_value());

    auto threw =
        session.dispatch(request("topology.throw", 1));
    ASSERT_EQ(threw.error, ssg::CommandError::HandlerFailed);
    ASSERT_EQ(session.revision(), ssg::Revision{1});
    topology = session.topology();
    ASSERT_FALSE(topology.activeWorkspace.has_value());
    ASSERT_FALSE(topology.activeView.has_value());

    auto committed =
        session.dispatch(request("topology.commit", 1));
    ASSERT_TRUE(committed.accepted());
    ASSERT_EQ(session.revision(), ssg::Revision{2});
    topology = session.topology();
    ASSERT_EQ(topology.activeWorkspace,
              std::optional<ssg::WorkspaceId>{ssg::WorkspaceId{2}});
    ASSERT_EQ(topology.activeView,
              std::optional<ssg::ViewId>{ssg::ViewId{3}});
}

// A duplicate id is rejected by the catalog, where registration happens, and
// that rule is covered by test_command_catalog.  It used to be checked twice
// here -- once per command set, once across sets -- because commands were
// assembled from several sets before reaching the session.  There is one place
// now.

}  // namespace

SSG_TEST_SUITE(test_session) {
    RUN(totalOrderAndRegisteredDispatch);
    RUN(staleRejectionAppliesOnlyToMutations);
    RUN(viewActionsAreStampedWithoutAdvancingSemanticState);
    RUN(handlerFailureIsAtomic);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
