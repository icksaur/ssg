#include "ssg/EditorSession.h"
#include "test_helpers.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>
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
    ssg::EditorSession& runtime) {
    (void)runtime.pump();
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
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
            statuses.emplace(*node.node.workspacePath,
                             node.node.gitStatus->status);
        }
    }
    return statuses;
}

class ScopedGitDiffMode {
public:
    explicit ScopedGitDiffMode(const char* mode) {
        const char* prior = std::getenv("SSG_GIT_DIFF_MODE");
        if (prior != nullptr) {
            hadPrevious_ = true;
            previous_ = prior;
        }
        if (mode == nullptr) {
            ::unsetenv("SSG_GIT_DIFF_MODE");
        } else {
            ::setenv("SSG_GIT_DIFF_MODE", mode, 1);
        }
    }

    ~ScopedGitDiffMode() {
        if (hadPrevious_) {
            ::setenv("SSG_GIT_DIFF_MODE", previous_.c_str(), 1);
        } else {
            ::unsetenv("SSG_GIT_DIFF_MODE");
        }
    }

private:
    bool hadPrevious_ = false;
    std::string previous_;
};

bool waitForDiffCount(ssg::EditorSession& runtime, std::size_t expectedCount,
                      std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)runtime.pump();
        auto snapshot = runtime.snapshot(ssg::ClientId{1});
        if (snapshot && snapshot->sections().diff.files.size() == expectedCount) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return false;
}

bool waitForGitTreeStatuses(ssg::EditorSession& runtime,
                            const std::map<std::string, ssg::GitTreeStatus>& expected,
                            std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (gitProviderStatuses(runtime) == expected) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return false;
}

bool waitForFullRefreshCount(ssg::EditorSession& runtime, std::uint64_t minimum,
                             std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)runtime.pump();
        if (runtime.gitFullRefreshCountForTest() >= minimum) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return false;
}

TEST(gitDiffHostWorkerPollingAndEventRefreshProduceExpectedDiffView) {
    for (const char* mode : {"poll", "event"}) {
        ScopedGitDiffMode scopedMode{mode};
        const auto uniqueSuffix =
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count());
        auto root = fs::temp_directory_path() / ("ssg-git-host-worker-" + uniqueSuffix);
        auto stateRoot =
            fs::temp_directory_path() / ("ssg-git-host-worker-state-" + uniqueSuffix);
        fs::remove_all(root);
        fs::remove_all(stateRoot);
        fs::create_directories(root);
        fs::create_directories(stateRoot / "scratch");
        fs::create_directories(stateRoot / "recovery");

        std::ofstream{root / "tracked.txt"} << "v1\n";
        ASSERT_EQ(runStatus(root, "init"), 0);
        ASSERT_EQ(runStatus(root, "config user.email a@b.c"), 0);
        ASSERT_EQ(runStatus(root, "config user.name tester"), 0);
        std::ofstream{root / "tracked.txt"} << "staged\n";
        ASSERT_EQ(runStatus(root, "add tracked.txt"), 0);
        ASSERT_EQ(runStatus(root, "commit -m init"), 0);

        auto created = ssg::EditorSession::create(
            {root, stateRoot / "scratch", stateRoot / "recovery"});
        ASSERT_TRUE(created.accepted());
        if (!created.accepted()) return;
        auto& runtime = *created.session;
        ASSERT_TRUE(
            runtime
                .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                        ssg::ViewId{1})
                .accepted());
        ASSERT_TRUE(runtime.gitDiffWakeDescriptor() >= 0);

        ASSERT_TRUE(
            waitForDiffCount(runtime, 0, std::chrono::milliseconds{3000}));

        std::ofstream{root / "tracked.txt"} << "v2\n";
        ASSERT_EQ(runStatus(root, "add tracked.txt"), 0);
        ASSERT_TRUE(
            waitForDiffCount(runtime, 1, std::chrono::milliseconds{3000}));
        auto changed =
            runtime.snapshot(ssg::ClientId{1});
        ASSERT_TRUE(changed.has_value());
        if (!changed) return;
        ASSERT_EQ(changed->sections().diff.files.front().id,
                  ssg::DiffFileId{"tracked.txt"});

        ASSERT_EQ(runStatus(root, "commit -m update"), 0);
        ASSERT_TRUE(
            waitForDiffCount(runtime, 0, std::chrono::milliseconds{3000}));

        std::ofstream{root / "tracked.txt"} << "v3\n";
        ASSERT_TRUE(
            waitForDiffCount(runtime, 1, std::chrono::milliseconds{3000}));
        ASSERT_EQ(runStatus(root, "checkout -- tracked.txt"), 0);
        ASSERT_TRUE(
            waitForDiffCount(runtime, 0, std::chrono::milliseconds{3000}));

        fs::remove_all(root);
        fs::remove_all(stateRoot);
    }
}

TEST(gitDiffHostWorkerStartsFromSubdirectoryWorkspace) {
    ScopedGitDiffMode scopedMode{"poll"};
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto root = fs::temp_directory_path() / ("ssg-git-subdir-host-" + uniqueSuffix);
    auto subdir = root / "src";
    auto stateRoot =
        fs::temp_directory_path() / ("ssg-git-subdir-host-state-" + uniqueSuffix);
    fs::remove_all(root);
    fs::remove_all(stateRoot);
    fs::create_directories(subdir);
    fs::create_directories(stateRoot / "scratch");
    fs::create_directories(stateRoot / "recovery");
    std::ofstream{root / "src" / "tracked.txt"} << "v1\n";
    ASSERT_EQ(runStatus(root, "init"), 0);
    ASSERT_EQ(runStatus(root, "config user.email a@b.c"), 0);
    ASSERT_EQ(runStatus(root, "config user.name tester"), 0);
    ASSERT_EQ(runStatus(root, "add src/tracked.txt"), 0);
    ASSERT_EQ(runStatus(root, "commit -m init"), 0);

    auto created = ssg::EditorSession::create(
        {subdir, stateRoot / "scratch", stateRoot / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(
        runtime
            .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                    ssg::ViewId{1})
            .accepted());
    ASSERT_TRUE(runtime.gitDiffWakeDescriptor() >= 0);

    std::ofstream{root / "src" / "tracked.txt"} << "v2\n";
    ASSERT_EQ(runStatus(root, "add src/tracked.txt"), 0);
    ASSERT_TRUE(waitForDiffCount(runtime, 1, std::chrono::milliseconds{3000}));

    auto snapshot = runtime.snapshot(ssg::ClientId{1});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().diff.files.size(), std::size_t{1});
    ASSERT_EQ(snapshot->sections().diff.files.front().id,
              ssg::DiffFileId{"src/tracked.txt"});

    fs::remove_all(root);
    fs::remove_all(stateRoot);
}

TEST(gitDiffHostWorkerPublishesGitTreeProviderMatchingPorcelain) {
    ScopedGitDiffMode scopedMode{"poll"};
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

    auto created = ssg::EditorSession::create(
        {root, stateRoot / "scratch", stateRoot / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(waitForGitTreeStatuses(runtime, porcelainStatuses(root),
                                       std::chrono::milliseconds{3000}));

    std::ofstream{root / "tracked.txt"} << "changed\n";
    std::ofstream{root / "added.txt"} << "added\n";
    ASSERT_EQ(runStatus(root, "rm delete-me.txt"), 0);
    ASSERT_EQ(runStatus(root, "mv rename-me.txt renamed.txt"), 0);
    auto expectedFirst = porcelainStatuses(root);
    ASSERT_TRUE(waitForGitTreeStatuses(runtime, expectedFirst,
                                       std::chrono::milliseconds{3000}));
    auto firstStatuses = gitProviderStatuses(runtime);
    ASSERT_TRUE(firstStatuses.contains("tracked.txt"));
    ASSERT_TRUE(firstStatuses.contains("added.txt"));
    ASSERT_TRUE(firstStatuses.contains("delete-me.txt"));
    ASSERT_TRUE(firstStatuses.contains("renamed.txt"));

    ASSERT_EQ(runStatus(root, "checkout -- tracked.txt"), 0);
    ASSERT_EQ(runStatus(root, "clean -fd"), 0);
    auto expectedSecond = porcelainStatuses(root);
    ASSERT_TRUE(waitForGitTreeStatuses(runtime, expectedSecond,
                                       std::chrono::milliseconds{3000}));
    auto secondStatuses = gitProviderStatuses(runtime);
    ASSERT_EQ(secondStatuses, expectedSecond);
    ASSERT_NE(secondStatuses, firstStatuses);

    fs::remove_all(root);
    fs::remove_all(stateRoot);
}

TEST(gitDiffHostPublishesFilesAlreadyDirtyAtSessionStart) {
    ScopedGitDiffMode scopedMode{"poll"};
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto root =
        fs::temp_directory_path() / ("ssg-git-initial-dirty-" + uniqueSuffix);
    auto stateRoot = fs::temp_directory_path() /
                     ("ssg-git-initial-dirty-state-" + uniqueSuffix);
    fs::remove_all(root);
    fs::remove_all(stateRoot);
    fs::create_directories(root);
    fs::create_directories(stateRoot / "scratch");
    fs::create_directories(stateRoot / "recovery");
    std::ofstream{root / "tracked.txt"} << "base\n";
    {
        std::ofstream large{root / "large.txt"};
        for (std::size_t line = 0; line < 200'001; ++line) {
            large << "a\n";
        }
    }
    ASSERT_EQ(runStatus(root, "init"), 0);
    ASSERT_EQ(runStatus(root, "config user.email a@b.c"), 0);
    ASSERT_EQ(runStatus(root, "config user.name tester"), 0);
    ASSERT_EQ(runStatus(root, "add tracked.txt"), 0);
    ASSERT_EQ(runStatus(root, "commit -m init"), 0);
    std::ofstream{root / "tracked.txt"} << "changed before startup\n";
    {
        std::ofstream large{root / "large.txt"};
        for (std::size_t line = 0; line < 200'001; ++line) {
            large << "b\n";
        }
    }
    std::ofstream{root / "untracked.txt"} << "new before startup\n";
    std::ofstream{root / "staged.txt"} << "staged before startup\n";
    ASSERT_EQ(runStatus(root, "add tracked.txt staged.txt"), 0);
    std::ofstream{root / "staged.txt"} << "changed again after staging\n";
    const auto expected = porcelainStatuses(root);

    auto created = ssg::EditorSession::create(
        {root, stateRoot / "scratch", stateRoot / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(waitForGitTreeStatuses(runtime, expected,
                                       std::chrono::milliseconds{3000}));

    fs::remove_all(root);
    fs::remove_all(stateRoot);
}

TEST(gitDiffHostWorkerLifecycleHasBoundedShutdownLatency) {
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto root = fs::temp_directory_path() / ("ssg-git-worker-life-" + uniqueSuffix);
    fs::remove_all(root);
    fs::create_directories(root);
    std::ofstream{root / "tracked.txt"} << "v1\n";
    ASSERT_EQ(runStatus(root, "init"), 0);
    ASSERT_EQ(runStatus(root, "config user.email a@b.c"), 0);
    ASSERT_EQ(runStatus(root, "config user.name tester"), 0);
    ASSERT_EQ(runStatus(root, "add tracked.txt"), 0);
    ASSERT_EQ(runStatus(root, "commit -m init"), 0);

    for (const char* mode : {"poll", "event"}) {
        ScopedGitDiffMode scopedMode{mode};
        for (int index = 0; index < 6; ++index) {
            auto stateRoot = fs::temp_directory_path() /
                             ("ssg-git-worker-life-state-" + uniqueSuffix + "-" +
                              std::to_string(index) + "-" + mode);
            fs::remove_all(stateRoot);
            fs::create_directories(stateRoot / "scratch");
            fs::create_directories(stateRoot / "recovery");
            auto created = ssg::EditorSession::create(
                {root, stateRoot / "scratch", stateRoot / "recovery"});
            ASSERT_TRUE(created.accepted());
            if (!created.accepted()) return;
            ASSERT_TRUE(created.session
                            ->attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                                     ssg::ViewId{1})
                            .accepted());
            ASSERT_TRUE(created.session->gitDiffWakeDescriptor() >= 0);

            std::atomic<bool> churn{true};
            std::thread writer([&]() {
                bool high = false;
                while (churn.load(std::memory_order_relaxed)) {
                    std::ofstream{root / "tracked.txt"}
                        << (high ? "v2\n" : "v3\n");
                    (void)runStatus(root, "add tracked.txt");
                    high = !high;
                    std::this_thread::sleep_for(std::chrono::milliseconds{20});
                }

            });
            std::this_thread::sleep_for(std::chrono::milliseconds{200});

            auto runtime = std::move(created.session);
            const auto start = std::chrono::steady_clock::now();
            runtime.reset();
            churn.store(false, std::memory_order_relaxed);
            writer.join();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);
            ASSERT_TRUE(elapsed < std::chrono::milliseconds{3000});
            fs::remove_all(stateRoot);
        }
    }
    fs::remove_all(root);
}

TEST(eventModeRefreshesGitMetadataWithoutIdleFullScans) {
    ScopedGitDiffMode scopedMode{"event"};
    const auto suffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = fs::temp_directory_path() / ("ssg-git-event-idle-" + suffix);
    const auto stateRoot =
        fs::temp_directory_path() / ("ssg-git-event-idle-state-" + suffix);
    fs::remove_all(root);
    fs::remove_all(stateRoot);
    fs::create_directories(root);
    fs::create_directories(stateRoot / "scratch");
    fs::create_directories(stateRoot / "recovery");
    std::ofstream{root / "tracked.txt"} << "base\n";
    ASSERT_EQ(runStatus(root, "init"), 0);
    ASSERT_EQ(runStatus(root, "config user.email a@b.c"), 0);
    ASSERT_EQ(runStatus(root, "config user.name tester"), 0);
    ASSERT_EQ(runStatus(root, "add tracked.txt"), 0);
    ASSERT_EQ(runStatus(root, "commit -m init"), 0);

    auto created = ssg::EditorSession::create(
        {root, stateRoot / "scratch", stateRoot / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(waitForFullRefreshCount(
        runtime, 1, std::chrono::seconds{3}));
    const auto idleCount = runtime.gitFullRefreshCountForTest();
    std::this_thread::sleep_for(std::chrono::milliseconds{3500});
    (void)runtime.pump();
    ASSERT_EQ(runtime.gitFullRefreshCountForTest(), idleCount);

    std::ofstream{root / "tracked.txt"} << "staged\n";
    ASSERT_EQ(runStatus(root, "add tracked.txt"), 0);
    ASSERT_TRUE(waitForFullRefreshCount(
        runtime, idleCount + 1, std::chrono::seconds{3}));
    fs::remove_all(root);
    fs::remove_all(stateRoot);
}

}  // namespace

int main() {
    RUN(gitDiffHostWorkerPollingAndEventRefreshProduceExpectedDiffView);
    RUN(gitDiffHostWorkerStartsFromSubdirectoryWorkspace);
    RUN(gitDiffHostWorkerPublishesGitTreeProviderMatchingPorcelain);
    RUN(gitDiffHostPublishesFilesAlreadyDirtyAtSessionStart);
    RUN(gitDiffHostWorkerLifecycleHasBoundedShutdownLatency);
    RUN(eventModeRefreshesGitMetadataWithoutIdleFullScans);
    std::cout << "\nPassed: " << passed << " Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
