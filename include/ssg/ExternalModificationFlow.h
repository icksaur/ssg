#pragma once

#include <ssg/DiffModel.h>
#include <ssg/RecoveryManager.h>
#include <ssg/FilesystemWatcher.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ssg {

enum class ExternalAction : std::uint8_t {
    Reload = 0,
    KeepBuffer = 1,
    OpenDiff = 2,
};

struct ExternalActionInvocation {
    DiffFileId fileId;
    ExternalAction action;

    friend bool operator==(const ExternalActionInvocation&,
                           const ExternalActionInvocation&) = default;
};

enum class ExternalDocumentStatus : std::uint8_t {
    ExternallyModified = 0,
    ExternallyRemoved = 1,
};

struct ExternalActionAffordance {
    ExternalAction action = ExternalAction::Reload;
    std::string label;
    std::string command;

    friend bool operator==(const ExternalActionAffordance&,
                           const ExternalActionAffordance&) = default;
};

[[nodiscard]] ExternalActionAffordance
externalActionAffordance(ExternalAction action);

struct ExternalDocumentView {
    DiffFileId id;
    std::filesystem::path path;
    ExternalDocumentStatus status =
        ExternalDocumentStatus::ExternallyModified;
    std::string accessibleStatus;
    std::string statusLabel;
    std::vector<ExternalActionAffordance> actions;

    friend bool operator==(const ExternalDocumentView&,
                           const ExternalDocumentView&) = default;
};

struct ExternalModificationViewState {
    std::uint64_t revision{0};
    std::string message;
    std::vector<ExternalDocumentView> files;
    // The library-owned selection, mirroring TreeProviderView::selected. An id is
    // stable across list mutation where an index is not. Invariant the flow
    // maintains and the wire codec enforces: present only when it names a file in
    // `files`, else nullopt -- never a dangling selection.
    std::optional<DiffFileId> selected;

    friend bool operator==(const ExternalModificationViewState&,
                           const ExternalModificationViewState&) = default;
};

struct ExternalEventInput {
    WatchEvent event;
    DiffFileId id;
    std::string baselineContent;
    std::optional<std::string> diskContent;
    // The old path's namespaced id on a rename. A rename changes the id (it is
    // derived from the path), so the reconcile threads the previous id here for the
    // flow to retire the pending entry staged under it, rather than orphan it.
    std::optional<DiffFileId> previousId;
};

enum class ExternalModificationError : std::uint8_t {
    None,
    StaleEvent,
    UnsupportedEvent,
    InvalidEvent,
    DocumentMissing,
    ContentRequired,
    DiffRejected,
    NoExternalChange,
    RecoveryFailed,
};

struct ExternalModificationResult {
    ExternalModificationError error = ExternalModificationError::None;
    bool statusPublished = false;
    bool diffRouted = true;
    std::optional<RecoveryRecordId> compensation;
    [[nodiscard]] bool accepted() const noexcept {
        return error == ExternalModificationError::None;
    }
};

struct ExternalOpenDiffResult {
    ExternalModificationError error = ExternalModificationError::None;
    std::optional<DiffOpenTarget> target;
    [[nodiscard]] bool accepted() const noexcept {
        return error == ExternalModificationError::None && target.has_value();
    }
};

// The outcome a workspace-commit callback reports back to resolveReload: whether
// the captured content reached the workspace, and the reversal record if it did.
struct ExternalReloadCommitResult {
    bool committed = false;
    std::optional<RecoveryRecordId> compensation;
};

// Commits the flow's captured pending disk content into the workspace, returning
// whether it landed. The flow supplies the EXACT bytes it captured when it raised
// the conflict (never a fresh disk read), so the buffer the user sees is replaced
// with the content the actions were offered about.
using ExternalReloadCommit =
    std::function<ExternalReloadCommitResult(const std::string& capturedContent)>;

// Commits a keep_buffer dismissal by advancing the document's IN-MEMORY external
// baseline and enqueuing a refresh of any already-persisted draft record to the
// same baseline, returning whether the in-memory advance held and the refresh was
// issued. `removed` is true when the dismissed state is a removal (the baseline
// advances to Missing); otherwise `content` carries the exact dismissed disk bytes
// the baseline advances to. The flow clears the pending action ONLY when this
// reports success, so a failed in-memory commit leaves the conflict raised. Durable
// persistence of the refreshed draft is best-effort through the scratch durability
// worker, identical to autosave -- not a synchronous guarantee.
using ExternalKeepBufferCommit =
    std::function<bool(bool removed, const std::optional<std::string>& content)>;

class ExternalModificationFlow {
public:
    ExternalModificationFlow(RecoveryManager& recovery, DiffModel& diff);
    ~ExternalModificationFlow();
    ExternalModificationFlow(ExternalModificationFlow&&) noexcept;
    ExternalModificationFlow& operator=(ExternalModificationFlow&&) noexcept;
    ExternalModificationFlow(const ExternalModificationFlow&) = delete;
    ExternalModificationFlow& operator=(const ExternalModificationFlow&) =
        delete;

    // CONTRACT
    // ExternalModificationFlow::processEvent enforces the watcher sequence: an
    //   event at or below the high-water mark is rejected as stale, and a processed
    //   event advances the mark. processResyncEvent is the ONLY sequence-free path;
    //   the runtime invokes it solely to reconcile an overflow resync (a synthetic,
    //   out-of-stream event), so it neither consults nor advances the mark. These
    //   are the two entry points; there is no way to run a NORMAL watcher event
    //   without sequence enforcement.
    [[nodiscard]] ExternalModificationResult processEvent(
        ExternalEventInput input, std::uint64_t diffRevision,
        std::optional<JournalDocument>& document,
        std::function<bool()> commitClean = {},
        std::function<bool()> commitConflictRename = {});
    // An overflow RESYNC: the watcher lost events, so the runtime re-derives which
    // open documents changed by comparing them to disk and drives this per
    // document. Such a synthetic event is outside the ordered watcher stream, so it
    // neither consults nor advances the sequence high-water mark -- otherwise the
    // real events that resume after the overflow would be rejected as stale.
    [[nodiscard]] ExternalModificationResult processResyncEvent(
        ExternalEventInput input, std::uint64_t diffRevision,
        std::optional<JournalDocument>& document,
        std::function<bool()> commitClean = {},
        std::function<bool()> commitConflictRename = {});
    [[nodiscard]] ExternalModificationResult reload(
        const DiffFileId& id, std::optional<JournalDocument>& document);
    // The library-owned atomic reload resolution both clients share: stages the
    // captured pending content, drives `commit` to write exactly those bytes into
    // the workspace, and clears the raised action ONLY when the commit reports it
    // landed. A failed commit leaves the action raised, so the section never clears
    // over a buffer the reload did not actually replace.
    [[nodiscard]] ExternalModificationResult resolveReload(
        const DiffFileId& id, const ExternalReloadCommit& commit);
    // CONTRACT
    // ExternalModificationFlow::keepBuffer dismisses an external change: it drives
    //   `commit` with the EXACT captured dismissed state (the removal flag and the
    //   bytes the conflict was raised about, never a fresh disk read) and clears the
    //   pending action ONLY when the commit reports success. The commit advances the
    //   document's authoritative IN-MEMORY external baseline (the workspace entry)
    //   synchronously and enqueues a refresh of any already-persisted draft record to
    //   the same baseline; it reports success once the in-memory advance holds and
    //   that refresh is issued, so an ordinary duplicate event, an overflow resync,
    //   or a live draft reopen no longer re-raises the dismissed state while the
    //   buffer is preserved. Durable persistence of the refreshed draft is
    //   best-effort through the scratch durability worker -- the same async window
    //   autosave already has, NOT a synchronous cross-store durability guarantee. A
    //   failed in-memory commit leaves the conflict raised and the workspace baseline
    //   at its prior state.
    [[nodiscard]] ExternalModificationResult keepBuffer(
        const DiffFileId& id, const ExternalKeepBufferCommit& commit);
    [[nodiscard]] ExternalOpenDiffResult openDiff(const DiffFileId& id) const;
    // Selection commands mirroring tree.select/select_next/select_previous: the
    // library owns which file is selected and the payload-less action commands act
    // on it. Each returns whether the selection moved (a move advances the revision
    // so a selection-only delta is published). selectFile is a no-op returning false
    // when the id names no present file; the wraparound movers are no-ops on an
    // empty section.
    [[nodiscard]] bool selectFile(const DiffFileId& id);
    // Whether the id names a file currently present in the section. selectFile's
    // false conflates an absent id with an already-selected id, so a caller that
    // must reject only absent ids (external.select gating a follow-up action)
    // tests presence here instead of using selectFile's bool.
    [[nodiscard]] bool hasFile(const DiffFileId& id);
    [[nodiscard]] bool selectNext();
    [[nodiscard]] bool selectPrevious();
    [[nodiscard]] ExternalModificationViewState viewState() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
