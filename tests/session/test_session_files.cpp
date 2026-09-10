#include "../test_helpers.h"
#include "../grid_test_view.h"
#include "../grid_test_frame.h"

#include <ssg/Editor.h>
#include <ssg/FileCommands.h>
#include <ssg/GitDiffSource.h>
#include <ssg/HitTester.h>
#include <ssg/Keymap.h>
#include <ssg/Settings.h>
#include <ssg/TextCodec.h>
#include <ssg/TextInputCommands.h>

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::filesystem::path uniqueRoot(std::string_view name) {
    auto root = testRuntimePath("runtime_files_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "recovery");
    std::filesystem::create_directories(root / "archive");
    return root;
}

ssg::EditorConfig configFor(const std::filesystem::path& root) {
    ssg::EditorConfig config{
        root / "workspace", root / "recovery", root / "archive"};
    config.enableGitDiffWorker = false;
    config.enableFilesystemWatcher = false;
    return config;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

TEST(openingAFileRevealsTheCaretResettingAStaleScroll) {
    // Reveal-policy audit: opening a document must show the
    // caret, not inherit the previous document's scroll offset. Two tall files.
    auto root = uniqueRoot("open_reveal");
    std::string tall;
    for (int i = 0; i < 100; ++i) tall += "line\n";
    std::ofstream{root / "workspace" / "a.txt", std::ios::binary} << tall;
    std::ofstream{root / "workspace" / "b.txt", std::ios::binary} << tall;
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    const ssg::ViewportDimensions dims{80, 24};
    ssg::test::GridTestView grid{dims};
    auto firstRow = [&] {
        auto snap = grid.present(runtime);
        return snap ? snap->viewport.firstVisualRow : 0U;
    };

    // Open A and scroll far down (free scroll leaves the caret off-screen above).
    ASSERT_TRUE(ssg::applyFilePathCompletion(runtime, ssg::PromptCompletion::FileOpen, "a.txt").accepted);
    ASSERT_TRUE(grid.input(runtime, ssg::ScrollLinesInput{
                                       {ssg::ScrollTarget::Document, 50}}).accepted());
    ASSERT_EQ(firstRow(), 50U);

    // Opening B resets the view so B's caret (its document start) is visible: the
    // stale offset of 50 must not carry over.
    ASSERT_TRUE(ssg::applyFilePathCompletion(runtime, ssg::PromptCompletion::FileOpen, "b.txt").accepted);
    ASSERT_EQ(firstRow(), 0U);
}

TEST(openEditSaveRoundTripsRealDiskBytes) {
    auto root = uniqueRoot("round_trip");
    {
        std::ofstream output{root / "workspace" / "note.txt", std::ios::binary};
        output << "hello";
    }

    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    auto open = ssg::applyFilePathCompletion(runtime, ssg::PromptCompletion::FileOpen, "note.txt");
    ASSERT_TRUE(open.accepted);
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"hello"});

    auto insert = ssg::test::typeText(runtime, "!");
    ASSERT_TRUE(insert.accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"!hello"});

    auto save = runtime.dispatch("file.save");
    ASSERT_TRUE(save.accepted());
    ASSERT_EQ(readText(root / "workspace" / "note.txt"), std::string{"!hello"});
}

TEST(droppedContentOpensAsANewDocument) {
    auto root = uniqueRoot("drop");
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    auto accepted = ssg::openDroppedContent(runtime, std::vector<std::uint8_t>{'a'}, "a.txt");
    ASSERT_TRUE(accepted.accepted);
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"a"});
}

TEST(closingTheLastTabClearsTheEditorDocument) {
    auto root = uniqueRoot("close_last_tab");
    std::ofstream{root / "workspace" / "a.txt", std::ios::binary} << "alpha";
    std::ofstream{root / "workspace" / "b.txt", std::ios::binary} << "beta";

    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    auto tabCount = [&] {
        return ssg::test::projectGridFrame(runtime)->tabs.tabs.size();
    };

    // Open two files: two tabs, the active document shows content.
    ASSERT_TRUE(ssg::applyFilePathCompletion(runtime, ssg::PromptCompletion::FileOpen, "a.txt").accepted);
    ASSERT_TRUE(ssg::applyFilePathCompletion(runtime, ssg::PromptCompletion::FileOpen, "b.txt").accepted);
    ASSERT_EQ(tabCount(), std::size_t{2});
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"beta"});

    // Closing one tab switches to the remaining tab's document (still shown).
    ASSERT_TRUE(runtime.dispatch("tab.close").accepted());
    ASSERT_EQ(tabCount(), std::size_t{1});
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"alpha"});

    // Closing the last tab must clear the editor document (empty state), not
    // leave a phantom document with no tab.
    ASSERT_TRUE(runtime.dispatch("tab.close").accepted());
    ASSERT_EQ(tabCount(), std::size_t{0});
    ASSERT_TRUE(ssg::test::activeDocumentText(runtime).empty());
    auto snapshot = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) ASSERT_TRUE(snapshot->documentText.empty());
}

TEST(tabActivateFocusesTheEditor) {
    auto root = uniqueRoot("tab_activate_focus");
    std::ofstream{root / "workspace" / "a.txt", std::ios::binary} << "alpha";
    std::ofstream{root / "workspace" / "b.txt", std::ios::binary} << "beta";

    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::applyFilePathCompletion(runtime, ssg::PromptCompletion::FileOpen, "a.txt").accepted);
    ASSERT_TRUE(ssg::applyFilePathCompletion(runtime, ssg::PromptCompletion::FileOpen, "b.txt").accepted);
    const ssg::ViewportDimensions dims{80, 24};
    auto focus = [&] {
        auto snap = ssg::test::projectGridFrame(runtime);
        return snap ? ssg::effectiveUiFocus(snap->uiTree)
                    : ssg::FocusTarget::Editor;
    };

    // Move focus to the panel, then activating a tab (a tab click) returns focus
    // to the editor.
    ASSERT_TRUE(runtime.dispatch("panel.toggle").accepted());
    ASSERT_TRUE(runtime.dispatch("panel.focus").accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);

    auto first = ssg::test::projectGridFrame(runtime)->tabs.tabs.front().id;
    ASSERT_TRUE(ssg::test::dispatchInput(runtime, ssg::TabPointerInput{first}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Editor);
}

TEST(switchingTabsRevealsTheNewDocumentsCaret) {
    // Reveal-policy: switching to a different tab shows that document's caret
    // instead of inheriting the previous tab's scroll offset.
    auto root = uniqueRoot("tab_switch_reveal");
    std::string tall;
    for (int i = 0; i < 100; ++i) tall += "line\n";
    std::ofstream{root / "workspace" / "a.txt", std::ios::binary} << tall;
    std::ofstream{root / "workspace" / "b.txt", std::ios::binary} << tall;
    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    const ssg::ViewportDimensions dims{80, 24};
    ssg::test::GridTestView grid{dims};
    auto firstRow = [&] {
        auto snap = grid.present(runtime);
        return snap ? snap->viewport.firstVisualRow : 0U;
    };
    ASSERT_TRUE(ssg::applyFilePathCompletion(runtime, ssg::PromptCompletion::FileOpen, "a.txt").accepted);
    ASSERT_TRUE(ssg::applyFilePathCompletion(runtime, ssg::PromptCompletion::FileOpen, "b.txt").accepted);
    // B is active; scroll it far down (free scroll leaves B's caret off-screen).
    ASSERT_TRUE(grid.input(runtime, ssg::ScrollLinesInput{
                                       {ssg::ScrollTarget::Document, 50}}).accepted());
    ASSERT_EQ(firstRow(), 50U);

    // Switch to A (previous tab): its caret (top) is revealed, not B's stale 50.
    ASSERT_TRUE(runtime.dispatch("tab.previous").accepted());
    ASSERT_EQ(firstRow(), 0U);

    // Moving a tab keeps the SAME active document and must NOT snap the scroll:
    // switch back to B, scroll away, move the tab, and the offset stays put.
    ASSERT_TRUE(runtime.dispatch("tab.next").accepted());
    ASSERT_TRUE(grid.input(runtime, ssg::ScrollLinesInput{
                                       {ssg::ScrollTarget::Document, 50}}).accepted());
    ASSERT_EQ(firstRow(), 50U);
    ASSERT_TRUE(runtime.dispatch("tab.move_left").accepted());
    ASSERT_EQ(firstRow(), 50U);  // same document -> no reveal snap
}

TEST(closingNonActiveDirtyTabReopensItsOwnContentWithNewDocumentId) {
    auto root = uniqueRoot("close_non_active_dirty");
    std::ofstream{root / "workspace" / "a.txt", std::ios::binary} << "alpha";
    std::ofstream{root / "workspace" / "b.txt", std::ios::binary} << "beta";

    auto created = ssg::createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    ASSERT_TRUE(ssg::applyFilePathCompletion(runtime, ssg::PromptCompletion::FileOpen, "a.txt").accepted);
    ASSERT_TRUE(ssg::test::typeText(runtime, "!").accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"!alpha"});
    ASSERT_TRUE(ssg::applyFilePathCompletion(runtime, ssg::PromptCompletion::FileOpen, "b.txt").accepted);
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"beta"});

    auto beforeClose = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(beforeClose.has_value());
    if (!beforeClose.has_value()) return;
    std::optional<ssg::TabId> tabA;
    std::optional<ssg::FileDocumentId> documentA;
    for (auto const& tab : beforeClose->tabs.tabs) {
        if (tab.label == "a.txt") {
            tabA = tab.id;
            documentA = tab.document;
            break;
        }
    }
    ASSERT_TRUE(tabA.has_value());
    ASSERT_TRUE(documentA.has_value());
    if (!tabA || !documentA) return;

    ASSERT_TRUE(ssg::closeTabById(runtime, *tabA).accepted);
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"beta"});
    ASSERT_TRUE(runtime
                    .dispatch("tab.reopen_closed")
                    .accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"!alpha"});

    auto afterReopen = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(afterReopen.has_value());
    if (!afterReopen.has_value()) return;
    std::optional<ssg::FileDocumentId> reopenedDocumentA;
    for (auto const& tab : afterReopen->tabs.tabs) {
        if (tab.label == "a.txt") {
            reopenedDocumentA = tab.document;
            break;
        }
    }
    ASSERT_TRUE(reopenedDocumentA.has_value());
    ASSERT_TRUE(*reopenedDocumentA != *documentA);
}

} // namespace

SSG_TEST_SUITE(test_session_files) {
    RUN(openEditSaveRoundTripsRealDiskBytes);
    RUN(openingAFileRevealsTheCaretResettingAStaleScroll);
    RUN(droppedContentOpensAsANewDocument);
    RUN(closingTheLastTabClearsTheEditorDocument);
    RUN(tabActivateFocusesTheEditor);
    RUN(switchingTabsRevealsTheNewDocumentsCaret);
    RUN(closingNonActiveDirtyTabReopensItsOwnContentWithNewDocumentId);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
