#include "test_helpers.h"

#include <ssg/PlatformRuntime.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

namespace {

using namespace std::chrono_literals;

bool woke(const ssg::PlatformReadiness& readiness, std::size_t index) {
    return std::find(readiness.wakes.begin(), readiness.wakes.end(), index) !=
           readiness.wakes.end();
}

TEST(notificationBeforeWaitRemainsReadyUntilConsumed) {
    ssg::PlatformWake wake;
    ssg::PlatformEventLoop loop{{false, false}};
    const std::array<const ssg::PlatformWake*, 1> wakes{&wake};

    wake.notify();
    ASSERT_TRUE(woke(loop.wait(100ms, wakes), 0));
    ASSERT_TRUE(woke(loop.wait(100ms, wakes), 0));

    wake.consume();
    ASSERT_FALSE(woke(loop.wait(1ms, wakes), 0));
}

TEST(coalescedNotificationsClearWithOneConsume) {
    ssg::PlatformWake wake;
    ssg::PlatformEventLoop loop{{false, false}};
    const std::array<const ssg::PlatformWake*, 1> wakes{&wake};

    wake.notify();
    wake.notify();
    ASSERT_TRUE(woke(loop.wait(100ms, wakes), 0));
    wake.consume();
    ASSERT_FALSE(woke(loop.wait(1ms, wakes), 0));
}

TEST(concurrentNotificationWakesBlockingWait) {
    ssg::PlatformWake wake;
    ssg::PlatformEventLoop loop{{false, false}};
    const std::array<const ssg::PlatformWake*, 1> wakes{&wake};
    std::thread producer{[&] {
        std::this_thread::sleep_for(10ms);
        wake.notify();
    }};

    const auto readiness = loop.wait(1s, wakes);
    producer.join();
    ASSERT_TRUE(woke(readiness, 0));
    wake.consume();
}

TEST(readyIndicesIdentifyIndependentSources) {
    ssg::PlatformWake first;
    ssg::PlatformWake second;
    ssg::PlatformEventLoop loop{{false, false}};
    const std::array<const ssg::PlatformWake*, 2> wakes{&first, &second};

    second.notify();
    const auto readiness = loop.wait(100ms, wakes);
    ASSERT_FALSE(woke(readiness, 0));
    ASSERT_TRUE(woke(readiness, 1));
    second.consume();
}

} // namespace

SSG_TEST_SUITE(test_platform_runtime) {
    RUN(notificationBeforeWaitRemainsReadyUntilConsumed);
    RUN(coalescedNotificationsClearWithOneConsume);
    RUN(concurrentNotificationWakesBlockingWait);
    RUN(readyIndicesIdentifyIndependentSources);
    return failed == 0 ? 0 : 1;
}
