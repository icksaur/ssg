#pragma once

#include <ssg/FilesystemWatcher.h>
#include <ssg/GitDiffSource.h>
#include <ssg/GitMetadataWatcher.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace ssg {

// One drain's worth of work handed from the worker thread to the runtime
// thread: git-diff scans and normalized external-modification events queued
// since the last drain, whether the watcher overflowed and lost events (so the
// runtime must resync every open document against disk), and whether the
// durable watcher-availability fact changed. GitDiffWorker only produces this;
// applying it to editor state (the DiffModel, the external-modification flow,
// the tree) is EditorSession's job.
struct GitDiffWorkerDrain {
    std::deque<GitDiffScan> scans;
    std::deque<WatchEvent> watchEvents;
    bool fullReconcile = false;
    bool watcherAvailabilityChanged = false;
};

// Owns the background thread that refreshes git-diff state (Poll or Event
// mode, per SSG_GIT_DIFF_MODE) and observes filesystem changes for external-
// modification detection: the wake pipe, the watcher and metadata-watcher and
// their construction, mode/env resolution, the pending scan/watch/save
// queues, and the durable watcher-availability fact. Construction never blocks
// on the recursive watch setup -- the watcher is built on the worker thread --
// so it is safe to construct eagerly at session startup.
//
// A no-op (wakeDescriptor() == -1, no thread, fullRefreshCount() == 0) when
// both git and watching are disabled, or if wake-pipe setup fails.
//
// EditorSession sees only this narrow surface: it never reaches the mutex,
// queues, fds, or thread directly.
class GitDiffWorker {
public:
    GitDiffWorker(const std::filesystem::path& root, bool enableGit,
                  bool enableWatcher);
    ~GitDiffWorker();

    GitDiffWorker(const GitDiffWorker&) = delete;
    GitDiffWorker& operator=(const GitDiffWorker&) = delete;

    // The read end of the wake pipe the runtime thread polls; -1 when the
    // worker never started (both disabled, or pipe setup failed).
    [[nodiscard]] int wakeDescriptor() const noexcept { return wakeReadFd_; }

    // Drains the wake pipe and every scan/watch-event/reconcile/availability
    // change queued since the last call. Runtime-thread only.
    [[nodiscard]] GitDiffWorkerDrain drain();

    // Hands a save expectation to the worker thread so it registers with the
    // real watcher's normalizer without racing poll(). A no-op before the
    // watcher exists; bounded so an expectation the worker never drains cannot
    // grow without limit.
    void registerSavedPath(SaveExpectation expectation);

    // Test hook: overrides the durable watcher-availability fact directly and
    // republishes it synchronously, mirroring the mid-session watcher-death
    // edge the worker thread signals in production.
    void setAvailabilityForTest(bool available);

    [[nodiscard]] std::uint64_t fullRefreshCount() const noexcept {
        return fullRefreshCount_.load(std::memory_order_relaxed);
    }

private:
    void run(bool gitUsable, bool enableWatcher,
             std::filesystem::path watcherRoot);

    std::unique_ptr<GitRepository> repository_;
    DiffModel sourceModel_;
    GitDiffSource source_;
    std::unique_ptr<FilesystemWatcher> watcher_;
    std::unique_ptr<GitMetadataWatcher> metadataWatcher_;
    std::vector<std::filesystem::path> metadataDirectories_;
    GitDiffMode mode_ = GitDiffMode::Poll;
    // Decision-13 durable capability fact: whether this session currently has a
    // filesystem watcher, so the runtime can publish "external changes are not
    // being watched" when the platform cannot provide one. Set optimistically at
    // construction and cleared by the worker thread if watcher construction
    // fails or the watcher dies mid-session; atomic because the worker thread
    // writes it and drain() (the runtime thread) reads it.
    std::atomic<bool> watcherAvailable_{false};
    // Test hook: shortens the Event-mode backstop so its full refresh is
    // deterministically triggerable in a unit test. Unset uses
    // kGitDiffEventRecoveryInterval.
    std::optional<std::chrono::steady_clock::duration> backstopIntervalOverride_;

    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    bool retryPending_ = false;
    std::chrono::steady_clock::time_point nextRetry_ =
        std::chrono::steady_clock::time_point::max();
    std::deque<GitDiffScan> pendingScans_;
    // Normalized external-modification events queued in order for the runtime-
    // thread reconcile, beside pendingScans_ and woken by the same wake byte.
    // The worker never touches the flow itself; it only hands these across the
    // thread boundary.
    std::deque<WatchEvent> pendingWatchEvents_;
    // Set when the watcher reports an Overflow (event loss). The runtime-thread
    // drain consumes it and re-scans every open document against disk, because
    // the individual change events were dropped.
    bool pendingExternalFullReconcile_ = false;
    // Save expectations handed from the save primitive to the worker thread,
    // applied to the watcher on the worker thread so registration never races
    // poll().
    std::deque<SaveExpectation> pendingSaveRegistrations_;
    std::thread thread_;
    std::atomic<std::uint64_t> fullRefreshCount_{0};

    int wakeReadFd_ = -1;
    int wakeWriteFd_ = -1;

    // Runtime-thread-only bookkeeping (never touched by the worker thread): the
    // last availability value drain() published, compared against
    // watcherAvailable_ to detect a transition.
    bool lastPublishedWatcherAvailable_ = false;
};

}  // namespace ssg
