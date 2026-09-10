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
    result.compactionThresholdBytes =
        std::numeric_limits<std::uintmax_t>::max();
    result.durabilityTarget = 100ms;
    return result;
}

std::filesystem::path scratchJournalPath(
    const std::filesystem::path& root,
    const std::filesystem::path& workspace) {
    const auto sessions =
        root / "workspaces" / ssg::scratchWorkspaceKey(workspace) / "sessions";
    const auto entries = ssg::listDirectory(sessions);
    if (!entries.ok() || entries.entries.size() != 1) {
        throw std::runtime_error("expected one scratch session");
    }
    return entries.entries.front().path() / "journal.bin";
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
    const auto scratchRoot = temporary.path() / "scratch";
    auto scratch =
        ssg::ScratchStore::create(scratchRoot, workspace, scratchConfig());
    std::filesystem::create_directory(
        scratchJournalPath(scratchRoot, workspace));
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
    ASSERT_TRUE(std::ranges::none_of(records, [&](const auto& record) {
        return record.id == *first.compensation;
    }));
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
    ASSERT_TRUE(std::ranges::none_of(retained, [&](const auto& record) {
        return record.id == *first.compensation;
    }));
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

} // namespace

SSG_TEST_SUITE(test_recovery) {
    RUN(dirtyCloseIsDurableBeforeRemovalAndRestoresExactDocument);
    RUN(dirtyCloseDurabilityFailurePreservesDocumentAndPublishesNothing);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}

SSG_TEST_SUITE(test_recovery_reconstruction) {
    RUN(reconstructionDiscardsPartiallyRemovedRecordDirectory);
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

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
