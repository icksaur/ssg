#include "test_helpers.h"

#include <ssg/SessionSnapshot.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

void writeBytes(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream{path, std::ios::binary};
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string readBytes(const fs::path& path) {
    std::ifstream stream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}};
}

ssg::EditorConfig editorConfig(const fs::path& root) {
    fs::create_directories(root / "workspace");
    ssg::EditorConfig config;
    config.cwd = root / "workspace";
    config.recoveryRoot = root / "recovery";
    config.archiveRoot = root / "archive";
    config.snapshotPath = root / "ssg" / "session.snapshot";
    config.enableGitDiffWorker = false;
    config.enableFilesystemWatcher = false;
    return config;
}

ssg::SessionSnapshot exampleSnapshot() {
    return {{
        {ssg::SessionBackingKind::Untitled, {}, "notes",
         ssg::DocumentMode::Edit, "draft \xc3\xa9", {}, true},
        {ssg::SessionBackingKind::NeverCreatedPath, "new.txt", "new.txt",
         ssg::DocumentMode::Edit, "", {}, false},
        {ssg::SessionBackingKind::PersistedPath, "saved.txt", "saved.txt",
         ssg::DocumentMode::Edit, "changed", {0xff, 0x00, 0x7f}, false},
    }};
}

TEST(codecRoundTripsAllBackingKinds) {
    const auto snapshot = exampleSnapshot();
    const auto encoded = ssg::encodeSessionSnapshot(snapshot);
    ASSERT_TRUE(encoded.accepted());
    const auto decoded = ssg::decodeSessionSnapshot(encoded.bytes);
    ASSERT_TRUE(decoded.accepted());
    if (decoded.snapshot) ASSERT_EQ(*decoded.snapshot, snapshot);
}

TEST(codecAcceptsCanonicalEmptyFixture) {
    const std::vector<std::uint8_t> fixture{
        'S', 'S', 'G', 'S', 'N', 'A', 'P', '\0',
        1, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    };
    const auto decoded = ssg::decodeSessionSnapshot(fixture);
    ASSERT_TRUE(decoded.accepted());
    if (decoded.snapshot) ASSERT_TRUE(decoded.snapshot->tabs.empty());
}

TEST(codecRejectsEveryTruncatedPrefix) {
    const auto encoded = ssg::encodeSessionSnapshot(exampleSnapshot());
    ASSERT_TRUE(encoded.accepted());
    for (std::size_t size = 0; size < encoded.bytes.size(); ++size) {
        ASSERT_FALSE(ssg::decodeSessionSnapshot(
                         std::span<const std::uint8_t>{encoded.bytes.data(), size})
                         .accepted());
    }
}

TEST(codecRejectsWrongMagicVersionAndTrailingBytes) {
    auto encoded = ssg::encodeSessionSnapshot(exampleSnapshot()).bytes;
    encoded.front() ^= 0xff;
    ASSERT_FALSE(ssg::decodeSessionSnapshot(encoded).accepted());

    encoded = ssg::encodeSessionSnapshot(exampleSnapshot()).bytes;
    encoded[8] = 2;
    ASSERT_FALSE(ssg::decodeSessionSnapshot(encoded).accepted());

    encoded = ssg::encodeSessionSnapshot(exampleSnapshot()).bytes;
    encoded.push_back(0);
    ASSERT_FALSE(ssg::decodeSessionSnapshot(encoded).accepted());
}

TEST(codecRejectsInvalidLengthsFlagsAndDuplicateActiveTabs) {
    auto encoded = ssg::encodeSessionSnapshot(exampleSnapshot()).bytes;
    std::fill(encoded.begin() + 12, encoded.begin() + 20, 0xff);
    ASSERT_FALSE(ssg::decodeSessionSnapshot(encoded).accepted());

    encoded = ssg::encodeSessionSnapshot(exampleSnapshot()).bytes;
    encoded[22] = 2;
    ASSERT_FALSE(ssg::decodeSessionSnapshot(encoded).accepted());

    auto snapshot = exampleSnapshot();
    snapshot.tabs[1].active = true;
    ASSERT_FALSE(ssg::encodeSessionSnapshot(snapshot).accepted());
}

TEST(codecRejectsInvalidUtf8AndBackingCombinations) {
    auto snapshot = exampleSnapshot();
    snapshot.tabs.front().draft = std::string{"\xff", 1};
    ASSERT_FALSE(ssg::encodeSessionSnapshot(snapshot).accepted());

    snapshot = exampleSnapshot();
    snapshot.tabs.front().path = "unexpected";
    ASSERT_FALSE(ssg::encodeSessionSnapshot(snapshot).accepted());

    snapshot = exampleSnapshot();
    snapshot.tabs[1].baseline = {1};
    ASSERT_FALSE(ssg::encodeSessionSnapshot(snapshot).accepted());
}

TEST(fileReadDoesNotConsumeAndEmptyWriteClearsStaleState) {
    const auto root = testRuntimePath("session_snapshot_codec");
    fs::remove_all(root);
    const auto path = root / "ssg" / "session.snapshot";
    ASSERT_TRUE(ssg::writeSessionSnapshot(path, exampleSnapshot()).accepted());

    const auto first = ssg::readSessionSnapshot(path);
    const auto second = ssg::readSessionSnapshot(path);
    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(second.accepted());
    ASSERT_TRUE(first.snapshot.has_value());
    ASSERT_TRUE(second.snapshot.has_value());
    ASSERT_TRUE(fs::exists(path));

    ASSERT_TRUE(ssg::writeSessionSnapshot(path, {}).accepted());
    const auto cleared = ssg::readSessionSnapshot(path);
    ASSERT_TRUE(cleared.accepted());
    ASSERT_TRUE(cleared.snapshot.has_value());
    if (cleared.snapshot) ASSERT_TRUE(cleared.snapshot->tabs.empty());
    fs::remove_all(root);
}

TEST(cleanEditorSaveReplacesStaleSnapshotWithEmptyState) {
    const auto root = testRuntimePath("session_snapshot_clean_exit");
    fs::remove_all(root);
    auto config = editorConfig(root);
    auto created = ssg::createEditor(config);
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    ASSERT_TRUE(ssg::writeSessionSnapshot(config.snapshotPath, exampleSnapshot())
                    .accepted());
    ASSERT_TRUE(created.session->saveSession().accepted);
    const auto cleared = ssg::readSessionSnapshot(config.snapshotPath);
    ASSERT_TRUE(cleared.accepted());
    ASSERT_TRUE(cleared.snapshot.has_value());
    if (cleared.snapshot) ASSERT_TRUE(cleared.snapshot->tabs.empty());
    fs::remove_all(root);
}

TEST(pathDerivationUsesOnlyStartingDirectory) {
    const fs::path starting{"/start"};
    ASSERT_EQ(ssg::sessionSnapshotPath(starting),
              starting / ssg::kSessionDirectoryName /
                  ssg::kSessionSnapshotFilename);
    ASSERT_TRUE(ssg::sessionSnapshotPath({}).empty());
}

TEST(twoEditorLifetimesRestoreAnUntitledDraft) {
    const auto root = testRuntimePath("session_snapshot_untitled_restart");
    fs::remove_all(root);
    {
        auto created = ssg::createEditor(editorConfig(root));
        ASSERT_TRUE(created.accepted());
        if (!created.accepted()) return;
        ASSERT_TRUE(created.session->dispatch("file.new").accepted());
        ASSERT_TRUE(ssg::test::typeText(*created.session, "remember me").accepted());
        ASSERT_TRUE(created.session->saveSession().accepted);
    }
    {
        auto recreated = ssg::createEditor(editorConfig(root));
        ASSERT_TRUE(recreated.accepted());
        if (!recreated.accepted()) return;
        ASSERT_EQ(recreated.session->tabs.viewState().tabs.size(), std::size_t{1});
        ASSERT_EQ(recreated.session->activeDocument()->snapshot().text,
                  std::string{"remember me"});
        ASSERT_EQ(recreated.session->activeWorkspaceState()->key.kind(),
                  ssg::DocumentKeyKind::Untitled);
        ASSERT_TRUE(recreated.session->activeWorkspaceState()->dirty);
    }
    fs::remove_all(root);
}

TEST(savedDraftRestoresFromItsExactRawBaseline) {
    const auto root = testRuntimePath("session_snapshot_saved_restart");
    fs::remove_all(root);
    writeBytes(root / "workspace" / "saved.txt",
               "\xef\xbb\xbf" "base\r\n");
    {
        auto created = ssg::createEditor(editorConfig(root));
        ASSERT_TRUE(created.accepted());
        if (!created.accepted()) return;
        ASSERT_TRUE(ssg::test::openFile(*created.session, "saved.txt").accepted());
        ASSERT_TRUE(ssg::test::typeText(*created.session, "draft ").accepted());
        ASSERT_TRUE(created.session->saveSession().accepted);
    }
    {
        const auto stored =
            ssg::readSessionSnapshot(editorConfig(root).snapshotPath);
        ASSERT_TRUE(stored.accepted());
        ASSERT_TRUE(stored.snapshot.has_value());
        if (stored.snapshot && !stored.snapshot->tabs.empty()) {
            ASSERT_EQ(stored.snapshot->tabs.front().baseline,
                      (std::vector<std::uint8_t>{
                          0xef, 0xbb, 0xbf, 'b', 'a', 's', 'e', '\r', '\n'}));
        }

        auto recreated = ssg::createEditor(editorConfig(root));
        ASSERT_TRUE(recreated.accepted());
        if (!recreated.accepted()) return;
        ASSERT_EQ(recreated.session->activeDocument()->snapshot().text,
                  std::string{"draft base\n"});
        ASSERT_TRUE(recreated.session->activeWorkspaceState()->dirty);
        ASSERT_EQ(readBytes(root / "workspace" / "saved.txt"),
                  std::string{"\xef\xbb\xbf" "base\r\n"});
    }
    fs::remove_all(root);
}

TEST(snapshotKeepsOnlyDirtyEditableTabsInOrderAndActiveIdentity) {
    const auto root = testRuntimePath("session_snapshot_dirty_tabs");
    fs::remove_all(root);
    writeBytes(root / "workspace" / "clean.txt", "clean");
    auto created = ssg::createEditor(editorConfig(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& editor = *created.session;

    ASSERT_TRUE(editor.dispatch("file.new").accepted());
    ASSERT_TRUE(ssg::test::typeText(editor, "first").accepted());
    const auto first = editor.tabs.viewState().active;
    ASSERT_TRUE(editor.dispatch("file.new").accepted());
    ASSERT_TRUE(ssg::test::typeText(editor, "second").accepted());
    ASSERT_TRUE(ssg::test::openFile(editor, "clean.txt").accepted());
    ASSERT_TRUE(editor.openReadOnlyTab(
                           ssg::TabKind::ReadOnlyOutput, "generated", "output",
                           "regenerable", ssg::LanguageId::plainText())
                    .accepted);
    ASSERT_TRUE(first.has_value());
    if (first) ASSERT_TRUE(ssg::activateTab(editor, *first).accepted);

    ASSERT_TRUE(editor.saveSession().accepted);
    const auto stored =
        ssg::readSessionSnapshot(editorConfig(root).snapshotPath);
    ASSERT_TRUE(stored.accepted());
    ASSERT_TRUE(stored.snapshot.has_value());
    if (stored.snapshot) {
        ASSERT_EQ(stored.snapshot->tabs.size(), std::size_t{2});
        ASSERT_EQ(stored.snapshot->tabs[0].draft, std::string{"first"});
        ASSERT_EQ(stored.snapshot->tabs[1].draft, std::string{"second"});
        ASSERT_TRUE(stored.snapshot->tabs[0].active);
        ASSERT_FALSE(stored.snapshot->tabs[1].active);
    }
    created.session.reset();

    auto recreated = ssg::createEditor(editorConfig(root));
    ASSERT_TRUE(recreated.accepted());
    if (!recreated.accepted()) return;
    const auto& tabs = recreated.session->tabs.viewState();
    ASSERT_EQ(tabs.tabs.size(), std::size_t{2});
    ASSERT_EQ(recreated.session->activeDocument()->snapshot().text,
              std::string{"first"});
    ASSERT_EQ(recreated.session->workspace.document(*tabs.tabs[0].document)
                  .snapshot()
                  .text,
              std::string{"first"});
    ASSERT_EQ(recreated.session->workspace.document(*tabs.tabs[1].document)
                  .snapshot()
                  .text,
              std::string{"second"});
    fs::remove_all(root);
}

TEST(savedFileReconciliationCoversTheCompleteMatrix) {
    const auto root = testRuntimePath("session_snapshot_matrix");
    fs::remove_all(root);
    const auto workspace = root / "workspace";
    fs::create_directories(workspace / "nonregular.txt");
    writeBytes(workspace / "equal.txt", "same\r\nline\n");
    writeBytes(workspace / "baseline.txt", "base");
    writeBytes(workspace / "appeared.txt", "appeared");
    writeBytes(workspace / "changed.txt", "new");
    writeBytes(workspace / "binary.txt", std::string{"a\0b", 3});
    writeBytes(workspace / "undecodable.txt", std::string{"\xff", 1});
    writeBytes(workspace / "unreadable.txt", "unreadable");
    std::error_code permissionsError;
    fs::permissions(workspace / "unreadable.txt", fs::perms::none,
                    fs::perm_options::replace, permissionsError);
    const bool unreadable =
        !permissionsError &&
        !ssg::readFile(workspace / "unreadable.txt").ok();

    ssg::SessionSnapshot snapshot{{
        {ssg::SessionBackingKind::PersistedPath, "equal.txt", "equal.txt",
         ssg::DocumentMode::Edit, "same\nline\n", {'o', 'l', 'd'}, false},
        {ssg::SessionBackingKind::PersistedPath, "baseline.txt", "baseline.txt",
         ssg::DocumentMode::Edit, "draft", {'b', 'a', 's', 'e'}, false},
        {ssg::SessionBackingKind::PersistedPath, "missing.txt", "missing.txt",
         ssg::DocumentMode::Edit, "missing draft", {'o', 'l', 'd'}, false},
        {ssg::SessionBackingKind::NeverCreatedPath, "appeared.txt",
         "appeared.txt", ssg::DocumentMode::Edit, "new draft", {}, false},
        {ssg::SessionBackingKind::PersistedPath, "changed.txt", "changed.txt",
         ssg::DocumentMode::Edit, "old draft", {'o', 'l', 'd'}, true},
        {ssg::SessionBackingKind::PersistedPath, "binary.txt", "binary.txt",
         ssg::DocumentMode::Edit, "binary draft", {'o', 'l', 'd'}, false},
        {ssg::SessionBackingKind::PersistedPath, "undecodable.txt",
         "undecodable.txt", ssg::DocumentMode::Edit, "decode draft",
         {'o', 'l', 'd'}, false},
        {ssg::SessionBackingKind::PersistedPath, "nonregular.txt",
         "nonregular.txt", ssg::DocumentMode::Edit, "directory draft",
         {'o', 'l', 'd'}, false},
    }};
    if (unreadable) {
        snapshot.tabs.push_back(
            {ssg::SessionBackingKind::PersistedPath, "unreadable.txt",
             "unreadable.txt", ssg::DocumentMode::Edit, "unreadable draft",
             {'o', 'l', 'd'}, false});
    }
    const auto config = editorConfig(root);
    ASSERT_TRUE(ssg::writeSessionSnapshot(config.snapshotPath, snapshot).accepted());
    const auto changedBefore = readBytes(workspace / "changed.txt");
    const auto binaryBefore = readBytes(workspace / "binary.txt");
    const auto undecodableBefore = readBytes(workspace / "undecodable.txt");

    auto created = ssg::createEditor(config);
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& editor = *created.session;
    ASSERT_EQ(editor.tabs.viewState().tabs.size(),
              std::size_t{8} + (unreadable ? 1U : 0U));
    ASSERT_EQ(editor.workspace.document(
                  *editor.tabs.viewState().tabs[0].document).snapshot().text,
              std::string{"same\nline\n"});
    ASSERT_FALSE(editor.workspace.state(
                    *editor.tabs.viewState().tabs[0].document)->dirty);
    ASSERT_EQ(editor.workspace.document(
                  *editor.tabs.viewState().tabs[1].document).snapshot().text,
              std::string{"draft"});
    ASSERT_TRUE(editor.workspace.state(
                   *editor.tabs.viewState().tabs[1].document)->dirty);
    ASSERT_EQ(editor.workspace.state(
                  *editor.tabs.viewState().tabs[2].document)->key.savedPath(),
              std::string{"missing.txt"});
    ASSERT_TRUE(editor.workspace.state(
                   *editor.tabs.viewState().tabs[2].document)->dirty);

    for (std::size_t index = 3; index < snapshot.tabs.size(); ++index) {
        const auto state =
            editor.workspace.state(*editor.tabs.viewState().tabs[index].document);
        ASSERT_EQ(state->key.kind(), ssg::DocumentKeyKind::Untitled);
        ASSERT_TRUE(state->displayLabel.find("(recovered)") != std::string::npos);
    }
    ASSERT_EQ(editor.activeWorkspaceState()->key.kind(),
              ssg::DocumentKeyKind::Untitled);
    ASSERT_TRUE(editor.activeWorkspaceState()->displayLabel.find("changed.txt") !=
                std::string::npos);
    ASSERT_TRUE(editor.statusText.find(
                    unreadable ? "6 session drafts" : "5 session drafts") !=
                std::string::npos);
    ASSERT_EQ(readBytes(workspace / "changed.txt"), changedBefore);
    ASSERT_EQ(readBytes(workspace / "binary.txt"), binaryBefore);
    ASSERT_EQ(readBytes(workspace / "undecodable.txt"), undecodableBefore);
    ASSERT_TRUE(fs::is_directory(workspace / "nonregular.txt"));
    if (!permissionsError) {
        fs::permissions(workspace / "unreadable.txt", fs::perms::owner_all,
                        fs::perm_options::replace, permissionsError);
    }
    fs::remove_all(root);
}

TEST(startupTargetFocusesRestoredPathWithoutDuplicatingIt) {
    const auto root = testRuntimePath("session_snapshot_startup_target");
    fs::remove_all(root);
    const auto config = editorConfig(root);
    const ssg::SessionSnapshot snapshot{{{
        ssg::SessionBackingKind::NeverCreatedPath, "new.txt", "new.txt",
        ssg::DocumentMode::Edit, "draft", {}, false,
    }}};
    ASSERT_TRUE(ssg::writeSessionSnapshot(config.snapshotPath, snapshot).accepted());
    auto created = ssg::createEditor(config);
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    ASSERT_TRUE(ssg::openStartupTarget(*created.session, "new.txt").accepted);
    ASSERT_EQ(created.session->tabs.viewState().tabs.size(), std::size_t{1});
    ASSERT_EQ(created.session->activeDocument()->snapshot().text,
              std::string{"draft"});
    fs::remove_all(root);
}

TEST(corruptSnapshotAndNonDirectorySessionPathStopStartup) {
    auto root = testRuntimePath("session_snapshot_corrupt");
    fs::remove_all(root);
    auto config = editorConfig(root);
    writeBytes(config.snapshotPath, "not a snapshot");
    auto corrupt = ssg::createEditor(config);
    ASSERT_FALSE(corrupt.accepted());
    ASSERT_TRUE(corrupt.message.find(config.snapshotPath.string()) !=
                std::string::npos);
    ASSERT_TRUE(corrupt.message.find("move or delete") != std::string::npos);
    ASSERT_EQ(readBytes(config.snapshotPath), std::string{"not a snapshot"});

    root = testRuntimePath("session_snapshot_nondirectory");
    fs::remove_all(root);
    config = editorConfig(root);
    fs::remove_all(root / "ssg");
    writeBytes(root / "ssg", "not a directory");
    auto blocked = ssg::createEditor(config);
    ASSERT_FALSE(blocked.accepted());
    ASSERT_TRUE(blocked.message.find(config.snapshotPath.string()) !=
                std::string::npos);
    ASSERT_TRUE(blocked.message.find("move or delete") != std::string::npos);
    fs::remove_all(root);
}

TEST(saveFailureIsReportedAndStartingDirectoryOwnsTheSnapshot) {
    const auto root = testRuntimePath("session_snapshot_write_failure");
    fs::remove_all(root);
    auto config = editorConfig(root);
    auto created = ssg::createEditor(config);
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    ASSERT_TRUE(created.session->dispatch("file.new").accepted());
    ASSERT_TRUE(ssg::test::typeText(*created.session, "draft").accepted());
    fs::create_directories(config.snapshotPath.parent_path());
    fs::remove_all(config.snapshotPath.parent_path());
    writeBytes(config.snapshotPath.parent_path(), "blocked");
    const auto writeResult = created.session->saveSession();
    ASSERT_FALSE(writeResult.accepted);
    ASSERT_TRUE(writeResult.message.find(config.snapshotPath.string()) !=
                std::string::npos);

    fs::remove_all(root);
    const auto start = root / "start";
    const auto openedWorkspace = root / "elsewhere" / "workspace";
    fs::create_directories(openedWorkspace);
    config.cwd = openedWorkspace;
    config.recoveryRoot = root / "recovery";
    config.archiveRoot = root / "archive";
    config.snapshotPath = ssg::sessionSnapshotPath(start);
    created = ssg::createEditor(config);
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    ASSERT_TRUE(created.session->dispatch("file.new").accepted());
    ASSERT_TRUE(ssg::test::typeText(*created.session, "cwd draft").accepted());
    ASSERT_TRUE(created.session->saveSession().accepted);
    ASSERT_TRUE(fs::exists(start / "ssg" / "session.snapshot"));
    ASSERT_FALSE(fs::exists(openedWorkspace / "ssg" / "session.snapshot"));
    fs::remove_all(root);
}

} // namespace

SSG_TEST_SUITE(test_session_snapshot) {
    RUN(codecRoundTripsAllBackingKinds);
    RUN(codecAcceptsCanonicalEmptyFixture);
    RUN(codecRejectsEveryTruncatedPrefix);
    RUN(codecRejectsWrongMagicVersionAndTrailingBytes);
    RUN(codecRejectsInvalidLengthsFlagsAndDuplicateActiveTabs);
    RUN(codecRejectsInvalidUtf8AndBackingCombinations);
    RUN(fileReadDoesNotConsumeAndEmptyWriteClearsStaleState);
    RUN(cleanEditorSaveReplacesStaleSnapshotWithEmptyState);
    RUN(pathDerivationUsesOnlyStartingDirectory);
    RUN(twoEditorLifetimesRestoreAnUntitledDraft);
    RUN(savedDraftRestoresFromItsExactRawBaseline);
    RUN(snapshotKeepsOnlyDirtyEditableTabsInOrderAndActiveIdentity);
    RUN(savedFileReconciliationCoversTheCompleteMatrix);
    RUN(startupTargetFocusesRestoredPathWithoutDuplicatingIt);
    RUN(corruptSnapshotAndNonDirectorySessionPathStopStartup);
    RUN(saveFailureIsReportedAndStartingDirectoryOwnsTheSnapshot);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
