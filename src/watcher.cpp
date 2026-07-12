#include "ssg/watcher.h"

#include <algorithm>
#include <deque>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace ssg {
namespace {

using EntryMap = std::map<std::filesystem::path, WatchFileState>;

EntryMap entry_map(const std::vector<WorkspaceEntry>& entries) {
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

std::optional<WatchFileState> state_for(const EntryMap& entries,
                                        const std::filesystem::path& path) {
    const auto found = entries.find(path);
    if (found == entries.end()) {
        return std::nullopt;
    }
    return found->second;
}

bool same_event_key(const WatchEvent& left, const WatchEvent& right) {
    return left.path == right.path;
}

auto identity_key(const FileIdentity& identity) {
    return std::tuple{
        identity.volume, identity.file[0], identity.file[1]};
}

WatchEvent event_from(NativeWatchAction action,
                      const std::filesystem::path& path,
                      const std::optional<WatchFileState>& state) {
    WatchEvent result;
    result.path = path;
    result.kind = action == NativeWatchAction::create
        ? WatchEventKind::create
        : action == NativeWatchAction::remove
            ? WatchEventKind::remove
            : WatchEventKind::modify;
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
         std::vector<WorkspaceEntry> initial_entries,
         WorkspaceScanner requested_scanner)
        : config(requested),
          cache(entry_map(initial_entries)),
          scanner(std::move(requested_scanner)) {
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

    void register_save(SaveExpectation expectation) {
        expectation.path = normalize_path(std::move(expectation.path));
        if (save_expectations.size() == config.max_save_expectations) {
            save_expectations.pop_front();
        }
        save_expectations.push_back(std::move(expectation));
    }

    void push(NativeWatchEvent native, WatchTimePoint observed_at) {
        if (native.action == NativeWatchAction::overflow) {
            request_overflow();
            return;
        }
        if (overflow_requested) {
            return;
        }
        native.path = normalize_path(std::move(native.path));
        if (native.action == NativeWatchAction::rename_from ||
            native.action == NativeWatchAction::rename_to) {
            push_rename(std::move(native), observed_at);
            return;
        }

        if (native.action == NativeWatchAction::remove && !native.observed) {
            native.observed = state_for(cache, native.path);
        }
        auto event = event_from(native.action, native.path, native.observed);
        update_cache(native.action, native.path, native.observed);
        enqueue(std::move(event), observed_at);
    }

    std::vector<WatchEvent> take_ready(WatchTimePoint now) {
        expire_renames(now);
        if (overflow_requested) {
            if (now < next_rescan_at) {
                return {};
            }
            return rescan(now);
        }

        std::vector<WatchEvent> result;
        auto current = pending.begin();
        while (current != pending.end()) {
            if (current->ready_at > now) {
                ++current;
                continue;
            }
            correlate_save(current->event);
            current->event.sequence = next_sequence++;
            result.push_back(std::move(current->event));
            current = pending.erase(current);
        }
        return result;
    }

private:
    static std::filesystem::path normalize_path(std::filesystem::path path) {
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

    void update_cache(NativeWatchAction action,
                      const std::filesystem::path& path,
                      const std::optional<WatchFileState>& observed) {
        if (action == NativeWatchAction::remove) {
            cache.erase(path);
        } else if (observed) {
            cache[path] = *observed;
        }
    }

    void push_rename(NativeWatchEvent native, WatchTimePoint observed_at) {
        if (native.rename_token == 0) {
            if (native.action == NativeWatchAction::rename_from &&
                !native.observed) {
                native.observed = state_for(cache, native.path);
            }
            auto boundary = event_from(
                native.action == NativeWatchAction::rename_from
                    ? NativeWatchAction::remove
                    : NativeWatchAction::create,
                native.path, native.observed);
            update_cache(
                native.action == NativeWatchAction::rename_from
                    ? NativeWatchAction::remove
                    : NativeWatchAction::create,
                native.path, native.observed);
            enqueue(std::move(boundary), observed_at);
            return;
        }

        auto found = renames.find(native.rename_token);
        if (found == renames.end()) {
            if (renames.size() == config.max_pending_renames) {
                request_overflow();
                return;
            }
            found = renames.emplace(
                native.rename_token,
                RenamePair{{}, {}, observed_at + config.debounce}).first;
        }
        found->second.ready_at = observed_at + config.debounce;
        if (native.action == NativeWatchAction::rename_from) {
            if (!native.observed) {
                native.observed = state_for(cache, native.path);
            }
            found->second.from = std::move(native);
        } else {
            found->second.to = std::move(native);
        }

        if (found->second.from && found->second.to) {
            emit_rename(found->second, observed_at);
            renames.erase(found);
        }
    }

    void emit_rename(const RenamePair& pair, WatchTimePoint observed_at) {
        auto observed = pair.to->observed;
        if (!observed) {
            observed = pair.from->observed;
        }
        WatchEvent event = event_from(
            NativeWatchAction::modify, pair.to->path, observed);
        event.kind = WatchEventKind::rename;
        event.previous_path = pair.from->path;
        cache.erase(pair.from->path);
        if (observed) {
            cache[pair.to->path] = *observed;
        }
        enqueue(std::move(event), observed_at);
    }

    void expire_renames(WatchTimePoint now) {
        for (auto current = renames.begin(); current != renames.end();) {
            if (current->second.ready_at > now) {
                ++current;
                continue;
            }
            const auto& half = current->second;
            if (half.from) {
                auto event = event_from(NativeWatchAction::remove,
                                        half.from->path, half.from->observed);
                cache.erase(half.from->path);
                enqueue(std::move(event), now - config.debounce);
            } else if (half.to) {
                auto event = event_from(NativeWatchAction::create,
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

    void enqueue(WatchEvent event, WatchTimePoint observed_at) {
        const auto same = std::find_if(
            pending.begin(), pending.end(),
            [&event](const PendingEvent& candidate) {
                return same_event_key(candidate.event, event);
            });
        if (same != pending.end()) {
            if (coalesce(same->event, event)) {
                same->ready_at = observed_at + config.debounce;
            } else {
                pending.erase(same);
            }
            return;
        }
        if (pending.size() == config.max_queued_events) {
            request_overflow();
            return;
        }
        pending.push_back({std::move(event), observed_at + config.debounce});
    }

    static bool coalesce(WatchEvent& current, const WatchEvent& next) {
        if (current.kind == WatchEventKind::create &&
            next.kind == WatchEventKind::remove) {
            return false;
        }
        if (current.kind == WatchEventKind::create &&
            next.kind == WatchEventKind::modify) {
            current.identity = next.identity;
            current.size = next.size;
            current.modification_time = next.modification_time;
            return true;
        }
        if (current.kind == WatchEventKind::modify &&
            next.kind == WatchEventKind::remove) {
            current = next;
            return true;
        }
        if (current.kind == WatchEventKind::remove &&
            next.kind == WatchEventKind::create) {
            current = next;
            current.kind = WatchEventKind::modify;
            return true;
        }
        if (current.kind == WatchEventKind::rename &&
            next.kind == WatchEventKind::remove) {
            const auto original_path = *current.previous_path;
            current = next;
            current.path = original_path;
            current.previous_path.reset();
            return true;
        }
        if (current.kind == WatchEventKind::rename &&
            next.kind == WatchEventKind::modify) {
            current.path = next.path;
            current.identity = next.identity;
            current.size = next.size;
            current.modification_time = next.modification_time;
            return true;
        }
        current = next;
        return true;
    }

    void request_overflow() {
        overflow_requested = true;
        overflow_announced = false;
        next_rescan_at = WatchTimePoint::min();
        pending.clear();
        renames.clear();
    }

    std::vector<WatchEvent> rescan(WatchTimePoint now) {
        std::vector<WatchEvent> result;
        if (!overflow_announced) {
            WatchEvent overflow;
            overflow.kind = WatchEventKind::overflow;
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
        const auto scanned = entry_map(snapshot.entries);
        std::multimap<
            std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>,
            std::filesystem::path> old_paths_by_identity;
        for (const auto& [path, state] : cache) {
            if (!scanned.contains(path)) {
                old_paths_by_identity.emplace(identity_key(state.identity), path);
            }
        }
        std::vector<std::filesystem::path> renamed_from;
        for (const auto& [path, state] : scanned) {
            const auto previous = cache.find(path);
            if (previous == cache.end()) {
                auto event = event_from(NativeWatchAction::create, path, state);
                const auto old =
                    old_paths_by_identity.find(identity_key(state.identity));
                if (old != old_paths_by_identity.end()) {
                    event.kind = WatchEventKind::rename;
                    event.previous_path = old->second;
                    renamed_from.push_back(old->second);
                    old_paths_by_identity.erase(old);
                }
                event.sequence = next_sequence++;
                correlate_save(event);
                result.push_back(std::move(event));
            } else if (previous->second != state) {
                auto event = event_from(NativeWatchAction::modify, path, state);
                event.sequence = next_sequence++;
                correlate_save(event);
                result.push_back(std::move(event));
            }
        }
        for (const auto& [path, state] : cache) {
            if (!scanned.contains(path) &&
                std::find(renamed_from.begin(), renamed_from.end(), path) ==
                    renamed_from.end()) {
                auto event = event_from(NativeWatchAction::remove, path, state);
                event.sequence = next_sequence++;
                correlate_save(event);
                result.push_back(std::move(event));
            }
        }
        cache = scanned;
        return result;
    }

    void correlate_save(WatchEvent& event) {
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
            event.origin = WatchEventOrigin::ssg_save;
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
    WatcherConfig config, std::vector<WorkspaceEntry> initial_entries,
    WorkspaceScanner scanner)
    : impl_(std::make_unique<Impl>(
          config, std::move(initial_entries), std::move(scanner))) {}

WatchEventNormalizer::~WatchEventNormalizer() = default;
WatchEventNormalizer::WatchEventNormalizer(WatchEventNormalizer&&) noexcept =
    default;
WatchEventNormalizer& WatchEventNormalizer::operator=(
    WatchEventNormalizer&&) noexcept = default;

void WatchEventNormalizer::register_save(SaveExpectation expectation) {
    impl_->register_save(std::move(expectation));
}

void WatchEventNormalizer::push(NativeWatchEvent event,
                                WatchTimePoint observed_at) {
    impl_->push(std::move(event), observed_at);
}

std::vector<WatchEvent> WatchEventNormalizer::take_ready(WatchTimePoint now) {
    return impl_->take_ready(now);
}

} // namespace ssg
