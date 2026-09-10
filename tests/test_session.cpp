#include "test_helpers.h"

#include <ssg/CommandCatalog.h>
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
        catalog->add(ssg::CommandSpec{
            .id = std::move(entry.id),
            .owner = "test-owner",
            .summary = "a command",
            .effect = entry.effect,
            .binding = ssg::bindUntypedHandler(std::move(entry.handler),
                                               std::nullopt, false),
        });
    }
    return catalog;
}

ssg::ClientCommand request(std::string id) {
    return {std::move(id), std::any{}};
}

TEST(registeredDispatchRunsInOrder) {
    int calls = 0;
    auto catalog = catalogOf({command(
        "state.advance", ssg::CommandEffect::Mutation,
        [&](ssg::CommandContext&, std::any const&) {
            ++calls;
            return ssg::CommandHandlerResult::success();
        })});
    auto& session = *catalog;

    auto first = session.dispatch(request("state.advance"));
    auto second = session.dispatch(request("state.advance"));
    auto third = session.dispatch(request("state.advance"));

    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(second.accepted());
    ASSERT_TRUE(third.accepted());
    ASSERT_EQ(calls, 3);

    auto unknown = session.dispatch(request("missing"));
    ASSERT_EQ(unknown.error, ssg::CommandError::UnknownCommand);
}

TEST(viewActionsRemainExplicit) {
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
    auto& session = *catalog;

    auto result =
        session.dispatch(request("view.scroll"));
    ASSERT_TRUE(result.accepted());
    ASSERT_TRUE(result.viewAction.has_value());
    if (result.viewAction) {
        ASSERT_EQ(*result.viewAction,
                  (ssg::ViewAction{ssg::ScrollLines{
                      ssg::ScrollTarget::Tree, -3}}));
    }

    ASSERT_EQ(session
                  .dispatch(request("view.missing_action"))
                  .error,
              ssg::CommandError::HandlerFailed);
    ASSERT_EQ(session
                  .dispatch(request("observe.invalid_action"))
                  .error,
              ssg::CommandError::HandlerFailed);
}

// A duplicate id is rejected by the catalog, where registration happens, and
// that rule is covered by test_command_catalog.  It used to be checked twice
// here -- once per command set, once across sets -- because commands were
// assembled from several sets before reaching the session.  There is one place
// now.

}  // namespace

SSG_TEST_SUITE(test_session) {
    RUN(registeredDispatchRunsInOrder);
    RUN(viewActionsRemainExplicit);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
