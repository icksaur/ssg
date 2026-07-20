#include "ssg/platform_files.h"
#include "ssg/recovery.h"
#include "ssg/scratch.h"
#include "ssg/scratch_journal.h"
#include "test_helpers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{0};
        path_ = std::filesystem::current_path() /
                (".ssg-recovery-test-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()) +
                 "-" + std::to_string(sequence++));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void write_bytes(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("failed to create test file");
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) throw std::runtime_error("failed to write test file");
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to read test file");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

std::span<const std::byte> bytes(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

enum class NodeKind {
    Missing,
    Directory,
    RegularFile,
    Symlink,
};

struct TreeNode {
    std::string relative_path;
    NodeKind kind;
    std::string value;

    friend bool operator==(const TreeNode&, const TreeNode&) = default;
};

std::vector<TreeNode> snapshot_tree(const std::filesystem::path& root) {
    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(root, error);
    if (error || root_status.type() == std::filesystem::file_type::not_found) {
        return {{".", NodeKind::Missing, {}}};
    }

    std::vector<TreeNode> result;
    const auto append = [&](const std::filesystem::path& path,
                            const std::filesystem::file_status& status,
                            std::vector<TreeNode>& destination) {
        const auto relative =
            path == root ? std::string{"."}
                         : path.lexically_relative(root).generic_string();
        if (std::filesystem::is_symlink(status)) {
            destination.push_back(
                {relative, NodeKind::Symlink,
                 std::filesystem::read_symlink(path).generic_string()});
        } else if (std::filesystem::is_directory(status)) {
            destination.push_back({relative, NodeKind::Directory, {}});
        } else if (std::filesystem::is_regular_file(status)) {
            destination.push_back(
                {relative, NodeKind::RegularFile, read_bytes(path)});
        }
    };

    append(root, root_status, result);
    if (std::filesystem::is_directory(root_status)) {
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(root)) {
            append(entry.path(), entry.symlink_status(), result);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const TreeNode& left, const TreeNode& right) {
                  return left.relative_path < right.relative_path;
              });
    return result;
}

ssg::UntitledDocumentId fixed_untitled_id() {
    std::array<std::byte, 16> value{};
    value[0] = std::byte{0x42};
    value[15] = std::byte{0x7f};
    return ssg::UntitledDocumentId{value};
}

ssg::JournalDocument saved_document(std::string path,
                                    std::string contents,
                                    bool dirty = true) {
    return {ssg::JournalDocumentKey::saved(path),
            ssg::DocumentMode::Edit,
            dirty,
            std::move(contents)};
}

ssg::RecoveryConfig recovery_config(
    std::size_t maximum_records = 16,
    std::uintmax_t maximum_bytes =
        std::numeric_limits<std::uintmax_t>::max()) {
    return {maximum_records, maximum_bytes};
}

ssg::ScratchStoreConfig scratch_config() {
    ssg::ScratchStoreConfig result;
    result.maximum_bytes = std::numeric_limits<std::uintmax_t>::max();
    result.maximum_age = std::chrono::hours{24 * 365};
    result.compaction_threshold_bytes =
        std::numeric_limits<std::uintmax_t>::max();
    result.durability_target = 100ms;
    return result;
}

class FailingScratchStorage final : public ssg::ScratchStorage {
public:
    void append_document(const std::filesystem::path&,
                         const ssg::JournalDocument&) override {
        throw std::runtime_error("injected scratch append failure");
    }

    void append_remove(const std::filesystem::path&,
                       const ssg::JournalDocumentKey&) override {
        throw std::runtime_error("injected scratch remove failure");
    }

    void replace_checkpoint(
        const std::filesystem::path&,
        const ssg::JournalRecoverySet&) override {
        throw std::runtime_error("injected scratch checkpoint failure");
    }
};

class InjectedRecoveryFailures final : public ssg::RecoveryFaultInjector {
public:
    struct Rule {
        ssg::RecoveryStep step;
        std::size_t skip;
        bool active = true;
    };

    void fail(ssg::RecoveryStep step, std::size_t skip = 0) {
        rules_.push_back({step, skip});
    }

    void clear_failures() { rules_.clear(); }
    void clear_events() { events.clear(); }

    void before_step(ssg::RecoveryStep step) override {
        events.push_back(step);
        for (auto& rule : rules_) {
            if (!rule.active || rule.step != step) continue;
            if (rule.skip != 0) {
                --rule.skip;
                continue;
            }
            rule.active = false;
            throw std::runtime_error("injected recovery I/O failure");
        }
    }

    std::vector<ssg::RecoveryStep> events;

private:
    std::vector<Rule> rules_;
};

std::size_t event_index(const std::vector<ssg::RecoveryStep>& events,
                        ssg::RecoveryStep step) {
    const auto found = std::find(events.begin(), events.end(), step);
    return found == events.end()
               ? std::numeric_limits<std::size_t>::max()
               : static_cast<std::size_t>(found - events.begin());
}

TEST(dirty_close_is_durable_before_removal_and_restores_exact_document) {
    TemporaryDirectory temporary;
    const auto workspace =
        std::filesystem::absolute(temporary.path() / "workspace");
    std::filesystem::create_directories(workspace);
    auto scratch = ssg::ScratchStore::create(
        temporary.path() / "scratch", workspace, scratch_config());
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config());
    const ssg::JournalDocument expected{
        ssg::JournalDocumentKey::untitled(fixed_untitled_id()),
        ssg::DocumentMode::ReadOnly,
        true,
        "dirty \xCE\xB2 draft\n"};
    std::optional<ssg::JournalDocument> document{expected};

    const auto closed = actions.close_document(document, scratch, 2s);

    ASSERT_TRUE(closed.accepted());
    ASSERT_TRUE(closed.compensation.has_value());
    ASSERT_FALSE(document.has_value());
    ASSERT_EQ(scratch.recovery().documents,
              std::vector<ssg::JournalDocument>{expected});

    actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config());
    const auto reconstructed_records = actions.records();
    ASSERT_EQ(reconstructed_records.front().document,
              std::optional<ssg::JournalDocumentKey>{expected.key});
    const auto restored =
        actions.restore_document(*closed.compensation, document);
    ASSERT_TRUE(restored.accepted());
    ASSERT_EQ(document, std::optional<ssg::JournalDocument>{expected});
    ASSERT_TRUE(actions.records().empty());
}

TEST(dirty_close_durability_failure_preserves_document_and_publishes_nothing) {
    TemporaryDirectory temporary;
    const auto workspace =
        std::filesystem::absolute(temporary.path() / "workspace");
    std::filesystem::create_directories(workspace);
    FailingScratchStorage scratch_storage;
    auto scratch = ssg::ScratchStore::create(
        temporary.path() / "scratch", workspace, scratch_config(),
        scratch_storage);
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config());
    const auto expected = saved_document("draft.txt", "not durable");
    std::optional<ssg::JournalDocument> document{expected};

    const auto closed = actions.close_document(document, scratch, 2s);

    ASSERT_FALSE(closed.accepted());
    ASSERT_EQ(closed.error->code, ssg::RecoveryErrorCode::DurabilityFailed);
    ASSERT_EQ(document, std::optional<ssg::JournalDocument>{expected});
    ASSERT_TRUE(actions.records().empty());
}

TEST(reload_compensation_survives_reconstruction_and_restores_exact_document) {
    TemporaryDirectory temporary;
    const auto recovery_root = temporary.path() / "recovery";
    const auto expected = saved_document("notes.txt", "unsaved text");
    const auto replacement =
        saved_document("notes.txt", "bytes from disk", false);
    std::optional<ssg::JournalDocument> document{expected};
    std::optional<ssg::RecoveryRecordId> compensation;
    {
        auto actions =
            ssg::RecoveryActions::create(recovery_root, recovery_config());
        const auto reloaded =
            actions.reload_document(document, replacement);
        ASSERT_TRUE(reloaded.accepted());
        ASSERT_EQ(document,
                  std::optional<ssg::JournalDocument>{replacement});
        compensation = reloaded.compensation;
    }

    auto reconstructed =
        ssg::RecoveryActions::create(recovery_root, recovery_config());
    ASSERT_EQ(reconstructed.records().size(), std::size_t{1});
    ASSERT_TRUE(
        reconstructed.restore_document(*compensation, document).accepted());
    ASSERT_EQ(document, std::optional<ssg::JournalDocument>{expected});
}

TEST(overwrite_existing_and_new_files_round_trip_to_filesystem_truth) {
    TemporaryDirectory temporary;
    const auto recovery_root = temporary.path() / "recovery";
    const auto existing = temporary.path() / "canonical" / "existing.bin";
    const auto created = temporary.path() / "canonical" / "created.bin";
    write_bytes(existing, std::string{"old\0bytes", 9});
    const auto before_existing = snapshot_tree(existing);
    const auto before_created = snapshot_tree(created);
    auto actions =
        ssg::RecoveryActions::create(recovery_root, recovery_config());

    const auto overwritten =
        actions.overwrite_file(existing, bytes("replacement"));
    const auto new_file = actions.overwrite_file(created, bytes("new"));

    ASSERT_TRUE(overwritten.accepted());
    ASSERT_TRUE(new_file.accepted());
    ASSERT_EQ(read_bytes(existing), "replacement");
    ASSERT_EQ(read_bytes(created), "new");
    actions =
        ssg::RecoveryActions::create(recovery_root, recovery_config());
    ASSERT_TRUE(
        actions.restore_filesystem(*new_file.compensation).accepted());
    ASSERT_TRUE(
        actions.restore_filesystem(*overwritten.compensation).accepted());
    ASSERT_EQ(snapshot_tree(existing), before_existing);
    ASSERT_EQ(snapshot_tree(created), before_created);
}

TEST(rename_round_trip_restores_both_paths_and_source_identity) {
    TemporaryDirectory temporary;
    const auto canonical = temporary.path() / "canonical";
    const auto source = canonical / "source.txt";
    const auto destination = canonical / "destination.txt";
    write_bytes(source, "source bytes");
    write_bytes(destination, "destination bytes");
    const auto before = snapshot_tree(canonical);
    const auto source_identity = ssg::file_identity(source);
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config());

    const auto renamed = actions.rename_path(source, destination);

    ASSERT_TRUE(renamed.accepted());
    ASSERT_FALSE(std::filesystem::exists(source));
    ASSERT_EQ(read_bytes(destination), "source bytes");
    ASSERT_EQ(ssg::file_identity(destination), source_identity);
    actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config());
    const auto reconstructed_records = actions.records();
    ASSERT_EQ(reconstructed_records.front().affected_paths,
              (std::vector<std::filesystem::path>{source, destination}));
    ASSERT_TRUE(
        actions.restore_filesystem(*renamed.compensation).accepted());
    ASSERT_EQ(snapshot_tree(canonical), before);
    ASSERT_EQ(ssg::file_identity(source), source_identity);
}

TEST(delete_tree_round_trip_restores_binary_files_and_empty_directories) {
    TemporaryDirectory temporary;
    const auto removed = temporary.path() / "canonical" / "removed";
    std::filesystem::create_directories(removed / "empty");
    write_bytes(removed / "nested" / "binary",
                std::string{"a\0b\xff", 4});
    write_bytes(removed / "root.txt", "root");
    const auto before = snapshot_tree(removed);
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config());

    const auto deleted = actions.delete_path(removed);

    ASSERT_TRUE(deleted.accepted());
    ASSERT_FALSE(std::filesystem::exists(removed));
    actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config());
    ASSERT_TRUE(
        actions.restore_filesystem(*deleted.compensation).accepted());
    ASSERT_EQ(snapshot_tree(removed), before);
}

TEST(workspace_replacement_and_compensation_match_independent_tree_snapshots) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto replacement = temporary.path() / "incoming";
    write_bytes(workspace / "old" / "a.txt", "old a");
    write_bytes(workspace / "obsolete.txt", "obsolete");
    std::filesystem::create_directories(workspace / "old-empty");
    write_bytes(replacement / "new" / "b.txt", "new b");
    std::filesystem::create_directories(replacement / "new-empty");
    const auto workspace_before = snapshot_tree(workspace);
    const auto replacement_before = snapshot_tree(replacement);
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config());

    const auto replaced =
        actions.replace_workspace(workspace, replacement);

    ASSERT_TRUE(replaced.accepted());
    ASSERT_EQ(snapshot_tree(workspace), replacement_before);
    ASSERT_EQ(snapshot_tree(replacement), replacement_before);
    actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config());
    ASSERT_TRUE(
        actions.restore_filesystem(*replaced.compensation).accepted());
    ASSERT_EQ(snapshot_tree(workspace), workspace_before);
    ASSERT_EQ(snapshot_tree(replacement), replacement_before);
}

TEST(record_and_artifact_installation_complete_before_canonical_mutation) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "canonical" / "document";
    write_bytes(target, "old");
    InjectedRecoveryFailures injection;
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config(), injection);

    const auto overwritten = actions.overwrite_file(target, bytes("new"));

    ASSERT_TRUE(overwritten.accepted());
    const auto artifact =
        event_index(injection.events, ssg::RecoveryStep::PrepareArtifact);
    const auto installed =
        event_index(injection.events, ssg::RecoveryStep::InstallRecord);
    const auto mutation =
        event_index(injection.events, ssg::RecoveryStep::MutateFilesystem);
    const auto published =
        event_index(injection.events, ssg::RecoveryStep::PublishRecord);
    ASSERT_TRUE(artifact < installed);
    ASSERT_TRUE(installed < mutation);
    ASSERT_TRUE(mutation < published);
}

TEST(preparation_failure_preserves_canonical_state_and_existing_records) {
    TemporaryDirectory temporary;
    const auto first = temporary.path() / "canonical" / "first";
    const auto second = temporary.path() / "canonical" / "second";
    write_bytes(first, "first old");
    write_bytes(second, "second old");
    InjectedRecoveryFailures injection;
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config(1), injection);
    const auto retained = actions.overwrite_file(first, bytes("first new"));
    ASSERT_TRUE(retained.accepted());
    injection.fail(ssg::RecoveryStep::InstallRecord);

    const auto rejected = actions.overwrite_file(second, bytes("second new"));

    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error->code,
              ssg::RecoveryErrorCode::PreparationFailed);
    ASSERT_FALSE(rejected.compensation.has_value());
    ASSERT_EQ(read_bytes(second), "second old");
    const auto records = actions.records();
    ASSERT_EQ(records.size(), std::size_t{1});
    ASSERT_EQ(records.front().id, *retained.compensation);
}

TEST(publication_failure_after_mutation_rolls_back_canonical_state) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "canonical" / "document";
    write_bytes(target, "old");
    const auto before = snapshot_tree(target);
    InjectedRecoveryFailures injection;
    injection.fail(ssg::RecoveryStep::PublishRecord);
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config(), injection);

    const auto result = actions.overwrite_file(target, bytes("new"));

    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.error->code, ssg::RecoveryErrorCode::ActionFailed);
    ASSERT_EQ(snapshot_tree(target), before);
    ASSERT_TRUE(event_index(injection.events,
                            ssg::RecoveryStep::RollbackFilesystem) !=
                std::numeric_limits<std::size_t>::max());
    ASSERT_TRUE(actions.records().empty());
}

TEST(each_action_mutation_failure_rolls_back_and_discards_its_record) {
    TemporaryDirectory temporary;

    {
        const auto workspace =
            std::filesystem::absolute(temporary.path() / "close-workspace");
        std::filesystem::create_directories(workspace);
        auto scratch = ssg::ScratchStore::create(
            temporary.path() / "close-scratch", workspace, scratch_config());
        InjectedRecoveryFailures injection;
        injection.fail(ssg::RecoveryStep::MutateDocument);
        auto actions = ssg::RecoveryActions::create(
            temporary.path() / "close-recovery", recovery_config(), injection);
        const auto expected = saved_document("close.txt", "dirty");
        std::optional<ssg::JournalDocument> document{expected};

        const auto result = actions.close_document(document, scratch, 2s);

        ASSERT_FALSE(result.accepted());
        ASSERT_EQ(result.error->code, ssg::RecoveryErrorCode::ActionFailed);
        ASSERT_EQ(document, std::optional<ssg::JournalDocument>{expected});
        ASSERT_TRUE(event_index(injection.events,
                                ssg::RecoveryStep::RollbackDocument) !=
                    std::numeric_limits<std::size_t>::max());
        ASSERT_TRUE(actions.records().empty());
    }

    {
        InjectedRecoveryFailures injection;
        injection.fail(ssg::RecoveryStep::MutateDocument);
        auto actions = ssg::RecoveryActions::create(
            temporary.path() / "reload-recovery", recovery_config(), injection);
        const auto expected = saved_document("reload.txt", "dirty");
        std::optional<ssg::JournalDocument> document{expected};

        const auto result = actions.reload_document(
            document, saved_document("reload.txt", "disk", false));

        ASSERT_FALSE(result.accepted());
        ASSERT_EQ(result.error->code, ssg::RecoveryErrorCode::ActionFailed);
        ASSERT_EQ(document, std::optional<ssg::JournalDocument>{expected});
        ASSERT_TRUE(event_index(injection.events,
                                ssg::RecoveryStep::RollbackDocument) !=
                    std::numeric_limits<std::size_t>::max());
        ASSERT_TRUE(actions.records().empty());
    }

    const auto assert_filesystem_action_failure =
        [&](std::string_view name, const auto& invoke) {
            InjectedRecoveryFailures injection;
            injection.fail(ssg::RecoveryStep::MutateFilesystem);
            auto actions = ssg::RecoveryActions::create(
                temporary.path() / std::string{name}, recovery_config(),
                injection);
            const auto result = invoke(actions);
            ASSERT_FALSE(result.accepted());
            ASSERT_EQ(result.error->code,
                      ssg::RecoveryErrorCode::ActionFailed);
            ASSERT_TRUE(event_index(injection.events,
                                    ssg::RecoveryStep::RollbackFilesystem) !=
                        std::numeric_limits<std::size_t>::max());
            ASSERT_TRUE(actions.records().empty());
        };

    const auto overwrite = temporary.path() / "canonical" / "overwrite";
    write_bytes(overwrite, "old");
    const auto overwrite_before = snapshot_tree(overwrite);
    assert_filesystem_action_failure(
        "overwrite-recovery", [&](ssg::RecoveryActions& actions) {
            return actions.overwrite_file(overwrite, bytes("new"));
        });
    ASSERT_EQ(snapshot_tree(overwrite), overwrite_before);

    const auto rename_source = temporary.path() / "canonical" / "rename-source";
    const auto rename_destination =
        temporary.path() / "canonical" / "rename-destination";
    write_bytes(rename_source, "source");
    write_bytes(rename_destination, "destination");
    const auto rename_before = snapshot_tree(temporary.path() / "canonical");
    assert_filesystem_action_failure(
        "rename-recovery", [&](ssg::RecoveryActions& actions) {
            return actions.rename_path(rename_source, rename_destination);
        });
    ASSERT_EQ(snapshot_tree(temporary.path() / "canonical"), rename_before);

    const auto removed = temporary.path() / "canonical" / "removed";
    write_bytes(removed / "nested", "remove");
    const auto removed_before = snapshot_tree(removed);
    assert_filesystem_action_failure(
        "delete-recovery", [&](ssg::RecoveryActions& actions) {
            return actions.delete_path(removed);
        });
    ASSERT_EQ(snapshot_tree(removed), removed_before);
}

TEST(partial_workspace_mutation_failure_rolls_back_to_exact_tree) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto replacement = temporary.path() / "incoming";
    write_bytes(workspace / "old.txt", "old");
    write_bytes(replacement / "new.txt", "new");
    const auto before = snapshot_tree(workspace);
    InjectedRecoveryFailures injection;
    injection.fail(ssg::RecoveryStep::MutateFilesystem, 1);
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config(), injection);

    const auto replaced =
        actions.replace_workspace(workspace, replacement);

    ASSERT_FALSE(replaced.accepted());
    ASSERT_EQ(replaced.error->code, ssg::RecoveryErrorCode::ActionFailed);
    ASSERT_TRUE(replaced.error->rollback_failure.empty());
    ASSERT_FALSE(replaced.compensation.has_value());
    ASSERT_EQ(snapshot_tree(workspace), before);
    ASSERT_TRUE(actions.records().empty());
}

TEST(action_and_rollback_failure_retains_record_for_successful_retry) {
    TemporaryDirectory temporary;
    const auto workspace = temporary.path() / "workspace";
    const auto replacement = temporary.path() / "incoming";
    write_bytes(workspace / "old.txt", "old");
    write_bytes(replacement / "new.txt", "new");
    const auto before = snapshot_tree(workspace);
    InjectedRecoveryFailures injection;
    injection.fail(ssg::RecoveryStep::MutateFilesystem, 1);
    injection.fail(ssg::RecoveryStep::RollbackFilesystem);
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config(), injection);

    const auto replaced =
        actions.replace_workspace(workspace, replacement);

    ASSERT_FALSE(replaced.accepted());
    ASSERT_EQ(replaced.error->code,
              ssg::RecoveryErrorCode::ActionAndRollbackFailed);
    ASSERT_FALSE(replaced.error->message.empty());
    ASSERT_FALSE(replaced.error->rollback_failure.empty());
    ASSERT_TRUE(replaced.compensation.has_value());
    ASSERT_EQ(actions.records().size(), std::size_t{1});

    injection.clear_failures();
    ASSERT_TRUE(
        actions.restore_filesystem(*replaced.compensation).accepted());
    ASSERT_EQ(snapshot_tree(workspace), before);
    ASSERT_TRUE(actions.records().empty());
}

TEST(reconstruction_discards_persisted_in_progress_document_record) {
    TemporaryDirectory temporary;
    const auto recovery_root = temporary.path() / "recovery";
    const auto expected = saved_document("notes.txt", "unsaved text");
    std::optional<ssg::JournalDocument> document{expected};
    {
        InjectedRecoveryFailures injection;
        injection.fail(ssg::RecoveryStep::MutateDocument);
        injection.fail(ssg::RecoveryStep::RollbackDocument);
        auto actions = ssg::RecoveryActions::create(
            recovery_root, recovery_config(), injection);

        const auto result = actions.reload_document(
            document, saved_document("notes.txt", "disk text", false));

        ASSERT_FALSE(result.accepted());
        ASSERT_EQ(result.error->code,
                  ssg::RecoveryErrorCode::ActionAndRollbackFailed);
        ASSERT_TRUE(result.compensation.has_value());
        ASSERT_EQ(actions.records().size(), std::size_t{1});
        ASSERT_EQ(document, std::optional<ssg::JournalDocument>{expected});
    }

    auto reconstructed =
        ssg::RecoveryActions::create(recovery_root, recovery_config());
    ASSERT_TRUE(reconstructed.records().empty());
    ASSERT_TRUE(std::filesystem::is_empty(recovery_root));
}

TEST(reconstruction_auto_rolls_back_persisted_in_progress_filesystem_record) {
    TemporaryDirectory temporary;
    const auto recovery_root = temporary.path() / "recovery";
    const auto workspace = temporary.path() / "workspace";
    const auto replacement = temporary.path() / "incoming";
    write_bytes(workspace / "old.txt", "old");
    write_bytes(replacement / "new.txt", "new");
    const auto before = snapshot_tree(workspace);
    {
        InjectedRecoveryFailures injection;
        injection.fail(ssg::RecoveryStep::MutateFilesystem, 1);
        injection.fail(ssg::RecoveryStep::RollbackFilesystem);
        auto actions = ssg::RecoveryActions::create(
            recovery_root, recovery_config(), injection);

        const auto result =
            actions.replace_workspace(workspace, replacement);

        ASSERT_FALSE(result.accepted());
        ASSERT_EQ(result.error->code,
                  ssg::RecoveryErrorCode::ActionAndRollbackFailed);
        ASSERT_TRUE(result.compensation.has_value());
        ASSERT_EQ(actions.records().size(), std::size_t{1});
        ASSERT_NE(snapshot_tree(workspace), before);
    }

    auto reconstructed =
        ssg::RecoveryActions::create(recovery_root, recovery_config());
    ASSERT_EQ(snapshot_tree(workspace), before);
    ASSERT_TRUE(reconstructed.records().empty());
    ASSERT_TRUE(std::filesystem::is_empty(recovery_root));
}

TEST(reconstruction_discards_partially_removed_record_directory) {
    TemporaryDirectory temporary;
    const auto recovery_root = temporary.path() / "recovery";
    write_bytes(recovery_root / "partial-record" / "artifacts" / "0",
                "orphaned artifact");

    bool reconstructed = false;
    try {
        auto actions =
            ssg::RecoveryActions::create(recovery_root, recovery_config());
        reconstructed = actions.records().empty();
    } catch (...) {
    }

    ASSERT_TRUE(reconstructed);
    ASSERT_TRUE(std::filesystem::is_empty(recovery_root));
}

TEST(rename_failed_publication_rollback_remains_safe_after_reconstruction) {
    TemporaryDirectory temporary;
    const auto recovery_root = temporary.path() / "recovery";
    const auto canonical = temporary.path() / "canonical";
    const auto source = canonical / "source.txt";
    const auto destination = canonical / "destination.txt";
    write_bytes(source, "source bytes");
    write_bytes(destination, "destination bytes");
    const auto before = snapshot_tree(canonical);
    const auto source_identity = ssg::file_identity(source);
    {
        InjectedRecoveryFailures injection;
        injection.fail(ssg::RecoveryStep::PublishRecord, 1);
        injection.fail(ssg::RecoveryStep::CleanupRecord);
        auto actions = ssg::RecoveryActions::create(
            recovery_root, recovery_config(), injection);

        const auto result = actions.rename_path(source, destination);

        ASSERT_FALSE(result.accepted());
        ASSERT_TRUE(result.error.has_value());
        if (result.error) {
            ASSERT_EQ(result.error->code, ssg::RecoveryErrorCode::CleanupFailed);
        }
        ASSERT_TRUE(result.compensation.has_value());
        ASSERT_EQ(snapshot_tree(canonical), before);
    }

    auto reconstructed =
        ssg::RecoveryActions::create(recovery_root, recovery_config());
    ASSERT_TRUE(reconstructed.records().empty());
    ASSERT_EQ(snapshot_tree(canonical), before);
    ASSERT_TRUE(std::filesystem::exists(source));
    if (std::filesystem::exists(source)) {
        ASSERT_EQ(ssg::file_identity(source), source_identity);
    }
}

TEST(restoration_failure_keeps_record_and_retry_restores_exact_tree) {
    TemporaryDirectory temporary;
    const auto removed = temporary.path() / "canonical" / "removed";
    write_bytes(removed / "nested.txt", "recover");
    const auto before = snapshot_tree(removed);
    InjectedRecoveryFailures injection;
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config(), injection);
    const auto deleted = actions.delete_path(removed);
    ASSERT_TRUE(deleted.accepted());
    injection.fail(ssg::RecoveryStep::RestoreFilesystem);

    const auto failed_restore =
        actions.restore_filesystem(*deleted.compensation);

    ASSERT_FALSE(failed_restore.accepted());
    ASSERT_EQ(failed_restore.error->code,
              ssg::RecoveryErrorCode::RestorationFailed);
    ASSERT_EQ(actions.records().size(), std::size_t{1});
    injection.clear_failures();
    ASSERT_TRUE(
        actions.restore_filesystem(*deleted.compensation).accepted());
    ASSERT_EQ(snapshot_tree(removed), before);
    ASSERT_TRUE(actions.records().empty());
}

TEST(rename_partial_restore_retry_preserves_source_identity) {
    TemporaryDirectory temporary;
    const auto source = temporary.path() / "canonical" / "source.txt";
    const auto destination =
        temporary.path() / "canonical" / "destination.txt";
    write_bytes(source, "source bytes");
    write_bytes(destination, "destination bytes");
    const auto before = snapshot_tree(temporary.path() / "canonical");
    const auto source_identity = ssg::file_identity(source);
    InjectedRecoveryFailures injection;
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config(), injection);
    const auto renamed = actions.rename_path(source, destination);
    ASSERT_TRUE(renamed.accepted());
    injection.fail(ssg::RecoveryStep::RestoreFilesystem, 1);

    const auto failed_restore =
        actions.restore_filesystem(*renamed.compensation);

    ASSERT_FALSE(failed_restore.accepted());
    ASSERT_TRUE(failed_restore.error.has_value());
    if (failed_restore.error) {
        ASSERT_EQ(failed_restore.error->code,
                  ssg::RecoveryErrorCode::RestorationFailed);
    }
    ASSERT_EQ(ssg::file_identity(source), source_identity);
    ASSERT_FALSE(std::filesystem::exists(destination));
    ASSERT_EQ(actions.records().size(), std::size_t{1});

    injection.clear_failures();
    ASSERT_TRUE(
        actions.restore_filesystem(*renamed.compensation).accepted());
    ASSERT_EQ(snapshot_tree(temporary.path() / "canonical"), before);
    ASSERT_EQ(ssg::file_identity(source), source_identity);
}

TEST(rename_completed_restore_retry_preserves_source_identity) {
    TemporaryDirectory temporary;
    const auto source = temporary.path() / "canonical" / "source.txt";
    const auto destination =
        temporary.path() / "canonical" / "destination.txt";
    write_bytes(source, "source bytes");
    write_bytes(destination, "destination bytes");
    const auto before = snapshot_tree(temporary.path() / "canonical");
    const auto source_identity = ssg::file_identity(source);
    InjectedRecoveryFailures injection;
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config(), injection);
    const auto renamed = actions.rename_path(source, destination);
    ASSERT_TRUE(renamed.accepted());
    injection.fail(ssg::RecoveryStep::RestoreFilesystem, 2);

    const auto failed_restore =
        actions.restore_filesystem(*renamed.compensation);

    ASSERT_FALSE(failed_restore.accepted());
    ASSERT_TRUE(failed_restore.error.has_value());
    ASSERT_EQ(snapshot_tree(temporary.path() / "canonical"), before);
    ASSERT_EQ(ssg::file_identity(source), source_identity);
    ASSERT_EQ(actions.records().size(), std::size_t{1});

    injection.clear_failures();
    ASSERT_TRUE(
        actions.restore_filesystem(*renamed.compensation).accepted());
    ASSERT_EQ(snapshot_tree(temporary.path() / "canonical"), before);
    ASSERT_EQ(ssg::file_identity(source), source_identity);
}

TEST(cleanup_failure_keeps_restored_record_retryable) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "canonical" / "document";
    write_bytes(target, "old");
    InjectedRecoveryFailures injection;
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config(), injection);
    const auto overwritten = actions.overwrite_file(target, bytes("new"));
    ASSERT_TRUE(overwritten.accepted());
    injection.fail(ssg::RecoveryStep::CleanupRecord);

    const auto cleanup_failed =
        actions.restore_filesystem(*overwritten.compensation);

    ASSERT_FALSE(cleanup_failed.accepted());
    ASSERT_EQ(cleanup_failed.error->code,
              ssg::RecoveryErrorCode::CleanupFailed);
    ASSERT_EQ(read_bytes(target), "old");
    ASSERT_EQ(actions.records().size(), std::size_t{1});
    injection.clear_failures();
    ASSERT_TRUE(
        actions.restore_filesystem(*overwritten.compensation).accepted());
    ASSERT_EQ(read_bytes(target), "old");
    ASSERT_TRUE(actions.records().empty());
}

TEST(count_budget_evicts_oldest_only_after_new_record_is_installed) {
    TemporaryDirectory temporary;
    const auto canonical = temporary.path() / "canonical";
    const auto a = canonical / "a";
    const auto b = canonical / "b";
    const auto c = canonical / "c";
    write_bytes(a, "a0");
    write_bytes(b, "b0");
    write_bytes(c, "c0");
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config(2));

    const auto first = actions.overwrite_file(a, bytes("a1"));
    const auto second = actions.overwrite_file(b, bytes("b1"));
    const auto third = actions.overwrite_file(c, bytes("c1"));

    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(second.accepted());
    ASSERT_TRUE(third.accepted());
    const auto records = actions.records();
    ASSERT_EQ(records.size(), std::size_t{2});
    ASSERT_EQ(records[0].id, *second.compensation);
    ASSERT_EQ(records[1].id, *third.compensation);
    const auto evicted = actions.restore_filesystem(*first.compensation);
    ASSERT_FALSE(evicted.accepted());
    ASSERT_EQ(evicted.error->code, ssg::RecoveryErrorCode::RecordNotFound);
}

TEST(byte_budget_evicts_oldest_when_new_record_fits_after_eviction) {
    TemporaryDirectory temporary;
    const auto recovery_root = temporary.path() / "recovery";
    const auto a = temporary.path() / "canonical" / "a.bin";
    const auto b = temporary.path() / "canonical" / "b.bin";
    const auto c = temporary.path() / "canonical" / "c.bin";
    const std::string original(256, 'o');
    write_bytes(a, original);
    write_bytes(b, original);
    write_bytes(c, original);
    auto actions =
        ssg::RecoveryActions::create(recovery_root, recovery_config());
    const auto first = actions.overwrite_file(a, bytes("new"));
    const auto second = actions.overwrite_file(b, bytes("new"));
    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(second.accepted());
    const auto initial_records = actions.records();
    const auto two_record_budget =
        initial_records[0].stored_bytes + initial_records[1].stored_bytes;
    actions = ssg::RecoveryActions::create(
        recovery_root, recovery_config(8, two_record_budget));

    const auto third = actions.overwrite_file(c, bytes("new"));

    ASSERT_TRUE(third.accepted());
    const auto retained = actions.records();
    ASSERT_EQ(retained.size(), std::size_t{2});
    ASSERT_EQ(retained[0].id, *second.compensation);
    ASSERT_EQ(retained[1].id, *third.compensation);
    const auto evicted = actions.restore_filesystem(*first.compensation);
    ASSERT_FALSE(evicted.accepted());
    ASSERT_EQ(evicted.error->code, ssg::RecoveryErrorCode::RecordNotFound);
}

TEST(byte_budget_rejects_before_mutating_when_newest_record_cannot_fit) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "canonical" / "large";
    const std::string original(4096, 'o');
    write_bytes(target, original);
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config(8, 32));

    const auto rejected = actions.overwrite_file(target, bytes("new"));

    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error->code, ssg::RecoveryErrorCode::BudgetExceeded);
    ASSERT_EQ(read_bytes(target), original);
    ASSERT_TRUE(actions.records().empty());
}

TEST(compensation_removes_only_its_own_record_and_artifacts) {
    TemporaryDirectory temporary;
    const auto first_path = temporary.path() / "canonical" / "first";
    const auto second_path = temporary.path() / "canonical" / "second";
    write_bytes(first_path, "first old");
    write_bytes(second_path, "second old");
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config());
    const auto first =
        actions.overwrite_file(first_path, bytes("first new"));
    const auto second =
        actions.overwrite_file(second_path, bytes("second new"));

    ASSERT_TRUE(
        actions.restore_filesystem(*first.compensation).accepted());

    ASSERT_EQ(read_bytes(first_path), "first old");
    ASSERT_EQ(read_bytes(second_path), "second new");
    const auto records = actions.records();
    ASSERT_EQ(records.size(), std::size_t{1});
    ASSERT_EQ(records.front().id, *second.compensation);
    ASSERT_TRUE(
        actions.restore_filesystem(*second.compensation).accepted());
}

TEST(record_kind_mismatches_are_typed_and_non_destructive) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "canonical" / "file";
    write_bytes(target, "old");
    auto actions = ssg::RecoveryActions::create(
        temporary.path() / "recovery", recovery_config());
    const auto overwritten = actions.overwrite_file(target, bytes("new"));
    std::optional<ssg::JournalDocument> document{
        saved_document("other.txt", "unchanged")};

    const auto mismatch =
        actions.restore_document(*overwritten.compensation, document);

    ASSERT_FALSE(mismatch.accepted());
    ASSERT_EQ(mismatch.error->code,
              ssg::RecoveryErrorCode::RecordKindMismatch);
    ASSERT_EQ(read_bytes(target), "new");
    ASSERT_EQ(actions.records().size(), std::size_t{1});

    const auto replacement = saved_document("other.txt", "reloaded", false);
    const auto reloaded = actions.reload_document(document, replacement);
    ASSERT_TRUE(reloaded.accepted());
    const auto inverse =
        actions.restore_filesystem(*reloaded.compensation);
    ASSERT_FALSE(inverse.accepted());
    ASSERT_EQ(inverse.error->code,
              ssg::RecoveryErrorCode::RecordKindMismatch);
    ASSERT_EQ(document,
              std::optional<ssg::JournalDocument>{replacement});
    ASSERT_EQ(actions.records().size(), std::size_t{2});
}

} // namespace

int main() {
    RUN(dirty_close_is_durable_before_removal_and_restores_exact_document);
    RUN(dirty_close_durability_failure_preserves_document_and_publishes_nothing);
    RUN(reload_compensation_survives_reconstruction_and_restores_exact_document);
    RUN(overwrite_existing_and_new_files_round_trip_to_filesystem_truth);
    RUN(rename_round_trip_restores_both_paths_and_source_identity);
    RUN(delete_tree_round_trip_restores_binary_files_and_empty_directories);
    RUN(workspace_replacement_and_compensation_match_independent_tree_snapshots);
    RUN(record_and_artifact_installation_complete_before_canonical_mutation);
    RUN(preparation_failure_preserves_canonical_state_and_existing_records);
    RUN(publication_failure_after_mutation_rolls_back_canonical_state);
    RUN(each_action_mutation_failure_rolls_back_and_discards_its_record);
    RUN(partial_workspace_mutation_failure_rolls_back_to_exact_tree);
    RUN(action_and_rollback_failure_retains_record_for_successful_retry);
    RUN(reconstruction_discards_persisted_in_progress_document_record);
    RUN(reconstruction_auto_rolls_back_persisted_in_progress_filesystem_record);
    RUN(reconstruction_discards_partially_removed_record_directory);
    RUN(rename_failed_publication_rollback_remains_safe_after_reconstruction);
    RUN(restoration_failure_keeps_record_and_retry_restores_exact_tree);
    RUN(rename_partial_restore_retry_preserves_source_identity);
    RUN(rename_completed_restore_retry_preserves_source_identity);
    RUN(cleanup_failure_keeps_restored_record_retryable);
    RUN(count_budget_evicts_oldest_only_after_new_record_is_installed);
    RUN(byte_budget_evicts_oldest_when_new_record_fits_after_eviction);
    RUN(byte_budget_rejects_before_mutating_when_newest_record_cannot_fit);
    RUN(compensation_removes_only_its_own_record_and_artifacts);
    RUN(record_kind_mismatches_are_typed_and_non_destructive);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
