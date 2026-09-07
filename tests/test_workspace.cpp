#include "test_helpers.h"

#include <ssg/Workspace.h>
#include <ssg/ScratchJournal.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("ssg-workspace-" + std::to_string(
                                        std::chrono::steady_clock::now()
                                            .time_since_epoch()
                                            .count()));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void writeBytes(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string readBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}};
}

TEST(openIsByteExactAndPreventsNormalizedDuplicates) {
    TemporaryDirectory temporary;
    writeBytes(temporary.path() / "a.txt", "\xef\xbb\xbfone\r\ntwo\r");
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);

    const auto first = workspace.openFile("a.txt");
    const auto duplicate = workspace.openFile("./a.txt");

    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(duplicate.accepted());
    ASSERT_EQ(*first.document, *duplicate.document);
    ASSERT_EQ(workspace.documents().size(), std::size_t{1});
    ASSERT_EQ(workspace.document(*first.document).snapshot().text,
              std::string{"one\ntwo\n"});
    ASSERT_TRUE(workspace.save(*first.document).accepted());
    ASSERT_EQ(readBytes(temporary.path() / "a.txt"),
              std::string{"\xef\xbb\xbfone\r\ntwo\r"});
}

TEST(pathsCannotEscapeWorkspaceBeforeMutation) {
    TemporaryDirectory temporary;
    TemporaryDirectory outside;
    writeBytes(outside.path() / "secret.txt", "secret");
    std::error_code symlinkError;
    std::filesystem::create_directory_symlink(
        outside.path(), temporary.path() / "escape", symlinkError);
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);

    ASSERT_EQ(workspace.openFile("../secret.txt").error,
              ssg::WorkspaceError::InvalidPath);
    ASSERT_EQ(workspace.openFile(outside.path().string()).error,
              ssg::WorkspaceError::InvalidPath);
    if (!symlinkError) {
        ASSERT_EQ(workspace.openFile("escape/secret.txt").error,
                  ssg::WorkspaceError::PathOutsideWorkspace);
    }
    ASSERT_EQ(workspace.documents().size(), std::size_t{0});
}

TEST(untitledIdentityChangesOnlyAfterSuccessfulSave) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    const auto created = workspace.newDocument();
    const auto id = *created.document;
    const auto before = workspace.state(id);
    ASSERT_EQ(before->key.kind(), ssg::JournalDocumentKeyKind::Untitled);
    // A brand-new untitled buffer holds nothing to lose, so it is not unsaved.
    ASSERT_FALSE(before->dirty);
    // Type something: now there IS something a failed save would lose, which is
    // the case worth asserting about below.
    ASSERT_TRUE(workspace
                    .apply(id, {workspace.document(id).revision(),
                                {{ssg::ByteOffset{0}, 0, "content"}}})
                    .accepted());
    ASSERT_TRUE(workspace.state(id)->dirty);

    std::filesystem::create_directories(temporary.path() / "blocked");
    const auto failedSave = workspace.saveAs(id, "blocked");
    ASSERT_FALSE(failedSave.accepted());
    const auto afterFailure = workspace.state(id);
    ASSERT_EQ(afterFailure->key, before->key);
    ASSERT_TRUE(afterFailure->dirty);

    const auto saved = workspace.saveAs(id, "named.txt");
    ASSERT_TRUE(saved.accepted());
    const auto afterSave = workspace.state(id);
    ASSERT_EQ(afterSave->key.kind(), ssg::JournalDocumentKeyKind::Saved);
    ASSERT_EQ(afterSave->key.savedPath(), std::string{"named.txt"});
    ASSERT_FALSE(afterSave->dirty);
}

TEST(newFileClaimsANameAndStaysUnsavedUntilItIsWritten) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);

    const auto created = workspace.newFile("fresh.txt");
    ASSERT_TRUE(created.accepted());
    const auto id = *created.document;
    const auto before = workspace.state(id);
    ASSERT_EQ(before->key.kind(), ssg::JournalDocumentKeyKind::Saved);
    ASSERT_EQ(before->key.savedPath(), std::string{"fresh.txt"});
    // Empty, but nothing is on disk yet, so the file only exists if it is saved.
    ASSERT_TRUE(before->dirty);
    ASSERT_FALSE(std::filesystem::exists(temporary.path() / "fresh.txt"));

    ASSERT_TRUE(workspace
                    .apply(id, {workspace.document(id).revision(),
                                {{ssg::ByteOffset{0}, 0, "typed"}}})
                    .accepted());
    ASSERT_TRUE(workspace.save(id).accepted());
    ASSERT_FALSE(workspace.state(id)->dirty);
    ASSERT_EQ(readBytes(temporary.path() / "fresh.txt"), std::string{"typed"});
}

TEST(newFileRefusesANameThatIsAlreadyTaken) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    writeBytes(temporary.path() / "taken.txt", "on disk");
    ASSERT_FALSE(workspace.newFile("taken.txt").accepted());
    ASSERT_FALSE(workspace.newFile("../outside.txt").accepted());
}

// The claimed name may be taken by something else between launch and the first
// save, and that file must survive.
TEST(firstSaveOfANewFileDoesNotClobberAFileCreatedMeanwhile) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    const auto created = workspace.newFile("racy.txt");
    ASSERT_TRUE(created.accepted());
    writeBytes(temporary.path() / "racy.txt", "someone else");
    ASSERT_FALSE(workspace.save(*created.document).accepted());
    ASSERT_EQ(readBytes(temporary.path() / "racy.txt"),
              std::string{"someone else"});
}

TEST(recentFilesAreBoundedMruAndDropMissingEntries) {    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    for (int index = 0; index < 34; ++index) {
        const auto name = std::to_string(index) + ".txt";
        writeBytes(temporary.path() / name, name);
        ASSERT_TRUE(workspace.openFile(name).accepted());
    }
    const auto recent = workspace.recentFiles();
    ASSERT_EQ(recent.size(), std::size_t{32});
    ASSERT_EQ(recent.front(), std::string{"33.txt"});
    ASSERT_EQ(recent.back(), std::string{"2.txt"});

    std::filesystem::remove(temporary.path() / "33.txt");
    ASSERT_EQ(workspace.openRecent(0).error, ssg::WorkspaceError::NotFound);
    const auto afterMissing = workspace.recentFiles();
    ASSERT_EQ(afterMissing.front(), std::string{"32.txt"});
}

TEST(renameDeleteAreCompensatableAndOpenDirectoryClosesWorkspace) {
    TemporaryDirectory first;
    TemporaryDirectory second;
    writeBytes(first.path() / "old.txt", "old");
    writeBytes(second.path() / "other.txt", "other");
    auto recovery = ssg::RecoveryManager::create(first.path() / ".recovery");
    auto workspace = ssg::Workspace::create(first.path(), recovery);
    const auto opened = workspace.openFile("old.txt");

    const auto renamed =
        workspace.renameFile(*opened.document, "renamed.txt");
    ASSERT_TRUE(renamed.accepted());
    ASSERT_TRUE(renamed.compensation.has_value());
    ASSERT_TRUE(workspace.restore(*renamed.compensation).accepted());
    ASSERT_TRUE(std::filesystem::exists(first.path() / "old.txt"));
    const auto snapshot = workspace.document(*opened.document).snapshot();
    ASSERT_TRUE(workspace
                    .apply(*opened.document,
                           {snapshot.revision,
                            {{ssg::ByteOffset{0}, snapshot.text.size(),
                              "unsaved"}}})
                    .accepted());

    const auto removed = workspace.deleteFile(*opened.document);
    ASSERT_TRUE(removed.accepted());
    const auto restored = workspace.restore(*removed.compensation);
    ASSERT_TRUE(restored.accepted());
    ASSERT_EQ(restored.document, opened.document);
    ASSERT_EQ(readBytes(first.path() / "old.txt"), std::string{"old"});
    ASSERT_EQ(workspace.document(*opened.document).snapshot().text,
              std::string{"old"});

    const auto openedDirectory = workspace.openDirectory(second.path());
    ASSERT_TRUE(openedDirectory.accepted());
    ASSERT_EQ(workspace.root(), std::filesystem::canonical(second.path()));
    ASSERT_TRUE(workspace.documents().empty());
}

TEST(evictedCompensationCannotRestore) {
    TemporaryDirectory temporary;
    writeBytes(temporary.path() / "first.txt", "first");
    writeBytes(temporary.path() / "second.txt", "second");
    auto recovery = ssg::RecoveryManager::create(
        temporary.path() / ".recovery", ssg::RecoveryConfig{1, 1024 * 1024});
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    const auto first = workspace.openFile("first.txt");
    const auto second = workspace.openFile("second.txt");

    const auto firstRename =
        workspace.renameFile(*first.document, "first-renamed.txt");
    ASSERT_TRUE(firstRename.accepted());
    const auto secondRename =
        workspace.renameFile(*second.document, "second-renamed.txt");
    ASSERT_TRUE(secondRename.accepted());

    ASSERT_FALSE(workspace.restore(*firstRename.compensation).accepted());
    ASSERT_TRUE(workspace.restore(*secondRename.compensation).accepted());
}

TEST(saveAndReloadDoNotProduceCompensations) {
    TemporaryDirectory temporary;
    writeBytes(temporary.path() / "file.txt", "disk");
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    const auto id = *workspace.openFile("file.txt").document;
    ASSERT_TRUE(workspace
                    .apply(id, {workspace.document(id).revision(),
                                {{ssg::ByteOffset{4}, 0, "-edited"}}})
                    .accepted());

    const auto saved = workspace.save(id);
    ASSERT_TRUE(saved.accepted());
    ASSERT_EQ(readBytes(temporary.path() / "file.txt"),
              std::string{"disk-edited"});
    ASSERT_FALSE(saved.compensation.has_value());

    writeBytes(temporary.path() / "file.txt", "external");
    const auto reloaded = workspace.reload(id);
    ASSERT_TRUE(reloaded.accepted());
    ASSERT_EQ(workspace.document(id).snapshot().text, std::string{"external"});
    ASSERT_FALSE(reloaded.compensation.has_value());
}

TEST(newDirectoryRejectsEscapeAndCreatesOnlyInsideRoot) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    ASSERT_TRUE(workspace.newDirectory("inside").accepted());
    ASSERT_TRUE(std::filesystem::is_directory(temporary.path() / "inside"));
    ASSERT_EQ(workspace.newDirectory("../outside").error,
              ssg::WorkspaceError::InvalidPath);
}

TEST(emptyAndMixedEndingEditsSaveWithExactMetadata) {
    TemporaryDirectory temporary;
    writeBytes(temporary.path() / "empty.txt", "disk");
    writeBytes(temporary.path() / "mixed.txt", "a\r\nb\nc");
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);

    const auto empty = *workspace.openFile("empty.txt").document;
    ASSERT_TRUE(workspace
                    .apply(empty, {workspace.document(empty).revision(),
                                   {{ssg::ByteOffset{0}, 4, ""}}})
                    .accepted());
    ASSERT_TRUE(workspace.save(empty).accepted());
    ASSERT_EQ(readBytes(temporary.path() / "empty.txt"), std::string{});

    const auto mixed = *workspace.openFile("mixed.txt").document;
    ASSERT_TRUE(workspace
                    .apply(mixed, {workspace.document(mixed).revision(),
                                   {{ssg::ByteOffset{0}, 0, "X\n"}}})
                    .accepted());
    ASSERT_TRUE(workspace.save(mixed).accepted());
    ASSERT_EQ(readBytes(temporary.path() / "mixed.txt"),
              std::string{"X\na\r\nb\nc"});
}

TEST(tryDocumentReturnsNullForAbsentId) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    ASSERT_EQ(workspace.tryDocument(ssg::FileDocumentId{777}), nullptr);

    const auto created = workspace.newDocument();
    ASSERT_TRUE(created.accepted());
    ASSERT_TRUE(created.document.has_value());
    ASSERT_TRUE(workspace.tryDocument(*created.document) != nullptr);
    ASSERT_EQ(workspace.tryDocument(ssg::FileDocumentId{778}), nullptr);
}

TEST(removeDocumentErasesOnlyInMemoryState) {
    TemporaryDirectory temporary;
    writeBytes(temporary.path() / "keep.txt", "keep me");
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    const auto opened = workspace.openFile("keep.txt");
    ASSERT_TRUE(opened.accepted());
    ASSERT_TRUE(opened.document.has_value());

    const auto removed = workspace.removeDocument(*opened.document);
    ASSERT_TRUE(removed.accepted());
    ASSERT_EQ(workspace.state(*opened.document), std::nullopt);
    ASSERT_EQ(workspace.tryDocument(*opened.document), nullptr);
    ASSERT_TRUE(std::filesystem::exists(temporary.path() / "keep.txt"));
    ASSERT_EQ(readBytes(temporary.path() / "keep.txt"), std::string{"keep me"});
}

TEST(openCapturesDiskBaselineAndUntitledHasNone) {
    TemporaryDirectory temporary;
    writeBytes(temporary.path() / "note.txt", "hello world\n");
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);

    const auto opened = workspace.openFile("note.txt");
    ASSERT_TRUE(opened.accepted());
    const auto baseline = workspace.baselineFor(*opened.document);
    ASSERT_TRUE(baseline.has_value());
    // The baseline hashes the RAW disk bytes and records the disk size, so it
    // reflects what the edits branch from, not the decoded buffer.
    ASSERT_EQ(baseline->size, std::uint64_t{12});
    ASSERT_EQ(baseline->contentHash, ssg::fastContentHash("hello world\n"));
    ASSERT_TRUE(baseline->mtimeNanos != 0);

    // An untitled buffer has no disk file, so no baseline.
    const auto untitled = workspace.newDocument("scratch");
    ASSERT_TRUE(untitled.accepted());
    ASSERT_FALSE(workspace.baselineFor(*untitled.document).has_value());
}

TEST(saveAsCapturesBaselineForWrittenBytes) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);

    // A fresh untitled buffer starts with no baseline; saving it to disk
    // captures a baseline for the bytes that were written.
    const auto untitled = workspace.openVirtualDocument(
        "draft", "fresh\n", ssg::DocumentMode::Edit);
    ASSERT_TRUE(untitled.accepted());
    ASSERT_FALSE(workspace.baselineFor(*untitled.document).has_value());

    const auto saved = workspace.saveAs(*untitled.document, "draft.txt");
    ASSERT_TRUE(saved.accepted());
    const auto baseline = workspace.baselineFor(*untitled.document);
    ASSERT_TRUE(baseline.has_value());
    ASSERT_EQ(baseline->contentHash,
              ssg::fastContentHash(readBytes(temporary.path() / "draft.txt")));
    ASSERT_EQ(baseline->size,
              readBytes(temporary.path() / "draft.txt").size());
}

TEST(reloadRefreshesBaselineFromDisk) {
    TemporaryDirectory temporary;
    writeBytes(temporary.path() / "live.txt", "first\n");
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);

    const auto opened = workspace.openFile("live.txt");
    ASSERT_TRUE(opened.accepted());
    const auto openedBaseline = workspace.baselineFor(*opened.document);
    ASSERT_TRUE(openedBaseline.has_value());
    if (openedBaseline) {
        ASSERT_EQ(openedBaseline->contentHash, ssg::fastContentHash("first\n"));
    }

    // Something external rewrites the file; reload re-reads disk, so the baseline
    // now reflects the new disk content (the authority is disk, not the buffer).
    writeBytes(temporary.path() / "live.txt", "second changed\n");
    ASSERT_TRUE(workspace.reload(*opened.document).accepted());
    const auto baseline = workspace.baselineFor(*opened.document);
    ASSERT_TRUE(baseline.has_value());
    ASSERT_EQ(baseline->contentHash, ssg::fastContentHash("second changed\n"));
    ASSERT_EQ(baseline->size, std::uint64_t{15});
}

}  // namespace

SSG_TEST_SUITE(test_workspace) {
    RUN(openIsByteExactAndPreventsNormalizedDuplicates);
    RUN(pathsCannotEscapeWorkspaceBeforeMutation);
    RUN(untitledIdentityChangesOnlyAfterSuccessfulSave);
    RUN(recentFilesAreBoundedMruAndDropMissingEntries);
    RUN(renameDeleteAreCompensatableAndOpenDirectoryClosesWorkspace);
    RUN(evictedCompensationCannotRestore);
    RUN(saveAndReloadDoNotProduceCompensations);
    RUN(newDirectoryRejectsEscapeAndCreatesOnlyInsideRoot);
    RUN(emptyAndMixedEndingEditsSaveWithExactMetadata);
    RUN(tryDocumentReturnsNullForAbsentId);
    RUN(removeDocumentErasesOnlyInMemoryState);
    RUN(openCapturesDiskBaselineAndUntitledHasNone);
    RUN(saveAsCapturesBaselineForWrittenBytes);
    RUN(reloadRefreshesBaselineFromDisk);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
