#include "test_helpers.h"
#include "editor_test_support.h"
#include "grid_test_frame.h"

#include <ssg/FilesystemWatcher.h>

#include <filesystem>
#include <fstream>
#include <cstdint>
#include <string>

namespace {

namespace fs = std::filesystem;

fs::path makeWorkspace(std::string const& name) {
    auto root = testRuntimePath("startup_path_" + name);
    fs::remove_all(root);
    fs::create_directories(root / "workspace");
    fs::create_directories(root / "recovery");
    std::string text;
    for (int i = 0; i < 200; ++i) text += "int value = 1; // a line of code\n";
    std::ofstream{root / "workspace" / "code.txt", std::ios::binary} << text;
    // A few extra files so the tree scan has something to find.
    for (int i = 0; i < 5; ++i) {
        std::ofstream{root / "workspace" / ("extra_" + std::to_string(i) + ".txt")}
            << "x\n";
    }
    return root;
}

ssg::EditorConfig configFor(fs::path const& root, bool defer) {
    ssg::EditorConfig config;
    config.cwd = root / "workspace";
    config.recoveryRoot = root / "recovery";
    config.deferEnrichment = defer;
    return config;
}

}  // namespace

  TEST(panelShowFilesRequiresPrimeDeferredFirst) {
    auto root = makeWorkspace("panel_ordering");
    auto created = ssg::createEditor(configFor(root, /*defer=*/true));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    auto tooEarly = runtime.dispatch("panel.show_files");
    ASSERT_FALSE(tooEarly.accepted());

    runtime.primeDeferred();
    auto onTime = runtime.dispatch("panel.show_files");
    ASSERT_TRUE(onTime.accepted());

    fs::remove_all(root);
}

// Pins the fileOpenedAtStartup re-focus contract: panel.show_files's
// showPanelProvider side effect moves keyboard focus to the panel
// unconditionally, so a startup sequence that opens a command-line file
// argument (and focuses the editor) BEFORE dispatching panel.show_files
// (matching src/main.cpp's ordering: file.open+focusEditor() pre-loop,
// panel.show_files after primeDeferred() inside the loop) must re-assert
// editor focus AFTER panel.show_files, or the file-argument launch silently
// ends with focus on the panel instead of the editor.
TEST(focusEditorSurvivesPanelShowFilesDispatchedAfter) {
    auto root = makeWorkspace("focus_ordering");
    auto created = ssg::createEditor(configFor(root, /*defer=*/true));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    // Mirrors src/main.cpp's pre-loop file-argument open.
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"code.txt"})
                    .accepted());
    runtime.focusEditor();
    auto beforePanel =
        ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(beforePanel.has_value());
    if (beforePanel) {
        ASSERT_EQ(ssg::effectiveUiFocus(beforePanel->uiTree),
                  ssg::FocusTarget::Editor);
    }

    // Mirrors src/main.cpp's post-primeDeferred panel dispatch: this
    // moves focus to the panel as a side effect, clobbering the above.
    runtime.primeDeferred();
    ASSERT_TRUE(runtime.dispatch("panel.show_files")
                    .accepted());
    auto afterPanel =
        ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(afterPanel.has_value());
    if (afterPanel) {
        ASSERT_EQ(ssg::effectiveUiFocus(afterPanel->uiTree),
                  ssg::FocusTarget::Panel);
    }

    // Mirrors src/main.cpp's fileOpenedAtStartup re-assert: calling
    // focusEditor() again restores the correct final focus.
    runtime.focusEditor();
    auto restored =
        ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(restored.has_value());
    if (restored) {
        ASSERT_EQ(ssg::effectiveUiFocus(restored->uiTree),
                  ssg::FocusTarget::Editor);
    }

    fs::remove_all(root);
}

TEST(sessionSnapshotPathStaysAtTheProcessStartingDirectory) {
    const fs::path starting{"/launch/directory"};
    const fs::path openedWorkspace{"/other/workspace"};
    const auto snapshot = ssg::sessionSnapshotPath(starting);
    ASSERT_EQ(snapshot,
              starting / ssg::kSessionDirectoryName /
                  ssg::kSessionSnapshotFilename);
    ASSERT_FALSE(snapshot.string().find(openedWorkspace.string()) == 0);
}

SSG_TEST_SUITE(test_startup_path) {
    RUN(panelShowFilesRequiresPrimeDeferredFirst);
    RUN(focusEditorSurvivesPanelShowFilesDispatchedAfter);
    RUN(sessionSnapshotPathStaysAtTheProcessStartingDirectory);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
