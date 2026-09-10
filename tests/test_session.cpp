#include "test_helpers.h"

#include <ssg/Command.h>

namespace {

TEST(registeredDispatchRunsInOrder) {
    ssg::Commands commands;
    int calls = 0;
    commands.add("state.advance", "Advance", [&] {
        ++calls;
        return ssg::CommandResult{};
    });

    ASSERT_TRUE(commands.dispatch("state.advance").accepted());
    ASSERT_TRUE(commands.dispatch("state.advance").accepted());
    ASSERT_TRUE(commands.dispatch("state.advance").accepted());
    ASSERT_EQ(calls, 3);
    ASSERT_EQ(commands.dispatch("missing").error,
              ssg::CommandError::UnknownCommand);
}

TEST(viewActionsRemainExplicit) {
    ssg::Commands commands;
    commands.add("view.scroll", "Scroll", [] {
        return ssg::CommandResult{
            ssg::CommandError::None, {},
            ssg::ViewAction{ssg::ScrollLines{ssg::ScrollTarget::Tree, -3}}};
    });
    const auto result = commands.dispatch("view.scroll");
    ASSERT_TRUE(result.accepted());
    const std::optional<ssg::ViewAction> expected{
        ssg::ScrollLines{ssg::ScrollTarget::Tree, -3}};
    ASSERT_EQ(result.viewAction, expected);
}

}  // namespace

SSG_TEST_SUITE(test_session) {
    RUN(registeredDispatchRunsInOrder);
    RUN(viewActionsRemainExplicit);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
