#include "ssg/DraftAutosaveScheduler.h"
#include "test_helpers.h"

#include <chrono>
#include <vector>

namespace {

using namespace std::chrono_literals;
using ssg::AutosaveCandidate;
using ssg::FileDocumentId;

std::vector<std::uint64_t> ids(const std::vector<FileDocumentId>& values) {
    std::vector<std::uint64_t> out;
    for (const auto id : values) out.push_back(id.value());
    return out;
}

AutosaveCandidate dirty(std::uint64_t id, std::uint64_t hash) {
    return {FileDocumentId{id}, true, hash};
}

AutosaveCandidate clean(std::uint64_t id) {
    return {FileDocumentId{id}, false, 0};
}

TEST(firstDirtyTickFlushesEagerly) {
    ssg::DraftAutosaveScheduler scheduler{10s};
    const std::vector<AutosaveCandidate> at{dirty(1, 0xAA)};
    const auto now = std::chrono::steady_clock::time_point{};
    // A document seen dirty for the first time flushes immediately, without
    // waiting for the interval — this bounds the crash window to one tick.
    ASSERT_EQ(ids(scheduler.due(now, at)), (std::vector<std::uint64_t>{1}));
}

TEST(unchangedContentIsNeverReflushedEvenAfterInterval) {
    ssg::DraftAutosaveScheduler scheduler{10s};
    const std::vector<AutosaveCandidate> at{dirty(1, 0xAA)};
    const auto t0 = std::chrono::steady_clock::time_point{};
    ASSERT_EQ(ids(scheduler.due(t0, at)), (std::vector<std::uint64_t>{1}));
    // Interval elapsed but content is identical: nothing new to persist.
    ASSERT_TRUE(scheduler.due(t0 + 20s, at).empty());
}

TEST(changedContentIsDebouncedThenFlushedAfterInterval) {
    ssg::DraftAutosaveScheduler scheduler{10s};
    const auto t0 = std::chrono::steady_clock::time_point{};
    ASSERT_EQ(ids(scheduler.due(t0, std::vector{dirty(1, 0xAA)})),
              (std::vector<std::uint64_t>{1}));  // eager
    // Changed within the interval: debounced (no flush).
    ASSERT_TRUE(scheduler.due(t0 + 3s, std::vector{dirty(1, 0xBB)}).empty());
    // Still within the interval since the last flush: still debounced.
    ASSERT_TRUE(scheduler.due(t0 + 9s, std::vector{dirty(1, 0xCC)}).empty());
    // Interval elapsed and content differs from the last flush: flush.
    ASSERT_EQ(ids(scheduler.due(t0 + 11s, std::vector{dirty(1, 0xCC)})),
              (std::vector<std::uint64_t>{1}));
}

TEST(cleanDocumentNeverFlushesAndResetsEagerness) {
    ssg::DraftAutosaveScheduler scheduler{10s};
    const auto t0 = std::chrono::steady_clock::time_point{};
    ASSERT_TRUE(scheduler.due(t0, std::vector{clean(1)}).empty());
    // Dirty then flushed eagerly.
    ASSERT_EQ(ids(scheduler.due(t0 + 1s, std::vector{dirty(1, 0xAA)})),
              (std::vector<std::uint64_t>{1}));
    // Goes clean (saved): state is dropped.
    ASSERT_TRUE(scheduler.due(t0 + 2s, std::vector{clean(1)}).empty());
    // Dirty again with the SAME content that was previously flushed: because the
    // clean transition reset its state, this is first-seen again and eager.
    ASSERT_EQ(ids(scheduler.due(t0 + 3s, std::vector{dirty(1, 0xAA)})),
              (std::vector<std::uint64_t>{1}));
}

TEST(intervalIsConfigurable) {
    ssg::DraftAutosaveScheduler scheduler{1s};
    const auto t0 = std::chrono::steady_clock::time_point{};
    ASSERT_EQ(ids(scheduler.due(t0, std::vector{dirty(1, 0xAA)})),
              (std::vector<std::uint64_t>{1}));
    // One second is enough with the shorter interval.
    ASSERT_EQ(ids(scheduler.due(t0 + 1s, std::vector{dirty(1, 0xBB)})),
              (std::vector<std::uint64_t>{1}));
}

TEST(flushAllForcesEveryDirtyDocumentRegardlessOfDebounce) {
    ssg::DraftAutosaveScheduler scheduler{10s};
    const auto t0 = std::chrono::steady_clock::time_point{};
    // Two docs flushed eagerly this tick.
    (void)scheduler.due(t0, std::vector{dirty(1, 0xAA), dirty(2, 0xBB)});
    // Immediately after (well within the interval), an exit flush still writes
    // both dirty docs; a clean doc is skipped.
    const auto forced = scheduler.flushAll(
        t0 + 1s, std::vector{dirty(1, 0xAA), dirty(2, 0xBB), clean(3)});
    auto sorted = ids(forced);
    std::sort(sorted.begin(), sorted.end());
    ASSERT_EQ(sorted, (std::vector<std::uint64_t>{1, 2}));
    // After the forced flush, a normal tick with unchanged content flushes
    // nothing (flushAll updated the records).
    ASSERT_TRUE(
        scheduler.due(t0 + 2s, std::vector{dirty(1, 0xAA), dirty(2, 0xBB)})
            .empty());
}

TEST(forgetResetsEagernessForAReopenedId) {
    ssg::DraftAutosaveScheduler scheduler{10s};
    const auto t0 = std::chrono::steady_clock::time_point{};
    ASSERT_EQ(ids(scheduler.due(t0, std::vector{dirty(1, 0xAA)})),
              (std::vector<std::uint64_t>{1}));
    scheduler.forget(FileDocumentId{1});
    // The same id, same content, flushes eagerly again after being forgotten.
    ASSERT_EQ(ids(scheduler.due(t0 + 1s, std::vector{dirty(1, 0xAA)})),
              (std::vector<std::uint64_t>{1}));
}

TEST(independentDocumentsDebounceIndependently) {
    ssg::DraftAutosaveScheduler scheduler{10s};
    const auto t0 = std::chrono::steady_clock::time_point{};
    ASSERT_EQ(ids(scheduler.due(t0, std::vector{dirty(1, 0xAA)})),
              (std::vector<std::uint64_t>{1}));
    // A different document appearing dirty later is eager on ITS first tick,
    // unaffected by doc 1's debounce window.
    ASSERT_EQ(ids(scheduler.due(t0 + 2s,
                                std::vector{dirty(1, 0xAA), dirty(2, 0xBB)})),
              (std::vector<std::uint64_t>{2}));
}

} // namespace

SSG_TEST_SUITE(test_draft_autosave) {
    RUN(firstDirtyTickFlushesEagerly);
    RUN(unchangedContentIsNeverReflushedEvenAfterInterval);
    RUN(changedContentIsDebouncedThenFlushedAfterInterval);
    RUN(cleanDocumentNeverFlushesAndResetsEagerness);
    RUN(intervalIsConfigurable);
    RUN(flushAllForcesEveryDirtyDocumentRegardlessOfDebounce);
    RUN(forgetResetsEagernessForAReopenedId);
    RUN(independentDocumentsDebounceIndependently);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
