#include "ssg/watcher.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace ssg {
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
        result.modification_time = state->modification_time;
    }
    return result;
}

} // namespace

class WatchEventNormalizer::Impl {
public:
    struct PendingEvent {
        WatchEvent event;
        WatchTimePoint ready_at;
    };

    struct RenamePair {
        std::optional<NativeWatchEvent> from;
        std::optional<NativeWatchEvent> to;
        WatchTimePoint ready_at;
    };

    Impl(WatcherConfig requested,
         std::vector<WorkspaceEntry> initialEntries,
         WorkspaceScanner requestedScanner)
        : config(requested),
          cache(entryMap(initialEntries)),
          scanner(std::move(requestedScanner)) {
        if (config.debounce.count() < 0 || config.max_queued_events == 0 ||
            config.rescan_retry.count() <= 0 ||
            config.max_pending_renames == 0 ||
            config.max_save_expectations == 0 ||
            config.max_rescan_entries == 0) {
            throw std::invalid_argument(
                "watcher debounce must be non-negative and all bounds must be positive");
        }
        if (!scanner) {
            throw std::invalid_argument("watcher scanner must be provided");
        }
    }

    void registerSave(SaveExpectation expectation) {
        expectation.path = normalizePath(std::move(expectation.path));
        if (save_expectations.size() == config.max_save_expectations) {
            save_expectations.pop_front();
        }
        save_expectations.push_back(std::move(expectation));
    }

    void push(NativeWatchEvent native, WatchTimePoint observedAt) {
        if (overflow_requested) {
            return;
        }
        if (native.action == NativeWatchAction::Overflow) {
            requestOverflow();
            return;
        }
        native.path = normalizePath(std::move(native.path));
        if (native.action == NativeWatchAction::RenameFrom ||
            native.action == NativeWatchAction::RenameTo) {
            pushRename(std::move(native), observedAt);
            return;
        }

        if (native.action == NativeWatchAction::Remove && !native.observed) {
            native.observed = stateFor(cache, native.path);
        }
        auto event = eventFrom(native.action, native.path, native.observed);
        updateCache(native.action, native.path, native.observed);
        enqueue(std::move(event), observedAt);
    }

    std::vector<WatchEvent> takeReady(WatchTimePoint now) {
        if (overflow_requested) {
            if (now < next_rescan_at) {
                return {};
            }
            return rescan(now);
        }
        expireRenames(now);
        if (overflow_requested) {
            return rescan(now);
        }

        std::vector<WatchEvent> result;
        auto current = pending.begin();
        while (current != pending.end()) {
            if (current->ready_at > now) {
                ++current;
                continue;
            }
            correlateSave(current->event);
            current->event.sequence = next_sequence++;
            result.push_back(std::move(current->event));
            current = pending.erase(current);
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
            cache.erase(path);
        } else if (observed) {
            cache[path] = *observed;
        }
    }

    void pushRename(NativeWatchEvent native, WatchTimePoint observedAt) {
        if (native.rename_token == 0) {
            if (native.action == NativeWatchAction::RenameFrom &&
                !native.observed) {
                native.observed = stateFor(cache, native.path);
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

        auto found = renames.find(native.rename_token);
        if (found == renames.end()) {
            if (renames.size() == config.max_pending_renames) {
                requestOverflow();
                return;
            }
            found = renames.emplace(
                native.rename_token,
                RenamePair{{}, {}, observedAt + config.debounce}).first;
        }
        found->second.ready_at = observedAt + config.debounce;
        if (native.action == NativeWatchAction::RenameFrom) {
            if (!native.observed) {
                native.observed = stateFor(cache, native.path);
            }
            found->second.from = std::move(native);
        } else {
            found->second.to = std::move(native);
        }

        if (found->second.from && found->second.to) {
            emitRename(found->second, observedAt);
            renames.erase(found);
        }
    }

    void emitRename(const RenamePair& pair, WatchTimePoint observedAt) {
        auto observed = pair.to->observed;
        if (!observed) {
            observed = pair.from->observed;
        }
        const auto sourcePending = std::find_if(
            pending.rbegin(), pending.rend(),
            [&pair](const PendingEvent& candidate) {
                return candidate.event.path == pair.from->path;
            });
        const auto sourceWasNew =
            sourcePending != pending.rend() &&
            sourcePending->event.kind == WatchEventKind::Create;
        if (sourcePending != pending.rend()) {
            pending.erase(std::next(sourcePending).base());
        }
        WatchEvent event = eventFrom(
            NativeWatchAction::Modify, pair.to->path, observed);
        if (sourceWasNew) {
            event.kind = cache.contains(pair.to->path)
                ? WatchEventKind::Modify
                : WatchEventKind::Create;
        } else {
            event.kind = WatchEventKind::Rename;
            event.previous_path = pair.from->path;
        }
        cache.erase(pair.from->path);
        if (observed) {
            cache[pair.to->path] = *observed;
        }
        enqueue(std::move(event), observedAt);
    }

    void expireRenames(WatchTimePoint now) {
        for (auto current = renames.begin(); current != renames.end();) {
            if (current->second.ready_at > now) {
                ++current;
                continue;
            }
            const auto& half = current->second;
            if (half.from) {
                auto event = eventFrom(NativeWatchAction::Remove,
                                        half.from->path, half.from->observed);
                cache.erase(half.from->path);
                enqueue(std::move(event), now - config.debounce);
            } else if (half.to) {
                auto event = eventFrom(NativeWatchAction::Create,
                                        half.to->path, half.to->observed);
                if (half.to->observed) {
                    cache[half.to->path] = *half.to->observed;
                }
                enqueue(std::move(event), now - config.debounce);
            }
            current = renames.erase(current);
            if (overflow_requested) {
                return;
            }
        }
    }

    void enqueue(WatchEvent event, WatchTimePoint observedAt) {
        const auto same = std::find_if(
            pending.rbegin(), pending.rend(),
            [&event](const PendingEvent& candidate) {
                return sameEventKey(candidate.event, event);
            });
        if (same != pending.rend()) {
            const auto distinctReplacement =
                same->event.kind == WatchEventKind::Remove &&
                event.kind == WatchEventKind::Create &&
                (!same->event.identity || !event.identity ||
                 same->event.identity != event.identity);
            if (!distinctReplacement) {
                if (coalesce(same->event, event)) {
                    same->ready_at = observedAt + config.debounce;
                } else {
                    pending.erase(std::next(same).base());
                }
                return;
            }
        }
        if (pending.size() == config.max_queued_events) {
            requestOverflow();
            return;
        }
        pending.push_back({std::move(event), observedAt + config.debounce});
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
            current.modification_time = next.modification_time;
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
            const auto originalPath = *current.previous_path;
            current = next;
            current.path = originalPath;
            current.previous_path.reset();
            return true;
        }
        if (current.kind == WatchEventKind::Rename &&
            next.kind == WatchEventKind::Modify) {
            current.path = next.path;
            current.identity = next.identity;
            current.size = next.size;
            current.modification_time = next.modification_time;
            return true;
        }
        current = next;
        return true;
    }

    void requestOverflow() {
        overflow_requested = true;
        overflow_announced = false;
        next_rescan_at = WatchTimePoint::min();
        pending.clear();
    }

    std::vector<WatchEvent> rescan(WatchTimePoint now) {
        renames.clear();
        std::vector<WatchEvent> result;
        if (!overflow_announced) {
            WatchEvent overflow;
            overflow.kind = WatchEventKind::Overflow;
            overflow.sequence = next_sequence++;
            result.push_back(std::move(overflow));
            overflow_announced = true;
        }

        const auto snapshot = scanner(config.max_rescan_entries);
        if (!snapshot.complete) {
            overflow_requested = true;
            next_rescan_at = now + config.rescan_retry;
            return result;
        }
        overflow_requested = false;
        overflow_announced = false;
        const auto scanned = entryMap(snapshot.entries);
        std::multimap<
            std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>,
            std::filesystem::path> oldPathsByIdentity;
        for (const auto& [path, state] : cache) {
            if (!scanned.contains(path)) {
                oldPathsByIdentity.emplace(identityKey(state.identity), path);
            }
        }
        std::vector<std::filesystem::path> renamedFrom;
        for (const auto& [path, state] : scanned) {
            const auto previous = cache.find(path);
            if (previous == cache.end()) {
                auto event = eventFrom(NativeWatchAction::Create, path, state);
                const auto old =
                    oldPathsByIdentity.find(identityKey(state.identity));
                if (old != oldPathsByIdentity.end()) {
                    event.kind = WatchEventKind::Rename;
                    event.previous_path = old->second;
                    renamedFrom.push_back(old->second);
                    oldPathsByIdentity.erase(old);
                }
                event.sequence = next_sequence++;
                correlateSave(event);
                result.push_back(std::move(event));
            } else if (previous->second != state) {
                auto event = eventFrom(NativeWatchAction::Modify, path, state);
                event.sequence = next_sequence++;
                correlateSave(event);
                result.push_back(std::move(event));
            }
        }
        for (const auto& [path, state] : cache) {
            if (!scanned.contains(path) &&
                std::find(renamedFrom.begin(), renamedFrom.end(), path) ==
                    renamedFrom.end()) {
                auto event = eventFrom(NativeWatchAction::Remove, path, state);
                event.sequence = next_sequence++;
                correlateSave(event);
                result.push_back(std::move(event));
            }
        }
        cache = scanned;
        return result;
    }

    void correlateSave(WatchEvent& event) {
        if (!event.identity || !event.size || !event.modification_time) {
            return;
        }
        const WatchFileState state{
            *event.identity, *event.size, *event.modification_time};
        const auto found = std::find_if(
            save_expectations.begin(), save_expectations.end(),
            [&event, &state](const SaveExpectation& expectation) {
                return expectation.path == event.path &&
                       expectation.state == state;
            });
        if (found != save_expectations.end()) {
            event.origin = WatchEventOrigin::SsgSave;
            save_expectations.erase(found);
        }
    }

    WatcherConfig config;
    EntryMap cache;
    WorkspaceScanner scanner;
    std::vector<PendingEvent> pending;
    std::map<std::uint64_t, RenamePair> renames;
    std::deque<SaveExpectation> save_expectations;
    std::uint64_t next_sequence = 1;
    bool overflow_requested = false;
    bool overflow_announced = false;
    WatchTimePoint next_rescan_at = WatchTimePoint::min();
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
