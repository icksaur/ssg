#include <ssg/PromptSurface.h>
#include "test_helpers.h"

#include <string>

namespace {

using namespace ssg;

PromptRequest request(PromptKind kind) {
    PromptRequest value;
    value.kind = kind;
    value.accessibleLabel =
        kind == PromptKind::Path ? "Path" :
        kind == PromptKind::Replace ? "Replace" : "Find";
    value.inputs.push_back(
        {kind == PromptKind::Path ? "path" : "find",
         kind == PromptKind::Path ? "Path" : "Find text",
         PromptEditState{"needle"}});
    if (kind == PromptKind::Replace) {
        value.inputs.push_back(
            {"replace", "Replacement text", PromptEditState{"value"}});
    }
    if (kind == PromptKind::Find || kind == PromptKind::Replace) {
        value.toggles.push_back({"case", "Case sensitive", false, 6});
        value.toggles.push_back({"word", "Whole word", true, 7});
        value.matchCount = PromptMatchCount{"matches", "Match count", "2/7"};
    }
    return value;
}

TEST(promptSubmitAndCancelAreNonModal) {
    PromptSurface surface;
    ASSERT_TRUE(surface.open(request(PromptKind::Replace)).accepted());
    const auto submitted = surface.submit();
    ASSERT_TRUE(submitted.accepted());
    ASSERT_TRUE(submitted.submission.has_value());
    ASSERT_EQ(submitted.submission->values,
              (std::vector<std::string>{"needle", "value"}));
    ASSERT_FALSE(surface.active());

    ASSERT_TRUE(surface.open(request(PromptKind::Path)).accepted());
    const auto cancelled = surface.cancel();
    ASSERT_TRUE(cancelled.accepted());
    ASSERT_FALSE(cancelled.submission.has_value());
    ASSERT_FALSE(surface.active());
}

TEST(promptOpenResetsTheActiveInputPerKindAndOnTransition) {
    PromptSurface surface;
    ASSERT_TRUE(surface.open(request(PromptKind::Replace)).accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{1});
    ASSERT_TRUE(surface.open(request(PromptKind::Find)).accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{0});
    ASSERT_TRUE(surface.open(request(PromptKind::Path)).accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{0});
}

TEST(promptFocusOnlyAddressesAnInputNeverAToggleOrCount) {
    PromptSurface surface;
    ASSERT_TRUE(surface.open(request(PromptKind::Replace)).accepted());
    ASSERT_TRUE(surface.focusInput("find").accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{0});
    ASSERT_TRUE(surface.focusInput("replace").accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{1});
    const auto rejected = surface.focusInput("case");
    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error->code, PromptErrorCode::UnknownInput);
    ASSERT_EQ(surface.activeInput(), std::size_t{1});
    ASSERT_TRUE(surface.focusNextInput().accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{0});
    ASSERT_TRUE(surface.focusNextInput().accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{1});
}

TEST(promptControlsCarryTheirOperatingCommands) {
    PromptRequest find;
    find.kind = PromptKind::Find;
    find.accessibleLabel = "Find";
    find.inputs.push_back(
        {"find.query", "Find text", PromptEditState{"needle"}});
    ASSERT_TRUE(resolvePromptControls(find).front().command.empty());

    PromptRequest replace;
    replace.kind = PromptKind::Replace;
    replace.accessibleLabel = "Replace";
    replace.inputs.push_back(
        {"find.query", "Find text", PromptEditState{"needle"}});
    replace.inputs.push_back(
        {"replace.replacement", "Replacement text", PromptEditState{"value"}});
    const auto replaceControls = resolvePromptControls(replace);
    ASSERT_TRUE(replaceControls.at(0).command.empty());
    ASSERT_TRUE(replaceControls.at(1).command.empty());

    PromptRequest path;
    path.kind = PromptKind::Path;
    path.accessibleLabel = "Path";
    path.inputs.push_back({"path", "Path", PromptEditState{"/tmp"}});
    ASSERT_TRUE(resolvePromptControls(path).front().command.empty());
}

} // namespace

SSG_TEST_SUITE(ssg_prompt_tests) {
    RUN(promptSubmitAndCancelAreNonModal);
    RUN(promptOpenResetsTheActiveInputPerKindAndOnTransition);
    RUN(promptFocusOnlyAddressesAnInputNeverAToggleOrCount);
    RUN(promptControlsCarryTheirOperatingCommands);
    return failed == 0 ? 0 : 1;
}
