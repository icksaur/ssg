#include "ssg/EditorRuntime.h"
#include "ssg/GitDiffSource.h"
#include "test_helpers.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int runStatus(const fs::path& root, std::string_view command) {
    auto full = "git -C \"" + root.string() + "\" " + std::string{command} +
                " >/dev/null 2>&1";
    return std::system(full.c_str());
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

}  // namespace

int main() {
    RUN(gitDiffHostPollingAndEventRefreshProduceExpectedDiffView);
    std::cout << "\nPassed: " << passed << " Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
