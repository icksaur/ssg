#include "ssg/external_modification_flow.h"

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
    return {ExternalModificationViewState{delta.revision, std::move(files)},
            ExternalDeltaError::None};
}

class ExternalModificationFlow::Impl {
public:
    struct PendingChange {
        ExternalDocumentView view;
        std::optional<std::string> diskContent;
    };

    Impl(RecoveryActions& recovery, DiffModel& diff)
        : recovery_{recovery}, diff_{diff} {}

    ExternalModificationResult processEvent(
        ExternalEventInput input,
        std::optional<JournalDocument>& document) {
        if (input.event.sequence <= lastWatcherSequence_) {
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

        const auto existing = std::find_if(
            stagedPending.begin(), stagedPending.end(), [&](const auto& item) {
                return item.view.id == input.id;
            });

        if (saveEvent) {
            if (existing != stagedPending.end()) {
                stagedPending.erase(existing);
            }
        } else if (!stagedDocument.dirty && !removed) {
            stagedDocument.utf8Content = *input.diskContent;
            stagedDocument.dirty = false;
            if (input.event.kind == WatchEventKind::Rename) {
                stagedDocument.key = JournalDocumentKey::saved(
                    input.event.path.generic_string());
            }
            if (existing != stagedPending.end()) {
                stagedPending.erase(existing);
            }
        } else {
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
            input.diskContent};
        const auto diffResult = diff_.applyNonGitEvent(
            std::move(diffEvent), Revision{input.event.sequence});

        if (!saveEvent && !document->dirty && !removed) {
            document->key = std::move(stagedDocument.key);
            document->utf8Content.swap(stagedDocument.utf8Content);
            document->dirty = false;
        }
        pending_.swap(stagedPending);
        lastWatcherSequence_ = input.event.sequence;
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
        pending_.erase(pending);
        advanceRevision();
        return {ExternalModificationError::None, false, true,
                std::move(result.compensation)};
    }

    ExternalModificationResult keepBuffer(const DiffFileId& id) {
        const auto pending = findPending(id);
        if (pending == pending_.end()) {
            return failure(ExternalModificationError::NoExternalChange);
        }
        pending_.erase(pending);
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
        return state;
    }

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

    RecoveryActions& recovery_;
    DiffModel& diff_;
    std::uint64_t lastWatcherSequence_ = 0;
    Revision revision_{0};
    std::vector<PendingChange> pending_;
};

ExternalModificationFlow::ExternalModificationFlow(RecoveryActions& recovery,
                                                   DiffModel& diff)
    : impl_{std::make_unique<Impl>(recovery, diff)} {}

ExternalModificationFlow::~ExternalModificationFlow() = default;
ExternalModificationFlow::ExternalModificationFlow(
    ExternalModificationFlow&&) noexcept = default;
ExternalModificationFlow& ExternalModificationFlow::operator=(
    ExternalModificationFlow&&) noexcept = default;

ExternalModificationResult ExternalModificationFlow::processEvent(
    ExternalEventInput input, std::optional<JournalDocument>& document) {
    return impl_->processEvent(std::move(input), document);
}

ExternalModificationResult ExternalModificationFlow::reload(
    const DiffFileId& id, std::optional<JournalDocument>& document) {
    return impl_->reload(id, document);
}

ExternalModificationResult ExternalModificationFlow::keepBuffer(
    const DiffFileId& id) {
    return impl_->keepBuffer(id);
}

ExternalOpenDiffResult ExternalModificationFlow::openDiff(
    const DiffFileId& id) const {
    return impl_->openDiff(id);
}

ExternalModificationViewState ExternalModificationFlow::viewState() const {
    return impl_->viewState();
}

}  // namespace ssg
