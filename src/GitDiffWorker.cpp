#include <ssg/GitDiffWorker.h>

#include <ssg/platform_files.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace ssg {
namespace {

constexpr auto kGitDiffPollInterval = std::chrono::milliseconds{250};
constexpr auto kGitMetadataWatchPollInterval = std::chrono::milliseconds{100};
constexpr auto kGitDiffRetryDelay = std::chrono::milliseconds{1000};
// Event mode refreshes instantly on watch events; this long-interval full-refresh
// backstop bounds the staleness of anything the watcher cannot observe -- external
// git operations, a linked worktree's metadata outside the tree, dropped events on
// a network filesystem -- without re-scanning at the Poll cadence. Much larger than
// kGitDiffPollInterval so idle CPU is a small fraction of Poll's.
constexpr auto kGitDiffEventRecoveryInterval = std::chrono::minutes{1};

bool setNonBlocking(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    return ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

GitDiffWorker::GitDiffWorker(const std::filesystem::path& root, bool enableGit,
                             bool enableWatcher)
    : repository_{enableGit || enableWatcher
                      ? makePlatformGitRepository(root)
                      : nullptr},
      source_{sourceModel_} {
    if (enableGit || enableWatcher) {
        // The unset-default mode follows watcher availability (Event when watching
        // is enabled, Poll otherwise); an explicit env override still wins. The
        // worker thread downgrades Event->Poll if the watcher then fails to
        // construct.
        mode_ = resolveGitDiffMode(std::getenv("SSG_GIT_DIFF_MODE"), enableWatcher);
        // Test seam (consistent with SSG_GIT_DIFF_MODE): a short backstop makes the
        // Event-mode full-refresh backstop deterministically triggerable. Not a
        // product knob; the production value lives in kGitDiffEventRecoveryInterval.
        if (const char* ms = std::getenv("SSG_GIT_DIFF_BACKSTOP_MS");
            ms != nullptr && *ms != '\0') {
            char* end = nullptr;
            const long value = std::strtol(ms, &end, 10);
            if (end != ms && value > 0) {
                backstopIntervalOverride_ = std::chrono::milliseconds{value};
            }
        }
        const bool gitUsable =
            enableGit && repository_ && repository_->isUsable();
        // Optimistic: the worker thread constructs the watcher off the
        // first-frame path, so startup never pays for the recursive watch setup.
        // The thread clears this if construction fails. False when watching is
        // disabled -- no watcher, so unavailable.
        watcherAvailable_.store(enableWatcher, std::memory_order_relaxed);
        int wakePipe[2] = {-1, -1};
        if (::pipe(wakePipe) == 0 && setNonBlocking(wakePipe[0]) &&
            setNonBlocking(wakePipe[1])) {
            wakeReadFd_ = wakePipe[0];
            wakeWriteFd_ = wakePipe[1];
            thread_ = std::thread(&GitDiffWorker::run, this, gitUsable,
                                  enableWatcher, root);
        } else {
            if (wakePipe[0] != -1) {
                (void)::close(wakePipe[0]);
            }
            if (wakePipe[1] != -1) {
                (void)::close(wakePipe[1]);
            }
            watcherAvailable_.store(false, std::memory_order_relaxed);
        }
    }
    lastPublishedWatcherAvailable_ =
        watcherAvailable_.load(std::memory_order_relaxed);
}

GitDiffWorker::~GitDiffWorker() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    if (wakeReadFd_ != -1) {
        (void)::close(wakeReadFd_);
        wakeReadFd_ = -1;
    }
    if (wakeWriteFd_ != -1) {
        (void)::close(wakeWriteFd_);
        wakeWriteFd_ = -1;
    }
}

void GitDiffWorker::run(bool gitUsable, bool enableWatcher,
                        std::filesystem::path watcherRoot) {
    // The watcher is a workspace service, not a git feature: construct it on the
    // worker thread (off the first-frame path) whenever the platform can and
    // watching is enabled, so external modification is observed in a non-git
    // workspace and in poll-for-git setups alike (Decision 1). Git's Event mode
    // is impossible without it.
    if (enableWatcher) {
        try {
            watcher_ = makePlatformFilesystemWatcher(watcherRoot);
        } catch (const std::runtime_error&) {
            watcher_ = nullptr;
        }
    }
    if (!watcher_) {
        watcherAvailable_.store(false, std::memory_order_relaxed);
        if (mode_ == GitDiffMode::Event) {
            mode_ = GitDiffMode::Poll;
        }
        // The optimistic `true` was already seeded before this thread ran, so
        // an initial construction failure is a real availability edge: write
        // the wake byte so the runtime-thread drain publishes the false and
        // advances the revision, exactly like the mid-session watcher-death
        // edge (Decision 13). Without this a client that saw the optimistic
        // `true` would never learn watching is off.
        const char byte = 'g';
        (void)::write(wakeWriteFd_, &byte, 1);
    }
    if (gitUsable && watcher_) {
        try {
            metadataDirectories_ = repository_->metadataDirectories();
            metadataWatcher_ = makePlatformGitMetadataWatcher(metadataDirectories_);
        } catch (const std::exception&) {
            mode_ = GitDiffMode::Poll;
        }
    }
    // Nothing to serve: no usable git repository to scan and no watcher to
    // observe. External modification is simply not observed.
    if (!gitUsable && !watcher_) {
        return;
    }
    const auto shouldStop = [&]() {
        std::lock_guard lock(mutex_);
        return stop_;
    };
    const auto maybeRefreshAll = [&]() -> std::optional<GitDiffRefreshResult> {
        if (shouldStop()) {
            return std::nullopt;
        }
        try {
            ++fullRefreshCount_;
            auto refreshed = source_.refresh(*repository_);
            const auto directories = repository_->metadataDirectories();
            if (metadataWatcher_ &&
                (directories != metadataDirectories_ ||
                 !metadataWatcher_->healthy())) {
                metadataWatcher_->replaceDirectories(directories);
                metadataDirectories_ = directories;
            }
            if (shouldStop()) {
                return std::nullopt;
            }
            return refreshed;
        } catch (const std::system_error&) {
            return GitDiffRefreshResult{
                .applied = false, .requestedRescan = true, .accepted = false};
        } catch (const std::exception&) {
            return GitDiffRefreshResult{
                .applied = false, .requestedRescan = true, .accepted = false};
        }
    };
    const auto maybeRefreshPaths =
        [&](const std::vector<std::filesystem::path>& paths)
        -> std::optional<GitDiffRefreshResult> {
        if (shouldStop()) {
            return std::nullopt;
        }
        try {
            auto refreshed = source_.refreshPaths(*repository_, paths);
            if (shouldStop()) {
                return std::nullopt;
            }
            return refreshed;
        } catch (const std::system_error&) {
            return GitDiffRefreshResult{
                .applied = false, .requestedRescan = true, .accepted = false};
        } catch (const std::exception&) {
            return GitDiffRefreshResult{
                .applied = false, .requestedRescan = true, .accepted = false};
        }
    };
    const auto scheduleRetry = [&]() {
        std::lock_guard lock(mutex_);
        retryPending_ = true;
        nextRetry_ = std::chrono::steady_clock::now() + kGitDiffRetryDelay;
    };
    const auto clearRetry = [&]() {
        std::lock_guard lock(mutex_);
        retryPending_ = false;
        nextRetry_ = std::chrono::steady_clock::time_point::max();
    };
    const auto queueLatestScan = [&]() {
        auto scan = source_.latestAppliedScan();
        if (!scan) {
            return;
        }
        bool signal = false;
        {
            std::lock_guard lock(mutex_);
            signal = pendingScans_.empty();
            pendingScans_.push_back(std::move(*scan));
        }
        if (signal) {
            const char byte = 'g';
            (void)::write(wakeWriteFd_, &byte, 1);
        }
    };
    // The branch is published independently of the diff so an incomplete
    // repository scan never hides the branch indicator.
    const auto queueBranchScan = [&]() {
        auto scan = source_.takeBranchOnlyScanIfChanged();
        if (!scan) {
            return;
        }
        bool signal = false;
        {
            std::lock_guard lock(mutex_);
            signal = pendingScans_.empty();
            pendingScans_.push_back(std::move(*scan));
        }
        if (signal) {
            const char byte = 'g';
            (void)::write(wakeWriteFd_, &byte, 1);
        }
    };

    std::function<void(const GitDiffRefreshResult&, bool)> handleResult;
    handleResult = [&](const GitDiffRefreshResult& refreshed, bool fullRefresh) {
        queueBranchScan();
        if (refreshed.applied) {
            queueLatestScan();
            clearRetry();
        }
        // Retry (or fall back a path scan to a full refresh) ONLY when the source
        // asked for a rescan -- a transient failure (incomplete scan, index.lock).
        if (refreshed.shouldRetry()) {
            if (!fullRefresh) {
                auto full = maybeRefreshAll();
                if (!full) {
                    return;
                }
                handleResult(*full, true);
                return;
            }
            scheduleRetry();
        }
    };

    // Hand normalized external events to the runtime-thread reconcile, coalescing
    // the wake byte with the git-scan queue so the host drains both at once.
    const auto queueWatchEvents = [&](const std::vector<WatchEvent>& events) {
        bool signal = false;
        {
            std::lock_guard lock(mutex_);
            signal = pendingWatchEvents_.empty() && pendingScans_.empty();
            for (const auto& event : events) {
                pendingWatchEvents_.push_back(event);
            }
        }
        if (signal) {
            const char byte = 'g';
            (void)::write(wakeWriteFd_, &byte, 1);
        }
    };

    if (gitUsable) {
        if (auto first = maybeRefreshAll()) {
            handleResult(*first, true);
        } else {
            return;
        }
    }
    auto nextPoll = std::chrono::steady_clock::now() + kGitDiffPollInterval;
    // Event mode has no periodic full refresh, so a long-interval backstop
    // bounds the staleness of anything the watcher cannot observe (Decision:
    // external git ops, worktree metadata outside the tree, dropped events).
    auto nextBackstop =
        std::chrono::steady_clock::now() + kGitDiffEventRecoveryInterval;
    // A test hook can shorten the backstop so it is deterministically triggerable.
    const auto backstopInterval = backstopIntervalOverride_
                                      ? *backstopIntervalOverride_
                                      : kGitDiffEventRecoveryInterval;
    nextBackstop = std::chrono::steady_clock::now() + backstopInterval;
    while (!shouldStop()) {
        const auto now = std::chrono::steady_clock::now();
        auto wakeAt = now + kGitMetadataWatchPollInterval;
        {
            std::lock_guard lock(mutex_);
            if (gitUsable && mode_ == GitDiffMode::Poll) {
                wakeAt = std::min(wakeAt, nextPoll);
            }
            if (gitUsable && mode_ == GitDiffMode::Event) {
                wakeAt = std::min(wakeAt, nextBackstop);
            }
            if (gitUsable && retryPending_) {
                wakeAt = std::min(wakeAt, nextRetry_);
            }
        }
        const auto timeout =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                wakeAt > now ? wakeAt - now : std::chrono::milliseconds{0});

        if (watcher_) {
            std::vector<WatchEvent> events;
            try {
                events = watcher_->poll(timeout);
            } catch (const std::runtime_error&) {
                // The watcher died mid-session; drop it and fall back to git
                // polling. External modification is no longer observed, so the
                // durable capability flips to unavailable and a wake byte makes
                // the runtime-thread drain observe the transition (Decision 13).
                watcher_.reset();
                metadataWatcher_.reset();
                watcherAvailable_.store(false, std::memory_order_relaxed);
                mode_ = GitDiffMode::Poll;
                {
                    const char byte = 'g';
                    (void)::write(wakeWriteFd_, &byte, 1);
                }
                if (gitUsable) {
                    auto full = maybeRefreshAll();
                    if (!full) {
                        break;
                    }
                    handleResult(*full, true);
                }
                continue;
            }
            bool overflowed = false;
            for (const auto& event : events) {
                if (event.kind == WatchEventKind::Overflow) {
                    overflowed = true;
                    break;
                }
            }
            std::vector<WatchEvent> workspaceEvents;
            if (!overflowed && !events.empty()) {
                workspaceEvents.reserve(events.size());
                bool metadataDirty = false;
                for (auto& event : events) {
                    if (!event.path.empty() && *event.path.begin() == ".git") {
                        metadataDirty = true;
                    } else {
                        workspaceEvents.push_back(std::move(event));
                    }
                }
                if (!workspaceEvents.empty()) {
                    queueWatchEvents(workspaceEvents);
                }
                if (metadataDirty && gitUsable) {
                    auto full = maybeRefreshAll();
                    if (!full) break;
                    handleResult(*full, true);
                }
            }
            if (overflowed) {
                // The watcher lost events: the external flow must resynchronize
                // every open document against disk, not just refresh git. Signal
                // the runtime-thread drain (which owns the flow) to do the full
                // re-scan; the worker never touches the flow itself.
                bool signal = false;
                {
                    std::lock_guard lock(mutex_);
                    signal = pendingScans_.empty() && pendingWatchEvents_.empty() &&
                             !pendingExternalFullReconcile_;
                    pendingExternalFullReconcile_ = true;
                }
                if (signal) {
                    const char byte = 'g';
                    (void)::write(wakeWriteFd_, &byte, 1);
                }
            }
            if (gitUsable && mode_ == GitDiffMode::Event) {
                if (overflowed) {
                    auto full = maybeRefreshAll();
                    if (!full) {
                        break;
                    }
                    handleResult(*full, true);
                } else if (!workspaceEvents.empty()) {
                    std::vector<std::filesystem::path> paths;
                    paths.reserve(workspaceEvents.size() * 2);
                    for (const auto& event : workspaceEvents) {
                        paths.push_back(event.path);
                        if (event.previousPath) {
                            paths.push_back(*event.previousPath);
                        }
                    }
                    std::sort(paths.begin(), paths.end());
                    paths.erase(std::unique(paths.begin(), paths.end()),
                                paths.end());
                    auto pathRefresh = maybeRefreshPaths(paths);
                    if (!pathRefresh) {
                        break;
                    }
                    handleResult(*pathRefresh, false);
                }
            }
        } else {
            std::unique_lock lock(mutex_);
            if (wake_.wait_until(lock, wakeAt, [&]() { return stop_; })) {
                break;
            }
            lock.unlock();
        }

        const auto afterWait = std::chrono::steady_clock::now();
        if (gitUsable) {
            if (mode_ == GitDiffMode::Event && metadataWatcher_) {
                try {
                    if (metadataWatcher_->poll(std::chrono::milliseconds{0})) {
                        auto full = maybeRefreshAll();
                        if (!full) break;
                        handleResult(*full, true);
                    }
                } catch (const std::exception&) {
                    metadataWatcher_.reset();
                    mode_ = GitDiffMode::Poll;
                }
            }
            bool retryDue = false;
            {
                std::lock_guard lock(mutex_);
                retryDue = retryPending_ && afterWait >= nextRetry_;
            }
            if (retryDue) {
                auto full = maybeRefreshAll();
                if (!full) {
                    break;
                }
                handleResult(*full, true);
            }
            if (mode_ == GitDiffMode::Poll && afterWait >= nextPoll) {
                auto full = maybeRefreshAll();
                if (!full) {
                    break;
                }
                handleResult(*full, true);
                nextPoll = afterWait + kGitDiffPollInterval;
            }
            if (mode_ == GitDiffMode::Event && afterWait >= nextBackstop) {
                auto full = maybeRefreshAll();
                if (!full) {
                    break;
                }
                handleResult(*full, true);
                nextBackstop = afterWait + backstopInterval;
            }
        }
    }
}

GitDiffWorkerDrain GitDiffWorker::drain() {
    GitDiffWorkerDrain result;
    const bool current = watcherAvailable_.load(std::memory_order_relaxed);
    if (current != lastPublishedWatcherAvailable_) {
        lastPublishedWatcherAvailable_ = current;
        result.watcherAvailabilityChanged = true;
    }
    if (wakeReadFd_ == -1) {
        return result;
    }
    char scratch[64];
    while (::read(wakeReadFd_, scratch, sizeof scratch) > 0) {
        // Drain every queued wake byte so a subsequent poll() never wakes on a
        // stale signal.
    }
    {
        std::lock_guard lock(mutex_);
        result.scans.swap(pendingScans_);
        result.watchEvents.swap(pendingWatchEvents_);
        result.fullReconcile = pendingExternalFullReconcile_;
        pendingExternalFullReconcile_ = false;
    }
    return result;
}

}  // namespace ssg
