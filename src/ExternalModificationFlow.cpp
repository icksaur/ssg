#include <ssg/ExternalModificationFlow.h>

#include <ssg/CommandCatalog.h>
#include <ssg/Editor.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ssg {
namespace {

struct ExternalActionDescriptor {
    std::string_view id;
    ExternalAction action;
    std::string_view label;
};

constexpr std::array<ExternalActionDescriptor, 3> kExternalActionDescriptors{{
    {"external.reload", ExternalAction::Reload, "Reload"},
    {"external.keep_buffer", ExternalAction::KeepBuffer, "Keep"},
    {"external.open_diff", ExternalAction::OpenDiff, "Diff"},
}};

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
    return kind == WatchEventKind::Create || kind == WatchEventKind::Modify ||
           kind == WatchEventKind::Rename;
}

std::optional<std::string> readFileText(const std::filesystem::path& path) {
    auto result = readFile(path);
    if (!result.ok()) return std::nullopt;
    return std::string{reinterpret_cast<const char*>(result.bytes.data()),
                       result.bytes.size()};
}

struct DiskObservation {
    std::optional<std::string> content;
    bool unknown = false;
};

DiskObservation observeDiskChange(const std::filesystem::path& root,
                                  WatchEvent& event) {
    DiskObservation observation;
    const auto path = root / event.path;
    if (event.kind != WatchEventKind::Remove) {
        observation.content = readFileText(path);
        if (!observation.content) {
            observation.unknown = true;
            event.kind = WatchEventKind::Remove;
        }
        return observation;
    }

    std::optional<FileStat> status;
    try {
        status = statFile(path);
    } catch (const std::system_error&) {
        observation.unknown = true;
    }
    if (status) {
        observation.content = readFileText(path);
        if (observation.content) {
            event.kind = WatchEventKind::Modify;
        } else {
            observation.unknown = true;
        }
    }
    return observation;
}

bool matchesExternalBaseline(const Workspace& workspace,
                             FileDocumentId document,
                             const WatchEvent& event,
                             const DiskObservation& observation) {
    return event.origin != WatchEventOrigin::SsgSave &&
           !observation.unknown && event.kind != WatchEventKind::Rename &&
           workspace.matchesExternalBaseline(document, observation.content);
}

} // namespace

DiffFileId externalDiffFileId(std::string_view savedPath) {
    return DiffFileId{"external:" + std::string{savedPath}};
}

ExternalActionAffordance externalActionAffordance(ExternalAction action) {
    for (auto const& descriptor : kExternalActionDescriptors) {
        if (descriptor.action == action) {
            return {action, std::string{descriptor.label},
                    std::string{descriptor.id}};
        }
    }
    throw std::invalid_argument("unknown external action");
}

void bindExternalModificationCommands(CommandCatalog& catalog,
                                      Editor& editor) {
    auto applyAction = [&editor](
                           ExternalAction action) -> CommandHandlerResult {
        const auto view = editor.external.viewState();
        if (!view.selected) {
            return failure("no external modification is selected");
        }
        const DiffFileId file = *view.selected;
        const auto selected = std::find_if(
            view.files.begin(), view.files.end(),
            [&](const ExternalDocumentView& candidate) {
                return candidate.id == file;
            });
        if (selected == view.files.end() ||
            std::none_of(selected->actions.begin(), selected->actions.end(),
                         [&](const ExternalActionAffordance& offered) {
                             return offered.action == action;
                         })) {
            return success();
        }
        if (action == ExternalAction::Reload) {
            const auto result = editor.external.resolveReload(file);
            return result.accepted()
                       ? success()
                       : failure("external modification reload failed");
        }
        if (action == ExternalAction::KeepBuffer) {
            const auto result = editor.external.keepBuffer(file);
            return result.accepted()
                       ? success()
                       : failure("external modification command failed");
        }
        auto opened = editor.external.openDiff(file);
        if (!opened.accepted() || !opened.target) {
            return failure("external diff target is unavailable");
        }
        const auto diffFile = editor.diff.file(opened.target->id);
        if (!diffFile) return failure("external diff is unavailable");
        return editor.openOrFocusLiveDiffTab(
            diffFile->get(), NavigationClass::Programmatic);
    };
    auto spec = [](std::string id, std::string summary) {
        return CommandSpec{
            .id = std::move(id),
            .owner = "external-modification-flow",
            .summary = std::move(summary),
            .effect = CommandEffect::Mutation,
            .luaApi = true,
        };
    };
    auto action = [&](std::string id, std::string summary,
                      ExternalAction which) {
        auto built = spec(std::move(id), std::move(summary));
        built.binding = bindNoArgumentHandler(
            [applyAction, which](CommandContext&) {
                return applyAction(which);
            });
        catalog.add(std::move(built));
    };
    action("external.reload", "Reload", ExternalAction::Reload);
    action("external.keep_buffer", "Keep Buffer", ExternalAction::KeepBuffer);
    action("external.open_diff", "Open Diff", ExternalAction::OpenDiff);

    {
        auto built =
            spec("external.invoke_action", "Invoke External Change Action");
        built.binding = bindWireHandler<ExternalActionInvocation>(
            [&editor, applyAction](
                CommandContext&, const ExternalActionInvocation& invocation) {
                if (!editor.external.hasFile(invocation.fileId)) {
                    return failure("external change is unavailable");
                }
                (void)editor.external.selectFile(invocation.fileId);
                return applyAction(invocation.action);
            });
        catalog.add(std::move(built));
    }
    {
        auto built =
            spec("external.select_next", "Select Next External Change");
        built.binding = bindNoArgumentHandler([&editor](CommandContext&) {
            (void)editor.external.selectNext();
            return success();
        });
        catalog.add(std::move(built));
    }
    {
        auto built =
            spec("external.select_previous", "Select Previous External Change");
        built.binding = bindNoArgumentHandler([&editor](CommandContext&) {
            (void)editor.external.selectPrevious();
            return success();
        });
        catalog.add(std::move(built));
    }
    {
        auto built = spec("external.select", "Select External Change");
        built.binding = bindInProcessHandler<DiffFileId>(
            [&editor](CommandContext&, const DiffFileId& file) {
                if (!editor.external.hasFile(file)) {
                    return failure("external change is unavailable");
                }
                (void)editor.external.selectFile(file);
                return success();
            });
        catalog.add(std::move(built));
    }
    {
        auto built = spec("external.focus", "Focus External Change Bar");
        built.binding = bindNoArgumentHandler([&editor](CommandContext&) {
            (void)editor.screen.captureExternalFocus();
            return success();
        });
        catalog.add(std::move(built));
    }
    {
        auto built =
            spec("external.focus_return", "Leave External Change Bar");
        built.binding = bindNoArgumentHandler([&editor](CommandContext&) {
            (void)editor.screen.releaseExternalFocus();
            return success();
        });
        catalog.add(std::move(built));
    }
}

ExternalModificationFlow::ExternalModificationFlow(Workspace& workspace,
                                                   DiffModel& diff)
    : workspace_{&workspace}, diff_{&diff} {}

ExternalModificationFlow::ExternalModificationFlow(
    ExternalModificationFlow&& other) noexcept {
    std::lock_guard lock(other.saveExpectationMutex_);
    workspace_ = other.workspace_;
    diff_ = other.diff_;
    lastWatcherSequence_ = other.lastWatcherSequence_;
    revision_ = other.revision_;
    pending_ = std::move(other.pending_);
    selected_ = std::move(other.selected_);
    pendingSaveExpectations_ = std::move(other.pendingSaveExpectations_);
}

ExternalModificationFlow& ExternalModificationFlow::operator=(
    ExternalModificationFlow&& other) noexcept {
    if (this == &other) return *this;
    std::scoped_lock lock(saveExpectationMutex_, other.saveExpectationMutex_);
    workspace_ = other.workspace_;
    diff_ = other.diff_;
    lastWatcherSequence_ = other.lastWatcherSequence_;
    revision_ = other.revision_;
    pending_ = std::move(other.pending_);
    selected_ = std::move(other.selected_);
    pendingSaveExpectations_ = std::move(other.pendingSaveExpectations_);
    return *this;
}

bool ExternalModificationFlow::ingest(std::vector<WatchEvent> events,
                                      bool resync) {
    const auto revisionBefore = revision_;
    for (auto& event : events) {
        if (event.kind == WatchEventKind::Overflow) continue;

        std::optional<WatchFileState> observed;
        if (event.identity && event.size && event.modificationTime) {
            observed =
                WatchFileState{*event.identity, *event.size,
                               *event.modificationTime};
        } else {
            observed = WatchFileState::observe(workspace_->root() / event.path);
        }
        if (observed) {
            std::lock_guard lock(saveExpectationMutex_);
            const auto found = std::find_if(
                pendingSaveExpectations_.begin(),
                pendingSaveExpectations_.end(),
                [&](const SaveExpectation& expectation) {
                    return expectation.path == event.path &&
                           expectation.state == *observed;
                });
            if (found != pendingSaveExpectations_.end()) {
                pendingSaveExpectations_.erase(found);
                event.origin = WatchEventOrigin::SsgSave;
            }
        }

        const auto documentId = resolveDocument(
            event.kind == WatchEventKind::Rename && event.previousPath
                ? *event.previousPath
                : event.path);
        if (!documentId) continue;
        const auto* document = workspace_->tryDocument(*documentId);
        if (document == nullptr) continue;

        const auto id = externalDiffFileId(event.path.generic_string());
        const auto baseline = document->snapshot().text;
        auto observation = observeDiskChange(workspace_->root(), event);
        if (matchesExternalBaseline(*workspace_, *documentId, event,
                                    observation)) {
            continue;
        }

        bool seededHere = false;
        if (event.origin != WatchEventOrigin::SsgSave &&
            !diff_->file(id).has_value()) {
            const auto seedRevision = diff_->viewState().revision + 1;
            (void)diff_->seedNonGit({{id, event.path, baseline}}, seedRevision);
            seededHere = true;
        }

        ExternalEventInput input{event, id, baseline, observation.content,
                                 std::nullopt};
        if (event.kind == WatchEventKind::Rename && event.previousPath) {
            input.previousId =
                externalDiffFileId(event.previousPath->generic_string());
        }
        const auto diffRevision = diff_->viewState().revision + 1;
        const auto result =
            processEvent(std::move(input), diffRevision, resync);
        if (!result.accepted()) {
            if (seededHere && diff_->file(id).has_value()) {
                (void)diff_->removeFile(id, diff_->viewState().revision + 1);
            }
            continue;
        }

        if (event.kind == WatchEventKind::Rename && event.previousPath) {
            const auto previousId =
                externalDiffFileId(event.previousPath->generic_string());
            if (previousId != id && diff_->file(previousId).has_value()) {
                (void)diff_->removeFile(previousId,
                                        diff_->viewState().revision + 1);
            }
        }
    }
    return revision_ != revisionBefore;
}

bool ExternalModificationFlow::reconcileAllOpenDocumentsAgainstDisk() {
    std::vector<WatchEvent> synthesized;
    std::uint64_t sequence = 0;
    for (const auto documentId : workspace_->documents()) {
        const auto state = workspace_->state(documentId);
        if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) {
            continue;
        }

        const std::filesystem::path relative{state->key.savedPath()};
        const auto absolute = workspace_->root() / relative;
        WatchEvent event;
        event.path = relative;
        event.origin = WatchEventOrigin::External;
        event.sequence = ++sequence;
        std::optional<FileStat> diskStatus;
        try {
            diskStatus = statFile(absolute);
        } catch (const std::system_error&) {
            event.kind = WatchEventKind::Modify;
            synthesized.push_back(std::move(event));
            continue;
        }

        if (!diskStatus) {
            if (workspace_->matchesExternalBaseline(documentId, std::nullopt)) {
                continue;
            }
            event.kind = WatchEventKind::Remove;
            synthesized.push_back(std::move(event));
            continue;
        }

        const auto disk = readFileText(absolute);
        if (disk && workspace_->matchesExternalBaseline(documentId, *disk)) {
            continue;
        }
        event.kind = WatchEventKind::Modify;
        synthesized.push_back(std::move(event));
    }
    if (synthesized.empty()) return false;
    return ingest(std::move(synthesized), true);
}

void ExternalModificationFlow::registerSaveExpectation(
    const std::filesystem::path& relativePath) {
    const auto observed =
        WatchFileState::observe(workspace_->root() / relativePath);
    if (!observed) return;

    std::lock_guard lock(saveExpectationMutex_);
    pendingSaveExpectations_.push_back({relativePath, *observed});
    constexpr std::size_t kMaxSaveExpectations = 256;
    while (pendingSaveExpectations_.size() > kMaxSaveExpectations) {
        pendingSaveExpectations_.pop_front();
    }
}

ExternalModificationResult
ExternalModificationFlow::processEvent(ExternalEventInput input,
                                       std::uint64_t diffRevision,
                                       bool resync) {
    if (!resync && input.event.sequence <= lastWatcherSequence_) {
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
    const auto documentId = resolveDocument(
        input.event.kind == WatchEventKind::Rename && input.event.previousPath
            ? *input.event.previousPath
            : input.event.path);
    if (!documentId) {
        return failure(ExternalModificationError::DocumentMissing);
    }
    const auto state = workspace_->state(*documentId);
    const auto* workspaceDocument = workspace_->tryDocument(*documentId);
    if (!state || workspaceDocument == nullptr) {
        return failure(ExternalModificationError::DocumentMissing);
    }
    if (requiresContent(input.event.kind) && !input.diskContent.has_value()) {
        return failure(ExternalModificationError::ContentRequired);
    }

    auto stagedPending = pending_;
    const bool saveEvent = input.event.origin == WatchEventOrigin::SsgSave;
    const bool removed = input.event.kind == WatchEventKind::Remove;
    bool publishStatus = false;

    // A rename changes the path, and the id is derived from the path, so any
    // pending entry staged under the previous id would be orphaned beside a
    // fresh one. Retire it first (before locating the entry for the new id).
    if (input.previousId.has_value() && *input.previousId != input.id) {
        const auto previous = std::find_if(
            stagedPending.begin(), stagedPending.end(), [&](const auto& item) {
                return item.view.id == *input.previousId;
            });
        if (previous != stagedPending.end()) {
            stagedPending.erase(previous);
        }
    }

    const auto existing = std::find_if(
        stagedPending.begin(), stagedPending.end(),
        [&](const auto& item) { return item.view.id == input.id; });

    // A clean document adopts the disk change only if the workspace commit the
    // reconcile supplies succeeds (Decision 11): stage->commit->publish, so a
    // failed commit falls through to raising the conflict rather than clearing
    // to a stale buffer.
    const bool canCleanAdopt = !saveEvent && !state->dirty && !removed;

    if (saveEvent) {
        if (existing != stagedPending.end()) {
            stagedPending.erase(existing);
        }
        pending_.swap(stagedPending);
        reconcileSelection(std::nullopt);
        if (!resync) {
            lastWatcherSequence_ = input.event.sequence;
        }
        advanceRevision();
        return {};
    } else if (canCleanAdopt &&
               (input.event.kind == WatchEventKind::Rename
                    ? workspace_
                          ->adoptExternalRename(
                              *documentId, input.event.path.generic_string(),
                              input.diskContent.value_or(std::string{}), true)
                          .accepted()
                    : workspace_
                          ->reloadWithContent(
                              *documentId,
                              input.diskContent.value_or(std::string{}))
                          .accepted())) {
        if (existing != stagedPending.end()) {
            stagedPending.erase(existing);
        }
    } else {
        // A dirty rename keeps its buffer but still publishes the conflict
        // under the NEW-path id, so the workspace must adopt the new path
        // (baseline + identity, buffer preserved) BEFORE that id is published
        // (stage->commit->publish, Decision 11). A failed adoption must not
        // leave a conflict keyed to a path no document owns, so it fails the
        // whole event rather than publishing an unresolvable entry.
        if (input.event.kind == WatchEventKind::Rename &&
            !workspace_
                 ->adoptExternalRename(
                     *documentId, input.event.path.generic_string(),
                     input.diskContent.value_or(std::string{}), false)
                 .accepted()) {
            return failure(ExternalModificationError::RecoveryFailed);
        }
        ExternalDocumentView view{
            input.id,
            input.event.path,
            removed ? ExternalDocumentStatus::ExternallyRemoved
                    : ExternalDocumentStatus::ExternallyModified,
            removed ? "File was removed outside SSG"
                    : "File was modified outside SSG",
            removed ? "D" : "M",
            removed
                ? std::vector<
                      ExternalActionAffordance>{externalActionAffordance(
                                                    ExternalAction::KeepBuffer),
                                                externalActionAffordance(
                                                    ExternalAction::OpenDiff)}
                : std::vector<ExternalActionAffordance>{
                      externalActionAffordance(ExternalAction::Reload),
                      externalActionAffordance(ExternalAction::KeepBuffer),
                      externalActionAffordance(ExternalAction::OpenDiff)}};
        PendingChange change{std::move(view), input.diskContent};
        if (existing == stagedPending.end()) {
            stagedPending.push_back(std::move(change));
        } else {
            *existing = std::move(change);
        }
        publishStatus = true;
    }

    NonGitDiffEvent diffEvent{diffKind(input.event.kind),
                              input.id,
                              input.event.path,
                              input.event.previousPath,
                              std::move(input.baselineContent),
                              input.diskContent};
    const auto diffResult =
        diff_->applyNonGitEvent(std::move(diffEvent), diffRevision);

    pending_.swap(stagedPending);
    reconcileSelection(std::nullopt);
    if (!resync) {
        // A resync (overflow recovery) is not part of the ordered watcher
        // stream: it must not advance the sequence high-water mark, or the
        // real events that follow the overflow would be rejected as stale.
        lastWatcherSequence_ = input.event.sequence;
    }
    advanceRevision();
    return {ExternalModificationError::None, publishStatus,
            diffResult.accepted()};
}

ExternalModificationResult
ExternalModificationFlow::reload(const DiffFileId& id,
                                 std::optional<JournalDocument>& document) {
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
        document->mode, false, *pending->diskContent};
    document = std::move(replacement);
    const auto idx = static_cast<std::size_t>(pending - pending_.begin());
    pending_.erase(pending);
    reconcileSelection(idx);
    advanceRevision();
    return {ExternalModificationError::None, false, true};
}

ExternalModificationResult
ExternalModificationFlow::resolveReload(const DiffFileId& id) {
    const auto pending = findPending(id);
    if (pending == pending_.end()) {
        return failure(ExternalModificationError::NoExternalChange);
    }
    if (!pending->diskContent.has_value()) {
        return failure(ExternalModificationError::ContentRequired);
    }
    const auto document = resolveDocument(pending->view.path);
    if (!document) {
        return failure(ExternalModificationError::DocumentMissing);
    }
    const auto committed =
        workspace_->reloadWithContent(*document, *pending->diskContent);
    if (!committed.accepted()) {
        return failure(ExternalModificationError::RecoveryFailed);
    }
    const auto idx = static_cast<std::size_t>(pending - pending_.begin());
    pending_.erase(pending);
    reconcileSelection(idx);
    advanceRevision();
    return {ExternalModificationError::None, false, true};
}

ExternalModificationResult
ExternalModificationFlow::keepBuffer(const DiffFileId& id) {
    const auto pending = findPending(id);
    if (pending == pending_.end()) {
        return failure(ExternalModificationError::NoExternalChange);
    }
    const auto document = resolveDocument(pending->view.path);
    if (!document) {
        return failure(ExternalModificationError::DocumentMissing);
    }
    const bool removed =
        pending->view.status == ExternalDocumentStatus::ExternallyRemoved;
    if (!workspace_->commitExternalDismissal(*document, removed,
                                             pending->diskContent)) {
        return failure(ExternalModificationError::RecoveryFailed);
    }
    const auto idx = static_cast<std::size_t>(pending - pending_.begin());
    pending_.erase(pending);
    reconcileSelection(idx);
    advanceRevision();
    return {};
}

ExternalOpenDiffResult
ExternalModificationFlow::openDiff(const DiffFileId& id) const {
    if (findPending(id) == pending_.end()) {
        return {ExternalModificationError::NoExternalChange, std::nullopt};
    }
    const auto file = diff_->file(id);
    if (!file.has_value()) {
        return {ExternalModificationError::DiffRejected, std::nullopt};
    }
    return {ExternalModificationError::None, diffOpenFile(file->get())};
}

ExternalModificationViewState ExternalModificationFlow::viewState() const {
    ExternalModificationViewState state{revision_};
    state.files.reserve(pending_.size());
    for (const auto& pending : pending_) {
        state.files.push_back(pending.view);
    }
    state.message = std::to_string(state.files.size()) +
                    (state.files.size() == 1 ? " file changed on disk"
                                             : " files changed on disk");
    state.selected = selected_;
    return state;
}

bool ExternalModificationFlow::selectFile(const DiffFileId& id) {
    if (findPending(id) == pending_.end())
        return false;
    if (selected_ && *selected_ == id)
        return false;
    selected_ = id;
    advanceRevision();
    return true;
}

bool ExternalModificationFlow::hasFile(const DiffFileId& id) {
    return findPending(id) != pending_.end();
}

bool ExternalModificationFlow::selectNext() { return moveSelection(+1); }
bool ExternalModificationFlow::selectPrevious() { return moveSelection(-1); }

ExternalModificationResult
ExternalModificationFlow::failure(ExternalModificationError error) {
    return {error, false, false};
}

std::optional<FileDocumentId> ExternalModificationFlow::resolveDocument(
    const std::filesystem::path& path) const {
    const auto savedPath = path.generic_string();
    for (const auto document : workspace_->documents()) {
        const auto state = workspace_->state(document);
        if (state && state->key.kind() == JournalDocumentKeyKind::Saved &&
            state->key.savedPath() == savedPath) {
            return document;
        }
    }
    return std::nullopt;
}

std::vector<ExternalModificationFlow::PendingChange>::iterator
ExternalModificationFlow::findPending(const DiffFileId& id) {
    return std::find_if(
        pending_.begin(), pending_.end(),
        [&](const auto& pending) { return pending.view.id == id; });
}

std::vector<ExternalModificationFlow::PendingChange>::const_iterator
ExternalModificationFlow::findPending(const DiffFileId& id) const {
    return std::find_if(
        pending_.begin(), pending_.end(),
        [&](const auto& pending) { return pending.view.id == id; });
}

void ExternalModificationFlow::advanceRevision() {
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("external modification revision exhausted");
    }
    revision_ = std::uint64_t{revision_ + 1};
}

// Keep the selection valid against the current file list: cleared when empty,
// set to the first file when the section became non-empty, kept when still
// valid, and re-homed to the file that now occupies the resolved-away slot
// (`preferred`) otherwise.
void ExternalModificationFlow::reconcileSelection(
    std::optional<std::size_t> preferred) {
    if (pending_.empty()) {
        selected_.reset();
        return;
    }
    if (selected_ && findPending(*selected_) != pending_.end())
        return;
    std::size_t index = 0;
    if (preferred)
        index = std::min(*preferred, pending_.size() - 1);
    selected_ = pending_[index].view.id;
}

bool ExternalModificationFlow::moveSelection(int direction) {
    if (pending_.empty())
        return false;
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
    if (selected_ && *selected_ == nextId)
        return false;
    selected_ = nextId;
    advanceRevision();
    return true;
}

} // namespace ssg
