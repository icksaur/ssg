#include <ssg/platform_files.h>
#include <ssg/RecoveryManager.h>
#include <ssg/ScratchStore.h>
#include <ssg/ScratchJournal.h>
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
        path_ = testRuntimePath(
            ".ssg-recovery-test-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
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

void writeBytes(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("failed to create test file");
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) throw std::runtime_error("failed to write test file");
}

std::string readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to read test file");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

enum class NodeKind {
    Missing,
    Directory,
    RegularFile,
    Symlink,
};

struct TreeNode {
    std::string relativePath;
    NodeKind kind;
    std::string value;

    friend bool operator==(const TreeNode&, const TreeNode&) = default;
};

std::vector<TreeNode> snapshotTree(const std::filesystem::path& root) {
    std::error_code error;
    const auto rootStatus = std::filesystem::symlink_status(root, error);
    if (error || rootStatus.type() == std::filesystem::file_type::not_found) {
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
                {relative, NodeKind::RegularFile, readBytes(path)});
        }
    };

    append(root, rootStatus, result);
    if (std::filesystem::is_directory(rootStatus)) {
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(root)) {
            append(entry.path(), entry.symlink_status(), result);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const TreeNode& left, const TreeNode& right) {
                  return left.relativePath < right.relativePath;
              });
    return result;
}

ssg::UntitledDocumentId fixedUntitledId() {
    std::array<std::byte, 16> value{};
    value[0] = std::byte{0x42};
    value[15] = std::byte{0x7f};
    return ssg::UntitledDocumentId{value};
}

ssg::JournalDocument savedDocument(std::string path,
                                    std::string contents,
                                    bool dirty = true) {
    return {ssg::JournalDocumentKey::saved(path),
            ssg::DocumentMode::Edit,
            dirty,
            std::move(contents)};
}

ssg::RecoveryConfig recoveryConfig(
    std::size_t maximumRecords = 16,
    std::uintmax_t maximumBytes =
        std::numeric_limits<std::uintmax_t>::max()) {
    return {maximumRecords, maximumBytes};
}

ssg::ScratchStoreConfig scratchConfig() {
    ssg::ScratchStoreConfig result;
    result.maximumBytes = std::numeric_limits<std::uintmax_t>::max();
    result.maximumAge = std::chrono::hours{24 * 365};
    result.compactionThresholdBytes =
        std::numeric_limits<std::uintmax_t>::max();
    result.durabilityTarget = 100ms;
    return result;
}

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

    void clearFailures() { rules_.clear(); }
    void clearEvents() { events.clear(); }

    void beforeStep(ssg::RecoveryStep step) override {
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

std::size_t eventIndex(const std::vector<ssg::RecoveryStep>& events,
                        ssg::RecoveryStep step) {
    const auto found = std::find(events.begin(), events.end(), step);
    return found == events.end()
               ? std::numeric_limits<std::size_t>::max()
               : static_cast<std::size_t>(found - events.begin());
}

TEST(dirtyCloseIsDurableBeforeRemovalAndRestoresExactDocument) {
    TemporaryDirectory temporary;
    const auto workspace =
        std::filesystem::absolute(temporary.path() / "workspace");
    std::filesystem::create_directories(workspace);
    auto scratch = ssg::ScratchStore::create(
        temporary.path() / "scratch", workspace, scratchConfig());
    auto actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig());
    const ssg::JournalDocument expected{
        ssg::JournalDocumentKey::untitled(fixedUntitledId()),
        ssg::DocumentMode::ReadOnly,
        true,
        "dirty \xCE\xB2 draft\n"};
    std::optional<ssg::JournalDocument> document{expected};

    const auto closed = actions.closeDocument(document, scratch, 2s);

    ASSERT_TRUE(closed.accepted());
    ASSERT_TRUE(closed.compensation.has_value());
    ASSERT_FALSE(document.has_value());
    ASSERT_EQ(scratch.recovery().documents,
              std::vector<ssg::JournalDocument>{expected});

    actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig());
    const auto reconstructedRecords = actions.records();
    ASSERT_EQ(reconstructedRecords.front().document,
              std::optional<ssg::JournalDocumentKey>{expected.key});
    const auto restored =
        actions.restoreDocument(*closed.compensation, document);
    ASSERT_TRUE(restored.accepted());
    ASSERT_EQ(document, std::optional<ssg::JournalDocument>{expected});
    ASSERT_TRUE(actions.records().empty());
}

TEST(dirtyCloseDurabilityFailurePreservesDocumentAndPublishesNothing) {
    TemporaryDirectory temporary;
    const auto workspace =
        std::filesystem::absolute(temporary.path() / "workspace");
    std::filesystem::create_directories(workspace);
    auto scratch = ssg::ScratchStore::create(
        temporary.path() / "scratch", workspace, scratchConfig());
    std::filesystem::create_directory(scratch.journalPath());
    auto actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig());
    const auto expected = savedDocument("draft.txt", "not durable");
    std::optional<ssg::JournalDocument> document{expected};

    const auto closed = actions.closeDocument(document, scratch, 2s);

    ASSERT_FALSE(closed.accepted());
    ASSERT_EQ(closed.error->code, ssg::RecoveryErrorCode::DurabilityFailed);
    ASSERT_EQ(document, std::optional<ssg::JournalDocument>{expected});
    ASSERT_TRUE(actions.records().empty());
}

TEST(renameRoundTripRestoresBothPathsAndSourceIdentity) {
    TemporaryDirectory temporary;
    const auto canonical = temporary.path() / "canonical";
    const auto source = canonical / "source.txt";
    const auto destination = canonical / "destination.txt";
    writeBytes(source, "source bytes");
    writeBytes(destination, "destination bytes");
    const auto before = snapshotTree(canonical);
    const auto sourceIdentity = ssg::statFile(source)->identity;
    auto actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig());

    const auto renamed = actions.renamePath(source, destination);

    ASSERT_TRUE(renamed.accepted());
    ASSERT_FALSE(std::filesystem::exists(source));
    ASSERT_EQ(readBytes(destination), "source bytes");
    ASSERT_EQ(ssg::statFile(destination)->identity, sourceIdentity);
    actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig());
    const auto reconstructedRecords = actions.records();
    ASSERT_EQ(reconstructedRecords.front().affectedPaths,
              (std::vector<std::filesystem::path>{source, destination}));
    ASSERT_TRUE(
        actions.restoreFilesystem(*renamed.compensation).accepted());
    ASSERT_EQ(snapshotTree(canonical), before);
    ASSERT_EQ(ssg::statFile(source)->identity, sourceIdentity);
}

TEST(deleteTreeRoundTripRestoresBinaryFilesAndEmptyDirectories) {
    TemporaryDirectory temporary;
    const auto removed = temporary.path() / "canonical" / "removed";
    std::filesystem::create_directories(removed / "empty");
    writeBytes(removed / "nested" / "binary",
                std::string{"a\0b\xff", 4});
    writeBytes(removed / "root.txt", "root");
    const auto before = snapshotTree(removed);
    auto actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig());

    const auto deleted = actions.deletePath(removed);

    ASSERT_TRUE(deleted.accepted());
    ASSERT_FALSE(std::filesystem::exists(removed));
    actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig());
    ASSERT_TRUE(
        actions.restoreFilesystem(*deleted.compensation).accepted());
    ASSERT_EQ(snapshotTree(removed), before);
}

TEST(recordAndArtifactInstallationCompleteBeforeCanonicalMutation) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "canonical" / "document";
    writeBytes(target, "old");
    InjectedRecoveryFailures injection;
    auto actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig(), injection);

    const auto deleted = actions.deletePath(target);

    ASSERT_TRUE(deleted.accepted());
    const auto artifact =
        eventIndex(injection.events, ssg::RecoveryStep::PrepareArtifact);
    const auto installed =
        eventIndex(injection.events, ssg::RecoveryStep::InstallRecord);
    const auto mutation =
        eventIndex(injection.events, ssg::RecoveryStep::MutateFilesystem);
    const auto published =
        eventIndex(injection.events, ssg::RecoveryStep::PublishRecord);
    ASSERT_TRUE(artifact < installed);
    ASSERT_TRUE(installed < mutation);
    ASSERT_TRUE(mutation < published);
}

TEST(preparationFailurePreservesCanonicalStateAndExistingRecords) {
    TemporaryDirectory temporary;
    const auto first = temporary.path() / "canonical" / "first";
    const auto second = temporary.path() / "canonical" / "second";
    writeBytes(first, "first old");
    writeBytes(second, "second old");
    InjectedRecoveryFailures injection;
    auto actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig(1), injection);
    const auto retained = actions.deletePath(first);
    ASSERT_TRUE(retained.accepted());
    injection.fail(ssg::RecoveryStep::InstallRecord);

    const auto rejected = actions.deletePath(second);

    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error->code,
              ssg::RecoveryErrorCode::PreparationFailed);
    ASSERT_FALSE(rejected.compensation.has_value());
    ASSERT_EQ(readBytes(second), "second old");
    const auto records = actions.records();
    ASSERT_EQ(records.size(), std::size_t{1});
    ASSERT_EQ(records.front().id, *retained.compensation);
}

TEST(publicationFailureAfterMutationRollsBackCanonicalState) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "canonical" / "document";
    writeBytes(target, "old");
    const auto before = snapshotTree(target);
    InjectedRecoveryFailures injection;
    injection.fail(ssg::RecoveryStep::PublishRecord);
    auto actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig(), injection);

    const auto result = actions.deletePath(target);

    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.error->code, ssg::RecoveryErrorCode::ActionFailed);
    ASSERT_EQ(snapshotTree(target), before);
    ASSERT_TRUE(eventIndex(injection.events,
                            ssg::RecoveryStep::RollbackFilesystem) !=
                std::numeric_limits<std::size_t>::max());
    ASSERT_TRUE(actions.records().empty());
}

TEST(eachActionMutationFailureRollsBackAndDiscardsItsRecord) {
    TemporaryDirectory temporary;

    {
        const auto workspace =
            std::filesystem::absolute(temporary.path() / "close-workspace");
        std::filesystem::create_directories(workspace);
        auto scratch = ssg::ScratchStore::create(
            temporary.path() / "close-scratch", workspace, scratchConfig());
        InjectedRecoveryFailures injection;
        injection.fail(ssg::RecoveryStep::MutateDocument);
        auto actions = ssg::RecoveryManager::create(
            temporary.path() / "close-recovery", recoveryConfig(), injection);
        const auto expected = savedDocument("close.txt", "dirty");
        std::optional<ssg::JournalDocument> document{expected};

        const auto result = actions.closeDocument(document, scratch, 2s);

        ASSERT_FALSE(result.accepted());
        ASSERT_EQ(result.error->code, ssg::RecoveryErrorCode::ActionFailed);
        ASSERT_EQ(document, std::optional<ssg::JournalDocument>{expected});
        ASSERT_TRUE(eventIndex(injection.events,
                                ssg::RecoveryStep::RollbackDocument) !=
                    std::numeric_limits<std::size_t>::max());
        ASSERT_TRUE(actions.records().empty());
    }

    const auto assertFilesystemActionFailure =
        [&](std::string_view name, const auto& invoke) {
            InjectedRecoveryFailures injection;
            injection.fail(ssg::RecoveryStep::MutateFilesystem);
            auto actions = ssg::RecoveryManager::create(
                temporary.path() / std::string{name}, recoveryConfig(),
                injection);
            const auto result = invoke(actions);
            ASSERT_FALSE(result.accepted());
            ASSERT_EQ(result.error->code,
                      ssg::RecoveryErrorCode::ActionFailed);
            ASSERT_TRUE(eventIndex(injection.events,
                                    ssg::RecoveryStep::RollbackFilesystem) !=
                        std::numeric_limits<std::size_t>::max());
            ASSERT_TRUE(actions.records().empty());
        };

    const auto renameSource = temporary.path() / "canonical" / "rename-source";
    const auto renameDestination =
        temporary.path() / "canonical" / "rename-destination";
    writeBytes(renameSource, "source");
    writeBytes(renameDestination, "destination");
    const auto renameBefore = snapshotTree(temporary.path() / "canonical");
    assertFilesystemActionFailure(
        "rename-recovery", [&](ssg::RecoveryManager& actions) {
            return actions.renamePath(renameSource, renameDestination);
        });
    ASSERT_EQ(snapshotTree(temporary.path() / "canonical"), renameBefore);

    const auto removed = temporary.path() / "canonical" / "removed";
    writeBytes(removed / "nested", "remove");
    const auto removedBefore = snapshotTree(removed);
    assertFilesystemActionFailure(
        "delete-recovery", [&](ssg::RecoveryManager& actions) {
            return actions.deletePath(removed);
        });
    ASSERT_EQ(snapshotTree(removed), removedBefore);
}

TEST(reconstructionDiscardsPersistedInProgressDocumentRecord) {
    TemporaryDirectory temporary;
    const auto recoveryRoot = temporary.path() / "recovery";
    const auto workspace =
        std::filesystem::absolute(temporary.path() / "workspace");
    std::filesystem::create_directories(workspace);
    auto scratch = ssg::ScratchStore::create(
        temporary.path() / "scratch", workspace, scratchConfig());
    const auto expected = savedDocument("notes.txt", "unsaved text");
    std::optional<ssg::JournalDocument> document{expected};
    {
        InjectedRecoveryFailures injection;
        injection.fail(ssg::RecoveryStep::MutateDocument);
        injection.fail(ssg::RecoveryStep::RollbackDocument);
        auto actions = ssg::RecoveryManager::create(
            recoveryRoot, recoveryConfig(), injection);

        const auto result = actions.closeDocument(document, scratch, 2s);

        ASSERT_FALSE(result.accepted());
        ASSERT_EQ(result.error->code,
                  ssg::RecoveryErrorCode::ActionAndRollbackFailed);
        ASSERT_TRUE(result.compensation.has_value());
        ASSERT_EQ(actions.records().size(), std::size_t{1});
        ASSERT_EQ(document, std::optional<ssg::JournalDocument>{expected});
    }

    auto reconstructed =
        ssg::RecoveryManager::create(recoveryRoot, recoveryConfig());
    ASSERT_TRUE(reconstructed.records().empty());
    ASSERT_TRUE(std::filesystem::is_empty(recoveryRoot));
}

TEST(reconstructionDiscardsPartiallyRemovedRecordDirectory) {
    TemporaryDirectory temporary;
    const auto recoveryRoot = temporary.path() / "recovery";
    writeBytes(recoveryRoot / "partial-record" / "artifacts" / "0",
                "orphaned artifact");

    bool reconstructed = false;
    try {
        auto actions =
            ssg::RecoveryManager::create(recoveryRoot, recoveryConfig());
        reconstructed = actions.records().empty();
    } catch (...) {
    }

    ASSERT_TRUE(reconstructed);
    ASSERT_TRUE(std::filesystem::is_empty(recoveryRoot));
}

TEST(renameFailedPublicationRollbackRemainsSafeAfterReconstruction) {
    TemporaryDirectory temporary;
    const auto recoveryRoot = temporary.path() / "recovery";
    const auto canonical = temporary.path() / "canonical";
    const auto source = canonical / "source.txt";
    const auto destination = canonical / "destination.txt";
    writeBytes(source, "source bytes");
    writeBytes(destination, "destination bytes");
    const auto before = snapshotTree(canonical);
    const auto sourceIdentity = ssg::statFile(source)->identity;
    {
        InjectedRecoveryFailures injection;
        injection.fail(ssg::RecoveryStep::PublishRecord, 1);
        injection.fail(ssg::RecoveryStep::CleanupRecord);
        auto actions = ssg::RecoveryManager::create(
            recoveryRoot, recoveryConfig(), injection);

        const auto result = actions.renamePath(source, destination);

        ASSERT_FALSE(result.accepted());
        ASSERT_TRUE(result.error.has_value());
        if (result.error) {
            ASSERT_EQ(result.error->code, ssg::RecoveryErrorCode::CleanupFailed);
        }
        ASSERT_TRUE(result.compensation.has_value());
        ASSERT_EQ(snapshotTree(canonical), before);
    }

    auto reconstructed =
        ssg::RecoveryManager::create(recoveryRoot, recoveryConfig());
    ASSERT_TRUE(reconstructed.records().empty());
    ASSERT_EQ(snapshotTree(canonical), before);
    ASSERT_TRUE(std::filesystem::exists(source));
    if (std::filesystem::exists(source)) {
        ASSERT_EQ(ssg::statFile(source)->identity, sourceIdentity);
    }
}

TEST(restorationFailureKeepsRecordAndRetryRestoresExactTree) {
    TemporaryDirectory temporary;
    const auto removed = temporary.path() / "canonical" / "removed";
    writeBytes(removed / "nested.txt", "recover");
    const auto before = snapshotTree(removed);
    InjectedRecoveryFailures injection;
    auto actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig(), injection);
    const auto deleted = actions.deletePath(removed);
    ASSERT_TRUE(deleted.accepted());
    injection.fail(ssg::RecoveryStep::RestoreFilesystem);

    const auto failedRestore =
        actions.restoreFilesystem(*deleted.compensation);

    ASSERT_FALSE(failedRestore.accepted());
    ASSERT_EQ(failedRestore.error->code,
              ssg::RecoveryErrorCode::RestorationFailed);
    ASSERT_EQ(actions.records().size(), std::size_t{1});
    injection.clearFailures();
    ASSERT_TRUE(
        actions.restoreFilesystem(*deleted.compensation).accepted());
    ASSERT_EQ(snapshotTree(removed), before);
    ASSERT_TRUE(actions.records().empty());
}

TEST(renamePartialRestoreRetryPreservesSourceIdentity) {
    TemporaryDirectory temporary;
    const auto source = temporary.path() / "canonical" / "source.txt";
    const auto destination =
        temporary.path() / "canonical" / "destination.txt";
    writeBytes(source, "source bytes");
    writeBytes(destination, "destination bytes");
    const auto before = snapshotTree(temporary.path() / "canonical");
    const auto sourceIdentity = ssg::statFile(source)->identity;
    InjectedRecoveryFailures injection;
    auto actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig(), injection);
    const auto renamed = actions.renamePath(source, destination);
    ASSERT_TRUE(renamed.accepted());
    injection.fail(ssg::RecoveryStep::RestoreFilesystem, 1);

    const auto failedRestore =
        actions.restoreFilesystem(*renamed.compensation);

    ASSERT_FALSE(failedRestore.accepted());
    ASSERT_TRUE(failedRestore.error.has_value());
    if (failedRestore.error) {
        ASSERT_EQ(failedRestore.error->code,
                  ssg::RecoveryErrorCode::RestorationFailed);
    }
    ASSERT_EQ(ssg::statFile(source)->identity, sourceIdentity);
    ASSERT_FALSE(std::filesystem::exists(destination));
    ASSERT_EQ(actions.records().size(), std::size_t{1});

    injection.clearFailures();
    ASSERT_TRUE(
        actions.restoreFilesystem(*renamed.compensation).accepted());
    ASSERT_EQ(snapshotTree(temporary.path() / "canonical"), before);
    ASSERT_EQ(ssg::statFile(source)->identity, sourceIdentity);
}

TEST(renameCompletedRestoreRetryPreservesSourceIdentity) {
    TemporaryDirectory temporary;
    const auto source = temporary.path() / "canonical" / "source.txt";
    const auto destination =
        temporary.path() / "canonical" / "destination.txt";
    writeBytes(source, "source bytes");
    writeBytes(destination, "destination bytes");
    const auto before = snapshotTree(temporary.path() / "canonical");
    const auto sourceIdentity = ssg::statFile(source)->identity;
    InjectedRecoveryFailures injection;
    auto actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig(), injection);
    const auto renamed = actions.renamePath(source, destination);
    ASSERT_TRUE(renamed.accepted());
    injection.fail(ssg::RecoveryStep::RestoreFilesystem, 2);

    const auto failedRestore =
        actions.restoreFilesystem(*renamed.compensation);

    ASSERT_FALSE(failedRestore.accepted());
    ASSERT_TRUE(failedRestore.error.has_value());
    ASSERT_EQ(snapshotTree(temporary.path() / "canonical"), before);
    ASSERT_EQ(ssg::statFile(source)->identity, sourceIdentity);
    ASSERT_EQ(actions.records().size(), std::size_t{1});

    injection.clearFailures();
    ASSERT_TRUE(
        actions.restoreFilesystem(*renamed.compensation).accepted());
    ASSERT_EQ(snapshotTree(temporary.path() / "canonical"), before);
    ASSERT_EQ(ssg::statFile(source)->identity, sourceIdentity);
}

TEST(cleanupFailureKeepsRestoredRecordRetryable) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "canonical" / "document";
    writeBytes(target, "old");
    InjectedRecoveryFailures injection;
    auto actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig(), injection);
    const auto deleted = actions.deletePath(target);
    ASSERT_TRUE(deleted.accepted());
    injection.fail(ssg::RecoveryStep::CleanupRecord);

    const auto cleanupFailed =
        actions.restoreFilesystem(*deleted.compensation);

    ASSERT_FALSE(cleanupFailed.accepted());
    ASSERT_EQ(cleanupFailed.error->code,
              ssg::RecoveryErrorCode::CleanupFailed);
    ASSERT_EQ(readBytes(target), "old");
    ASSERT_EQ(actions.records().size(), std::size_t{1});
    injection.clearFailures();
    ASSERT_TRUE(
        actions.restoreFilesystem(*deleted.compensation).accepted());
    ASSERT_EQ(readBytes(target), "old");
    ASSERT_TRUE(actions.records().empty());
}

TEST(countBudgetEvictsOldestOnlyAfterNewRecordIsInstalled) {
    TemporaryDirectory temporary;
    const auto canonical = temporary.path() / "canonical";
    const auto a = canonical / "a";
    const auto b = canonical / "b";
    const auto c = canonical / "c";
    writeBytes(a, "a0");
    writeBytes(b, "b0");
    writeBytes(c, "c0");
    auto actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig(2));

    const auto first = actions.deletePath(a);
    const auto second = actions.deletePath(b);
    const auto third = actions.deletePath(c);

    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(second.accepted());
    ASSERT_TRUE(third.accepted());
    const auto records = actions.records();
    ASSERT_EQ(records.size(), std::size_t{2});
    ASSERT_EQ(records[0].id, *second.compensation);
    ASSERT_EQ(records[1].id, *third.compensation);
    const auto evicted = actions.restoreFilesystem(*first.compensation);
    ASSERT_FALSE(evicted.accepted());
    ASSERT_EQ(evicted.error->code, ssg::RecoveryErrorCode::RecordNotFound);
}

TEST(byteBudgetEvictsOldestWhenNewRecordFitsAfterEviction) {
    TemporaryDirectory temporary;
    const auto recoveryRoot = temporary.path() / "recovery";
    const auto a = temporary.path() / "canonical" / "a.bin";
    const auto b = temporary.path() / "canonical" / "b.bin";
    const auto c = temporary.path() / "canonical" / "c.bin";
    const std::string original(256, 'o');
    writeBytes(a, original);
    writeBytes(b, original);
    writeBytes(c, original);
    auto actions =
        ssg::RecoveryManager::create(recoveryRoot, recoveryConfig());
    const auto first = actions.deletePath(a);
    const auto second = actions.deletePath(b);
    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(second.accepted());
    const auto initialRecords = actions.records();
    const auto twoRecordBudget =
        initialRecords[0].storedBytes + initialRecords[1].storedBytes;
    actions = ssg::RecoveryManager::create(
        recoveryRoot, recoveryConfig(8, twoRecordBudget));

    const auto third = actions.deletePath(c);

    ASSERT_TRUE(third.accepted());
    const auto retained = actions.records();
    ASSERT_EQ(retained.size(), std::size_t{2});
    ASSERT_EQ(retained[0].id, *second.compensation);
    ASSERT_EQ(retained[1].id, *third.compensation);
    const auto evicted = actions.restoreFilesystem(*first.compensation);
    ASSERT_FALSE(evicted.accepted());
    ASSERT_EQ(evicted.error->code, ssg::RecoveryErrorCode::RecordNotFound);
}

TEST(byteBudgetRejectsBeforeMutatingWhenNewestRecordCannotFit) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "canonical" / "large";
    const std::string original(4096, 'o');
    writeBytes(target, original);
    auto actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig(8, 32));

    const auto rejected = actions.deletePath(target);

    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error->code, ssg::RecoveryErrorCode::BudgetExceeded);
    ASSERT_EQ(readBytes(target), original);
    ASSERT_TRUE(actions.records().empty());
}

TEST(compensationRemovesOnlyItsOwnRecordAndArtifacts) {
    TemporaryDirectory temporary;
    const auto firstPath = temporary.path() / "canonical" / "first";
    const auto secondPath = temporary.path() / "canonical" / "second";
    writeBytes(firstPath, "first old");
    writeBytes(secondPath, "second old");
    auto actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig());
    const auto first = actions.deletePath(firstPath);
    const auto second = actions.deletePath(secondPath);

    ASSERT_TRUE(
        actions.restoreFilesystem(*first.compensation).accepted());

    ASSERT_EQ(readBytes(firstPath), "first old");
    ASSERT_FALSE(std::filesystem::exists(secondPath));
    const auto records = actions.records();
    ASSERT_EQ(records.size(), std::size_t{1});
    ASSERT_EQ(records.front().id, *second.compensation);
    ASSERT_TRUE(
        actions.restoreFilesystem(*second.compensation).accepted());
}

TEST(recordKindMismatchesAreTypedAndNonDestructive) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "canonical" / "file";
    writeBytes(target, "old");
    auto actions = ssg::RecoveryManager::create(
        temporary.path() / "recovery", recoveryConfig());
    const auto deleted = actions.deletePath(target);
    std::optional<ssg::JournalDocument> document{
        savedDocument("other.txt", "unchanged")};

    const auto mismatch =
        actions.restoreDocument(*deleted.compensation, document);

    ASSERT_FALSE(mismatch.accepted());
    ASSERT_EQ(mismatch.error->code,
              ssg::RecoveryErrorCode::RecordKindMismatch);
    ASSERT_FALSE(std::filesystem::exists(target));
    ASSERT_EQ(actions.records().size(), std::size_t{1});

    const auto workspace =
        std::filesystem::absolute(temporary.path() / "workspace");
    std::filesystem::create_directories(workspace);
    auto scratch = ssg::ScratchStore::create(
        temporary.path() / "scratch", workspace, scratchConfig());
    const auto closed = actions.closeDocument(document, scratch, 2s);
    ASSERT_TRUE(closed.accepted());
    const auto inverse =
        actions.restoreFilesystem(*closed.compensation);
    ASSERT_FALSE(inverse.accepted());
    ASSERT_EQ(inverse.error->code,
              ssg::RecoveryErrorCode::RecordKindMismatch);
    ASSERT_FALSE(document.has_value());
    ASSERT_EQ(actions.records().size(), std::size_t{2});
}

} // namespace

SSG_TEST_SUITE(test_recovery) {
    RUN(dirtyCloseIsDurableBeforeRemovalAndRestoresExactDocument);
    RUN(dirtyCloseDurabilityFailurePreservesDocumentAndPublishesNothing);
    RUN(renameRoundTripRestoresBothPathsAndSourceIdentity);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}

SSG_TEST_SUITE(test_recovery_tree) {
    RUN(deleteTreeRoundTripRestoresBinaryFilesAndEmptyDirectories);
    RUN(recordAndArtifactInstallationCompleteBeforeCanonicalMutation);
    RUN(preparationFailurePreservesCanonicalStateAndExistingRecords);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}

SSG_TEST_SUITE(test_recovery_failures) {
    RUN(publicationFailureAfterMutationRollsBackCanonicalState);
    RUN(eachActionMutationFailureRollsBackAndDiscardsItsRecord);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}

SSG_TEST_SUITE(test_recovery_reconstruction) {
    RUN(reconstructionDiscardsPersistedInProgressDocumentRecord);
    RUN(reconstructionDiscardsPartiallyRemovedRecordDirectory);
    RUN(renameFailedPublicationRollbackRemainsSafeAfterReconstruction);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}

SSG_TEST_SUITE(test_recovery_retry) {
    RUN(restorationFailureKeepsRecordAndRetryRestoresExactTree);
    RUN(renamePartialRestoreRetryPreservesSourceIdentity);
    RUN(renameCompletedRestoreRetryPreservesSourceIdentity);
    RUN(cleanupFailureKeepsRestoredRecordRetryable);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}

SSG_TEST_SUITE(test_recovery_budget) {
    RUN(countBudgetEvictsOldestOnlyAfterNewRecordIsInstalled);
    RUN(byteBudgetEvictsOldestWhenNewRecordFitsAfterEviction);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}

SSG_TEST_SUITE(test_recovery_budget_rejection) {
    RUN(byteBudgetRejectsBeforeMutatingWhenNewestRecordCannotFit);
    RUN(compensationRemovesOnlyItsOwnRecordAndArtifacts);
    RUN(recordKindMismatchesAreTypedAndNonDestructive);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
