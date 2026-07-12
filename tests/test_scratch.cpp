#include "ssg/platform_files.h"
#include "ssg/scratch.h"
#include "ssg/scratch_journal.h"
#include "ssg/scratch_session.h"
#include "test_helpers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{0};
        path_ = std::filesystem::temp_directory_path() /
                ("ssg-scratch-" +
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

ssg::JournalDocument document(std::string path, std::string contents) {
    return {ssg::JournalDocumentKey::saved(path),
            ssg::DocumentMode::edit,
            true,
            std::move(contents)};
}

std::filesystem::path workspace(const TemporaryDirectory& temporary,
                                std::string_view name = "workspace") {
    return std::filesystem::absolute(temporary.path() / name).lexically_normal();
}

std::string make_restored_remnant(const std::filesystem::path& root,
                                  const std::filesystem::path& workspace_path,
                                  std::string contents) {
    std::string id;
    {
        auto remnant = ssg::ScratchSession::create(root, workspace_path);
        id = std::string{remnant.id().value()};
        ssg::ScratchJournal{remnant.journal_path()}.append_document(
            document("file.txt", std::move(contents)));
    }
    {
        auto selector = ssg::ScratchSession::create(root, workspace_path);
        auto claim = selector.claim_newest_restorable();
        if (!claim) throw std::runtime_error("failed to create test remnant");
        claim->mark_restored();
    }
    return id;
}

class TestStorage final : public ssg::ScratchStorage {
public:
    void append_document(const std::filesystem::path& path,
                         const ssg::JournalDocument& value) override {
        before_write();
        ssg::ScratchJournal{path}.append_document(value);
    }

    void append_remove(const std::filesystem::path& path,
                       const ssg::JournalDocumentKey& key) override {
        before_write();
        ssg::ScratchJournal{path}.append_remove(key);
    }

    void replace_checkpoint(
        const std::filesystem::path& path,
        const ssg::JournalRecoverySet& recovery) override {
        before_write();
        const auto bytes = ssg::encode_checkpoint_record(recovery);
        ssg::replace_file_atomically(path, bytes);
    }

    std::chrono::milliseconds delay{};
    bool fail = false;

private:
    void before_write() const {
        if (delay.count() != 0) std::this_thread::sleep_for(delay);
        if (fail) throw std::runtime_error("injected scratch write failure");
    }
};

ssg::ScratchStoreConfig configuration() {
    ssg::ScratchStoreConfig result;
    result.maximum_bytes = std::numeric_limits<std::uintmax_t>::max();
    result.maximum_age = std::chrono::hours{24 * 365};
    result.compaction_threshold_bytes =
        std::numeric_limits<std::uintmax_t>::max();
    result.durability_target = 100ms;
    return result;
}

TEST(compaction_preserves_replay_and_leaves_one_atomic_checkpoint) {
    TemporaryDirectory temporary;
    auto store =
        ssg::ScratchStore::create(temporary.path(), workspace(temporary),
                                  configuration());
    store.update_document(document("a.txt", "one"));
    store.update_document(document("b.txt", "two"));
    store.update_document(document("a.txt", "three"));
    ASSERT_TRUE(store.wait_until_durable(2s));
    const auto before = ssg::ScratchJournal{store.journal_path()}.replay();

    store.compact();
    ASSERT_TRUE(store.wait_until_durable(2s));
    const auto after = ssg::ScratchJournal{store.journal_path()}.replay();
    ASSERT_EQ(after.recovery, before.recovery);
    ASSERT_FALSE(after.discarded_tail);
    ASSERT_EQ(after.valid_bytes,
              ssg::encode_checkpoint_record(after.recovery).size());
}

TEST(startup_imports_before_marking_remnant_restored) {
    TemporaryDirectory temporary;
    const auto workspace_path = workspace(temporary);
    std::filesystem::path remnant_path;
    {
        auto remnant =
            ssg::ScratchSession::create(temporary.path(), workspace_path);
        remnant_path = remnant.path();
        ssg::ScratchJournal{remnant.journal_path()}.append_document(
            document("draft.txt", "recover me"));
    }

    auto store = ssg::ScratchStore::create(
        temporary.path(), workspace_path, configuration());
    ASSERT_EQ(store.recovery().documents,
              std::vector<ssg::JournalDocument>{
                  document("draft.txt", "recover me")});
    ASSERT_TRUE(std::filesystem::exists(remnant_path / "restored"));
    ASSERT_EQ(ssg::ScratchJournal{store.journal_path()}.replay().recovery,
              store.recovery());
}

TEST(failed_import_keeps_remnant_retryable) {
    TemporaryDirectory temporary;
    const auto workspace_path = workspace(temporary);
    std::filesystem::path remnant_path;
    {
        auto remnant =
            ssg::ScratchSession::create(temporary.path(), workspace_path);
        remnant_path = remnant.path();
        ssg::ScratchJournal{remnant.journal_path()}.append_document(
            document("draft.txt", "retry me"));
    }
    TestStorage storage;
    storage.fail = true;

    ASSERT_THROWS(ssg::ScratchStore::create(
                      temporary.path(), workspace_path, configuration(), storage),
                  std::runtime_error);
    ASSERT_FALSE(std::filesystem::exists(remnant_path / "restored"));
}

TEST(quota_evicts_only_restored_remnants_oldest_first) {
    TemporaryDirectory temporary;
    const auto workspace_path = workspace(temporary);
    std::vector<std::string> ids;
    ids.push_back(
        make_restored_remnant(temporary.path(), workspace_path, "oldest"));
    ids.push_back(
        make_restored_remnant(temporary.path(), workspace_path, "middle"));
    ids.push_back(
        make_restored_remnant(temporary.path(), workspace_path, "newest"));
    ASSERT_TRUE(std::is_sorted(ids.begin(), ids.end()));

    auto config = configuration();
    config.maximum_bytes = 0;
    auto store =
        ssg::ScratchStore::create(temporary.path(), workspace_path, config);
    {
        auto protected_remnant =
            ssg::ScratchSession::create(temporary.path(), workspace_path);
        ssg::ScratchJournal{protected_remnant.journal_path()}.append_document(
            document("protected.txt", "unrestored quota state"));
    }
    const auto result = store.apply_quotas();
    ASSERT_EQ(result.evicted_session_ids, ids);
    ASSERT_FALSE(result.within_byte_quota);
    ASSERT_TRUE(std::filesystem::exists(store.session_path()));
}

TEST(purge_workspace_and_all_leave_unrestored_state) {
    TemporaryDirectory temporary;
    const auto workspace_a = workspace(temporary, "a");
    const auto workspace_b = workspace(temporary, "b");
    make_restored_remnant(temporary.path(), workspace_a, "a");
    make_restored_remnant(temporary.path(), workspace_b, "b");
    std::filesystem::path protected_path;
    {
        auto protected_remnant =
            ssg::ScratchSession::create(temporary.path(), workspace_b);
        protected_path = protected_remnant.path();
        ssg::ScratchJournal{protected_remnant.journal_path()}.append_document(
            document("protected.txt", "unrestored"));
    }

    auto store = ssg::ScratchStore::create(
        temporary.path(), workspace_a, configuration());
    ASSERT_EQ(store.purge_workspace(), std::size_t{1});
    ASSERT_EQ(store.purge_all(), std::size_t{1});
    ASSERT_TRUE(std::filesystem::exists(protected_path));
}

TEST(write_failure_is_actionable_and_never_reports_durable) {
    TemporaryDirectory temporary;
    TestStorage storage;
    storage.fail = true;
    auto store = ssg::ScratchStore::create(
        temporary.path(), workspace(temporary), configuration(), storage);

    store.update_document(document("failed.txt", "not durable"));
    ASSERT_FALSE(store.wait_until_durable(2s));
    const auto state = store.durability_state();
    ASSERT_EQ(state.kind, ssg::ScratchDurability::failed);
    ASSERT_EQ(state.accepted_generation, std::uint64_t{1});
    ASSERT_EQ(state.durable_generation, std::uint64_t{0});
    ASSERT_FALSE(state.failure.empty());
}

TEST(hundred_millisecond_lag_is_observable_until_durable) {
    TemporaryDirectory temporary;
    TestStorage storage;
    storage.delay = 175ms;
    auto store = ssg::ScratchStore::create(
        temporary.path(), workspace(temporary), configuration(), storage);

    store.update_document(document("slow.txt", "pending"));
    ASSERT_EQ(store.durability_state().kind,
              ssg::ScratchDurability::pending);
    std::this_thread::sleep_for(125ms);
    const auto lagged = store.durability_state();
    ASSERT_EQ(lagged.kind, ssg::ScratchDurability::pending);
    ASSERT_TRUE(lagged.overdue);
    ASSERT_TRUE(store.wait_until_durable(2s));
    ASSERT_EQ(store.durability_state().kind,
              ssg::ScratchDurability::durable);
}

TEST(shutdown_drains_and_rejects_new_mutations) {
    TemporaryDirectory temporary;
    TestStorage storage;
    storage.delay = 25ms;
    auto store = ssg::ScratchStore::create(
        temporary.path(), workspace(temporary), configuration(), storage);
    store.update_document(document("drain.txt", "accepted"));
    store.shutdown();

    ASSERT_EQ(ssg::ScratchJournal{store.journal_path()}.replay().recovery,
              store.recovery());
    ASSERT_THROWS(store.update_document(document("late.txt", "rejected")),
                  std::logic_error);
}

} // namespace

int main() {
    RUN(compaction_preserves_replay_and_leaves_one_atomic_checkpoint);
    RUN(startup_imports_before_marking_remnant_restored);
    RUN(failed_import_keeps_remnant_retryable);
    RUN(quota_evicts_only_restored_remnants_oldest_first);
    RUN(purge_workspace_and_all_leave_unrestored_state);
    RUN(write_failure_is_actionable_and_never_reports_durable);
    RUN(hundred_millisecond_lag_is_observable_until_durable);
    RUN(shutdown_drains_and_rejects_new_mutations);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
