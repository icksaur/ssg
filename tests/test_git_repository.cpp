#include "ssg/GitDiffSource.h"
#include "test_helpers.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace ssg;
namespace fs = std::filesystem;

std::string run(const fs::path& root, std::string_view command) {
    auto full = "git -C \"" + root.string() + "\" " + std::string{command} +
                " 2>/dev/null";
    std::array<char, 4096> buffer{};
    std::string output;
    auto* pipe = popen(full.c_str(), "r");
    if (!pipe) {
        return output;
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    (void)pclose(pipe);
    return output;
}

int runStatus(const fs::path& root, std::string_view command) {
    auto full = "git -C \"" + root.string() + "\" " + std::string{command} +
                " >/dev/null 2>&1";
    return std::system(full.c_str());
}

std::set<std::string> porcelainCurrentPaths(const fs::path& root) {
    std::set<std::string> paths;
    std::istringstream input{run(root, "status --porcelain")};
    for (std::string line; std::getline(input, line);) {
        if (line.size() < 4) {
            continue;
        }
        auto payload = line.substr(3);
        auto arrow = payload.find(" -> ");
        if (arrow != std::string::npos) {
            paths.insert(payload.substr(arrow + 4));
        } else {
            paths.insert(payload);
        }
    }
    return paths;
}

std::set<std::string> scanCurrentPaths(const GitDiffScan& scan) {
    std::set<std::string> paths;
    for (const auto& file : scan.files) {
        paths.insert(file.path.generic_string());
    }
    return paths;
}

TEST(platformRepositoryMatchesGitStatusAcrossWorkflow) {
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto root = fs::temp_directory_path() / ("ssg-git-repository-" + uniqueSuffix);
    fs::remove_all(root);
    fs::create_directories(root);
    std::ofstream{root / "a.txt"} << "a0\n";
    ASSERT_EQ(runStatus(root, "init"), 0);
    ASSERT_EQ(runStatus(root, "config user.email a@b.c"), 0);
    ASSERT_EQ(runStatus(root, "config user.name tester"), 0);
    ASSERT_EQ(runStatus(root, "add a.txt"), 0);
    ASSERT_EQ(runStatus(root, "commit -m init"), 0);

    auto repository = makePlatformGitRepository(root);
    auto initial = repository->scanDiff({});
    ASSERT_TRUE(initial.complete);
    ASSERT_TRUE(initial.files.empty());

    std::ofstream{root / "a.txt"} << "a1\n";
    auto modified = repository->scanDiff({});
    ASSERT_TRUE(modified.complete);
    ASSERT_EQ(scanCurrentPaths(modified), porcelainCurrentPaths(root));
    ASSERT_EQ(modified.files.size(), std::size_t{1});
    if (!modified.files.empty()) {
        ASSERT_EQ(modified.files.front().baselineContent, std::optional<std::string>{"a0\n"});
        ASSERT_EQ(modified.files.front().workingContent, std::optional<std::string>{"a1\n"});
    }

    std::ofstream{root / ".gitignore"} << "*.tmp\n";
    std::ofstream{root / "ignored.tmp"} << "x\n";
    auto ignored = repository->scanDiff({});
    ASSERT_TRUE(ignored.complete);
    ASSERT_EQ(scanCurrentPaths(ignored), porcelainCurrentPaths(root));
    ASSERT_FALSE(scanCurrentPaths(ignored).contains("ignored.tmp"));

    ASSERT_EQ(runStatus(root, "add -A"), 0);
    ASSERT_EQ(runStatus(root, "commit -m prepare-rename"), 0);

    ASSERT_EQ(runStatus(root, "mv a.txt renamed.txt"), 0);
    auto renamed = repository->scanDiff({});
    ASSERT_TRUE(renamed.complete);
    ASSERT_EQ(scanCurrentPaths(renamed), porcelainCurrentPaths(root));
    bool sawRename = false;
    for (const auto& file : renamed.files) {
        if (file.path == fs::path{"renamed.txt"}) {
            sawRename = file.previousPath == std::optional<fs::path>{"a.txt"};
        }
    }
    ASSERT_TRUE(sawRename);

    auto identityBeforeCommit = renamed.baselineIdentity;
    ASSERT_EQ(runStatus(root, "commit -m rename"), 0);
    auto afterCommit = repository->scanDiff({});
    ASSERT_TRUE(afterCommit.complete);
    ASSERT_TRUE(afterCommit.files.empty());
    ASSERT_NE(afterCommit.baselineIdentity, identityBeforeCommit);

    ASSERT_EQ(runStatus(root, "rm renamed.txt"), 0);
    auto deleted = repository->scanDiff({});
    ASSERT_TRUE(deleted.complete);
    ASSERT_EQ(scanCurrentPaths(deleted), porcelainCurrentPaths(root));
    bool sawDeleted = false;
    for (const auto& file : deleted.files) {
        if (file.path == fs::path{"renamed.txt"}) {
            sawDeleted = !file.workingContent.has_value();
        }
    }
    ASSERT_TRUE(sawDeleted);

    ASSERT_EQ(runStatus(root, "checkout HEAD -- renamed.txt"), 0);
    auto restored = repository->scanDiff({});
    ASSERT_TRUE(restored.complete);
    ASSERT_EQ(scanCurrentPaths(restored), porcelainCurrentPaths(root));
    fs::remove_all(root);
}

}  // namespace

int main() {
    RUN(platformRepositoryMatchesGitStatusAcrossWorkflow);
    std::cout << "\nPassed: " << passed << " Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
