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

std::filesystem::path journalPath(
    const TemporaryDirectory& temporary,
    const std::filesystem::path& workspacePath) {
    const auto sessions =
        temporary.path() / "workspaces" /
        ssg::scratchWorkspaceKey(workspacePath) / "sessions";
    const auto entries = ssg::listDirectory(sessions);
    if (!entries.ok() || entries.entries.size() != 1) {
        throw std::runtime_error("expected one scratch session");
    }
    return entries.entries.front().path() / "journal.bin";
}

ssg::ScratchStoreConfig configuration() {
    ssg::ScratchStoreConfig result;
    result.compactionThresholdBytes =
        std::numeric_limits<std::uintmax_t>::max();
    result.durabilityTarget = 100ms;
    return result;
}

TEST(compactionPreservesReplayAndLeavesOneAtomicCheckpoint) {
    TemporaryDirectory temporary;
    const auto workspacePath = workspace(temporary);
    auto config = configuration();
    config.compactionThresholdBytes = 1;
    auto store =
        ssg::ScratchStore::create(temporary.path(), workspacePath, config);
    store.updateDocument(document("a.txt", "one"));
    store.updateDocument(document("b.txt", "two"));
    store.updateDocument(document("a.txt", "three"));
    ASSERT_TRUE(store.waitUntilDurable(2s));
    const auto after =
        ssg::ScratchJournal{journalPath(temporary, workspacePath)}.replay();
    ASSERT_EQ(after.recovery, store.recovery());
    ASSERT_FALSE(after.discardedTail);
    ASSERT_EQ(after.validBytes,
              ssg::encodeJournalCheckpoint(after.recovery).size());
}

TEST(startupImportsBeforeMarkingRemnantRestored) {
    TemporaryDirectory temporary;
    const auto workspacePath = workspace(temporary);
    std::filesystem::path remnantPath;
    {
        auto remnant =
            ssg::ScratchSession::create(temporary.path(), workspacePath);
        remnantPath = remnant.journalPath().parent_path();
        ssg::ScratchJournal{remnant.journalPath()}.appendDocument(
            document("draft.txt", "recover me"));
    }

    auto store = ssg::ScratchStore::create(
        temporary.path(), workspacePath, configuration());
    ASSERT_EQ(store.recovery().documents,
              std::vector<ssg::JournalDocument>{
                  document("draft.txt", "recover me")});
    ASSERT_TRUE(std::filesystem::exists(remnantPath / "restored"));
}

TEST(writeFailureIsActionableAndNeverReportsDurable) {
    TemporaryDirectory temporary;
    const auto workspacePath = workspace(temporary);
    auto store = ssg::ScratchStore::create(
        temporary.path(), workspacePath, configuration());
    std::filesystem::create_directory(
        journalPath(temporary, workspacePath));

    store.updateDocument(document("failed.txt", "not durable"));
    ASSERT_FALSE(store.waitUntilDurable(2s));
    const auto state = store.durabilityState();
    ASSERT_EQ(state.kind, ssg::ScratchDurability::Failed);
    ASSERT_EQ(state.acceptedGeneration, std::uint64_t{1});
    ASSERT_EQ(state.durableGeneration, std::uint64_t{0});
    ASSERT_FALSE(state.failure.empty());
}

TEST(destructionDrainsAcceptedMutations) {
    TemporaryDirectory temporary;
    const auto workspacePath = workspace(temporary);
    std::filesystem::path path;
    {
        auto store = ssg::ScratchStore::create(
            temporary.path(), workspacePath, configuration());
        path = journalPath(temporary, workspacePath);
        store.updateDocument(document("drain.txt", "accepted"));
    }
    ASSERT_EQ(ssg::ScratchJournal{path}.replay().recovery.documents,
              std::vector<ssg::JournalDocument>{
                  document("drain.txt", "accepted")});
}

} // namespace

SSG_TEST_SUITE(test_scratch) {
    RUN(compactionPreservesReplayAndLeavesOneAtomicCheckpoint);
    RUN(startupImportsBeforeMarkingRemnantRestored);
    RUN(writeFailureIsActionableAndNeverReportsDurable);
    RUN(destructionDrainsAcceptedMutations);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
