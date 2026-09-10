#pragma once

#include <ssg/DiffModel.h>
#include <ssg/FilesystemWatcher.h>
#include <ssg/Workspace.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ssg {

class Commands;
struct Editor;

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
void bindExternalModificationCommands(Commands& commands, Editor& editor);

struct ExternalDocumentView {
    DiffFileId id;
    std::filesystem::path path;
    ExternalDocumentStatus status = ExternalDocumentStatus::ExternallyModified;
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
    // The library-owned selection, mirroring TreeProviderView::selected. An id
    // is stable across list mutation where an index is not. Invariant the flow
    // maintains and the wire codec enforces: present only when it names a file
    // in `files`, else nullopt -- never a dangling selection.
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
    // derived from the path), so the reconcile threads the previous id here for
    // the flow to retire the pending entry staged under it, rather than orphan
    // it.
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

class ExternalModificationFlow {
  public:
    ExternalModificationFlow(Workspace& workspace, DiffModel& diff);
    ~ExternalModificationFlow() = default;
    ExternalModificationFlow(ExternalModificationFlow&& other) noexcept;
    ExternalModificationFlow&
    operator=(ExternalModificationFlow&& other) noexcept;
    ExternalModificationFlow(const ExternalModificationFlow&) = delete;
    ExternalModificationFlow&
    operator=(const ExternalModificationFlow&) = delete;

    bool ingest(std::vector<WatchEvent> events, bool resync = false);
    bool reconcileAllOpenDocumentsAgainstDisk();
    void registerSaveExpectation(const std::filesystem::path& relativePath);

    // CONTRACT
    // Normal events enforce and advance the watcher sequence. Resync events are
    // synthetic and outside that ordered stream, so they do neither.
    [[nodiscard]] ExternalModificationResult
    processEvent(ExternalEventInput input, std::uint64_t diffRevision,
                 bool resync = false);
    [[nodiscard]] ExternalModificationResult
    reload(const DiffFileId& id, std::optional<ClosedDocumentSnapshot>& document);
    [[nodiscard]] ExternalModificationResult
    resolveReload(const DiffFileId& id);
    [[nodiscard]] ExternalModificationResult keepBuffer(const DiffFileId& id);
    [[nodiscard]] ExternalOpenDiffResult openDiff(const DiffFileId& id) const;
    // Selection commands mirroring tree.select/select_next/select_previous: the
    // library owns which file is selected and the payload-less action commands
    // act on it. Each returns whether the selection moved (a move advances the
    // revision so a selection-only delta is published). selectFile is a no-op
    // returning false when the id names no present file; the wraparound movers
    // are no-ops on an empty section.
    [[nodiscard]] bool selectFile(const DiffFileId& id);
    // Whether the id names a file currently present in the section.
    // selectFile's false conflates an absent id with an already-selected id, so
    // a caller that must reject only absent ids (external.select gating a
    // follow-up action) tests presence here instead of using selectFile's bool.
    [[nodiscard]] bool hasFile(const DiffFileId& id);
    [[nodiscard]] bool selectNext();
    [[nodiscard]] bool selectPrevious();
    [[nodiscard]] ExternalModificationViewState viewState() const;

  private:
    struct PendingChange {
        ExternalDocumentView view;
        std::optional<std::string> diskContent;
    };

    [[nodiscard]] std::optional<FileDocumentId>
    resolveDocument(const std::filesystem::path& path) const;
    [[nodiscard]] static ExternalModificationResult
    failure(ExternalModificationError error);
    [[nodiscard]] std::vector<PendingChange>::iterator
    findPending(const DiffFileId& id);
    [[nodiscard]] std::vector<PendingChange>::const_iterator
    findPending(const DiffFileId& id) const;
    void advanceRevision();
    void reconcileSelection(std::optional<std::size_t> preferred);
    [[nodiscard]] bool moveSelection(int direction);

    Workspace* workspace_;
    DiffModel* diff_;
    std::uint64_t lastWatcherSequence_ = 0;
    std::uint64_t revision_{0};
    std::vector<PendingChange> pending_;
    std::optional<DiffFileId> selected_;
    std::mutex saveExpectationMutex_;
    std::deque<SaveExpectation> pendingSaveExpectations_;
};

} // namespace ssg
