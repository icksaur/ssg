#include "ssg/external_modification.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ssg {
namespace {

NonGitDiffEventKind diff_kind(WatchEventKind kind) {
    switch (kind) {
        case WatchEventKind::create:
            return NonGitDiffEventKind::create;
        case WatchEventKind::modify:
            return NonGitDiffEventKind::modify;
        case WatchEventKind::rename:
            return NonGitDiffEventKind::rename;
        case WatchEventKind::remove:
            return NonGitDiffEventKind::remove;
        case WatchEventKind::overflow:
            break;
    }
    throw std::invalid_argument("overflow has no diff event kind");
}

bool requires_content(WatchEventKind kind) {
    return kind == WatchEventKind::create ||
           kind == WatchEventKind::modify ||
           kind == WatchEventKind::rename;
}

auto find_file(std::vector<ExternalDocumentView>& files,
               const DiffFileId& id) {
    return std::find_if(files.begin(), files.end(), [&](const auto& file) {
        return file.id == id;
    });
}

auto find_file(const std::vector<ExternalDocumentView>& files,
               const DiffFileId& id) {
    return std::find_if(files.begin(), files.end(), [&](const auto& file) {
        return file.id == id;
    });
}

}  // namespace

ExternalModificationCommandSet external_modification_command_set() {
    return {};
}

ExternalModificationDelta derive_external_modification_delta(
    const ExternalModificationViewState& base,
    const ExternalModificationViewState& target) {
    ExternalModificationDelta delta{base.revision, target.revision};
    for (const auto& target_file : target.files) {
        const auto base_file = find_file(base.files, target_file.id);
        if (base_file == base.files.end() || *base_file != target_file) {
            delta.upserted.push_back(target_file);
        }
    }
    for (const auto& base_file : base.files) {
        if (find_file(target.files, base_file.id) == target.files.end()) {
            delta.removed.push_back(base_file.id);
        }
    }
    return delta;
}

ExternalDeltaReplayResult replay_external_modification_delta(
    const ExternalModificationViewState& base,
    const ExternalModificationDelta& delta) {
    if (base.revision != delta.base_revision) {
        return {std::nullopt, ExternalDeltaError::stale_revision};
    }
    if (delta.revision < delta.base_revision) {
        return {std::nullopt, ExternalDeltaError::malformed_delta};
    }

    auto files = base.files;
    for (const auto& removed : delta.removed) {
        const auto found = find_file(files, removed);
        if (found == files.end()) {
            return {std::nullopt, ExternalDeltaError::malformed_delta};
        }
        files.erase(found);
    }
    for (const auto& upserted : delta.upserted) {
        const auto found = find_file(files, upserted.id);
        if (found == files.end()) {
            files.push_back(upserted);
        } else {
            *found = upserted;
        }
    }
    return {ExternalModificationViewState{delta.revision, std::move(files)},
            ExternalDeltaError::none};
}

class ExternalModificationFlow::Impl {
public:
    struct PendingChange {
        ExternalDocumentView view;
        std::optional<std::string> disk_content;
    };

    Impl(RecoveryActions& recovery, DiffModel& diff)
        : recovery_{recovery}, diff_{diff} {}

    ExternalModificationResult process_event(
        ExternalEventInput input,
        std::optional<JournalDocument>& document) {
        if (input.event.sequence <= last_watcher_sequence_) {
            return failure(ExternalModificationError::stale_event);
        }
        if (input.event.kind == WatchEventKind::overflow) {
            return failure(ExternalModificationError::unsupported_event);
        }
        if (input.event.path.empty() ||
            (input.event.kind == WatchEventKind::rename &&
             !input.event.previous_path.has_value())) {
            return failure(ExternalModificationError::invalid_event);
        }
        if (!document.has_value()) {
            return failure(ExternalModificationError::document_missing);
        }
        if (requires_content(input.event.kind) &&
            !input.disk_content.has_value()) {
            return failure(ExternalModificationError::content_required);
        }

        auto staged_pending = pending_;
        auto staged_document = *document;
        const bool save_event =
            input.event.origin == WatchEventOrigin::ssg_save;
        const bool removed = input.event.kind == WatchEventKind::remove;
        bool publish_status = false;

        const auto existing = std::find_if(
            staged_pending.begin(), staged_pending.end(), [&](const auto& item) {
                return item.view.id == input.id;
            });

        if (save_event) {
            if (existing != staged_pending.end()) {
                staged_pending.erase(existing);
            }
        } else if (!staged_document.dirty && !removed) {
            staged_document.utf8_content = *input.disk_content;
            staged_document.dirty = false;
            if (input.event.kind == WatchEventKind::rename) {
                staged_document.key = JournalDocumentKey::saved(
                    input.event.path.generic_string());
            }
            if (existing != staged_pending.end()) {
                staged_pending.erase(existing);
            }
        } else {
            ExternalDocumentView view{
                input.id,
                input.event.path,
                removed ? ExternalDocumentStatus::externally_removed
                        : ExternalDocumentStatus::externally_modified,
                removed ? "File was removed outside SSG"
                        : "File was modified outside SSG",
                removed ? std::vector<ExternalAction>{
                              ExternalAction::keep_buffer,
                              ExternalAction::open_diff}
                        : std::vector<ExternalAction>{
                              ExternalAction::reload,
                              ExternalAction::keep_buffer,
                              ExternalAction::open_diff}};
            PendingChange change{std::move(view), input.disk_content};
            if (existing == staged_pending.end()) {
                staged_pending.push_back(std::move(change));
            } else {
                *existing = std::move(change);
            }
            publish_status = true;
        }

        NonGitDiffEvent diff_event{
            diff_kind(input.event.kind),
            input.id,
            input.event.path,
            input.event.previous_path,
            input.disk_content};
        const auto diff_result = diff_.apply_non_git_event(
            std::move(diff_event), Revision{input.event.sequence});

        if (!save_event && !document->dirty && !removed) {
            document->key = std::move(staged_document.key);
            document->utf8_content.swap(staged_document.utf8_content);
            document->dirty = false;
        }
        pending_.swap(staged_pending);
        last_watcher_sequence_ = input.event.sequence;
        advance_revision();
        return {ExternalModificationError::none, publish_status,
                diff_result.accepted(), std::nullopt};
    }

    ExternalModificationResult reload(
        const DiffFileId& id, std::optional<JournalDocument>& document) {
        const auto pending = find_pending(id);
        if (pending == pending_.end()) {
            return failure(ExternalModificationError::no_external_change);
        }
        if (!pending->disk_content.has_value()) {
            return failure(ExternalModificationError::content_required);
        }
        if (!document.has_value()) {
            return failure(ExternalModificationError::document_missing);
        }

        JournalDocument replacement{
            JournalDocumentKey::saved(pending->view.path.generic_string()),
            document->mode,
            false,
            *pending->disk_content};
        auto result =
            recovery_.reload_document(document, std::move(replacement));
        if (!result.accepted()) {
            return failure(ExternalModificationError::recovery_failed);
        }
        pending_.erase(pending);
        advance_revision();
        return {ExternalModificationError::none, false, true,
                std::move(result.compensation)};
    }

    ExternalModificationResult keep_buffer(const DiffFileId& id) {
        const auto pending = find_pending(id);
        if (pending == pending_.end()) {
            return failure(ExternalModificationError::no_external_change);
        }
        pending_.erase(pending);
        advance_revision();
        return {};
    }

    ExternalOpenDiffResult open_diff(const DiffFileId& id) const {
        if (find_pending(id) == pending_.end()) {
            return {ExternalModificationError::no_external_change,
                    std::nullopt};
        }
        const auto file = diff_.file(id);
        if (!file.has_value()) {
            return {ExternalModificationError::diff_rejected, std::nullopt};
        }
        return {ExternalModificationError::none,
                diff_open_file(file->get())};
    }

    ExternalModificationViewState view_state() const {
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

    std::vector<PendingChange>::iterator find_pending(const DiffFileId& id) {
        return std::find_if(pending_.begin(), pending_.end(),
                            [&](const auto& pending) {
                                return pending.view.id == id;
                            });
    }

    std::vector<PendingChange>::const_iterator find_pending(
        const DiffFileId& id) const {
        return std::find_if(pending_.begin(), pending_.end(),
                            [&](const auto& pending) {
                                return pending.view.id == id;
                            });
    }

    void advance_revision() {
        if (revision_.value() == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("external modification revision exhausted");
        }
        revision_ = Revision{revision_.value() + 1};
    }

    RecoveryActions& recovery_;
    DiffModel& diff_;
    std::uint64_t last_watcher_sequence_ = 0;
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

ExternalModificationResult ExternalModificationFlow::process_event(
    ExternalEventInput input, std::optional<JournalDocument>& document) {
    return impl_->process_event(std::move(input), document);
}

ExternalModificationResult ExternalModificationFlow::reload(
    const DiffFileId& id, std::optional<JournalDocument>& document) {
    return impl_->reload(id, document);
}

ExternalModificationResult ExternalModificationFlow::keep_buffer(
    const DiffFileId& id) {
    return impl_->keep_buffer(id);
}

ExternalOpenDiffResult ExternalModificationFlow::open_diff(
    const DiffFileId& id) const {
    return impl_->open_diff(id);
}

ExternalModificationViewState ExternalModificationFlow::view_state() const {
    return impl_->view_state();
}

}  // namespace ssg
