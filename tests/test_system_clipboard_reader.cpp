#include <ssg/SystemClipboardReader.h>

#include "test_helpers.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace {

std::filesystem::path helper(std::string_view name, std::string_view body) {
    static const auto root = testRuntimePath("system_clipboard_reader");
    std::filesystem::create_directories(root);
    const auto path = root / name;
    std::ofstream output{path};
    output << "#!/bin/sh\n" << body << '\n';
    output.close();
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    return path;
}

} // namespace

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

TEST(readerReturnsExactMultilineTextAndEmptySuccess) {
    const auto multiline = helper(
        "multiline",
        "[ \"$1\" = \"--no-newline\" ] || exit 2\nprintf 'one\\ntwo'");
    auto read = ssg::SystemClipboardReader{
                    {{multiline, {"--no-newline"}}}}
                    .read();
    ASSERT_TRUE(read.accepted());
    ASSERT_EQ(read.text, std::string{"one\ntwo"});

    const auto empty = helper("empty", "exit 0");
    read = ssg::SystemClipboardReader{{{empty, {}}}}.read();
    ASSERT_TRUE(read.accepted());
    ASSERT_TRUE(read.text.empty());
}

TEST(readerTriesTheNextAvailableClipboardProgram) {
    const auto failure = helper("failure", "exit 1");
    const auto success = helper("success", "printf fallback");
    const auto read =
        ssg::SystemClipboardReader{{{failure, {}}, {success, {}}}}.read();
    ASSERT_TRUE(read.accepted());
    ASSERT_EQ(read.text, std::string{"fallback"});
}

TEST(readerRejectsInvalidOversizedAndStalledOutput) {
    const auto invalid = helper("invalid", "printf '\\377'");
    auto read = ssg::SystemClipboardReader{{{invalid, {}}}}.read();
    ASSERT_EQ(read.status, ssg::SystemClipboardReadStatus::InvalidUtf8);

    const auto oversized = helper("oversized", "printf '12345'");
    read = ssg::SystemClipboardReader{
               {{oversized, {}}}, std::chrono::milliseconds{100}, 4}
               .read();
    ASSERT_EQ(read.status, ssg::SystemClipboardReadStatus::TooLarge);

    const auto stalled = helper("stalled", "sleep 1");
    read = ssg::SystemClipboardReader{
               {{stalled, {}}}, std::chrono::milliseconds{20}, 1024}
               .read();
    ASSERT_EQ(read.status, ssg::SystemClipboardReadStatus::TimedOut);
}

TEST(readerReportsUnavailableWithoutAUsableDisplayHelper) {
    const auto environment = [](std::string_view)
        -> std::optional<std::string> { return std::nullopt; };
    const auto executable = [](std::string_view)
        -> std::optional<std::filesystem::path> {
        return std::filesystem::path{"/unused"};
    };
    const auto programs =
        ssg::discoverSystemClipboardPrograms(environment, executable);
    ASSERT_TRUE(programs.empty());
    ASSERT_EQ(ssg::SystemClipboardReader{programs}.read().status,
              ssg::SystemClipboardReadStatus::Unavailable);
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
    RUN(readerReturnsExactMultilineTextAndEmptySuccess);
    RUN(readerTriesTheNextAvailableClipboardProgram);
    RUN(readerRejectsInvalidOversizedAndStalledOutput);
    RUN(readerReportsUnavailableWithoutAUsableDisplayHelper);
    RUN(pastePlanPreservesContextSpecificFallbacks);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
