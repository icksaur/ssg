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

TEST(commandModeSubmitsThroughTheGenericPickerCommand) {
    const PickerActivation activation{SearchMode::Command,
                                      PickerActivationId{7}};
    auto const submit = paletteSubmitCommand(activation, "view.split");
    ASSERT_TRUE(submit.has_value());
    ASSERT_TRUE(submit->command == CommandName{"picker.submit"});
    auto const* args = std::any_cast<PickerSubmitArguments>(&submit->payload);
    ASSERT_TRUE(args != nullptr);
    if (!args) return;
    ASSERT_TRUE(args->activation == activation);
    ASSERT_EQ(args->candidateId, std::string{"view.split"});
}

TEST(fileModeSubmitsThroughTheGenericPickerCommand) {
    const PickerActivation activation{SearchMode::File,
                                      PickerActivationId{8}};
    auto const submit = paletteSubmitCommand(activation, "src/main.cpp");
    ASSERT_TRUE(submit.has_value());
    ASSERT_TRUE(submit->command == CommandName{"picker.submit"});
    auto const* args = std::any_cast<PickerSubmitArguments>(&submit->payload);
    ASSERT_TRUE(args != nullptr);
    if (!args) return;
    ASSERT_TRUE(args->activation == activation);
    ASSERT_EQ(args->candidateId, std::string{"src/main.cpp"});
}

TEST(modesWithoutASubmitActionResolveToNothing) {
    ASSERT_FALSE(paletteSubmitCommand(
                     {SearchMode::Line, PickerActivationId{1}}, "x")
                     .has_value());
    ASSERT_FALSE(paletteSubmitCommand(
                     {SearchMode::Symbol, PickerActivationId{1}}, "x")
                     .has_value());
    ASSERT_FALSE(paletteSubmitCommand(
                     {SearchMode::Text, PickerActivationId{1}}, "x")
                     .has_value());
}

}  // namespace

int main() {
    RUN(commandModeSubmitsThroughTheGenericPickerCommand);
    RUN(fileModeSubmitsThroughTheGenericPickerCommand);
    RUN(modesWithoutASubmitActionResolveToNothing);
    return 0;
}
