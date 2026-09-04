#pragma once

#include <ssg/Workspace.h>

#include <chrono>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace ssg {

// One open document's autosave-relevant state for a single scheduling decision.
// `contentHash` is a fast hash of the current buffer text, used to avoid
// re-flushing a draft whose content has not changed since its last flush.
struct AutosaveCandidate {
    FileDocumentId id;
    bool dirty = false;
    std::uint64_t contentHash = 0;
};

// Decides WHICH dirty open documents should have their draft flushed on a given
// heartbeat. This is the autosave *policy* — the library owns the debounce
// decision; the app only supplies a monotonic clock tick ("a moment passed").
//
// The policy: a document that has just become dirty flushes immediately on the
// next tick (EAGER — bounds the crash/kill loss window to one tick, not a full
// interval). After that it flushes again only when BOTH its content has changed
// since the last flush AND at least `interval` has elapsed. A document that is
// clean, or no longer open, drops its state, so its next dirty edit is eager
// again. The scheduler performs no I/O and holds only timing/identity state.
class DraftAutosaveScheduler {
public:
    explicit DraftAutosaveScheduler(
        std::chrono::milliseconds interval = std::chrono::seconds{10});

    void setInterval(std::chrono::milliseconds interval) noexcept;
    [[nodiscard]] std::chrono::milliseconds interval() const noexcept {
        return interval_;
    }

    // The ids to flush now, per the eager + debounced-interval policy above.
    // Also updates internal state: flushed ids record their content+time, and
    // clean or absent ids are forgotten.
    [[nodiscard]] std::vector<FileDocumentId> due(
        std::chrono::steady_clock::time_point now,
        std::span<const AutosaveCandidate> candidates);

    // Every dirty candidate, regardless of the debounce — for a clean process
    // exit, where a best-effort final flush must capture edits newer than the
    // last tick. State is updated so an immediately-following due() will not
    // re-flush the same unchanged content.
    [[nodiscard]] std::vector<FileDocumentId> flushAll(
        std::chrono::steady_clock::time_point now,
        std::span<const AutosaveCandidate> candidates);

    // Drop a document's state (e.g. when its tab closes), so a later reopen is
    // treated as first-seen (eager).
    void forget(FileDocumentId id) noexcept;

private:
    struct FlushRecord {
        std::uint64_t contentHash;
        std::chrono::steady_clock::time_point at;
    };

    std::chrono::milliseconds interval_;
    std::unordered_map<std::uint64_t, FlushRecord> flushed_;
};

} // namespace ssg
