#pragma once

#include <ssg/ClipboardRegister.h>
#include <ssg/CommandTransition.h>
#include <ssg/InteractionAuthority.h>
#include <ssg/DiffModel.h>
#include <ssg/DraftAutosaveScheduler.h>
#include <ssg/EditCommands.h>
#include <ssg/EditorSession.h>
#include <ssg/ExternalModificationFlow.h>
#include <ssg/FileCommands.h>
#include <ssg/FindReplace.h>
#include <ssg/FollowEditsModel.h>
#include <ssg/DocumentHistory.h>
#include <ssg/CommandCatalog.h>
#include <ssg/CompiledKeymap.h>
#include <ssg/Keymap.h>
#include <ssg/LspFeatureController.h>
#include <ssg/LspWorkspaceEditController.h>
#include <ssg/LineLayoutCache.h>
#include <ssg/LuaCommandHost.h>
#include <ssg/Picker.h>
#include <ssg/PromptRouting.h>
#include <ssg/PromptSurface.h>
#include <ssg/Search.h>
#include <ssg/Settings.h>
#include <ssg/StatusFields.h>
#include <ssg/StatusQueue.h>
#include <ssg/SyntaxModel.h>
#include <ssg/TabManager.h>
#include <ssg/TreeModel.h>
#include <ssg/WorkspaceFileIndex.h>
#include <ssg/ShellState.h>
#include <ssg/Workspace.h>

#include <any>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "command_executor.h"

namespace ssg {

// Casts a command payload to the expected type, or null when it holds something
// else. The one definition shared by every runtime handler file, which each
// used to re-declare in its own anonymous namespace.
template <typename T>
[[nodiscard]] T const* payloadAs(std::any const& payload) {
    return std::any_cast<T>(&payload);
}

// Resolves a boolean setting, falling back when the stored value is not a bool.
// Shared by the presentation commands and the runtime's own reads.
inline bool boolSetting(SettingsModel const& settings, SettingKey key,
                        bool fallback) {
    auto value = settings.resolve(key).value;
    if (auto const* typed = std::get_if<bool>(&value)) return *typed;
    return fallback;
}

inline std::uint32_t uint32Setting(SettingsModel const& settings, SettingKey key,
                                   std::uint32_t fallback) {
    auto value = settings.resolve(key).value;
    if (auto const* typed = std::get_if<std::uint32_t>(&value)) return *typed;
    return fallback;
}

struct GitDiffRefreshWorkerState;

// The reopen outcome of a document's recovered draft (single-file draft
// recovery, M15). Mirrors EditorSession::DraftReopenNotice; lives per-document
// so the notice (p5) and discard (p6) phases can read it by document id.
enum class DraftReopenOutcome { None, Restored, Conflict };

struct DocumentRuntimeState {
    explicit DocumentRuntimeState(
        HistoryConfig historyConfig = HistoryConfig::defaults(),
        std::shared_ptr<SyntaxParser> parser = nullptr)
        : history{historyConfig}, syntax{std::move(parser)} {
        ++liveCount;
    }

    DocumentRuntimeState(DocumentRuntimeState&& other) noexcept
        : history{std::move(other.history)},
          syntax{std::move(other.syntax)},
          reopen{other.reopen},
          autosaveOversizeReported{other.autosaveOversizeReported} {
        ++liveCount;
    }

    DocumentRuntimeState(const DocumentRuntimeState&) = delete;
    DocumentRuntimeState& operator=(const DocumentRuntimeState&) = delete;
    DocumentRuntimeState& operator=(DocumentRuntimeState&&) = default;

    ~DocumentRuntimeState() { --liveCount; }

    static std::uint64_t liveInstances() noexcept {
        return liveCount.load(std::memory_order_relaxed);
    }

    DocumentHistory history;
    SyntaxModel syntax;
    DraftReopenOutcome reopen = DraftReopenOutcome::None;
    // Whether the "too large to autosave a draft" warning has already been
    // surfaced for this document, so an oversized buffer is reported once rather
    // than on every flush tick. Cleared if the buffer drops back under the cap.
    bool autosaveOversizeReported = false;

private:
    inline static std::atomic<std::uint64_t> liveCount{0};
};

void bindRuntimeEditing(CommandCatalog& catalog, EditorSession::Impl& runtime);
void bindRuntimeFiles(CommandCatalog& catalog, EditorSession::Impl& runtime);
void bindRuntimePresentation(CommandCatalog& catalog, EditorSession::Impl& runtime);
void bindRuntimeNavigation(CommandCatalog& catalog, EditorSession::Impl& runtime);
void bindRuntimeLanguageServices(CommandCatalog& catalog, EditorSession::Impl& runtime);
void bindRuntimeHelp(CommandCatalog& catalog, EditorSession::Impl& runtime);
[[nodiscard]] CommandHandlerResult executeFindReplaceCommand(
    EditorSession::Impl& runtime, ViewId viewId, Revision revision,
    FindReplaceCommand command, std::any const& payload);

struct EditorSession::Impl final : CommandServices,
                                   TabLifecycle,
                                   SearchWorkspaceSource,
                                   SearchCommandSource,
                                   FindReplaceWorkspace,
                                   WorkspaceRecoverySink,
                                   LspWorkspaceEditDocuments,
                                   LspWorkspaceFileOperations {
    Impl(std::filesystem::path canonicalCwd,
         std::filesystem::path scratchRoot,
         std::filesystem::path recoveryRoot,
         std::filesystem::path archiveRoot,
         bool deferEnrichment = false,
         std::shared_ptr<SyntaxParser> parser = nullptr,
         std::vector<StatusFieldProviderBinding> statusFieldProviderOverrides = {},
         bool enableGitDiffWorker = true,
         bool enableFilesystemWatcher = true);
    ~Impl();

    std::filesystem::path root;
    std::filesystem::path scratchRoot;
    std::filesystem::path recoveryRoot;
    std::filesystem::path archiveRoot;
    RecoveryManager recovery;
    ScratchStore scratch;
    Workspace workspace;
    SelectionViewState selection;
    std::map<std::uint64_t, DocumentRuntimeState> documentRuntimeStates;
    ClipboardRegister clipboard;
    SettingsModel settings;
    // Autosave debounce state for open dirty documents (single-file draft
    // recovery, M15). The policy lives here (library-owned); the app supplies
    // only a periodic tick and a clean-exit call.
    DraftAutosaveScheduler autosave;
    // Per-draft byte cap: a buffer larger than this is not autosaved (writing a
    // multi-hundred-MiB draft every debounce would blow the scratch quota and
    // stall the fsync thread). A field, not a constant, so a test can lower it
    // without materialising a huge buffer; production keeps the default. Over-cap
    // is reported (a one-time status), never a silent partial draft.
    std::uintmax_t autosaveDraftByteCap = 64U * 1024U * 1024U;
    FindReplaceController findReplace;
    // The document the find/replace controller last evaluated against.  Find
    // matches are byte offsets into one specific document; when the active
    // document identity or revision drifts from this, the controller is stale and
    // must be dismissed (see reconcile_find_document).
    std::optional<FileDocumentId> findDocumentId;
    StatusQueue status;
    ShellState shell;
    TabManager tabs;
    DiffModel diff;
    ExternalModificationFlow external;
    FollowEditsModel follow;
    Revision lastGitScanRevision{0};
    std::optional<std::string> currentGitBranch;
    TreeModel tree;
    std::shared_ptr<SyntaxParser> syntaxParser;
    std::vector<StatusFieldCatalogEntry> statusFieldCatalog;
    // The single interaction authority: owner of the whole-screen schema generation, the
    // prompt surface, panel/focus/provider truth, the interaction projection, and the tree
    // revision source. ShellState and the snapshot read its projection; every focus,
    // presence, and prompt change flows through it. Declared after `tree` and
    // `statusFieldCatalog` so both are constructed before it.
    InteractionAuthority interaction;
    std::unordered_map<std::string, StatusFieldProvider> statusFieldProviders;
    std::unordered_map<std::string, FileDocumentId> liveDiffDocuments;
    // Read-only, in-memory "output" tabs (help, and any future generated-content
    // tab), keyed by the tab's content identity. Mirrors liveDiffDocuments: a
    // content tab does not store its document id in TabState, so the document is
    // resolved through this side map. The backing documents are DocumentMode::
    // ReadOnly and untitled, so they are excluded from autosave and cannot be
    // saved; their content is refreshed by remove+recreate, never edited in
    // place (see openReadOnlyTab).
    std::unordered_map<std::string, FileDocumentId> readOnlyTabDocuments;
    // The user's home directory, resolved once at construction (HOME, then
    // USERPROFILE, with trailing separators stripped) so the header path field's
    // "~" abbreviation is deterministic across a session rather than re-reading
    // the process environment on every snapshot. Empty disables abbreviation.
    std::string homeDirectory;
    // Per-document syntax language override for documents with no on-disk path
    // to infer a language from (a read-only help/output tab). refreshSyntax
    // consults this before falling back to path-derived detection, so a help
    // tab can be highlighted as e.g. Markdown despite being untitled.
    std::unordered_map<std::uint64_t, LanguageId> documentLanguageOverrides;
    SearchController search;
    NavigationHistory navigation{64};
    LspSyncViewState lspSync;
    LspFeatureViewState lspFeatures;
    KeymapViewState keymap{"default", {}};
    // Advances on every keymap mutation (keymap.bind/unbind, reset to default).
    // The catalog revision does NOT move on a rebind -- binding an existing
    // command registers nothing -- so routing-change detection needs this
    // separate counter.
    std::uint64_t keymapGeneration = 0;
    std::unique_ptr<CompiledKeymap> inputKeymap;
    std::optional<std::uint64_t> inputKeymapGeneration;
    std::optional<CatalogRevision> inputCatalogRevision;
    ThemeSnapshot theme{};
    // Chrome glyphs and dimensions, beside the theme because they are the same
    // kind of thing: presentation this runtime owns and hands to layout.
    Style style{};
    // The init.lua-composed header/footer,
    // pushed by the host after each init.lua evaluation via
    // EditorSession::setComposedUi. nullopt keeps the built-in chrome; a
    // present region REPLACES that region's built-in status fields in shellView.
    std::optional<ValidatedComposition> composedUi;
    // Bumped whenever composedUi changes, so the published medium-agnostic UI
    // schema is stamped with a generation that advances only
    // on a real chrome/structure change.
    std::uint64_t chromeGeneration = 0;
    std::optional<WorkspaceReplacePreview> workspaceReplacePreview;
    mutable std::mutex operationMutex;
    std::shared_ptr<CommandCatalog> catalog =
        std::make_shared<CommandCatalog>();
    std::unique_ptr<CommandExecutor> session;
    // Commands a running handler asked to dispatch, run in order once the
    // session lock releases.  The session mutex is not reentrant, so a handler
    // cannot dispatch; this is how it asks for one.
    //
    // ONE queue, drained inside the dispatch wrapper, rather than a field per
    // caller: palette.execute, prompt.submit and a script's ssg.command all
    // want the same thing.  Separate single-slot fields each needed their own
    // early return, and a return that forgot to drain silently postponed the
    // work to some later, unrelated dispatch.
    //
    // Deferring rather than nesting also keeps revisions sequential: each
    // command is rebased on the revision left by the one before it, whereas a
    // truly nested dispatch would advance the revision underneath a caller that
    // had already read it.
    struct DeferredCommand {
        // Absent means "whichever client's dispatch this is", which is what a
        // follow-up to the user's own action wants: palette.execute's target
        // and prompt.submit's command are the user acting, and must carry the
        // user's principal.
        //
        // Present names a different one.  A script's request runs as the SCRIPT
        // client, so it is gated by the script's capabilities rather than
        // inheriting those of whoever pressed the key.
        std::optional<ClientId> client;
        ClientCommand command;
    };

    // CONTRACT
    // A view's client-derived geometry and scroll projection never become
    // aggregate-wide behavior. Distinct ViewIds retain independent presentation
    // state; clients intentionally sharing a ViewId share this record.
    struct ViewPresentationState {
        ViewportDimensions dimensions{80, 24};
        std::uint32_t paneContentRows = 24;
        std::uint32_t paneContentColumns = 80;
        std::uint32_t reservedPromptRows = 0;
        std::uint32_t panelContentRows = 0;
        std::uint32_t requestedFirstVisualRow = 0;
        std::uint32_t requestedFirstVisualColumn = 0;
        std::optional<CellIndex> desiredCell;
        std::uint32_t treeFirstVisible = 0;
        LineLayoutCache viewportLineCache;
    };

    // The queue, shaped so `Impl::defer` is the only way to ADD to it -- by
    // construction, not by convention.
    //
    // It was previously a bare vector that two callers pushed to directly while
    // a third went through a checked method, so the shortest way to queue a
    // command was the only unchecked one.  Hiding the vector alone would just
    // move that hole to the wrapper, so writing is private and `Impl` is the
    // only friend: a future caller cannot reach the write path at all, whereas
    // reading (which the drain needs) is harmless and stays public.
    class DeferredCommandQueue {
    public:
        // A handler that queued without limit would spin the drain loop
        // forever; refusing says so, where the alternative is an editor that
        // stops responding for no visible reason.
        static constexpr std::size_t kMaximum = 64;

        [[nodiscard]] bool empty() const noexcept { return commands_.empty(); }
        [[nodiscard]] bool contains(std::string_view commandId) const noexcept {
            for (const auto& deferred : commands_) {
                if (deferred.command.id == commandId) return true;
            }
            return false;
        }
        [[nodiscard]] DeferredCommand takeFront() {
            auto front = std::move(commands_.front());
            commands_.erase(commands_.begin());
            return front;
        }
        void clear() noexcept { commands_.clear(); }

    private:
        // Only reachable through Impl::defer, which is what enforces that a
        // dispatch is actually in progress.  Queueing outside one would strand
        // the command until some later, unrelated dispatch drained it.
        friend struct EditorSession::Impl;
        [[nodiscard]] bool enqueue(DeferredCommand deferred) {
            if (commands_.size() >= kMaximum) return false;
            commands_.push_back(std::move(deferred));
            return true;
        }

        std::vector<DeferredCommand> commands_;
    };

    DeferredCommandQueue deferredCommands;

    // Asks for `command` to run once the dispatch in progress finishes.
    //
    // THE one way to queue: it checks that a dispatch is actually in progress
    // (queueing outside one would strand the command until some later,
    // unrelated dispatch drained it) and enforces the bound.  Returns false if
    // either fails.
    [[nodiscard]] bool defer(std::optional<ClientId> as, ClientCommand command);
    // The open file picker's candidate set, built when the picker opens and
    // Published continuously and rebuilt with the workspace tree.
    std::vector<PaletteCandidate> fileCandidates;
    // Command-mode palette candidates are the whole catalog with each command's
    // key hint resolved -- O(bindings x commands) -- so they are cached and
    // rebuilt only when the catalog or keymap changes, keeping the palette off
    // the per-frame O(BxC) path (like fileCandidates caches the file walk).
    mutable std::vector<PaletteCandidate> commandCandidateCache;
    mutable CatalogRevision commandCandidateCatalogRevision = 0;
    mutable KeymapViewState commandCandidateKeymap;
    mutable bool commandCandidateCacheValid = false;
    std::map<ViewId, ViewPresentationState> viewPresentations;
    std::map<ViewId, std::size_t> viewReferences;
    std::map<ClientId, ViewId> clientViews;
    [[nodiscard]] ViewPresentationState& presentation(ViewId viewId);
    [[nodiscard]] ViewPresentationState const& presentation(ViewId viewId) const;
    bool wordWrap = false;
    bool lineNumbers = false;
    // Cache of the active document's logical line count keyed by its revision,
    // so the line-number gutter width is not recomputed by scanning the whole
    // document every frame.
    mutable std::optional<Revision> lineCountRevision;
    mutable std::optional<FileDocumentId> lineCountDocument;
    mutable std::uint32_t lineCountCache = 1;
    // Cache of the active document's per-logical-line cell runs (wrap-mode
    // shaping), keyed by (revision, documentId) exactly like the line-count
    // cache above. Word-wrap shaping is O(document); without this it re-shapes
    // every line each frame even when navigation left the document unchanged.
    mutable std::optional<Revision> cellRunsRevision;
    mutable std::optional<FileDocumentId> cellRunsDocument;
    mutable std::vector<CellRun> cellRunsCache;
    // The active document's immutable flattened text, shared by navigation and
    // semantic snapshots until its document revision changes.
    mutable std::optional<Revision> activeTextRevision;
    mutable std::optional<FileDocumentId> activeTextDocument;
    mutable std::string activeTextCache;
    std::uint64_t nextStatusId = 1;

    [[nodiscard]] CommandHandlerResult runTransaction(
        std::function<CommandHandlerResult()> operation) override;

    [[nodiscard]] std::any& featureStateValue(std::type_index type) override;
    void publishStatusValue(std::type_index type, std::any status) override;
    void publishDeltaValue(std::type_index type, std::any delta) override;

    [[nodiscard]] TabLifecycleResult close(
        const TabState& tab, std::chrono::milliseconds durabilityTimeout) override;
    [[nodiscard]] TabLifecycleResult reopen(
        const TabState& tab, const RecoveryRecordId& compensation) override;

    [[nodiscard]] WorkspaceSnapshot snapshot(Revision revision) const override;
    [[nodiscard]] std::vector<SearchCommandDescriptor> descriptors() const override;
    PaletteExecutionResult execute(std::string_view commandId) override;

    [[nodiscard]] WorkspaceApplyResult apply(
        const WorkspaceReplacePreview& preview,
        WorkspaceRecoverySink& recoverySink) override;
    [[nodiscard]] WorkspaceApplyResult recover(
        const WorkspaceRecoveryRecord& record) override;
    bool store(const WorkspaceRecoveryRecord& record) override;

    [[nodiscard]] std::optional<LspDocumentSnapshot> snapshot(
        std::string_view uri) const override;
    [[nodiscard]] LspWorkspaceDocumentWriteResult apply(
        std::string uri, Revision expectedRevision, std::string text) override;
    [[nodiscard]] LspWorkspaceFileResult snapshot(
        std::string_view uri, LspWorkspaceFileNode& node) const override;
    [[nodiscard]] LspWorkspaceFileResult createFile(
        std::string uri, bool overwrite) override;
    [[nodiscard]] LspWorkspaceFileResult writeFile(
        std::string uri, std::string content) override;
    [[nodiscard]] LspWorkspaceFileResult renamePath(
        std::string oldUri, std::string newUri, bool overwrite) override;
    [[nodiscard]] LspWorkspaceFileResult deletePath(
        std::string uri, bool recursive) override;
    [[nodiscard]] LspWorkspaceFileResult restorePath(
        std::string uri, const LspWorkspaceFileNode& node) override;

    [[nodiscard]] std::optional<FileDocumentId> activeDocumentId() const;
    [[nodiscard]] const TabState* activeTabState() const;
    // Whether the active tab shows a live diff. A guard several command handlers
    // share (a live-diff tab is read-only for edits), read from the active tab's
    // own kind so the rule lives in one place.
    [[nodiscard]] bool activeTabIsLiveDiff() const {
        const auto* tab = activeTabState();
        return tab != nullptr && tab->kind == TabKind::LiveDiff;
    }
    [[nodiscard]] Document const* activeDocument() const;
    [[nodiscard]] Document* activeDocument();
    void ensureDocumentRuntimeState(FileDocumentId document);
    // Discards every per-document association for a document that no longer
    // exists. Normally the tab close lifecycle does this; delete bypasses that
    // lifecycle (there is nothing left to flush), so it must do the same
    // cleanup or the state outlives the document.
    void discardDocumentRuntimeState(FileDocumentId document);
    [[nodiscard]] DocumentHistory& historyFor(FileDocumentId document);
    [[nodiscard]] SyntaxModel& syntaxFor(FileDocumentId document);
    [[nodiscard]] SyntaxViewState activeSyntaxView() const;
    [[nodiscard]] std::optional<WorkspaceDocumentState> activeWorkspaceState() const;
    [[nodiscard]] std::optional<DiffFileView> activeDiffFile() const;
    [[nodiscard]] std::string const& activeText() const;
    // The line-number gutter width for the active document: 0 when the setting is
    // off or there is no editor document, else digits(lineCount)+1. The whole-
    // document line count is cached by revision.
    [[nodiscard]] int lineNumberGutterWidth() const;
    void resetSelectionForActiveDocument();
    // Collapses to a SINGLE caret at the primary's clamped position.  For a
    // document switch, where the carried selection belongs to the previous
    // document and must not survive.
    void clampSelectionToActiveDocument();
    // Clamps EVERY selection into the active document, preserving their number
    // and ranges.  For in-document edits, where a multi-cursor set must survive
    // (typing over N selections leaves N carets, Sublime-style).
    void clampSelectionsToActiveDocument();
    [[nodiscard]] const std::vector<CellRun>& activeCellRuns() const;
    // The editor viewport, gated on word wrap: exact wrapped geometry when word
    // wrap is on; O(visible rows) unwrapped projection (compute_viewport_unwrapped)
    // when off, so a large document's first frame is viewport-bounded (M12).
    [[nodiscard]] ViewportViewState computeEditorViewport(
        ViewPresentationState& presentation, std::uint32_t firstRow,
        std::uint32_t firstColumn) const;
    [[nodiscard]] ViewportViewState viewport(
        ViewPresentationState& presentation) const;
    [[nodiscard]] SessionSnapshotSections sections(
        PaletteReport const& paletteReport = {}) const;
    [[nodiscard]] PromptStatusViewState promptStatusView() const;
    [[nodiscard]] std::optional<PromptViewState> promptProjection(
        ViewportDimensions dimensions,
        std::optional<Rect> promptReservation = std::nullopt) const;
    void projectFindReplacePrompt(PromptViewState& promptView) const;
    // The geometry-free semantic projection of the active footer-region prompt,
    // or nullopt unless a footer-region prompt is open. Shares the one
    // resolvePromptControls authority with the grid promptProjection.
    [[nodiscard]] std::optional<PromptView> promptView() const;
    // The one draft-conflict notice resolver: the geometry-free NoticeView for the
    // active document, or nullopt unless its reopen outcome is Conflict. Both the
    // grid ShellNotice (shellView) and the semantic section (noticeView) derive from
    // this single literal.
    [[nodiscard]] std::optional<NoticeView> draftNotice() const;
    // The geometry-free semantic draft-conflict notice for the snapshot section;
    // exactly draftNotice(), named to sit beside promptView() in sections().
    [[nodiscard]] std::optional<NoticeView> noticeView() const;
    // Whether the active document currently raises a draft-conflict notice. The
    // notice's tree-node presence lives outside the prompt/panel transitions, so the
    // runtime reconciles this into the interaction authority after each dispatch.
    [[nodiscard]] bool noticePresent() const;
    // Whether any file is externally modified (the external-modification section is
    // non-empty). Like noticePresent, reconciled into the interaction authority so
    // the external-modification node's presence tracks it -- after each dispatch and
    // in the watcher drain.
    [[nodiscard]] bool externalModificationPresent() const {
        return !external.viewState().files.empty();
    }
    // Dismiss the find/replace controller (and its prompt) when the active
    // document identity or revision no longer matches what it evaluated against,
    // so stale matches are never navigable or projected.
    void reconcileFindDocument();
    [[nodiscard]] ShellViewState shellView(ViewportDimensions dimensions,
                                            PaletteReport const& paletteReport = {}) const;
    // The projected + command-bound header/footer status fields the composed chrome
    // resolves its provider widgets against. Shared by shellView (built-in fields +
    // the grid resolver) and sections (the semantic dynamic-state resolver), so the
    // two resolve provider values identically.
    // The status-field styling a chrome resolution wants: the grid path prefixes
    // the cwd with a terminal glyph; the semantic dynamic-state path takes none, so
    // a native client receives no presentation styling. A strong mode (not a raw
    // prefix) makes semantic purity a named choice at each call site.
    enum class ChromeFieldMode { Grid, Semantic };
    [[nodiscard]] StatusFieldProjection chromeStatusFields(
        ChromeFieldMode mode) const;
    [[nodiscard]] PaletteViewState paletteView() const;
    // The tree view state with its scroll offset, scrollbar, and visible-window
    // hit map resolved against the last panel height (keep-selection-visible).
    [[nodiscard]] TreeViewState treeView() const;
    [[nodiscard]] std::vector<TreeWindow> treeWindows(
        ViewPresentationState const& presentation) const;
    // Scroll the tree so the selected node is visible, using the last cached
    // panel height. Called on the command path after a selection/expansion change
    // (never during snapshot generation), so it cannot perturb another client.
    void revealTreeSelection(ViewId viewId);
    // Scroll the tree viewport by `rows` (wheel), adjusting the server-owned
    // offset clamped to [0, maximum_first_row] WITHOUT moving the selection --
    // the tree analog of the editor's view.scroll_lines.
    void scrollTree(ViewId viewId, std::int64_t rows);
    void scrollTreeToFraction(ViewId viewId, std::uint32_t numerator,
                              std::uint32_t denominator);
    // Scroll the editor viewport minimally so the PRIMARY caret is visible, using
    // the last cached pane dimensions. Called on the command path after any edit
    // moves the caret (typing, delete, undo/redo, paste), so the view follows the
    // caret instead of leaving the user typing off-screen. The plain-caret
    // analog of reveal_active_find_match.
    void revealPrimaryCaret(ViewId viewId);
    [[nodiscard]] TextEncodingViewState textEncodingView() const;
    [[nodiscard]] DocumentViewState documentView() const;
    [[nodiscard]] CommandHandlerResult updateTabsFor(FileDocumentId document);
    [[nodiscard]] CommandHandlerResult activateDocument(FileDocumentId document);
    [[nodiscard]] ExternalDiffBurstResult applyExternalDiffBurst(
        std::vector<ExternalDiffRevision> changes);
    [[nodiscard]] GitDiffScanResult applyGitDiffScan(GitDiffScan scan);
    // CONTRACT
    // EditorSession::Impl: reconcileExternalWatchEvents and every mutation of
    //   `external` and its shared DiffModel it drives run only on the runtime
    //   thread, reached through the wake drain (or the test hook that stands in for
    //   it); the watcher worker thread only queues normalized events and never
    //   touches the flow. A host never observes a snapshot mid-drain.
    void reconcileExternalWatchEvents(std::vector<WatchEvent> events,
                                      bool resync = false);
    // Overflow recovery: the watcher lost events, so re-derive which OPEN documents
    // changed by comparing each to its disk baseline and reconcile the differences
    // through a sequence-free resync. Runs on the runtime thread (the worker only
    // signals). Without this, modifications during the overflow window are lost.
    void reconcileAllOpenDocumentsAgainstDisk();
    // The one runtime-owned external diff identity: a namespaced id derived from a
    // saved document key, and its reverse resolution to the open document. Both the
    // ingress reconcile and the action handlers resolve through these, so the id the
    // section publishes is exactly the id the handlers resolve, and it can never
    // collide with a git path-keyed entry in the shared DiffModel.
    [[nodiscard]] static DiffFileId externalDiffFileId(std::string_view savedPath);
    [[nodiscard]] static std::optional<std::string> savedPathFromExternalDiffId(
        const DiffFileId& id);
    [[nodiscard]] std::optional<FileDocumentId> resolveExternalDocument(
        const DiffFileId& id) const;
    // Drives the workspace's atomic external-baseline dismissal and refreshes an
    // already-persisted draft record (Decision 5) to the same baseline, so a
    // keep_buffer dismissal stops an overflow resync or a crash-reopen from
    // resurrecting the change. Returns whether the whole cross-store commit landed.
    [[nodiscard]] bool commitExternalDismissal(
        FileDocumentId document, bool removed,
        const std::optional<std::string>& dismissedContent);
    [[nodiscard]] std::optional<FileDocumentId> resolveOpenSavedDocumentByPath(
        const std::filesystem::path& relativePath) const;
    // Records that SSG itself wrote `relativePath`, so the matching watcher event is
    // correlated as a self-save and never raises a false external conflict. Ordered
    // by the save primitive before the write is observable; the library owns this,
    // a client never participates.
    void registerExternalSaveExpectation(
        const std::filesystem::path& relativePath);
    [[nodiscard]] CommandHandlerResult openOrFocusLiveDiffTab(
        const DiffFileView& file, NavigationClass classification,
        std::optional<ClientId> userClient,
        std::optional<ViewId> userView = std::nullopt);
    // Open (or re-focus) a read-only, in-memory tab of generated text content.
    // The reusable primitive behind the help page and any future
    // generated-content tab. Opens the text as a DocumentMode::ReadOnly virtual
    // document (untitled -> never autosaved, never savable) in a tab of `kind`
    // deduped by `contentIdentity`, and activates it. Re-opening the same
    // identity REFRESHES the content by remove+recreate -- it constructs a fresh
    // read-only document rather than editing the existing one, so the read-only
    // edit chokepoint (Document::apply) is never bypassed.
    [[nodiscard]] CommandHandlerResult openReadOnlyTab(
        TabKind kind, std::string contentIdentity, std::string label,
        std::string text, LanguageId language = LanguageId::plainText());
    // Open a live diff tab of the active saved document's buffer (the draft,
    // the target) against its CURRENT disk content (the baseline), via the
    // source-agnostic non-git diff engine. A missing/unreadable disk file diffs
    // the draft against empty. The tab is a derived view, not persisted.
    [[nodiscard]] CommandHandlerResult openDraftDiff();
    // Discard the active saved document's unsaved edits in favour of the disk
    // content (the notice's "Use disk"). The discarded edits are archived first
    // (reversible), then the buffer is reloaded from disk, the scratch draft
    // removed, and the reopen notice cleared. Non-destructive: no user file is
    // written, and the draft survives in the archive.
    [[nodiscard]] CommandHandlerResult discardDraft();
    // Dismiss the draft-conflict notice for the active document, leaving the
    // draft in place (the "Dismiss" action). Clears only the notice state.
    [[nodiscard]] CommandHandlerResult dismissDraftNotice();
    // Copy discarded draft content into the draft archive (beside the scratch
    // store) under a unique name, so a mis-clicked discard is recoverable.
    // Returns false only when the archive copy could not be written.
    [[nodiscard]] bool archiveDiscardedDraft(std::string_view savedPath,
                                             std::string_view content);
    void refreshLiveDiffDocuments(const DiffViewState& view);
    [[nodiscard]] bool openOrRevealFollowTargetProgrammatic(
        const FollowTarget& target);
    [[nodiscard]] bool revealCurrentDiffTarget(
        const FollowTarget& target, NavigationClass classification);
    [[nodiscard]] bool revealDiffTarget(
        const FollowTarget& target, NavigationClass classification);
    void recordNavigation(ClientId client, ViewId viewId,
                          NavigationClass classification);
    [[nodiscard]] bool refreshTree();
    void refreshTreeForPublication(Revision drainEntryRevision);
    // Re-assemble the authority-owned whole-screen schema from the given chrome inputs and
    // migrate the interaction over it. Takes the inputs as parameters (not members) so a
    // caller can build+migrate BEFORE adopting the new style/composition, keeping chrome
    // truth and the schema consistent if assembly ever throws.
    void rebuildInteractionSchema(const StyleDimensions& dimensions,
                                  std::string_view promptSigil,
                                  const std::optional<ValidatedComposition>& composed);
    // Refresh the file picker's candidates off the authority's picker epoch: a newly
    // (re)opened File picker rebuilds synchronously, any other picker state clears.
    [[nodiscard]] bool openPickerPrompt(PickerKind kind);
    // Walks the workspace into `fileCandidates`, honoring the gitignore setting.
    void rebuildFileCandidates();
    void refreshSyntax(std::vector<SyntaxEdit> edits = {});
    // M10 fast startup deferral.  While
    // `deferring_enrichment` is set (the pre-first-frame window when created with
    // defer_enrichment=true), refresh_tree and refresh_syntax record that work is
    // pending instead of running the O(workspace)/O(document) scan, so the first
    // frame is not blocked by it.  prime_deferred() clears the flag and runs any
    // pending scan.  The run counters exist for the startup oracle to assert no
    // scan happened before priming.
    void primeDeferred();
    // Flush drafts of open dirty documents. `flushDueAutosaveDrafts` applies the
    // debounce policy (eager first, then once per AutosaveDebounceMs); called on
    // the app's periodic tick. `flushAllAutosaveDrafts` forces every dirty draft,
    // for a clean process exit. Both return the number of drafts written this
    // call. Non-blocking: durability is the background fsync thread's job.
    std::size_t flushDueAutosaveDrafts();
    std::size_t flushAllAutosaveDrafts();
    std::size_t persistAutosaveDraft(FileDocumentId document);
    // On the first open of a saved document from disk, reconcile any dirty draft
    // recovered for its path against the current disk file (single-file draft
    // recovery, M15). Converged drafts are dropped and the clean disk buffer
    // kept; otherwise the draft is loaded as a dirty buffer and the document's
    // reopen outcome recorded (Restored when disk is unchanged, Conflict when it
    // changed externally). A no-op when there is no dirty draft for the path.
    void reconcileDraftOnOpen(FileDocumentId document);
    bool deferringEnrichment = false;
    bool pendingTreeRefresh = false;
    bool pendingSyntaxRefresh = false;
    std::uint64_t treeScanCount = 0;
    std::uint64_t syntaxRunCount = 0;
    std::unique_ptr<GitDiffRefreshWorkerState> gitDiffWorker;
    // Decision-13 durable capability fact: whether this session has a filesystem
    // watcher, so the semantic view model can publish "external changes are not
    // being watched" when the platform cannot provide one. Set optimistically when
    // a worker is enabled and cleared by the worker thread if watcher construction
    // fails; atomic because the worker thread writes it and snapshots read it.
    std::atomic<bool> watcherAvailable{false};
    // Runtime-owned save correlation. The save primitive records the intended
    // post-write disk state here; the reconcile consumes a match so an SSG write is
    // never mistaken for an external modification. Guarded because the save runs on
    // the dispatch thread and the reconcile on the runtime-thread drain.
    mutable std::mutex externalSaveMutex;
    std::deque<SaveExpectation> pendingSaveExpectations;
    // Runtime-thread mirror of watcherAvailable, so a transition detected during a
    // drain advances the session revision exactly once per edge. Owned by the
    // runtime thread; the atomic is the cross-thread carrier.
    bool lastPublishedWatcherAvailable = false;

    void enqueueStatus(StatusPriority priority, std::string text);
    void startGitDiffWorker(bool enableGit, bool enableWatcher);
    void stopGitDiffWorker();
    [[nodiscard]] bool drainGitDiffScans();
    void drainWatcherAvailability();
    [[nodiscard]] int gitDiffWakeDescriptor() const;
};

[[nodiscard]] CommandHandlerResult success();
[[nodiscard]] CommandHandlerResult failure(std::string message);
[[nodiscard]] std::string workspaceMessage(WorkspaceResult const& result);
[[nodiscard]] std::string tabMessage(TabResult const& result);
[[nodiscard]] std::string wrongPayload(std::string_view commandId);

} // namespace ssg
