#include <ssg/FilesystemWatcher.h>

#include <algorithm>
#include <deque>
#include <iterator>
#include <map>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>

namespace ssg {

std::optional<WatchFileState> WatchFileState::observe(
    const std::filesystem::path& path) {
    try {
        const auto status = statFile(path);
        if (!status) {
            return std::nullopt;
        }
        return WatchFileState{
            status->identity,
            status->kind == FileKind::Regular
                ? static_cast<std::uint64_t>(status->size)
                : 0,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                status->mtime.time_since_epoch())
                .count()};
    } catch (const std::system_error&) {
        return std::nullopt;
    }
}

namespace {

using EntryMap = std::map<std::filesystem::path, WatchFileState>;

EntryMap entryMap(const std::vector<WorkspaceEntry>& entries) {
    EntryMap result;
    for (const auto& entry : entries) {
        if (entry.path.empty() || entry.path.is_absolute()) {
            throw std::invalid_argument(
                "watcher entries must have non-empty workspace-relative paths");
        }
        if (!result.emplace(entry.path.lexically_normal(), entry.state).second) {
            throw std::invalid_argument("watcher entries must have unique paths");
        }
    }
    return result;
}

std::optional<WatchFileState> stateFor(const EntryMap& entries,
                                        const std::filesystem::path& path) {
    const auto found = entries.find(path);
    if (found == entries.end()) {
        return std::nullopt;
    }
    return found->second;
}

bool sameEventKey(const WatchEvent& left, const WatchEvent& right) {
    return left.path == right.path;
}

auto identityKey(const FileIdentity& identity) {
    return std::tuple{
        identity.volume, identity.file[0], identity.file[1]};
}

WatchEvent eventFrom(NativeWatchAction action,
                      const std::filesystem::path& path,
                      const std::optional<WatchFileState>& state) {
    WatchEvent result;
    result.path = path;
    result.kind = action == NativeWatchAction::Create
        ? WatchEventKind::Create
        : action == NativeWatchAction::Remove
            ? WatchEventKind::Remove
            : WatchEventKind::Modify;
    if (state) {
        result.identity = state->identity;
        result.size = state->size;
        result.modificationTime = state->modificationTime;
    }
    return result;
}

} // namespace

class WatchEventNormalizer::Impl {
public:
    struct PendingEvent {
        WatchEvent event;
        WatchTimePoint readyAt;
        std::uint64_t ingest = 0;
    };

    struct RenamePair {
        std::optional<NativeWatchEvent> from;
        std::optional<NativeWatchEvent> to;
        WatchTimePoint readyAt;
        std::uint64_t fromIngest = 0;
    };

    Impl(WatcherConfig requested,
         std::vector<WorkspaceEntry> initialEntries,
         WorkspaceScanner requestedScanner)
        : config_(requested),
          cache_(entryMap(initialEntries)),
          scanner_(std::move(requestedScanner)) {
        if (config_.debounce.count() < 0 || config_.maxQueuedEvents == 0 ||
            config_.rescanRetry.count() <= 0 ||
            config_.maxPendingRenames == 0 ||
            config_.maxSaveExpectations == 0 ||
            config_.maxRescanEntries == 0) {
            throw std::invalid_argument(
                "watcher debounce must be non-negative and all bounds must be positive");
        }
        if (!scanner_) {
            throw std::invalid_argument("watcher scanner must be provided");
        }
    }

    void registerSave(SaveExpectation expectation) {
        expectation.path = normalizePath(std::move(expectation.path));
        if (saveExpectations_.size() == config_.maxSaveExpectations) {
            saveExpectations_.pop_front();
        }
        saveExpectations_.push_back(std::move(expectation));
    }

    void push(NativeWatchEvent native, WatchTimePoint observedAt) {
        if (overflowRequested_) {
            return;
        }
        if (native.action == NativeWatchAction::Overflow) {
            requestOverflow();
            return;
        }
        // A strictly monotonic ingestion ordinal orders events by ARRIVAL, not by
        // the coarse observation clock (which can stamp a whole poll batch with one
        // value). expireRenames uses it to tell an atomic-replace recreate -- a
        // Create that arrived AFTER the rename-away -- from a pre-existing edit.
        currentIngest_ = nextIngest_++;
        native.path = normalizePath(std::move(native.path));
        if (native.action == NativeWatchAction::RenameFrom ||
            native.action == NativeWatchAction::RenameTo) {
            pushRename(std::move(native), observedAt);
            return;
        }

        if (native.action == NativeWatchAction::Remove && !native.observed) {
            native.observed = stateFor(cache_, native.path);
        }
        auto event = eventFrom(native.action, native.path, native.observed);
        updateCache(native.action, native.path, native.observed);
        enqueue(std::move(event), observedAt);
    }

    std::vector<WatchEvent> takeReady(WatchTimePoint now) {
        if (overflowRequested_) {
            if (now < nextRescanAt_) {
                return {};
            }
            return rescan(now);
        }
        expireRenames(now);
        if (overflowRequested_) {
            return rescan(now);
        }

        std::vector<WatchEvent> result;
        auto current = pending_.begin();
        while (current != pending_.end()) {
            if (current->readyAt > now) {
                ++current;
                continue;
            }
            correlateSave(current->event);
            current->event.sequence = nextSequence_++;
            result.push_back(std::move(current->event));
            current = pending_.erase(current);
        }
        return result;
    }

private:
    static std::filesystem::path normalizePath(std::filesystem::path path) {
        path = path.lexically_normal();
        if (path.empty() || path == "." || path.is_absolute()) {
            throw std::invalid_argument(
                "watcher paths must be non-empty and workspace-relative");
        }
        for (const auto& component : path) {
            if (component == "..") {
                throw std::invalid_argument(
                    "watcher paths must remain beneath the workspace");
            }
        }
        return path;
    }

    void updateCache(NativeWatchAction action,
                      const std::filesystem::path& path,
                      const std::optional<WatchFileState>& observed) {
        if (action == NativeWatchAction::Remove) {
            cache_.erase(path);
        } else if (observed) {
            cache_[path] = *observed;
        }
    }

    void pushRename(NativeWatchEvent native, WatchTimePoint observedAt) {
        if (native.renameToken == 0) {
            if (native.action == NativeWatchAction::RenameFrom &&
                !native.observed) {
                native.observed = stateFor(cache_, native.path);
            }
            auto boundary = eventFrom(
                native.action == NativeWatchAction::RenameFrom
                    ? NativeWatchAction::Remove
                    : NativeWatchAction::Create,
                native.path, native.observed);
            updateCache(
                native.action == NativeWatchAction::RenameFrom
                    ? NativeWatchAction::Remove
                    : NativeWatchAction::Create,
                native.path, native.observed);
            enqueue(std::move(boundary), observedAt);
            return;
        }

        auto found = renames_.find(native.renameToken);
        if (found == renames_.end()) {
            if (renames_.size() == config_.maxPendingRenames) {
                requestOverflow();
                return;
            }
            found = renames_.emplace(
                native.renameToken,
                RenamePair{{}, {}, observedAt + config_.debounce}).first;
        }
        found->second.readyAt = observedAt + config_.debounce;
        if (native.action == NativeWatchAction::RenameFrom) {
            if (!native.observed) {
                native.observed = stateFor(cache_, native.path);
            }
            found->second.from = std::move(native);
            found->second.fromIngest = currentIngest_;
        } else {
            found->second.to = std::move(native);
        }

        if (found->second.from && found->second.to) {
            emitRename(found->second, observedAt);
            renames_.erase(found);
        }
    }

    void emitRename(const RenamePair& pair, WatchTimePoint observedAt) {
        auto observed = pair.to->observed;
        if (!observed) {
            observed = pair.from->observed;
        }
        const auto sourcePending = std::find_if(
            pending_.rbegin(), pending_.rend(),
            [&pair](const PendingEvent& candidate) {
                return candidate.event.path == pair.from->path;
            });
        const auto sourceWasNew =
            sourcePending != pending_.rend() &&
            sourcePending->event.kind == WatchEventKind::Create;
        if (sourcePending != pending_.rend()) {
            pending_.erase(std::next(sourcePending).base());
        }
        WatchEvent event = eventFrom(
            NativeWatchAction::Modify, pair.to->path, observed);
        if (sourceWasNew) {
            event.kind = cache_.contains(pair.to->path)
                ? WatchEventKind::Modify
                : WatchEventKind::Create;
        } else {
            event.kind = WatchEventKind::Rename;
            event.previousPath = pair.from->path;
        }
        cache_.erase(pair.from->path);
        if (observed) {
            cache_[pair.to->path] = *observed;
        }
        enqueue(std::move(event), observedAt);
    }

    void expireRenames(WatchTimePoint now) {
        for (auto current = renames_.begin(); current != renames_.end();) {
            if (current->second.readyAt > now) {
                ++current;
                continue;
            }
            const auto& half = current->second;
            if (half.from) {
                // An unpaired rename-away whose path is recreated AFTER it (a pending
                // Create/Modify observed later than the rename) is an ATOMIC REPLACE
                // -- an editor saved by renaming the original out of the watched tree
                // and writing a fresh file at the same path. The workspace effect is a
                // MODIFY, not a removal: emitting a Remove here would coalesce-cancel
                // that recreate (Create+Remove nets to nothing) and hide the change.
                // The recreate must post-date the rename-away by INGESTION ORDER (a
                // strict monotonic arrival ordinal, not the coarse observation clock
                // which can stamp a whole poll batch alike): a file modified and THEN
                // genuinely moved away leaves a pending event ingested before the
                // rename, and must still surface as a Remove.
                const auto renameFromIngest = current->second.fromIngest;
                const auto recreated = std::find_if(
                    pending_.begin(), pending_.end(),
                    [&half, renameFromIngest](const PendingEvent& candidate) {
                        return candidate.event.path == half.from->path &&
                               candidate.ingest > renameFromIngest &&
                               (candidate.event.kind == WatchEventKind::Create ||
                                candidate.event.kind == WatchEventKind::Modify);
                    });
                if (recreated != pending_.end()) {
                    recreated->event.kind = WatchEventKind::Modify;
                } else {
                    auto event = eventFrom(NativeWatchAction::Remove,
                                            half.from->path, half.from->observed);
                    cache_.erase(half.from->path);
                    enqueue(std::move(event), now - config_.debounce);
                }
            } else if (half.to) {
                auto event = eventFrom(NativeWatchAction::Create,
                                        half.to->path, half.to->observed);
                if (half.to->observed) {
                    cache_[half.to->path] = *half.to->observed;
                }
                enqueue(std::move(event), now - config_.debounce);
            }
            current = renames_.erase(current);
            if (overflowRequested_) {
                return;
            }
        }
    }

    void enqueue(WatchEvent event, WatchTimePoint observedAt) {
        const auto same = std::find_if(
            pending_.rbegin(), pending_.rend(),
            [&event](const PendingEvent& candidate) {
                return sameEventKey(candidate.event, event);
            });
        if (same != pending_.rend()) {
            const auto distinctReplacement =
                same->event.kind == WatchEventKind::Remove &&
                event.kind == WatchEventKind::Create &&
                (!same->event.identity || !event.identity ||
                 same->event.identity != event.identity);
            if (!distinctReplacement) {
                if (coalesce(same->event, event)) {
                    same->readyAt = observedAt + config_.debounce;
                    same->ingest = currentIngest_;
                } else {
                    pending_.erase(std::next(same).base());
                }
                return;
            }
        }
        if (pending_.size() == config_.maxQueuedEvents) {
            requestOverflow();
            return;
        }
        pending_.push_back({std::move(event), observedAt + config_.debounce,
                            currentIngest_});
    }

    static bool coalesce(WatchEvent& current, const WatchEvent& next) {
        if (current.kind == WatchEventKind::Create &&
            next.kind == WatchEventKind::Remove) {
            return false;
        }
        if (current.kind == WatchEventKind::Create &&
            next.kind == WatchEventKind::Modify) {
            current.identity = next.identity;
            current.size = next.size;
            current.modificationTime = next.modificationTime;
            return true;
        }
        if (current.kind == WatchEventKind::Modify &&
            next.kind == WatchEventKind::Remove) {
            current = next;
            return true;
        }
        if (current.kind == WatchEventKind::Remove &&
            next.kind == WatchEventKind::Create) {
            current = next;
            current.kind = WatchEventKind::Modify;
            return true;
        }
        if (current.kind == WatchEventKind::Rename &&
            next.kind == WatchEventKind::Remove) {
            const auto originalPath = *current.previousPath;
            current = next;
            current.path = originalPath;
            current.previousPath.reset();
            return true;
        }
        if (current.kind == WatchEventKind::Rename &&
            next.kind == WatchEventKind::Modify) {
            current.path = next.path;
            current.identity = next.identity;
            current.size = next.size;
            current.modificationTime = next.modificationTime;
            return true;
        }
        current = next;
        return true;
    }

    void requestOverflow() {
        overflowRequested_ = true;
        overflowAnnounced_ = false;
        nextRescanAt_ = WatchTimePoint::min();
        pending_.clear();
    }

    std::vector<WatchEvent> rescan(WatchTimePoint now) {
        renames_.clear();
        std::vector<WatchEvent> result;
        if (!overflowAnnounced_) {
            WatchEvent overflow;
            overflow.kind = WatchEventKind::Overflow;
            overflow.sequence = nextSequence_++;
            result.push_back(std::move(overflow));
            overflowAnnounced_ = true;
        }

        const auto snapshot = scanner_(config_.maxRescanEntries);
        if (!snapshot.complete) {
            overflowRequested_ = true;
            nextRescanAt_ = now + config_.rescanRetry;
            return result;
        }
        overflowRequested_ = false;
        overflowAnnounced_ = false;
        const auto scanned = entryMap(snapshot.entries);
        std::multimap<
            std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>,
            std::filesystem::path> oldPathsByIdentity;
        for (const auto& [path, state] : cache_) {
            if (!scanned.contains(path)) {
                oldPathsByIdentity.emplace(identityKey(state.identity), path);
            }
        }
        std::vector<std::filesystem::path> renamedFrom;
        for (const auto& [path, state] : scanned) {
            const auto previous = cache_.find(path);
            if (previous == cache_.end()) {
                auto event = eventFrom(NativeWatchAction::Create, path, state);
                const auto old =
                    oldPathsByIdentity.find(identityKey(state.identity));
                if (old != oldPathsByIdentity.end()) {
                    event.kind = WatchEventKind::Rename;
                    event.previousPath = old->second;
                    renamedFrom.push_back(old->second);
                    oldPathsByIdentity.erase(old);
                }
                event.sequence = nextSequence_++;
                correlateSave(event);
                result.push_back(std::move(event));
            } else if (previous->second != state) {
                auto event = eventFrom(NativeWatchAction::Modify, path, state);
                event.sequence = nextSequence_++;
                correlateSave(event);
                result.push_back(std::move(event));
            }
        }
        for (const auto& [path, state] : cache_) {
            if (!scanned.contains(path) &&
                std::find(renamedFrom.begin(), renamedFrom.end(), path) ==
                    renamedFrom.end()) {
                auto event = eventFrom(NativeWatchAction::Remove, path, state);
                event.sequence = nextSequence_++;
                correlateSave(event);
                result.push_back(std::move(event));
            }
        }
        cache_ = scanned;
        return result;
    }

    void correlateSave(WatchEvent& event) {
        if (!event.identity || !event.size || !event.modificationTime) {
            return;
        }
        const WatchFileState state{
            *event.identity, *event.size, *event.modificationTime};
        const auto found = std::find_if(
            saveExpectations_.begin(), saveExpectations_.end(),
            [&event, &state](const SaveExpectation& expectation) {
                return expectation.path == event.path &&
                       expectation.state == state;
            });
        if (found != saveExpectations_.end()) {
            event.origin = WatchEventOrigin::SsgSave;
            saveExpectations_.erase(found);
        }
    }

    WatcherConfig config_;
    EntryMap cache_;
    WorkspaceScanner scanner_;
    std::vector<PendingEvent> pending_;
    std::map<std::uint64_t, RenamePair> renames_;
    std::deque<SaveExpectation> saveExpectations_;
    std::uint64_t nextSequence_ = 1;
    std::uint64_t nextIngest_ = 0;
    std::uint64_t currentIngest_ = 0;
    bool overflowRequested_ = false;
    bool overflowAnnounced_ = false;
    WatchTimePoint nextRescanAt_ = WatchTimePoint::min();
};

WatchEventNormalizer::WatchEventNormalizer(
    WatcherConfig config, std::vector<WorkspaceEntry> initialEntries,
    WorkspaceScanner scanner)
    : impl_(std::make_unique<Impl>(
          config, std::move(initialEntries), std::move(scanner))) {}

WatchEventNormalizer::~WatchEventNormalizer() = default;
WatchEventNormalizer::WatchEventNormalizer(WatchEventNormalizer&&) noexcept =
    default;
WatchEventNormalizer& WatchEventNormalizer::operator=(
    WatchEventNormalizer&&) noexcept = default;

void WatchEventNormalizer::registerSave(SaveExpectation expectation) {
    impl_->registerSave(std::move(expectation));
}

void WatchEventNormalizer::push(NativeWatchEvent event,
                                WatchTimePoint observedAt) {
    impl_->push(std::move(event), observedAt);
}

std::vector<WatchEvent> WatchEventNormalizer::takeReady(WatchTimePoint now) {
    return impl_->takeReady(now);
}

} // namespace ssg
