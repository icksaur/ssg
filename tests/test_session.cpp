#include "test_helpers.h"

#include <ssg/command_registry.h>
#include <ssg/session.h>

#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using namespace std::chrono_literals;

static_assert(!std::is_copy_assignable_v<ssg::InvocationPrincipal>);
static_assert(!std::is_move_assignable_v<ssg::InvocationPrincipal>);

ssg::CommandRegistration command(
    std::string id,
    ssg::CommandEffect effect,
    ssg::CommandHandler handler,
    std::vector<ssg::CapabilityId> required_capabilities = {}) {
    return {{std::move(id), effect, std::move(required_capabilities)},
            std::move(handler)};
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

TEST(total_order_and_registered_dispatch) {
    std::vector<std::uint64_t> clients;
    ssg::CommandSet commands{{command(
        "state.advance", ssg::CommandEffect::Mutation,
        [&](ssg::CommandContext& context, std::any const&) {
            clients.push_back(context.principal().client_id().value());
            return ssg::CommandHandlerResult::success();
        })}};
    ssg::EditorSession session{
        ssg::CommandRegistry{{std::move(commands)}}};

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

TEST(stale_rejection_applies_only_to_mutations) {
    int observations = 0;
    ssg::CommandSet commands{{
        command("state.advance", ssg::CommandEffect::Mutation,
                [](ssg::CommandContext&, std::any const&) {
                    return ssg::CommandHandlerResult::success();
                }),
        command("state.inspect", ssg::CommandEffect::Observation,
                [&](ssg::CommandContext&, std::any const&) {
                    ++observations;
                    return ssg::CommandHandlerResult::success();
                }),
    }};
    ssg::EditorSession session{
        ssg::CommandRegistry{{std::move(commands)}}};
    ASSERT_TRUE(session.attach(principal(1), ssg::ViewId{1}).accepted());
    ASSERT_TRUE(
        session.dispatch(ssg::ClientId{1}, request("state.advance", 1)).accepted());

    auto stale_mutation =
        session.dispatch(ssg::ClientId{1}, request("state.advance", 1));
    auto stale_observation =
        session.dispatch(ssg::ClientId{1}, request("state.inspect", 1));

    ASSERT_EQ(stale_mutation.error, ssg::CommandError::StaleRevision);
    ASSERT_TRUE(stale_observation.accepted());
    ASSERT_EQ(observations, 1);
    ASSERT_EQ(session.revision(), ssg::Revision{2});
}

TEST(client_identity_and_principal_are_isolated) {
    std::vector<std::uint64_t> observed_clients;
    ssg::CommandSet commands{{command(
        "identity.inspect", ssg::CommandEffect::Observation,
        [&](ssg::CommandContext& context, std::any const&) {
            observed_clients.push_back(context.principal().client_id().value());
            return ssg::CommandHandlerResult::success();
        })}};
    ssg::EditorSession session{
        ssg::CommandRegistry{{std::move(commands)}}};
    ASSERT_TRUE(session.attach(principal(7), ssg::ViewId{70}).accepted());
    ASSERT_TRUE(session.attach(
        principal(8, ssg::InvocationOrigin::Websocket), ssg::ViewId{80})
                    .accepted());

    auto client7 = session.attached_client(ssg::ClientId{7});
    auto client8 = session.attached_client(ssg::ClientId{8});
    ASSERT_EQ(client7->view_id, ssg::ViewId{70});
    ASSERT_EQ(client8->view_id, ssg::ViewId{80});
    ASSERT_FALSE(session.attach(principal(7), ssg::ViewId{71}).accepted());

    ASSERT_TRUE(
        session.dispatch(ssg::ClientId{8}, request("identity.inspect", 0))
            .accepted());
    ASSERT_TRUE(
        session.dispatch(ssg::ClientId{7}, request("identity.inspect", 0))
            .accepted());
    ASSERT_EQ(observed_clients, (std::vector<std::uint64_t>{8, 7}));

    auto unattached =
        session.dispatch(ssg::ClientId{9}, request("identity.inspect", 0));
    ASSERT_EQ(unattached.error, ssg::CommandError::UnknownClient);
}

TEST(handler_failure_is_atomic) {
    ssg::CommandSet commands{{
        command("topology.fail", ssg::CommandEffect::Mutation,
                [](ssg::CommandContext& context, std::any const&) {
                    context.set_active_workspace(ssg::WorkspaceId{5});
                    context.set_active_view(ssg::ViewId{6});
                    return ssg::CommandHandlerResult::failure("injected");
                }),
        command("topology.throw", ssg::CommandEffect::Mutation,
                [](ssg::CommandContext& context, std::any const&)
                    -> ssg::CommandHandlerResult {
                    context.set_active_workspace(ssg::WorkspaceId{8});
                    context.set_active_view(ssg::ViewId{9});
                    throw std::runtime_error{"injected"};
                }),
        command("topology.commit", ssg::CommandEffect::Mutation,
                [](ssg::CommandContext& context, std::any const&) {
                    context.set_active_workspace(ssg::WorkspaceId{2});
                    context.set_active_view(ssg::ViewId{3});
                    return ssg::CommandHandlerResult::success();
                }),
    }};
    ssg::EditorSession session{
        ssg::CommandRegistry{{std::move(commands)}}};
    ASSERT_TRUE(session.attach(principal(1), ssg::ViewId{1}).accepted());

    auto rejected_result =
        session.dispatch(ssg::ClientId{1}, request("topology.fail", 1));
    ASSERT_EQ(rejected_result.error, ssg::CommandError::HandlerFailed);
    ASSERT_EQ(session.revision(), ssg::Revision{1});
    auto topology = session.topology();
    ASSERT_FALSE(topology.active_workspace.has_value());
    ASSERT_FALSE(topology.active_view.has_value());

    auto threw =
        session.dispatch(ssg::ClientId{1}, request("topology.throw", 1));
    ASSERT_EQ(threw.error, ssg::CommandError::HandlerFailed);
    ASSERT_EQ(session.revision(), ssg::Revision{1});
    topology = session.topology();
    ASSERT_FALSE(topology.active_workspace.has_value());
    ASSERT_FALSE(topology.active_view.has_value());

    auto committed =
        session.dispatch(ssg::ClientId{1}, request("topology.commit", 1));
    ASSERT_TRUE(committed.accepted());
    ASSERT_EQ(session.revision(), ssg::Revision{2});
    topology = session.topology();
    ASSERT_EQ(topology.active_workspace,
              std::optional<ssg::WorkspaceId>{ssg::WorkspaceId{2}});
    ASSERT_EQ(topology.active_view,
              std::optional<ssg::ViewId>{ssg::ViewId{3}});
}

TEST(duplicate_registration_is_rejected_eagerly) {
    auto no_op = [](ssg::CommandContext&, std::any const&) {
        return ssg::CommandHandlerResult::success();
    };

    ASSERT_THROWS(
        ssg::CommandSet({
            command("duplicate", ssg::CommandEffect::Observation, no_op),
            command("duplicate", ssg::CommandEffect::Mutation, no_op),
        }),
        std::invalid_argument);

    ssg::CommandSet first{{
        command("duplicate", ssg::CommandEffect::Observation, no_op),
    }};
    ssg::CommandSet second{{
        command("duplicate", ssg::CommandEffect::Observation, no_op),
    }};
    ASSERT_THROWS(
        ssg::CommandRegistry(
            {std::move(first), std::move(second)}),
        std::invalid_argument);
}

TEST(principal_capability_enforcement_has_origin_parity) {
    int calls = 0;
    ssg::CommandSet commands{{command(
        "local.ingress", ssg::CommandEffect::Observation,
        [&](ssg::CommandContext&, std::any const&) {
            ++calls;
            return ssg::CommandHandlerResult::success();
        },
        {ssg::CapabilityId{"local_file_drop"}})}};
    ssg::EditorSession session{
        ssg::CommandRegistry{{std::move(commands)}}};

    ASSERT_TRUE(session.attach(
        principal(1, ssg::InvocationOrigin::InProcess,
                  {ssg::CapabilityId{"local_file_drop"}}),
        ssg::ViewId{1}).accepted());
    ASSERT_TRUE(session.attach(
        principal(2, ssg::InvocationOrigin::Websocket,
                  {ssg::CapabilityId{"local_file_drop"}}),
        ssg::ViewId{2}).accepted());
    ASSERT_TRUE(session.attach(
        principal(3, ssg::InvocationOrigin::InProcess), ssg::ViewId{3})
                    .accepted());
    ASSERT_TRUE(session.attach(
        principal(4, ssg::InvocationOrigin::Websocket), ssg::ViewId{4})
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

TEST(executor_serializes_concurrent_handlers) {
    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
    ssg::CommandSet commands{{command(
        "executor.observe", ssg::CommandEffect::Observation,
        [&](ssg::CommandContext&, std::any const&) {
            int now = active.fetch_add(1) + 1;
            int seen = maximum.load();
            while (seen < now &&
                   !maximum.compare_exchange_weak(seen, now)) {
            }
            std::this_thread::sleep_for(2ms);
            active.fetch_sub(1);
            return ssg::CommandHandlerResult::success();
        })}};
    ssg::EditorSession session{
        ssg::CommandRegistry{{std::move(commands)}}};
    ASSERT_TRUE(session.attach(principal(1), ssg::ViewId{1}).accepted());
    ASSERT_TRUE(session.attach(principal(2), ssg::ViewId{2}).accepted());

    std::vector<std::thread> threads;
    std::atomic<int> dispatch_failures{0};
    for (std::uint64_t i = 0; i < 12; ++i) {
        threads.emplace_back([&, i] {
            auto client = ssg::ClientId{(i % 2) + 1};
            if (!session.dispatch(client, request("executor.observe", 0))
                     .accepted()) {
                dispatch_failures.fetch_add(1);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    ASSERT_EQ(maximum.load(), 1);
    ASSERT_EQ(dispatch_failures.load(), 0);
}

}  // namespace

int main() {
    RUN(total_order_and_registered_dispatch);
    RUN(stale_rejection_applies_only_to_mutations);
    RUN(client_identity_and_principal_are_isolated);
    RUN(handler_failure_is_atomic);
    RUN(duplicate_registration_is_rejected_eagerly);
    RUN(principal_capability_enforcement_has_origin_parity);
    RUN(executor_serializes_concurrent_handlers);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
