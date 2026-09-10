#include <ssg/platform_files.h>
#include <ssg/RecoveryManager.h>
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

ssg::RecoveryConfig recoveryConfig(
    std::size_t maximumRecords = 16,
    std::uintmax_t maximumBytes =
        std::numeric_limits<std::uintmax_t>::max()) {
    return {maximumRecords, maximumBytes};
}

TEST(closedDocumentRestoresExactSnapshot) {
    TemporaryDirectory temporary;
    const auto recoveryRoot = temporary.path() / "recovery";
    auto actions =
        ssg::RecoveryManager::create(recoveryRoot, recoveryConfig());
    const ssg::ClosedDocumentSnapshot expected{
        ssg::DocumentKey::untitled(fixedUntitledId()),
        ssg::DocumentMode::ReadOnly,
        true,
        "dirty \xCE\xB2 text\n"};
    std::optional<ssg::ClosedDocumentSnapshot> document{expected};

    const auto closed = actions.closeDocument(document);

    ASSERT_TRUE(closed.accepted());
    ASSERT_TRUE(closed.compensation.has_value());
    ASSERT_FALSE(document.has_value());

    actions = ssg::RecoveryManager::create(recoveryRoot, recoveryConfig());
    const auto restored =
        actions.restoreDocument(*closed.compensation, document);
    ASSERT_TRUE(restored.accepted());
    ASSERT_EQ(document, std::optional<ssg::ClosedDocumentSnapshot>{expected});
    ASSERT_TRUE(std::filesystem::is_empty(recoveryRoot));
}

TEST(reconstructionDiscardsPartiallyRemovedRecordDirectory) {
    TemporaryDirectory temporary;
    const auto recoveryRoot = temporary.path() / "recovery";
    writeBytes(recoveryRoot / "partial-record" / "artifacts" / "0",
                "orphaned artifact");

    auto actions =
        ssg::RecoveryManager::create(recoveryRoot, recoveryConfig());

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
    const auto recoveryRoot = temporary.path() / "recovery";
    auto actions =
        ssg::RecoveryManager::create(recoveryRoot, recoveryConfig(2));

    const auto first = actions.deletePath(a);
    const auto second = actions.deletePath(b);
    const auto third = actions.deletePath(c);

    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(second.accepted());
    ASSERT_TRUE(third.accepted());
    ASSERT_FALSE(std::filesystem::exists(
        recoveryRoot / std::string{first.compensation->value()}));
    ASSERT_TRUE(std::filesystem::exists(
        recoveryRoot / std::string{second.compensation->value()}));
    ASSERT_TRUE(std::filesystem::exists(
        recoveryRoot / std::string{third.compensation->value()}));
}

 TEST(byteBudgetEvictsOldestWhenNewRecordFitsAfterEviction) {
     TemporaryDirectory temporary;
     const auto recoveryRoot = temporary.path() / "recovery";
     const auto firstPath = temporary.path() / "canonical" / "first";
     const auto secondPath = temporary.path() / "canonical" / "second";
     const std::string original(4096, 'o');
     writeBytes(firstPath, original);
     writeBytes(secondPath, original);
     auto actions = ssg::RecoveryManager::create(
         recoveryRoot, recoveryConfig(8, 5000));

     const auto first = actions.deletePath(firstPath);
     const auto second = actions.deletePath(secondPath);

     ASSERT_TRUE(first.accepted());
     ASSERT_TRUE(second.accepted());
     ASSERT_FALSE(std::filesystem::exists(
         recoveryRoot / std::string{first.compensation->value()}));
     ASSERT_TRUE(std::filesystem::exists(
         recoveryRoot / std::string{second.compensation->value()}));
 }

 TEST(byteBudgetRejectsBeforeMutatingWhenNewestRecordCannotFit) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "canonical" / "large";
    const std::string original(4096, 'o');
    writeBytes(target, original);
    const auto recoveryRoot = temporary.path() / "recovery";
    auto actions =
        ssg::RecoveryManager::create(recoveryRoot, recoveryConfig(8, 32));

    const auto rejected = actions.deletePath(target);

    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error->code, ssg::RecoveryErrorCode::BudgetExceeded);
    ASSERT_EQ(readBytes(target), original);
    ASSERT_TRUE(std::filesystem::is_empty(recoveryRoot));
}

} // namespace

SSG_TEST_SUITE(test_recovery) {
    RUN(closedDocumentRestoresExactSnapshot);
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
