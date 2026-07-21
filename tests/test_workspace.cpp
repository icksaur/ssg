#include "test_helpers.h"

#include <ssg/Workspace.h>

#include <chrono>
#include <filesystem>
#include <fstream>
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
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
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
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
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
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    const auto created = workspace.newDocument();
    const auto id = *created.document;
    const auto before = workspace.state(id);
    ASSERT_EQ(before->key.kind(), ssg::JournalDocumentKeyKind::Untitled);

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

TEST(recentFilesAreBoundedMruAndDropMissingEntries) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
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

TEST(renameDeleteAndWorkspaceReplaceAreCompensatable) {
    TemporaryDirectory first;
    TemporaryDirectory second;
    writeBytes(first.path() / "old.txt", "old");
    writeBytes(second.path() / "other.txt", "other");
    auto recovery = ssg::RecoveryActions::create(first.path() / ".recovery");
    auto workspace = ssg::Workspace::create(first.path(), recovery);
    const auto opened = workspace.openFile("old.txt");

    const auto renamed =
        workspace.renameFile(*opened.document, "renamed.txt");
    ASSERT_TRUE(renamed.accepted());
    ASSERT_TRUE(renamed.compensation.has_value());
    ASSERT_TRUE(workspace.restore(*renamed.compensation).accepted());
    ASSERT_TRUE(std::filesystem::exists(first.path() / "old.txt"));

    const auto removed = workspace.deleteFile(*opened.document);
    ASSERT_TRUE(removed.accepted());
    ASSERT_TRUE(workspace.restore(*removed.compensation).accepted());
    ASSERT_EQ(readBytes(first.path() / "old.txt"), std::string{"old"});

    const auto replaced = workspace.openDirectory(second.path());
    ASSERT_TRUE(replaced.accepted());
    ASSERT_EQ(workspace.root(), std::filesystem::canonical(second.path()));
    ASSERT_TRUE(replaced.workspaceCompensation.has_value());
    ASSERT_TRUE(
        workspace.restoreWorkspace(*replaced.workspaceCompensation).accepted());
    ASSERT_EQ(workspace.root(), std::filesystem::canonical(first.path()));
}

TEST(saveOverwriteAndReloadCompensationsRestoreGroundTruth) {
    TemporaryDirectory temporary;
    writeBytes(temporary.path() / "file.txt", "disk");
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
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
    ASSERT_TRUE(workspace.restore(*saved.compensation).accepted());
    ASSERT_EQ(readBytes(temporary.path() / "file.txt"), std::string{"disk"});
    const auto afterSaveRestore = workspace.state(id);
    ASSERT_TRUE(afterSaveRestore->dirty);

    writeBytes(temporary.path() / "file.txt", "external");
    const auto reloaded = workspace.reload(id);
    ASSERT_TRUE(reloaded.accepted());
    ASSERT_EQ(workspace.document(id).snapshot().text, std::string{"external"});
    ASSERT_TRUE(workspace.restore(*reloaded.compensation).accepted());
    ASSERT_EQ(workspace.document(id).snapshot().text,
              std::string{"disk-edited"});
    const auto afterReloadRestore = workspace.state(id);
    ASSERT_TRUE(afterReloadRestore->dirty);
}

TEST(newDirectoryRejectsEscapeAndCreatesOnlyInsideRoot) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
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
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
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
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    ASSERT_EQ(workspace.tryDocument(ssg::FileDocumentId{777}), nullptr);

    const auto created = workspace.newDocument();
    ASSERT_TRUE(created.accepted());
    ASSERT_TRUE(created.document.has_value());
    ASSERT_TRUE(workspace.tryDocument(*created.document) != nullptr);
    ASSERT_EQ(workspace.tryDocument(ssg::FileDocumentId{778}), nullptr);
}

}  // namespace

int main() {
    RUN(openIsByteExactAndPreventsNormalizedDuplicates);
    RUN(pathsCannotEscapeWorkspaceBeforeMutation);
    RUN(untitledIdentityChangesOnlyAfterSuccessfulSave);
    RUN(recentFilesAreBoundedMruAndDropMissingEntries);
    RUN(renameDeleteAndWorkspaceReplaceAreCompensatable);
    RUN(saveOverwriteAndReloadCompensationsRestoreGroundTruth);
    RUN(newDirectoryRejectsEscapeAndCreatesOnlyInsideRoot);
    RUN(emptyAndMixedEndingEditsSaveWithExactMetadata);
    RUN(tryDocumentReturnsNullForAbsentId);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
