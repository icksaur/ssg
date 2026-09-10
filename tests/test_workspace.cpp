#include "test_helpers.h"

#include <ssg/Workspace.h>

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
    ASSERT_EQ(before->key.kind(), ssg::DocumentKeyKind::Untitled);
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
    ASSERT_EQ(afterSave->key.kind(), ssg::DocumentKeyKind::Saved);
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
    ASSERT_EQ(before->key.kind(), ssg::DocumentKeyKind::Saved);
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

TEST(restoredDocumentsKeepTheirBackingAndDraftState) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);

    const auto untitled = workspace.restoreUntitled(
        "notes", "untitled draft", ssg::DocumentMode::Edit);
    ASSERT_TRUE(untitled.accepted());
    ASSERT_EQ(workspace.state(*untitled.document)->key.kind(),
              ssg::DocumentKeyKind::Untitled);
    ASSERT_TRUE(workspace.state(*untitled.document)->dirty);

    const auto pathBound = workspace.restorePathBound(
        "new.txt", "custom label", "new draft", ssg::DocumentMode::Edit);
    ASSERT_TRUE(pathBound.accepted());
    ASSERT_EQ(workspace.state(*pathBound.document)->key.savedPath(),
              std::string{"new.txt"});
    ASSERT_EQ(workspace.state(*pathBound.document)->displayLabel,
              std::string{"custom label"});
    ASSERT_TRUE(workspace.state(*pathBound.document)->dirty);
    ASSERT_FALSE(workspace.persistenceState(*pathBound.document)->persisted);

    const std::vector<std::uint8_t> baseline{'o', 'l', 'd', '\r', '\n'};
    const auto persisted = workspace.restorePersisted(
        "saved.txt", "saved label", baseline, "draft\n",
        ssg::DocumentMode::Edit);
    ASSERT_TRUE(persisted.accepted());
    ASSERT_EQ(workspace.document(*persisted.document).snapshot().text,
              std::string{"draft\n"});
    ASSERT_TRUE(workspace.state(*persisted.document)->dirty);
    ASSERT_TRUE(workspace.persistenceState(*persisted.document)->persisted);
    ASSERT_EQ(workspace.persistenceState(*persisted.document)->baseline,
              baseline);

    const auto recovered = workspace.restoreUntitled(
        "dir/original.txt (recovered)", "recovered draft",
        ssg::DocumentMode::Edit);
    ASSERT_TRUE(recovered.accepted());
    ASSERT_EQ(workspace.state(*recovered.document)->displayLabel,
              std::string{"dir/original.txt (recovered)"});
    ASSERT_EQ(workspace.state(*recovered.document)->key.kind(),
              ssg::DocumentKeyKind::Untitled);
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

TEST(renameDeleteAndOpenDirectoryUpdateWorkspaceState) {
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
    ASSERT_FALSE(std::filesystem::exists(first.path() / "old.txt"));
    ASSERT_TRUE(std::filesystem::exists(first.path() / "renamed.txt"));

    const auto removed = workspace.deleteFile(*opened.document);
    ASSERT_TRUE(removed.accepted());
    ASSERT_FALSE(std::filesystem::exists(first.path() / "renamed.txt"));
    ASSERT_TRUE(workspace.documents().empty());

    const auto openedDirectory = workspace.openDirectory(second.path());
    ASSERT_TRUE(openedDirectory.accepted());
    ASSERT_EQ(workspace.root(), std::filesystem::canonical(second.path()));
}

TEST(saveAndReloadUpdateDiskAndDocument) {
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

    ASSERT_TRUE(workspace.save(id).accepted());
    ASSERT_EQ(readBytes(temporary.path() / "file.txt"),
              std::string{"disk-edited"});

    writeBytes(temporary.path() / "file.txt", "external");
    ASSERT_TRUE(workspace.reload(id).accepted());
    ASSERT_EQ(workspace.document(id).snapshot().text, std::string{"external"});
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

TEST(documentKeysPreserveSavedAndUntitledIdentity) {
    const auto first = ssg::UntitledDocumentId::generate();
    const auto second = ssg::UntitledDocumentId::generate();
    ASSERT_NE(first, second);
    ASSERT_EQ(ssg::DocumentKey::untitled(first).untitledId(), first);
    ASSERT_EQ(ssg::DocumentKey::saved("src/file.cpp").savedPath(),
              std::string{"src/file.cpp"});
    ASSERT_THROWS(ssg::DocumentKey::saved("../escape"),
                  std::invalid_argument);
    ASSERT_THROWS(ssg::DocumentKey::saved(""), std::invalid_argument);
    ASSERT_THROWS(ssg::DocumentKey::saved("file").untitledId(),
                  std::logic_error);
}

}  // namespace

SSG_TEST_SUITE(test_workspace) {
    RUN(openIsByteExactAndPreventsNormalizedDuplicates);
    RUN(pathsCannotEscapeWorkspaceBeforeMutation);
    RUN(untitledIdentityChangesOnlyAfterSuccessfulSave);
    RUN(restoredDocumentsKeepTheirBackingAndDraftState);
    RUN(renameDeleteAndOpenDirectoryUpdateWorkspaceState);
    RUN(saveAndReloadUpdateDiskAndDocument);
    RUN(newDirectoryRejectsEscapeAndCreatesOnlyInsideRoot);
    RUN(emptyAndMixedEndingEditsSaveWithExactMetadata);
    RUN(tryDocumentReturnsNullForAbsentId);
    RUN(removeDocumentErasesOnlyInMemoryState);
    RUN(documentKeysPreserveSavedAndUntitledIdentity);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
