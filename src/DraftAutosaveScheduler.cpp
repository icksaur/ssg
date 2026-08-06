#include "ssg/DraftAutosaveScheduler.h"

#include <algorithm>
#include <unordered_set>

namespace ssg {

DraftAutosaveScheduler::DraftAutosaveScheduler(
    std::chrono::milliseconds interval)
    : interval_(interval) {}

void DraftAutosaveScheduler::setInterval(
    std::chrono::milliseconds interval) noexcept {
    interval_ = interval;
}

std::vector<FileDocumentId> DraftAutosaveScheduler::due(
    std::chrono::steady_clock::time_point now,
    std::span<const AutosaveCandidate> candidates) {
    std::vector<FileDocumentId> flush;
    std::unordered_set<std::uint64_t> present;
    present.reserve(candidates.size());

    for (const auto& candidate : candidates) {
        present.insert(candidate.id.value());
        if (!candidate.dirty) {
            // A clean document forgets its history: its next dirty edit is eager.
            flushed_.erase(candidate.id.value());
            continue;
        }
        const auto found = flushed_.find(candidate.id.value());
        if (found == flushed_.end()) {
            // First tick seeing this document dirty: flush eagerly.
            flush.push_back(candidate.id);
            flushed_.emplace(candidate.id.value(),
                             FlushRecord{candidate.contentHash, now});
            continue;
        }
        const bool changed = candidate.contentHash != found->second.contentHash;
        const bool elapsed = now - found->second.at >= interval_;
        if (changed && elapsed) {
            flush.push_back(candidate.id);
            found->second = FlushRecord{candidate.contentHash, now};
        }
    }

    // Forget documents that are no longer open.
    std::erase_if(flushed_, [&](const auto& entry) {
        return !present.contains(entry.first);
    });
    return flush;
}

std::vector<FileDocumentId> DraftAutosaveScheduler::flushAll(
    std::chrono::steady_clock::time_point now,
    std::span<const AutosaveCandidate> candidates) {
    std::vector<FileDocumentId> flush;
    for (const auto& candidate : candidates) {
        if (!candidate.dirty) {
            flushed_.erase(candidate.id.value());
            continue;
        }
        flush.push_back(candidate.id);
        flushed_[candidate.id.value()] = FlushRecord{candidate.contentHash, now};
    }
    return flush;
}

void DraftAutosaveScheduler::forget(FileDocumentId id) noexcept {
    flushed_.erase(id.value());
}

} // namespace ssg
