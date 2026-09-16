#include "test_helpers.h"

#include <ssg/RuntimeTiming.h>

#include <chrono>

namespace {

using namespace std::chrono_literals;

TEST(indefiniteIdleHasNoDeadline) {
    ASSERT_FALSE(ssg::selectRuntimeWaitTimeout({}).has_value());
}

TEST(escapeDisambiguationUsesItsDeadline) {
    ASSERT_EQ(ssg::selectRuntimeWaitTimeout(
                  {.escapeSequencePending = true}),
              std::optional{30ms});
}

TEST(edgeScrollUsesItsCadence) {
    ASSERT_EQ(ssg::selectRuntimeWaitTimeout({.edgeScrollActive = true}),
              std::optional{40ms});
}

TEST(pendingSearchIsImmediate) {
    ASSERT_EQ(ssg::selectRuntimeWaitTimeout(
                  {.workspaceSearchPending = true}),
              std::optional{0ms});
}

TEST(ordinaryDeadlineIsPreservedAndBounded) {
    ASSERT_EQ(ssg::selectRuntimeWaitTimeout({.ordinaryTimeout = 75ms}),
              std::optional{75ms});
    ASSERT_EQ(ssg::selectRuntimeWaitTimeout({.ordinaryTimeout = -1ms}),
              std::optional{0ms});
}

TEST(earliestDeadlineWins) {
    ASSERT_EQ(ssg::selectRuntimeWaitTimeout(
                  {.ordinaryTimeout = 10ms,
                   .escapeSequencePending = true,
                   .edgeScrollActive = true}),
              std::optional{10ms});
}

} // namespace

SSG_TEST_SUITE(test_runtime_timing) {
    RUN(indefiniteIdleHasNoDeadline);
    RUN(escapeDisambiguationUsesItsDeadline);
    RUN(edgeScrollUsesItsCadence);
    RUN(pendingSearchIsImmediate);
    RUN(ordinaryDeadlineIsPreservedAndBounded);
    RUN(earliestDeadlineWins);
    return failed == 0 ? 0 : 1;
}
