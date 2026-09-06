#include <ssg/DraftAutosaveScheduler.h>

#include <ssg/Editor.h>

#include <algorithm>

namespace ssg {
namespace {

std::vector<AutosaveCandidate> autosaveCandidates(const Workspace& workspace) {
    std::vector<AutosaveCandidate> candidates;
    for (const auto id : workspace.documents()) {
        auto state = workspace.state(id);
        if (!state) continue;
        auto const* current = workspace.tryDocument(id);
        if (current == nullptr || current->mode() != DocumentMode::Edit) {
            continue;
        }
        const auto contentHash =
            state->dirty ? fastContentHash(current->snapshot().text) : 0;
        candidates.push_back({id, state->dirty, contentHash});
    }
    return candidates;
}

} // namespace

DraftAutosaveScheduler::DraftAutosaveScheduler(
    std::chrono::milliseconds interval)
    : interval_(interval) {}

void DraftAutosaveScheduler::setInterval(
    std::chrono::milliseconds interval) noexcept {
    interval_ = interval;
}

std::size_t DraftAutosaveScheduler::flushDueDrafts(Editor& editor) {
    setInterval(std::chrono::milliseconds{uint32Setting(
        editor.settings, SettingKey::AutosaveDebounceMs, 10000)});
    const auto candidates = autosaveCandidates(editor.workspace);
    std::size_t flushed = 0;
    for (const auto id : due(std::chrono::steady_clock::now(), candidates)) {
        flushed += persistDraft(editor, id);
    }
    return flushed;
}

std::size_t DraftAutosaveScheduler::flushAllDrafts(Editor& editor) {
    const auto candidates = autosaveCandidates(editor.workspace);
    std::size_t flushed = 0;
    for (const auto id :
         flushAll(std::chrono::steady_clock::now(), candidates)) {
        flushed += persistDraft(editor, id);
    }
    return flushed;
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
    oversizeReported_.erase(id.value());
}

std::size_t DraftAutosaveScheduler::persistDraft(
    Editor& editor, FileDocumentId document) {
    auto state = editor.workspace.state(document);
    auto const* current = editor.workspace.tryDocument(document);
    if (!state || current == nullptr) return 0;

    const auto& text = current->snapshot().text;
    if (text.size() > draftByteCap) {
        if (oversizeReported_.insert(document.value()).second) {
            editor.scratch.removeDocument(state->key);
            editor.enqueueStatus(
                StatusPriority::Warning,
                "file is too large to autosave a draft; unsaved edits are not "
                "crash-protected until saved");
        }
        return 0;
    }
    oversizeReported_.erase(document.value());
    editor.scratch.updateDocument(
        JournalDocument{state->key, current->mode(), state->dirty, text,
                        editor.workspace.baselineFor(document)});
    return 1;
}

} // namespace ssg
