#include "test_helpers.h"
#include "grid_test_frame.h"

#include <ssg/Editor.h>
#include <ssg/OptionalSubsystemAudit.h>
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
    fs::create_directories(root / "scratch");
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
    config.scratchRoot = root / "scratch";
    config.recoveryRoot = root / "recovery";
    config.deferEnrichment = defer;
    return config;
}

}  // namespace

TEST(firstFrameConstructsNoOptionalSubsystem) {
    // Producing the first frame must construct no optional subsystem.
    ssg::resetOptionalConstructionAudit();
    auto root = makeWorkspace("no_optional");
    // The git-diff worker owns the workspace filesystem watcher, which Phase 5a
    // makes an always-available background service (Decision 1). It is constructed
    // on the worker thread, off the first-frame path, so disabling it here isolates
    // the audit to the MAIN-thread first-frame path this invariant guards; the
    // background watcher never blocks the first frame.
    auto config = configFor(root, /*defer=*/true);
    config.enableGitDiffWorker = false;
    config.enableFilesystemWatcher = false;
    auto created = ssg::createEditor(config);
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"code.txt"}})
                    .accepted());
    (void)ssg::test::projectGridFrame(runtime);

    // Exhaustive over the enumerated subsystems (a missing enum entry fails the
    // static_assert in OptionalSubsystemAudit.h, so the list cannot silently omit one).
    for (auto subsystem : ssg::kAllOptionalSubsystems) {
        ASSERT_EQ(ssg::optionalConstructionCount(subsystem), std::uint64_t{0});
    }
    ASSERT_EQ(ssg::optionalConstructionTotal(), std::uint64_t{0});

    // Priming (post-first-frame enrichment) also constructs nothing optional:
    // the plain-text syntax pass uses no Tree-sitter grammar.
    runtime.primeDeferred();
    ASSERT_EQ(ssg::optionalConstructionTotal(), std::uint64_t{0});

    fs::remove_all(root);
}

TEST(optionalConstructionAuditIsWiredPositiveControl) {
    // Guards against a false pass from broken instrumentation: constructing a
    // real optional subsystem (a filesystem watcher) MUST increment its counter.
    ssg::resetOptionalConstructionAudit();
    ASSERT_EQ(ssg::optionalConstructionCount(ssg::OptionalSubsystem::FilesystemWatcher),
              std::uint64_t{0});
    auto root = makeWorkspace("positive_control");
    {
        auto watcher = ssg::makePlatformFilesystemWatcher(
            std::filesystem::canonical(root / "workspace"));
        ASSERT_TRUE(watcher != nullptr);
    }
    ASSERT_TRUE(ssg::optionalConstructionCount(
                    ssg::OptionalSubsystem::FilesystemWatcher) >= 1);
    fs::remove_all(root);
}

// Pins the exact ordering src/main.cpp's startup sequence depends on
// (regression coverage for the "ssg: could not open Files sidebar: files
// tree provider is unavailable" bug): with deferred enrichment, the tree's
// "filesystem" provider does not exist until primeDeferred() runs, so
// panel.show_files (dispatched before that) legitimately fails; dispatched
// AFTER primeDeferred(), it must succeed cleanly. A future startup-path edit
// that moves panel.show_files back before primeDeferred() would fail this
// test's first assertion becoming the WRONG one to rely on silently.
TEST(panelShowFilesRequiresPrimeDeferredFirst) {
    auto root = makeWorkspace("panel_ordering");
    auto created = ssg::createEditor(configFor(root, /*defer=*/true));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    auto tooEarly = runtime.dispatch({"panel.show_files",  {}});
    ASSERT_FALSE(tooEarly.accepted());

    runtime.primeDeferred();
    auto onTime = runtime.dispatch({"panel.show_files",  {}});
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
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"code.txt"}})
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
    ASSERT_TRUE(runtime.dispatch({"panel.show_files",  {}})
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

SSG_TEST_SUITE(test_startup_path) {
    // Nothing optional may construct before main (no
    // self-registering globals); the ledger must be empty at process entry.
    if (ssg::optionalConstructionTotal() != 0) {
        std::cerr << "  FAIL: an optional subsystem constructed before main "
                     "(static-init side effect)\n";
        ++failed;
    }
    RUN(firstFrameConstructsNoOptionalSubsystem);
    RUN(optionalConstructionAuditIsWiredPositiveControl);
    RUN(panelShowFilesRequiresPrimeDeferredFirst);
    RUN(focusEditorSurvivesPanelShowFilesDispatchedAfter);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
