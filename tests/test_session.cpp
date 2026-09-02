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


static_assert(!std::is_copy_assignable_v<ssg::InvocationPrincipal>);
static_assert(!std::is_move_assignable_v<ssg::InvocationPrincipal>);

// The session dispatches from a catalog, so these tests register into one.
// Kept as a tuple-shaped helper so each test still reads as "a command with
// this effect, this handler and these capabilities".
struct TestCommand {
    std::string id;
    ssg::CommandEffect effect;
    ssg::CommandHandler handler;
    std::vector<ssg::CapabilityId> requiredCapabilities;
};

TestCommand command(
    std::string id,
    ssg::CommandEffect effect,
    ssg::CommandHandler handler,
    std::vector<ssg::CapabilityId> requiredCapabilities = {}) {
    return {std::move(id), effect, std::move(handler),
            std::move(requiredCapabilities)};
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
        for (auto const& capability : entry.requiredCapabilities) {
            spec.capability(std::string{capability.value()});
        }
        spec.untypedHandler(std::move(entry.handler), std::nullopt);
        catalog->add(std::move(spec));
    }
    return catalog;
}

ssg::InvocationPrincipal principal(
    std::uint64_t id,
    ssg::InvocationOrigin origin = ssg::InvocationOrigin::InProcess,
    std::vector<ssg::CapabilityId> capabilities = {}) {
    return {ssg::ClientId{id}, origin, std::move(capabilities)};
}

ssg::ClientCommand request(std::string id, std::uint64_t revision) {
    return {std::move(id), ssg::Revision{revision}, std::any{}};
}

TEST(totalOrderAndRegisteredDispatch) {
    std::vector<std::uint64_t> clients;
    auto catalog = catalogOf({command(
        "state.advance", ssg::CommandEffect::Mutation,
        [&](ssg::CommandContext& context, std::any const&) {
            clients.push_back(context.principal().clientId().value());
            return ssg::CommandHandlerResult::success();
        })});
    ssg::CommandExecutor session{catalog};

    ASSERT_TRUE(session.attach(principal(1), ssg::ViewId{11}).accepted());
    ASSERT_TRUE(session.attach(principal(2), ssg::ViewId{22}).accepted());
    ASSERT_EQ(session.revision(), ssg::Revision{1});

    auto first = session.dispatch(ssg::ClientId{2}, request("state.advance", 1));
    auto second = session.dispatch(ssg::ClientId{1}, request("state.advance", 2));
    auto third = session.dispatch(ssg::ClientId{2}, request("state.advance", 3));

    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(second.accepted());
    ASSERT_TRUE(third.accepted());
    ASSERT_EQ(first.revision, ssg::Revision{2});
    ASSERT_EQ(second.revision, ssg::Revision{3});
    ASSERT_EQ(third.revision, ssg::Revision{4});
    ASSERT_EQ(clients, (std::vector<std::uint64_t>{2, 1, 2}));

    auto unknown = session.dispatch(ssg::ClientId{1}, request("missing", 4));
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
    ASSERT_TRUE(session.attach(principal(1), ssg::ViewId{1}).accepted());
    ASSERT_TRUE(
        session.dispatch(ssg::ClientId{1}, request("state.advance", 1)).accepted());

    auto staleMutation =
        session.dispatch(ssg::ClientId{1}, request("state.advance", 1));
    auto staleObservation =
        session.dispatch(ssg::ClientId{1}, request("state.inspect", 1));

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
    ASSERT_TRUE(session.attach(principal(1), ssg::ViewId{42}).accepted());

    auto result =
        session.dispatch(ssg::ClientId{1}, request("view.scroll", 1));
    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(result.revision, ssg::Revision{1});
    ASSERT_EQ(session.revision(), ssg::Revision{1});
    ASSERT_TRUE(result.viewAction.has_value());
    if (result.viewAction) {
        ASSERT_EQ(result.viewAction->viewId, ssg::ViewId{42});
        ASSERT_EQ(result.viewAction->semanticRevision, ssg::Revision{1});
        ASSERT_EQ(result.viewAction->action,
                  (ssg::ViewAction{ssg::ScrollLines{
                      ssg::ScrollTarget::Tree, -3}}));
    }

    auto stale =
        session.dispatch(ssg::ClientId{1}, request("view.scroll", 0));
    ASSERT_EQ(stale.error, ssg::CommandError::StaleRevision);
    ASSERT_FALSE(stale.viewAction.has_value());
    ASSERT_EQ(session.revision(), ssg::Revision{1});

    ASSERT_EQ(session
                  .dispatch(ssg::ClientId{1},
                            request("view.missing_action", 1))
                  .error,
              ssg::CommandError::HandlerFailed);
    ASSERT_EQ(session
                  .dispatch(ssg::ClientId{1},
                            request("observe.invalid_action", 1))
                  .error,
              ssg::CommandError::HandlerFailed);
}

TEST(clientIdentityAndPrincipalAreIsolated) {
    std::vector<std::uint64_t> observedClients;
    auto catalog = catalogOf({command(
        "identity.inspect", ssg::CommandEffect::Observation,
        [&](ssg::CommandContext& context, std::any const&) {
            observedClients.push_back(context.principal().clientId().value());
            return ssg::CommandHandlerResult::success();
        })});
    ssg::CommandExecutor session{catalog};
    ASSERT_TRUE(session.attach(principal(7), ssg::ViewId{70}).accepted());
    ASSERT_TRUE(session.attach(
        principal(8, ssg::InvocationOrigin::InProcess), ssg::ViewId{80})
                    .accepted());

    auto client7 = session.attachedClient(ssg::ClientId{7});
    auto client8 = session.attachedClient(ssg::ClientId{8});
    ASSERT_EQ(client7->viewId, ssg::ViewId{70});
    ASSERT_EQ(client8->viewId, ssg::ViewId{80});
    ASSERT_FALSE(session.attach(principal(7), ssg::ViewId{71}).accepted());

    ASSERT_TRUE(
        session.dispatch(ssg::ClientId{8}, request("identity.inspect", 0))
            .accepted());
    ASSERT_TRUE(
        session.dispatch(ssg::ClientId{7}, request("identity.inspect", 0))
            .accepted());
    ASSERT_EQ(observedClients, (std::vector<std::uint64_t>{8, 7}));

    auto unattached =
        session.dispatch(ssg::ClientId{9}, request("identity.inspect", 0));
    ASSERT_EQ(unattached.error, ssg::CommandError::UnknownClient);
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
    ASSERT_TRUE(session.attach(principal(1), ssg::ViewId{1}).accepted());

    auto rejectedResult =
        session.dispatch(ssg::ClientId{1}, request("topology.fail", 1));
    ASSERT_EQ(rejectedResult.error, ssg::CommandError::HandlerFailed);
    ASSERT_EQ(session.revision(), ssg::Revision{1});
    auto topology = session.topology();
    ASSERT_FALSE(topology.activeWorkspace.has_value());
    ASSERT_FALSE(topology.activeView.has_value());

    auto threw =
        session.dispatch(ssg::ClientId{1}, request("topology.throw", 1));
    ASSERT_EQ(threw.error, ssg::CommandError::HandlerFailed);
    ASSERT_EQ(session.revision(), ssg::Revision{1});
    topology = session.topology();
    ASSERT_FALSE(topology.activeWorkspace.has_value());
    ASSERT_FALSE(topology.activeView.has_value());

    auto committed =
        session.dispatch(ssg::ClientId{1}, request("topology.commit", 1));
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

TEST(principalCapabilityEnforcementUsesTheInProcessAuthority) {
    int calls = 0;
    auto catalog = catalogOf({command(
        "local.ingress", ssg::CommandEffect::Observation,
        [&](ssg::CommandContext&, std::any const&) {
            ++calls;
            return ssg::CommandHandlerResult::success();
        },
        {ssg::CapabilityId{"local_file_drop"}})});
    ssg::CommandExecutor session{catalog};

    ASSERT_TRUE(session.attach(
        principal(1, ssg::InvocationOrigin::InProcess,
                  {ssg::CapabilityId{"local_file_drop"}}),
        ssg::ViewId{1}).accepted());
    ASSERT_TRUE(session.attach(
        principal(2, ssg::InvocationOrigin::InProcess,
                  {ssg::CapabilityId{"local_file_drop"}}),
        ssg::ViewId{2}).accepted());
    ASSERT_TRUE(session.attach(
        principal(3, ssg::InvocationOrigin::InProcess), ssg::ViewId{3})
                    .accepted());
    ASSERT_TRUE(session.attach(
        principal(4, ssg::InvocationOrigin::InProcess), ssg::ViewId{4})
                    .accepted());

    ASSERT_TRUE(
        session.dispatch(ssg::ClientId{1}, request("local.ingress", 0))
            .accepted());
    ASSERT_TRUE(
        session.dispatch(ssg::ClientId{2}, request("local.ingress", 0))
            .accepted());
    ASSERT_EQ(
        session.dispatch(ssg::ClientId{3}, request("local.ingress", 0)).error,
        ssg::CommandError::CapabilityDenied);
    ASSERT_EQ(
        session.dispatch(ssg::ClientId{4}, request("local.ingress", 0)).error,
        ssg::CommandError::CapabilityDenied);
    ASSERT_EQ(calls, 2);
}

}  // namespace

int main() {
    RUN(totalOrderAndRegisteredDispatch);
    RUN(staleRejectionAppliesOnlyToMutations);
    RUN(viewActionsAreStampedWithoutAdvancingSemanticState);
    RUN(clientIdentityAndPrincipalAreIsolated);
    RUN(handlerFailureIsAtomic);
    RUN(principalCapabilityEnforcementUsesTheInProcessAuthority);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
