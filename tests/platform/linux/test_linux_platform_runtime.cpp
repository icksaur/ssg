#include "test_helpers.h"

#include <ssg/PlatformRuntime.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <pthread.h>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

bool childSucceeded(pid_t child) {
    int status = 0;
    return ::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

TEST(workerThreadsInheritBlockedProcessControlSignals) {
    ssg::PlatformEventLoop loop{{false, true}};
    bool inherited = false;
    std::thread worker{[&] {
        sigset_t current{};
        const int result = ::pthread_sigmask(SIG_SETMASK, nullptr, &current);
        inherited = result == 0 &&
                    ::sigismember(&current, SIGWINCH) == 1 &&
                    ::sigismember(&current, SIGTERM) == 1 &&
                    ::sigismember(&current, SIGHUP) == 1 &&
                    ::sigismember(&current, SIGINT) == 1;
    }};
    worker.join();
    ASSERT_TRUE(inherited);
}

TEST(signalsBecomeTypedReadiness) {
    ssg::PlatformEventLoop loop{{false, true}};

    ASSERT_EQ(::kill(::getpid(), SIGWINCH), 0);
    ASSERT_TRUE(loop.wait(100ms, {}).resize);

    ASSERT_EQ(::kill(::getpid(), SIGHUP), 0);
    const auto hangup = loop.wait(100ms, {});
    ASSERT_TRUE(hangup.termination.has_value());
    ASSERT_EQ(hangup.termination->kind, ssg::TerminationKind::Hangup);

    ASSERT_EQ(::kill(::getpid(), SIGINT), 0);
    const auto interrupt = loop.wait(100ms, {});
    ASSERT_TRUE(interrupt.termination.has_value());
    ASSERT_EQ(interrupt.termination->kind, ssg::TerminationKind::Interrupt);
}

TEST(standardInputIsAcquiredThroughTheEventLoop) {
    const pid_t child = ::fork();
    if (child == 0) {
        std::array<int, 2> descriptors{-1, -1};
        if (::pipe(descriptors.data()) != 0 ||
            ::dup2(descriptors[0], STDIN_FILENO) < 0) {
            std::_Exit(1);
        }
        (void)::close(descriptors[0]);
        constexpr std::string_view expected{"input"};
        if (::write(descriptors[1], expected.data(), expected.size()) !=
            static_cast<ssize_t>(expected.size())) {
            std::_Exit(2);
        }
        (void)::close(descriptors[1]);

        ssg::PlatformEventLoop loop{{true, false}};
        if (!loop.wait(100ms, {}).input) std::_Exit(3);
        std::array<char, 16> bytes{};
        const auto count = loop.readInput(bytes);
        const std::string_view actual{bytes.data(), count};
        std::_Exit(actual == expected ? 0 : 4);
    }
    ASSERT_TRUE(child > 0);
    if (child > 0) ASSERT_TRUE(childSucceeded(child));
}

TEST(terminationRestoresDefaultSignalSemantics) {
    const pid_t child = ::fork();
    if (child == 0) {
        ssg::PlatformEventLoop loop{{false, true}};
        ssg::terminateProcess({ssg::TerminationKind::Terminate});
    }
    ASSERT_TRUE(child > 0);
    if (child <= 0) return;

    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGTERM);
}

TEST(platformIdentityAndClockAreNative) {
    ASSERT_EQ(ssg::processId(), static_cast<std::uint64_t>(::getpid()));
    const auto before = ssg::monotonicTime();
    const auto after = ssg::monotonicTime();
    ASSERT_TRUE(after >= before);
}

} // namespace

SSG_TEST_SUITE(test_linux_platform_runtime) {
    RUN(workerThreadsInheritBlockedProcessControlSignals);
    RUN(signalsBecomeTypedReadiness);
    RUN(standardInputIsAcquiredThroughTheEventLoop);
    RUN(terminationRestoresDefaultSignalSemantics);
    RUN(platformIdentityAndClockAreNative);
    return failed == 0 ? 0 : 1;
}
