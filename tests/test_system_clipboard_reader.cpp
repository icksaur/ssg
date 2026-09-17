#include <ssg/SystemClipboardReader.h>

#include "test_helpers.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

TEST(discoveryPrefersWaylandThenX11WithExplicitSelections) {
    const auto environment = [](std::string_view name)
        -> std::optional<std::string> {
        if (name == "WAYLAND_DISPLAY" || name == "DISPLAY") {
            return "present";
        }
        return std::nullopt;
    };
    const auto executable = [](std::string_view name)
        -> std::optional<std::filesystem::path> {
        return std::filesystem::path{"/helpers"} / name;
    };

    const auto programs =
        ssg::discoverSystemClipboardPrograms(environment, executable);
    ASSERT_EQ(programs.size(), std::size_t{2});
    ASSERT_EQ(programs[0],
              (ssg::SystemClipboardProgram{
                  "/helpers/wl-paste", {"--no-newline"}}));
    ASSERT_EQ(programs[1],
              (ssg::SystemClipboardProgram{
                  "/helpers/xclip",
                  {"-selection", "clipboard", "-out", "-target",
                   "UTF8_STRING"}}));
}

TEST(discoveryReportsNoProgramsWithoutAUsableDisplay) {
    const auto environment = [](std::string_view)
        -> std::optional<std::string> { return std::nullopt; };
    const auto executable = [](std::string_view)
        -> std::optional<std::filesystem::path> {
        return std::filesystem::path{"/unused"};
    };
    const auto programs =
        ssg::discoverSystemClipboardPrograms(environment, executable);
    ASSERT_TRUE(programs.empty());
}

TEST(pastePlanPreservesContextSpecificFallbacks) {
    const auto editor = ssg::ClientOwnedInput{
        ssg::ClientOwnedInputKind::SystemClipboardPasteIntoEditor, {},
        "internal"};
    const auto text = ssg::ClientOwnedInput{
        ssg::ClientOwnedInputKind::SystemClipboardPasteIntoText, {},
        "internal"};

    auto plan = ssg::planSystemClipboardPaste(
        editor, {ssg::SystemClipboardReadStatus::Success, "desktop"});
    ASSERT_EQ(plan.kind, ssg::SystemClipboardPasteKind::CommittedText);
    ASSERT_EQ(plan.text, std::string{"desktop"});

    plan = ssg::planSystemClipboardPaste(
        editor, {ssg::SystemClipboardReadStatus::Failed, {}});
    ASSERT_EQ(plan.kind, ssg::SystemClipboardPasteKind::InternalRegister);

    plan = ssg::planSystemClipboardPaste(
        text, {ssg::SystemClipboardReadStatus::Failed, {}});
    ASSERT_EQ(plan.kind, ssg::SystemClipboardPasteKind::CommittedText);
    ASSERT_EQ(plan.text, std::string{"internal"});

    plan = ssg::planSystemClipboardPaste(
        text, {ssg::SystemClipboardReadStatus::Success, {}});
    ASSERT_EQ(plan.kind, ssg::SystemClipboardPasteKind::None);
}

SSG_TEST_SUITE(test_system_clipboard_reader) {
    RUN(discoveryPrefersWaylandThenX11WithExplicitSelections);
    RUN(discoveryReportsNoProgramsWithoutAUsableDisplay);
    RUN(pastePlanPreservesContextSpecificFallbacks);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
