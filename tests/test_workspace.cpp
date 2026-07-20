#include "test_helpers.h"

#include <ssg/workspace.h>

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

void write_bytes(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}};
}

TEST(open_is_byte_exact_and_prevents_normalized_duplicates) {
    TemporaryDirectory temporary;
    write_bytes(temporary.path() / "a.txt", "\xef\xbb\xbfone\r\ntwo\r");
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);

    const auto first = workspace.open_file("a.txt");
    const auto duplicate = workspace.open_file("./a.txt");

    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(duplicate.accepted());
    ASSERT_EQ(*first.document, *duplicate.document);
    ASSERT_EQ(workspace.documents().size(), std::size_t{1});
    ASSERT_EQ(workspace.document(*first.document).snapshot().text,
              std::string{"one\ntwo\n"});
    ASSERT_TRUE(workspace.save(*first.document).accepted());
    ASSERT_EQ(read_bytes(temporary.path() / "a.txt"),
              std::string{"\xef\xbb\xbfone\r\ntwo\r"});
}

TEST(paths_cannot_escape_workspace_before_mutation) {
    TemporaryDirectory temporary;
    TemporaryDirectory outside;
    write_bytes(outside.path() / "secret.txt", "secret");
    std::error_code symlink_error;
    std::filesystem::create_directory_symlink(
        outside.path(), temporary.path() / "escape", symlink_error);
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);

    ASSERT_EQ(workspace.open_file("../secret.txt").error,
              ssg::WorkspaceError::InvalidPath);
    ASSERT_EQ(workspace.open_file(outside.path().string()).error,
              ssg::WorkspaceError::InvalidPath);
    if (!symlink_error) {
        ASSERT_EQ(workspace.open_file("escape/secret.txt").error,
                  ssg::WorkspaceError::PathOutsideWorkspace);
    }
    ASSERT_EQ(workspace.documents().size(), std::size_t{0});
}

TEST(untitled_identity_changes_only_after_successful_save) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    const auto created = workspace.new_document();
    const auto id = *created.document;
    const auto before = workspace.state(id);
    ASSERT_EQ(before->key.kind(), ssg::JournalDocumentKeyKind::Untitled);

    std::filesystem::create_directories(temporary.path() / "blocked");
    const auto failed_save = workspace.save_as(id, "blocked");
    ASSERT_FALSE(failed_save.accepted());
    const auto after_failure = workspace.state(id);
    ASSERT_EQ(after_failure->key, before->key);
    ASSERT_TRUE(after_failure->dirty);

    const auto saved = workspace.save_as(id, "named.txt");
    ASSERT_TRUE(saved.accepted());
    const auto after_save = workspace.state(id);
    ASSERT_EQ(after_save->key.kind(), ssg::JournalDocumentKeyKind::Saved);
    ASSERT_EQ(after_save->key.saved_path(), std::string{"named.txt"});
    ASSERT_FALSE(after_save->dirty);
}

TEST(recent_files_are_bounded_mru_and_drop_missing_entries) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    for (int index = 0; index < 34; ++index) {
        const auto name = std::to_string(index) + ".txt";
        write_bytes(temporary.path() / name, name);
        ASSERT_TRUE(workspace.open_file(name).accepted());
    }
    const auto recent = workspace.recent_files();
    ASSERT_EQ(recent.size(), std::size_t{32});
    ASSERT_EQ(recent.front(), std::string{"33.txt"});
    ASSERT_EQ(recent.back(), std::string{"2.txt"});

    std::filesystem::remove(temporary.path() / "33.txt");
    ASSERT_EQ(workspace.open_recent(0).error, ssg::WorkspaceError::NotFound);
    const auto after_missing = workspace.recent_files();
    ASSERT_EQ(after_missing.front(), std::string{"32.txt"});
}

TEST(rename_delete_and_workspace_replace_are_compensatable) {
    TemporaryDirectory first;
    TemporaryDirectory second;
    write_bytes(first.path() / "old.txt", "old");
    write_bytes(second.path() / "other.txt", "other");
    auto recovery = ssg::RecoveryActions::create(first.path() / ".recovery");
    auto workspace = ssg::Workspace::create(first.path(), recovery);
    const auto opened = workspace.open_file("old.txt");

    const auto renamed =
        workspace.rename_file(*opened.document, "renamed.txt");
    ASSERT_TRUE(renamed.accepted());
    ASSERT_TRUE(renamed.compensation.has_value());
    ASSERT_TRUE(workspace.restore(*renamed.compensation).accepted());
    ASSERT_TRUE(std::filesystem::exists(first.path() / "old.txt"));

    const auto removed = workspace.delete_file(*opened.document);
    ASSERT_TRUE(removed.accepted());
    ASSERT_TRUE(workspace.restore(*removed.compensation).accepted());
    ASSERT_EQ(read_bytes(first.path() / "old.txt"), std::string{"old"});

    const auto replaced = workspace.open_directory(second.path());
    ASSERT_TRUE(replaced.accepted());
    ASSERT_EQ(workspace.root(), std::filesystem::canonical(second.path()));
    ASSERT_TRUE(replaced.workspace_compensation.has_value());
    ASSERT_TRUE(
        workspace.restore_workspace(*replaced.workspace_compensation).accepted());
    ASSERT_EQ(workspace.root(), std::filesystem::canonical(first.path()));
}

TEST(save_overwrite_and_reload_compensations_restore_ground_truth) {
    TemporaryDirectory temporary;
    write_bytes(temporary.path() / "file.txt", "disk");
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    const auto id = *workspace.open_file("file.txt").document;
    ASSERT_TRUE(workspace
                    .apply(id, {workspace.document(id).revision(),
                                {{ssg::ByteOffset{4}, 0, "-edited"}}})
                    .accepted());

    const auto saved = workspace.save(id);
    ASSERT_TRUE(saved.accepted());
    ASSERT_EQ(read_bytes(temporary.path() / "file.txt"),
              std::string{"disk-edited"});
    ASSERT_TRUE(workspace.restore(*saved.compensation).accepted());
    ASSERT_EQ(read_bytes(temporary.path() / "file.txt"), std::string{"disk"});
    const auto after_save_restore = workspace.state(id);
    ASSERT_TRUE(after_save_restore->dirty);

    write_bytes(temporary.path() / "file.txt", "external");
    const auto reloaded = workspace.reload(id);
    ASSERT_TRUE(reloaded.accepted());
    ASSERT_EQ(workspace.document(id).snapshot().text, std::string{"external"});
    ASSERT_TRUE(workspace.restore(*reloaded.compensation).accepted());
    ASSERT_EQ(workspace.document(id).snapshot().text,
              std::string{"disk-edited"});
    const auto after_reload_restore = workspace.state(id);
    ASSERT_TRUE(after_reload_restore->dirty);
}

TEST(new_directory_rejects_escape_and_creates_only_inside_root) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    ASSERT_TRUE(workspace.new_directory("inside").accepted());
    ASSERT_TRUE(std::filesystem::is_directory(temporary.path() / "inside"));
    ASSERT_EQ(workspace.new_directory("../outside").error,
              ssg::WorkspaceError::InvalidPath);
}

TEST(empty_and_mixed_ending_edits_save_with_exact_metadata) {
    TemporaryDirectory temporary;
    write_bytes(temporary.path() / "empty.txt", "disk");
    write_bytes(temporary.path() / "mixed.txt", "a\r\nb\nc");
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);

    const auto empty = *workspace.open_file("empty.txt").document;
    ASSERT_TRUE(workspace
                    .apply(empty, {workspace.document(empty).revision(),
                                   {{ssg::ByteOffset{0}, 4, ""}}})
                    .accepted());
    ASSERT_TRUE(workspace.save(empty).accepted());
    ASSERT_EQ(read_bytes(temporary.path() / "empty.txt"), std::string{});

    const auto mixed = *workspace.open_file("mixed.txt").document;
    ASSERT_TRUE(workspace
                    .apply(mixed, {workspace.document(mixed).revision(),
                                   {{ssg::ByteOffset{0}, 0, "X\n"}}})
                    .accepted());
    ASSERT_TRUE(workspace.save(mixed).accepted());
    ASSERT_EQ(read_bytes(temporary.path() / "mixed.txt"),
              std::string{"X\na\r\nb\nc"});
}

}  // namespace

int main() {
    RUN(open_is_byte_exact_and_prevents_normalized_duplicates);
    RUN(paths_cannot_escape_workspace_before_mutation);
    RUN(untitled_identity_changes_only_after_successful_save);
    RUN(recent_files_are_bounded_mru_and_drop_missing_entries);
    RUN(rename_delete_and_workspace_replace_are_compensatable);
    RUN(save_overwrite_and_reload_compensations_restore_ground_truth);
    RUN(new_directory_rejects_escape_and_creates_only_inside_root);
    RUN(empty_and_mixed_ending_edits_save_with_exact_metadata);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
