#include "ssg/EditorRuntime.h"
#include "ssg/GitDiffSource.h"
#include "test_helpers.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int runStatus(const fs::path& root, std::string_view command) {
    auto full = "git -C \"" + root.string() + "\" " + std::string{command} +
                " >/dev/null 2>&1";
    return std::system(full.c_str());
}

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

ssg::GitTreeStatus gitStatusFromPorcelainLine(std::string_view line) {
    if (line.size() >= 2 && line[0] == '?' && line[1] == '?') {
        return ssg::GitTreeStatus::Added;
    }
    if (line.size() >= 2 && (line[0] == 'D' || line[1] == 'D')) {
        return ssg::GitTreeStatus::Deleted;
    }
    if (line.size() >= 2 && (line[0] == 'A' || line[1] == 'A')) {
        return ssg::GitTreeStatus::Added;
    }
    if (line.size() >= 2 && (line[0] == 'R' || line[1] == 'R')) {
        return ssg::GitTreeStatus::Renamed;
    }
    return ssg::GitTreeStatus::Modified;
}

std::map<std::string, ssg::GitTreeStatus> porcelainStatuses(const fs::path& root) {
    std::map<std::string, ssg::GitTreeStatus> statuses;
    std::istringstream input{run(root, "status --porcelain=v1")};
    for (std::string line; std::getline(input, line);) {
        if (line.size() < 4) {
            continue;
        }
        auto payload = line.substr(3);
        auto arrow = payload.find(" -> ");
        if (arrow != std::string::npos) {
            payload = payload.substr(arrow + 4);
        }
        statuses.emplace(std::move(payload), gitStatusFromPorcelainLine(line));
    }
    return statuses;
}

std::map<std::string, ssg::GitTreeStatus> gitProviderStatuses(
    const ssg::EditorRuntime& runtime) {
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) {
        return {};
    }
    std::map<std::string, ssg::GitTreeStatus> statuses;
    for (const auto& provider : snapshot->sections().tree.providers) {
        if (provider.kind != ssg::TreeProviderKind::Git) {
            continue;
        }
        for (const auto& node : provider.nodes) {
            ASSERT_TRUE(node.node.workspacePath.has_value());
            ASSERT_TRUE(node.node.gitStatus.has_value());
            if (!node.node.workspacePath || !node.node.gitStatus) {
                continue;
            }
            statuses.emplace(*node.node.workspacePath, *node.node.gitStatus);
        }
    }
    return statuses;
}

void applyPollTick(ssg::EditorRuntime& runtime,
                   ssg::GitDiffSource& source,
                   ssg::GitRepository& repository) {
    auto refreshed = source.refresh(repository);
    ASSERT_TRUE(refreshed.accepted);
    if (!refreshed.accepted || !refreshed.applied) return;
    auto scan = source.latestAppliedScan();
    ASSERT_TRUE(scan.has_value());
    if (!scan) return;
    auto result = runtime.applyGitDiffScan(std::move(*scan));
    ASSERT_TRUE(result.accepted());
}

void applyEventTick(ssg::EditorRuntime& runtime,
                    ssg::GitDiffSource& source,
                    ssg::GitRepository& repository,
                    std::vector<fs::path> paths) {
    auto refreshed = source.refreshPaths(repository, paths);
    ASSERT_TRUE(refreshed.accepted);
    if (!refreshed.accepted || !refreshed.applied) return;
    auto scan = source.latestAppliedScan();
    ASSERT_TRUE(scan.has_value());
    if (!scan) return;
    auto result = runtime.applyGitDiffScan(std::move(*scan));
    ASSERT_TRUE(result.accepted());
}

TEST(gitDiffHostPollingAndEventRefreshProduceExpectedDiffView) {
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto root = fs::temp_directory_path() / ("ssg-git-host-" + uniqueSuffix);
    auto stateRoot =
        fs::temp_directory_path() / ("ssg-git-host-state-" + uniqueSuffix);
    fs::remove_all(root);
    fs::remove_all(stateRoot);
    fs::create_directories(root);
    fs::create_directories(stateRoot / "scratch");
    fs::create_directories(stateRoot / "recovery");

    std::ofstream{root / "tracked.txt"} << "v1\n";
    ASSERT_EQ(runStatus(root, "init"), 0);
    ASSERT_EQ(runStatus(root, "config user.email a@b.c"), 0);
    ASSERT_EQ(runStatus(root, "config user.name tester"), 0);
    ASSERT_EQ(runStatus(root, "add tracked.txt"), 0);
    ASSERT_EQ(runStatus(root, "commit -m init"), 0);

    auto created = ssg::EditorRuntime::create(
        {root, stateRoot / "scratch", stateRoot / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());

    auto repository = ssg::makePlatformGitRepository(root);
    ssg::DiffModel sourceModel;
    ssg::GitDiffSource source{sourceModel};

    applyPollTick(runtime, source, *repository);
    auto initial = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(initial.has_value());
    if (!initial) return;
    ASSERT_TRUE(initial->sections().diff.files.empty());

    std::ofstream{root / "tracked.txt"} << "v2\n";
    applyPollTick(runtime, source, *repository);
    auto polled = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(polled.has_value());
    if (!polled) return;
    ASSERT_EQ(polled->sections().diff.files.size(), std::size_t{1});
    ASSERT_EQ(polled->sections().diff.files.front().id, ssg::DiffFileId{"tracked.txt"});

    ASSERT_EQ(runStatus(root, "add tracked.txt"), 0);
    ASSERT_EQ(runStatus(root, "commit -m update"), 0);
    applyPollTick(runtime, source, *repository);
    auto afterCommit = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(afterCommit.has_value());
    if (!afterCommit) return;
    ASSERT_TRUE(afterCommit->sections().diff.files.empty());

    std::ofstream{root / "tracked.txt"} << "v3\n";
    applyEventTick(runtime, source, *repository, {fs::path{"tracked.txt"}});
    auto eventScan = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(eventScan.has_value());
    if (!eventScan) return;
    ASSERT_EQ(eventScan->sections().diff.files.size(), std::size_t{1});
    ASSERT_EQ(eventScan->sections().diff.files.front().id, ssg::DiffFileId{"tracked.txt"});

    ASSERT_EQ(runStatus(root, "checkout -- tracked.txt"), 0);
    applyEventTick(runtime, source, *repository, {fs::path{"tracked.txt"}});
    auto afterRestore = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(afterRestore.has_value());
    if (!afterRestore) return;
    ASSERT_TRUE(afterRestore->sections().diff.files.empty());

    fs::remove_all(root);
    fs::remove_all(stateRoot);
}

TEST(gitDiffHostPublishesGitTreeProviderMatchingPorcelain) {
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto root = fs::temp_directory_path() / ("ssg-git-tree-host-" + uniqueSuffix);
    auto stateRoot =
        fs::temp_directory_path() / ("ssg-git-tree-host-state-" + uniqueSuffix);
    fs::remove_all(root);
    fs::remove_all(stateRoot);
    fs::create_directories(root);
    fs::create_directories(stateRoot / "scratch");
    fs::create_directories(stateRoot / "recovery");

    std::ofstream{root / "tracked.txt"} << "base\n";
    std::ofstream{root / "rename-me.txt"} << "rename-base\n";
    std::ofstream{root / "delete-me.txt"} << "delete-base\n";
    ASSERT_EQ(runStatus(root, "init"), 0);
    ASSERT_EQ(runStatus(root, "config user.email a@b.c"), 0);
    ASSERT_EQ(runStatus(root, "config user.name tester"), 0);
    ASSERT_EQ(runStatus(root, "add tracked.txt rename-me.txt delete-me.txt"), 0);
    ASSERT_EQ(runStatus(root, "commit -m init"), 0);

    auto created = ssg::EditorRuntime::create(
        {root, stateRoot / "scratch", stateRoot / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());

    auto repository = ssg::makePlatformGitRepository(root);
    ssg::DiffModel sourceModel;
    ssg::GitDiffSource source{sourceModel};

    applyPollTick(runtime, source, *repository);
    ASSERT_EQ(gitProviderStatuses(runtime), porcelainStatuses(root));

    std::ofstream{root / "tracked.txt"} << "changed\n";
    std::ofstream{root / "added.txt"} << "added\n";
    ASSERT_EQ(runStatus(root, "rm delete-me.txt"), 0);
    ASSERT_EQ(runStatus(root, "mv rename-me.txt renamed.txt"), 0);
    applyPollTick(runtime, source, *repository);
    auto firstStatuses = gitProviderStatuses(runtime);
    ASSERT_EQ(firstStatuses, porcelainStatuses(root));
    ASSERT_TRUE(firstStatuses.contains("tracked.txt"));
    ASSERT_TRUE(firstStatuses.contains("added.txt"));
    ASSERT_TRUE(firstStatuses.contains("delete-me.txt"));
    ASSERT_TRUE(firstStatuses.contains("renamed.txt"));

    ASSERT_EQ(runStatus(root, "checkout -- tracked.txt"), 0);
    ASSERT_EQ(runStatus(root, "clean -fd"), 0);
    applyPollTick(runtime, source, *repository);
    auto secondStatuses = gitProviderStatuses(runtime);
    ASSERT_EQ(secondStatuses, porcelainStatuses(root));
    ASSERT_NE(secondStatuses, firstStatuses);

    fs::remove_all(root);
    fs::remove_all(stateRoot);
}

}  // namespace

int main() {
    RUN(gitDiffHostPollingAndEventRefreshProduceExpectedDiffView);
    RUN(gitDiffHostPublishesGitTreeProviderMatchingPorcelain);
    std::cout << "\nPassed: " << passed << " Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
