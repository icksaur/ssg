#include "test_helpers.h"

#include <ssg/GitDiffWorker.h>
#include <ssg/PlatformRuntime.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {

using namespace std::chrono_literals;

TEST(disabledWorkerHasNoWakeOrQueuedWork) {
    const auto root = testRuntimePath("git_diff_worker_disabled");
    std::filesystem::create_directories(root);
    ssg::GitDiffWorker worker{root, false, false};
    ASSERT_TRUE(worker.wake() == nullptr);
    const auto drained = worker.drain();
    ASSERT_TRUE(drained.scans.empty());
    ASSERT_TRUE(drained.watchEvents.empty());
    ASSERT_FALSE(drained.fullReconcile);
}

TEST(filesystemEventQueuesBeforeNotification) {
    const auto root = testRuntimePath("git_diff_worker_watcher");
    std::filesystem::create_directories(root);
    ssg::GitDiffWorker worker{root, false, true};
    ASSERT_TRUE(worker.wake() != nullptr);
    if (worker.wake() == nullptr) return;

    std::this_thread::sleep_for(200ms);
    {
        std::ofstream output{root / "changed.txt"};
        output << "changed\n";
    }

    ssg::PlatformEventLoop loop{{false, false}};
    const std::array<const ssg::PlatformWake*, 1> wakes{worker.wake()};
    const auto readiness = loop.wait(2s, wakes);
    ASSERT_EQ(readiness.wakes.size(), std::size_t{1});
    const auto drained = worker.drain();
    ASSERT_FALSE(drained.watchEvents.empty());
}

} // namespace

SSG_TEST_SUITE(test_git_diff_worker) {
    RUN(disabledWorkerHasNoWakeOrQueuedWork);
    RUN(filesystemEventQueuesBeforeNotification);
    return failed == 0 ? 0 : 1;
}
