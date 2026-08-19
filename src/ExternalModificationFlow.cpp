#include "ssg/ExternalModificationFlow.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ssg {
namespace {

NonGitDiffEventKind diffKind(WatchEventKind kind) {
    switch (kind) {
        case WatchEventKind::Create:
            return NonGitDiffEventKind::Create;
        case WatchEventKind::Modify:
            return NonGitDiffEventKind::Modify;
        case WatchEventKind::Rename:
            return NonGitDiffEventKind::Rename;
        case WatchEventKind::Remove:
            return NonGitDiffEventKind::Remove;
        case WatchEventKind::Overflow:
            break;
    }
    throw std::invalid_argument("overflow has no diff event kind");
}

bool requiresContent(WatchEventKind kind) {
    return kind == WatchEventKind::Create ||
           kind == WatchEventKind::Modify ||
           kind == WatchEventKind::Rename;
}

auto findFile(std::vector<ExternalDocumentView>& files,
               const DiffFileId& id) {
    return std::find_if(files.begin(), files.end(), [&](const auto& file) {
        return file.id == id;
    });
}

auto findFile(const std::vector<ExternalDocumentView>& files,
               const DiffFileId& id) {
    return std::find_if(files.begin(), files.end(), [&](const auto& file) {
        return file.id == id;
    });
}

}  // namespace

ExternalModificationCommandSet externalModificationCommandSet() {
    return {};
}

ExternalModificationDelta ExternalModificationDeltaCodec::derive(
    const ExternalModificationViewState& base,
    const ExternalModificationViewState& target) {
    ExternalModificationDelta delta{base.revision, target.revision};
    for (const auto& targetFile : target.files) {
        const auto baseFile = findFile(base.files, targetFile.id);
        if (baseFile == base.files.end() || *baseFile != targetFile) {
            delta.upserted.push_back(targetFile);
        }
    }
    for (const auto& baseFile : base.files) {
        if (findFile(target.files, baseFile.id) == target.files.end()) {
            delta.removed.push_back(baseFile.id);
        }
    }
    delta.selected = target.selected;
    return delta;
}

ExternalDeltaReplayResult ExternalModificationDeltaCodec::replay(
    const ExternalModificationViewState& base,
    const ExternalModificationDelta& delta) {
    if (base.revision != delta.baseRevision) {
        return {std::nullopt, ExternalDeltaError::StaleRevision};
    }
    if (delta.revision < delta.baseRevision) {
        return {std::nullopt, ExternalDeltaError::MalformedDelta};
    }

    auto files = base.files;
    for (const auto& removed : delta.removed) {
        const auto found = findFile(files, removed);
        if (found == files.end()) {
            return {std::nullopt, ExternalDeltaError::MalformedDelta};
        }
        files.erase(found);
    }
    for (const auto& upserted : delta.upserted) {
        const auto found = findFile(files, upserted.id);
        if (found == files.end()) {
            files.push_back(upserted);
        } else {
            *found = upserted;
        }
    }
    // A present selection must name a surviving file, or the replayed state would
    // carry a dangling selection -- fail loud rather than replay it.
    if (delta.selected.has_value() &&
        findFile(files, *delta.selected) == files.end()) {
        return {std::nullopt, ExternalDeltaError::MalformedDelta};
    }
    return {ExternalModificationViewState{delta.revision, std::move(files),
                                          delta.selected},
            ExternalDeltaError::None};
}

class ExternalModificationFlow::Impl {
public:
    struct PendingChange {
        ExternalDocumentView view;
        std::optional<std::string> diskContent;
    };

    Impl(RecoveryManager& recovery, DiffModel& diff)
        : recovery_{recovery}, diff_{diff} {}

    ExternalModificationResult processEvent(
        ExternalEventInput input, Revision diffRevision,
        std::optional<JournalDocument>& document,
        std::function<bool()> commitClean = {},
        std::function<bool()> commitConflictRename = {},
        bool enforceSequence = true) {
        if (enforceSequence && input.event.sequence <= lastWatcherSequence_) {
            return failure(ExternalModificationError::StaleEvent);
        }
        if (input.event.kind == WatchEventKind::Overflow) {
            return failure(ExternalModificationError::UnsupportedEvent);
        }
        if (input.event.path.empty() ||
            (input.event.kind == WatchEventKind::Rename &&
             !input.event.previousPath.has_value())) {
            return failure(ExternalModificationError::InvalidEvent);
        }
        if (!document.has_value()) {
            return failure(ExternalModificationError::DocumentMissing);
        }
        if (requiresContent(input.event.kind) &&
            !input.diskContent.has_value()) {
            return failure(ExternalModificationError::ContentRequired);
        }

        auto stagedPending = pending_;
        auto stagedDocument = *document;
        const bool saveEvent =
            input.event.origin == WatchEventOrigin::SsgSave;
        const bool removed = input.event.kind == WatchEventKind::Remove;
        bool publishStatus = false;

        // A rename changes the path, and the id is derived from the path, so any
        // pending entry staged under the previous id would be orphaned beside a
        // fresh one. Retire it first (before locating the entry for the new id).
        if (input.previousId.has_value() && *input.previousId != input.id) {
            const auto previous = std::find_if(
                stagedPending.begin(), stagedPending.end(),
                [&](const auto& item) {
                    return item.view.id == *input.previousId;
                });
            if (previous != stagedPending.end()) {
                stagedPending.erase(previous);
            }
        }

        const auto existing = std::find_if(
            stagedPending.begin(), stagedPending.end(), [&](const auto& item) {
                return item.view.id == input.id;
            });

        // A clean document adopts the disk change only if the workspace commit the
        // reconcile supplies succeeds (Decision 11): stage->commit->publish, so a
        // failed commit falls through to raising the conflict rather than clearing
        // to a stale buffer.
        const bool canCleanAdopt = !saveEvent && !stagedDocument.dirty && !removed;
        bool cleanCommitted = false;

        if (saveEvent) {
            if (existing != stagedPending.end()) {
                stagedPending.erase(existing);
            }
        } else if (canCleanAdopt && (!commitClean || commitClean())) {
            stagedDocument.utf8Content = *input.diskContent;
            stagedDocument.dirty = false;
            if (input.event.kind == WatchEventKind::Rename) {
                stagedDocument.key = JournalDocumentKey::saved(
                    input.event.path.generic_string());
            }
            if (existing != stagedPending.end()) {
                stagedPending.erase(existing);
            }
            cleanCommitted = true;
        } else {
            // A dirty rename keeps its buffer but still publishes the conflict
            // under the NEW-path id, so the workspace must adopt the new path
            // (baseline + identity, buffer preserved) BEFORE that id is published
            // (stage->commit->publish, Decision 11). A failed adoption must not
            // leave a conflict keyed to a path no document owns, so it fails the
            // whole event rather than publishing an unresolvable entry.
            if (input.event.kind == WatchEventKind::Rename &&
                commitConflictRename && !commitConflictRename()) {
                return failure(ExternalModificationError::RecoveryFailed);
            }
            ExternalDocumentView view{
                input.id,
                input.event.path,
                removed ? ExternalDocumentStatus::ExternallyRemoved
                        : ExternalDocumentStatus::ExternallyModified,
                removed ? "File was removed outside SSG"
                        : "File was modified outside SSG",
                removed ? std::vector<ExternalAction>{
                              ExternalAction::KeepBuffer,
                              ExternalAction::OpenDiff}
                        : std::vector<ExternalAction>{
                              ExternalAction::Reload,
                              ExternalAction::KeepBuffer,
                              ExternalAction::OpenDiff}};
            PendingChange change{std::move(view), input.diskContent};
            if (existing == stagedPending.end()) {
                stagedPending.push_back(std::move(change));
            } else {
                *existing = std::move(change);
            }
            publishStatus = true;
        }

        NonGitDiffEvent diffEvent{
            diffKind(input.event.kind),
            input.id,
            input.event.path,
            input.event.previousPath,
            std::move(input.baselineContent),
            input.diskContent};
        const auto diffResult = diff_.applyNonGitEvent(
            std::move(diffEvent), diffRevision);

        if (cleanCommitted) {
            document->key = std::move(stagedDocument.key);
            document->utf8Content.swap(stagedDocument.utf8Content);
            document->dirty = false;
        }
        pending_.swap(stagedPending);
        reconcileSelection(std::nullopt);
        if (enforceSequence) {
            // A resync (overflow recovery) is not part of the ordered watcher
            // stream: it must not advance the sequence high-water mark, or the
            // real events that follow the overflow would be rejected as stale.
            lastWatcherSequence_ = input.event.sequence;
        }
        advanceRevision();
        return {ExternalModificationError::None, publishStatus,
                diffResult.accepted(), std::nullopt};
    }

    ExternalModificationResult reload(
        const DiffFileId& id, std::optional<JournalDocument>& document) {
        const auto pending = findPending(id);
        if (pending == pending_.end()) {
            return failure(ExternalModificationError::NoExternalChange);
        }
        if (!pending->diskContent.has_value()) {
            return failure(ExternalModificationError::ContentRequired);
        }
        if (!document.has_value()) {
            return failure(ExternalModificationError::DocumentMissing);
        }

        JournalDocument replacement{
            JournalDocumentKey::saved(pending->view.path.generic_string()),
            document->mode,
            false,
            *pending->diskContent};
        auto result =
            recovery_.reloadDocument(document, std::move(replacement));
        if (!result.accepted()) {
            return failure(ExternalModificationError::RecoveryFailed);
        }
        const auto idx = static_cast<std::size_t>(pending - pending_.begin());
        pending_.erase(pending);
        reconcileSelection(idx);
        advanceRevision();
        return {ExternalModificationError::None, false, true,
                std::move(result.compensation)};
    }

    ExternalModificationResult resolveReload(
        const DiffFileId& id, const ExternalReloadCommit& commit) {
        const auto pending = findPending(id);
        if (pending == pending_.end()) {
            return failure(ExternalModificationError::NoExternalChange);
        }
        if (!pending->diskContent.has_value()) {
            return failure(ExternalModificationError::ContentRequired);
        }
        const auto committed = commit(*pending->diskContent);
        if (!committed.committed) {
            return failure(ExternalModificationError::RecoveryFailed);
        }
        const auto idx = static_cast<std::size_t>(pending - pending_.begin());
        pending_.erase(pending);
        reconcileSelection(idx);
        advanceRevision();
        return {ExternalModificationError::None, false, true,
                committed.compensation};
    }

    ExternalModificationResult keepBuffer(
        const DiffFileId& id, const ExternalKeepBufferCommit& commit) {
        const auto pending = findPending(id);
        if (pending == pending_.end()) {
            return failure(ExternalModificationError::NoExternalChange);
        }
        // Dismissing hands the EXACT captured dismissed state to the commit, which
        // advances the document's IN-MEMORY external baseline and enqueues a
        // best-effort refresh of any persisted draft record. The pending action
        // clears ONLY when that in-memory commit lands, so a failed commit leaves
        // the conflict raised.
        const bool removed =
            pending->view.status == ExternalDocumentStatus::ExternallyRemoved;
        // The commit is the ONLY thing that advances the baseline; an absent or
        // failed commit must leave the conflict raised. Clearing the pending action
        // without a landed baseline-advancing commit would resurrect the dismissed
        // conflict on the next duplicate event, overflow resync, or draft reopen.
        if (!commit || !commit(removed, pending->diskContent)) {
            return failure(ExternalModificationError::RecoveryFailed);
        }
        const auto idx = static_cast<std::size_t>(pending - pending_.begin());
        pending_.erase(pending);
        reconcileSelection(idx);
        advanceRevision();
        return {};
    }

    ExternalOpenDiffResult openDiff(const DiffFileId& id) const {
        if (findPending(id) == pending_.end()) {
            return {ExternalModificationError::NoExternalChange,
                    std::nullopt};
        }
        const auto file = diff_.file(id);
        if (!file.has_value()) {
            return {ExternalModificationError::DiffRejected, std::nullopt};
        }
        return {ExternalModificationError::None,
                diffOpenFile(file->get())};
    }

    ExternalModificationViewState viewState() const {
        ExternalModificationViewState state{revision_};
        state.files.reserve(pending_.size());
        for (const auto& pending : pending_) {
            state.files.push_back(pending.view);
        }
        state.selected = selected_;
        return state;
    }

    bool selectFile(const DiffFileId& id) {
        if (findPending(id) == pending_.end()) return false;
        if (selected_ && *selected_ == id) return false;
        selected_ = id;
        advanceRevision();
        return true;
    }

    bool hasFile(const DiffFileId& id) {
        return findPending(id) != pending_.end();
    }

    bool selectNext() { return moveSelection(+1); }
    bool selectPrevious() { return moveSelection(-1); }

private:
    static ExternalModificationResult failure(
        ExternalModificationError error) {
        return {error, false, false, std::nullopt};
    }

    std::vector<PendingChange>::iterator findPending(const DiffFileId& id) {
        return std::find_if(pending_.begin(), pending_.end(),
                            [&](const auto& pending) {
                                return pending.view.id == id;
                            });
    }

    std::vector<PendingChange>::const_iterator findPending(
        const DiffFileId& id) const {
        return std::find_if(pending_.begin(), pending_.end(),
                            [&](const auto& pending) {
                                return pending.view.id == id;
                            });
    }

    void advanceRevision() {
        if (revision_.value() == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("external modification revision exhausted");
        }
        revision_ = Revision{revision_.value() + 1};
    }

    // Keep the selection valid against the current file list: cleared when empty,
    // set to the first file when the section became non-empty, kept when still
    // valid, and re-homed to the file that now occupies the resolved-away slot
    // (`preferred`) otherwise.
    void reconcileSelection(std::optional<std::size_t> preferred) {
        if (pending_.empty()) {
            selected_.reset();
            return;
        }
        if (selected_ && findPending(*selected_) != pending_.end()) return;
        std::size_t index = 0;
        if (preferred) index = std::min(*preferred, pending_.size() - 1);
        selected_ = pending_[index].view.id;
    }

    bool moveSelection(int direction) {
        if (pending_.empty()) return false;
        std::size_t current = 0;
        if (selected_) {
            const auto found = findPending(*selected_);
            if (found != pending_.end()) {
                current = static_cast<std::size_t>(found - pending_.begin());
            }
        }
        const std::size_t size = pending_.size();
        const std::size_t next =
            direction > 0 ? (current + 1) % size : (current + size - 1) % size;
        const DiffFileId nextId = pending_[next].view.id;
        if (selected_ && *selected_ == nextId) return false;
        selected_ = nextId;
        advanceRevision();
        return true;
    }

    RecoveryManager& recovery_;
    DiffModel& diff_;
    std::uint64_t lastWatcherSequence_ = 0;
    Revision revision_{0};
    std::vector<PendingChange> pending_;
    std::optional<DiffFileId> selected_;
};

ExternalModificationFlow::ExternalModificationFlow(RecoveryManager& recovery,
                                                   DiffModel& diff)
    : impl_{std::make_unique<Impl>(recovery, diff)} {}

ExternalModificationFlow::~ExternalModificationFlow() = default;
ExternalModificationFlow::ExternalModificationFlow(
    ExternalModificationFlow&&) noexcept = default;
ExternalModificationFlow& ExternalModificationFlow::operator=(
    ExternalModificationFlow&&) noexcept = default;

ExternalModificationResult ExternalModificationFlow::processEvent(
    ExternalEventInput input, Revision diffRevision,
    std::optional<JournalDocument>& document,
    std::function<bool()> commitClean,
    std::function<bool()> commitConflictRename) {
    return impl_->processEvent(std::move(input), diffRevision, document,
                               std::move(commitClean),
                               std::move(commitConflictRename),
                               /*enforceSequence=*/true);
}

ExternalModificationResult ExternalModificationFlow::processResyncEvent(
    ExternalEventInput input, Revision diffRevision,
    std::optional<JournalDocument>& document,
    std::function<bool()> commitClean,
    std::function<bool()> commitConflictRename) {
    return impl_->processEvent(std::move(input), diffRevision, document,
                               std::move(commitClean),
                               std::move(commitConflictRename),
                               /*enforceSequence=*/false);
}

ExternalModificationResult ExternalModificationFlow::reload(
    const DiffFileId& id, std::optional<JournalDocument>& document) {
    return impl_->reload(id, document);
}

ExternalModificationResult ExternalModificationFlow::resolveReload(
    const DiffFileId& id, const ExternalReloadCommit& commit) {
    return impl_->resolveReload(id, commit);
}

ExternalModificationResult ExternalModificationFlow::keepBuffer(
    const DiffFileId& id, const ExternalKeepBufferCommit& commit) {
    return impl_->keepBuffer(id, commit);
}

ExternalOpenDiffResult ExternalModificationFlow::openDiff(
    const DiffFileId& id) const {
    return impl_->openDiff(id);
}

bool ExternalModificationFlow::selectFile(const DiffFileId& id) {
    return impl_->selectFile(id);
}

bool ExternalModificationFlow::hasFile(const DiffFileId& id) {
    return impl_->hasFile(id);
}

bool ExternalModificationFlow::selectNext() { return impl_->selectNext(); }

bool ExternalModificationFlow::selectPrevious() {
    return impl_->selectPrevious();
}

ExternalModificationViewState ExternalModificationFlow::viewState() const {
    return impl_->viewState();
}

}  // namespace ssg
