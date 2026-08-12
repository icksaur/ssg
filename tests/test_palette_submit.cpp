// seam test — a palette/finder submit resolves to exactly one library command
// per picker mode, and this decision lives in ONE place both the TUI and the web
// host call, so the two clients cannot drift into different submit behavior.

#include <ssg/PaletteSearcher.h>
#include <ssg/PaletteSubmit.h>
#include <ssg/Search.h>

#include "test_helpers.h"

#include <any>
#include <string>

namespace {

using namespace ssg;

TEST(commandModeSubmitsPaletteExecuteWithTheCandidateId) {
    auto const submit = paletteSubmitCommand(SearchMode::Command, "view.split");
    ASSERT_TRUE(submit.has_value());
    ASSERT_TRUE(submit->command == CommandName{"palette.execute"});
    auto const* args = std::any_cast<PaletteExecuteArguments>(&submit->payload);
    ASSERT_TRUE(args != nullptr);
    ASSERT_EQ(args->commandId, std::string{"view.split"});
}

TEST(fileModeSubmitsFileOpenWithTheCandidateIdAsAString) {
    auto const submit = paletteSubmitCommand(SearchMode::File, "src/main.cpp");
    ASSERT_TRUE(submit.has_value());
    ASSERT_TRUE(submit->command == CommandName{"file.open"});
    auto const* path = std::any_cast<std::string>(&submit->payload);
    ASSERT_TRUE(path != nullptr);
    ASSERT_EQ(*path, std::string{"src/main.cpp"});
}

TEST(modesWithoutASubmitActionResolveToNothing) {
    ASSERT_FALSE(paletteSubmitCommand(SearchMode::Line, "x").has_value());
    ASSERT_FALSE(paletteSubmitCommand(SearchMode::Symbol, "x").has_value());
    ASSERT_FALSE(paletteSubmitCommand(SearchMode::Text, "x").has_value());
}

}  // namespace

int main() {
    RUN(commandModeSubmitsPaletteExecuteWithTheCandidateId);
    RUN(fileModeSubmitsFileOpenWithTheCandidateIdAsAString);
    RUN(modesWithoutASubmitActionResolveToNothing);
    return 0;
}
