#include <ssg/platform_files.h>
#include <ssg/ScratchStore.h>
#include <ssg/ScratchJournal.h>
#include "test_helpers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
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
            ssg::DocumentMode::Edit,
            true,
            std::move(contents)};
}

std::filesystem::path workspace(const TemporaryDirectory& temporary,
                                std::string_view name = "workspace") {
    return std::filesystem::absolute(temporary.path() / name).lexically_normal();
}

std::string makeRestoredRemnant(const std::filesystem::path& root,
                                  const std::filesystem::path& workspacePath,
                                  std::string contents) {
    std::string id;
    {
        auto remnant = ssg::ScratchSession::create(root, workspacePath);
        id = std::string{remnant.id().value()};
        ssg::ScratchJournal{remnant.journalPath()}.appendDocument(
            document("file.txt", std::move(contents)));
    }
    {
        auto selector = ssg::ScratchSession::create(root, workspacePath);
        auto claim = selector.claimNewestRestorable();
        if (!claim) throw std::runtime_error("failed to create test remnant");
        claim->markRestored();
    }
    return id;
}

ssg::ScratchStoreConfig configuration() {
    ssg::ScratchStoreConfig result;
    result.maximumBytes = std::numeric_limits<std::uintmax_t>::max();
    result.maximumAge = std::chrono::hours{24 * 365};
    result.compactionThresholdBytes =
        std::numeric_limits<std::uintmax_t>::max();
    result.durabilityTarget = 100ms;
    return result;
}

TEST(compactionPreservesReplayAndLeavesOneAtomicCheckpoint) {
    TemporaryDirectory temporary;
    auto store =
        ssg::ScratchStore::create(temporary.path(), workspace(temporary),
                                  configuration());
    store.updateDocument(document("a.txt", "one"));
    store.updateDocument(document("b.txt", "two"));
    store.updateDocument(document("a.txt", "three"));
    ASSERT_TRUE(store.waitUntilDurable(2s));
    const auto before = ssg::ScratchJournal{store.journalPath()}.replay();

    store.compact();
    ASSERT_TRUE(store.waitUntilDurable(2s));
    const auto after = ssg::ScratchJournal{store.journalPath()}.replay();
    ASSERT_EQ(after.recovery, before.recovery);
    ASSERT_FALSE(after.discardedTail);
    ASSERT_EQ(after.validBytes,
              ssg::JournalCodec{}.encodeCheckpoint(after.recovery).size());
}

TEST(startupImportsBeforeMarkingRemnantRestored) {
    TemporaryDirectory temporary;
    const auto workspacePath = workspace(temporary);
    std::filesystem::path remnantPath;
    {
        auto remnant =
            ssg::ScratchSession::create(temporary.path(), workspacePath);
        remnantPath = remnant.path();
        ssg::ScratchJournal{remnant.journalPath()}.appendDocument(
            document("draft.txt", "recover me"));
    }

    auto store = ssg::ScratchStore::create(
        temporary.path(), workspacePath, configuration());
    ASSERT_EQ(store.recovery().documents,
              std::vector<ssg::JournalDocument>{
                  document("draft.txt", "recover me")});
    ASSERT_TRUE(std::filesystem::exists(remnantPath / "restored"));
    ASSERT_EQ(ssg::ScratchJournal{store.journalPath()}.replay().recovery,
              store.recovery());
}

TEST(quotaEvictsOnlyRestoredRemnantsOldestFirst) {
    TemporaryDirectory temporary;
    const auto workspacePath = workspace(temporary);
    std::vector<std::string> ids;
    ids.push_back(
        makeRestoredRemnant(temporary.path(), workspacePath, "oldest"));
    ids.push_back(
        makeRestoredRemnant(temporary.path(), workspacePath, "middle"));
    ids.push_back(
        makeRestoredRemnant(temporary.path(), workspacePath, "newest"));
    ASSERT_TRUE(std::is_sorted(ids.begin(), ids.end()));

    auto config = configuration();
    config.maximumBytes = 0;
    auto store =
        ssg::ScratchStore::create(temporary.path(), workspacePath, config);
    {
        auto protectedRemnant =
            ssg::ScratchSession::create(temporary.path(), workspacePath);
        ssg::ScratchJournal{protectedRemnant.journalPath()}.appendDocument(
            document("protected.txt", "unrestored quota state"));
    }
    const auto result = store.applyQuotas();
    ASSERT_EQ(result.evictedSessionIds, ids);
    ASSERT_FALSE(result.withinByteQuota);
    ASSERT_TRUE(std::filesystem::exists(store.sessionPath()));
}

TEST(purgeWorkspaceAndAllLeaveUnrestoredState) {
    TemporaryDirectory temporary;
    const auto workspaceA = workspace(temporary, "a");
    const auto workspaceB = workspace(temporary, "b");
    makeRestoredRemnant(temporary.path(), workspaceA, "a");
    makeRestoredRemnant(temporary.path(), workspaceB, "b");
    std::filesystem::path protectedPath;
    {
        auto protectedRemnant =
            ssg::ScratchSession::create(temporary.path(), workspaceB);
        protectedPath = protectedRemnant.path();
        ssg::ScratchJournal{protectedRemnant.journalPath()}.appendDocument(
            document("protected.txt", "unrestored"));
    }

    auto store = ssg::ScratchStore::create(
        temporary.path(), workspaceA, configuration());
    ASSERT_EQ(store.purgeWorkspace(), std::size_t{1});
    ASSERT_EQ(store.purgeAll(), std::size_t{1});
    ASSERT_TRUE(std::filesystem::exists(protectedPath));
}

TEST(writeFailureIsActionableAndNeverReportsDurable) {
    TemporaryDirectory temporary;
    auto store = ssg::ScratchStore::create(
        temporary.path(), workspace(temporary), configuration());
    std::filesystem::create_directory(store.journalPath());

    store.updateDocument(document("failed.txt", "not durable"));
    ASSERT_FALSE(store.waitUntilDurable(2s));
    const auto state = store.durabilityState();
    ASSERT_EQ(state.kind, ssg::ScratchDurability::Failed);
    ASSERT_EQ(state.acceptedGeneration, std::uint64_t{1});
    ASSERT_EQ(state.durableGeneration, std::uint64_t{0});
    ASSERT_FALSE(state.failure.empty());
}

TEST(shutdownDrainsAndRejectsNewMutations) {
    TemporaryDirectory temporary;
    auto store = ssg::ScratchStore::create(
        temporary.path(), workspace(temporary), configuration());
    store.updateDocument(document("drain.txt", "accepted"));
    store.shutdown();

    ASSERT_EQ(ssg::ScratchJournal{store.journalPath()}.replay().recovery,
              store.recovery());
    ASSERT_THROWS(store.updateDocument(document("late.txt", "rejected")),
                  std::logic_error);
}

} // namespace

SSG_TEST_SUITE(test_scratch) {
    RUN(compactionPreservesReplayAndLeavesOneAtomicCheckpoint);
    RUN(startupImportsBeforeMarkingRemnantRestored);
    RUN(quotaEvictsOnlyRestoredRemnantsOldestFirst);
    RUN(purgeWorkspaceAndAllLeaveUnrestoredState);
    RUN(writeFailureIsActionableAndNeverReportsDurable);
    RUN(shutdownDrainsAndRejectsNewMutations);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
