#pragma once

#include <ssg/ClientInput.h>
#include <ssg/ClipboardRegister.h>
#include <ssg/CommandCatalog.h>
#include <ssg/CompiledKeymap.h>
#include <ssg/DiffModel.h>
#include <ssg/DocumentHistory.h>
#include <ssg/DocumentPointerGesture.h>
#include <ssg/DraftAutosaveScheduler.h>
#include <ssg/EditCommands.h>
#include <ssg/ExternalModificationFlow.h>
#include <ssg/FileCommands.h>
#include <ssg/FindReplace.h>
#include <ssg/FollowEditsModel.h>
#include <ssg/GitDiffIngress.h>
#include <ssg/Keymap.h>
#include <ssg/LineLayoutCache.h>
#include <ssg/LspFeatureController.h>
#include <ssg/LuaCommandHost.h>
#include <ssg/PaneTopology.h>
#include <ssg/Picker.h>
#include <ssg/PromptSurface.h>
#include <ssg/ScreenState.h>
#include <ssg/Search.h>
#include <ssg/Settings.h>
#include <ssg/StatusBar.h>
#include <ssg/StatusFields.h>
#include <ssg/Style.h>
#include <ssg/SyntaxModel.h>
#include <ssg/TabManager.h>
#include <ssg/Theme.h>
#include <ssg/TreeModel.h>
#include <ssg/UiTree.h>
#include <ssg/Viewport.h>
#include <ssg/Workspace.h>
#include <ssg/WorkspaceFileIndex.h>

#include <any>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ssg {

struct NoticeView {
    std::string text;
    std::vector<UiAction> actions;
    friend bool operator==(const NoticeView&, const NoticeView&) = default;
};

struct EditorConfig {
    std::filesystem::path cwd;
    std::filesystem::path scratchRoot;
    std::filesystem::path recoveryRoot;
    std::filesystem::path archiveRoot;
    bool deferEnrichment = false;
    std::shared_ptr<SyntaxParser> syntaxParser;
    bool enableGitDiffWorker = true;
    bool enableFilesystemWatcher = true;
};

struct Editor;

struct EditorCreateResult {
    std::unique_ptr<Editor> session;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept { return session != nullptr; }
};

[[nodiscard]] EditorCreateResult createEditor(EditorConfig config);

struct ExternalDiffRevision {
    NonGitDiffEvent event;
    std::uint64_t revision{0};
};

enum class DiffIngressError {
    None,
    EmptyBurst,
    DiffRejected,
    FollowRejected,
};

struct DiffIngressResult {
    DiffIngressError error = DiffIngressError::None;
    [[nodiscard]] bool accepted() const noexcept {
        return error == DiffIngressError::None;
    }
};

struct PumpResult {
    bool advanced;
};

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

// The reopen outcome of a document's recovered draft (single-file draft
// recovery, M15). Mirrors Editor::DraftReopenNotice; lives per-document
// so the notice (p5) and discard (p6) phases can read it by document id.
enum class DraftReopenOutcome { None, Restored, Conflict };

struct DocumentRuntimeState {
    explicit DocumentRuntimeState(
        const SettingsModel& settings,
        std::shared_ptr<SyntaxParser> parser = nullptr)
        : history{settings}, syntax{std::move(parser)} {
        ++liveCount;
    }

    DocumentRuntimeState(DocumentRuntimeState&& other) noexcept
        : history{std::move(other.history)}, syntax{std::move(other.syntax)},
          reopen{other.reopen} {
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

private:
    inline static std::atomic<std::uint64_t> liveCount{0};
};

void bindRuntimeEditing(CommandCatalog& catalog, Editor& runtime);
void bindRuntimeFiles(CommandCatalog& catalog, Editor& runtime);
void bindRuntimePresentation(CommandCatalog& catalog, Editor& runtime);
void bindRuntimeNavigation(CommandCatalog& catalog, Editor& runtime);
void bindRuntimeLanguageServices(CommandCatalog& catalog, Editor& runtime);
void bindRuntimeHelp(CommandCatalog& catalog, Editor& runtime);
void registerAllCommands(CommandCatalog& catalog, Editor& runtime);
[[nodiscard]] CommandHandlerResult executeFindReplaceCommand(
    Editor& runtime, FindReplaceCommand command,
    std::any const& payload);

struct Editor final {
private:
    friend EditorCreateResult createEditor(EditorConfig config);

    Editor(std::filesystem::path canonicalCwd,
           std::filesystem::path scratchRoot,
           std::filesystem::path recoveryRoot,
           std::filesystem::path archiveRoot,
           bool deferEnrichment = false,
           std::shared_ptr<SyntaxParser> parser = nullptr,
           bool enableGitDiffWorker = true,
           bool enableFilesystemWatcher = true);

public:
    ~Editor();
    Editor(Editor const&) = delete;
    Editor& operator=(Editor const&) = delete;
    Editor(Editor&&) = delete;
    Editor& operator=(Editor&&) = delete;

    [[nodiscard]] PumpResult pump();
    [[nodiscard]] CommandResult dispatch(ClientCommand const& command);
    [[nodiscard]] ClientInputResult input(ClientInput const& input);
    [[nodiscard]] SessionTopology topology() const;
    [[nodiscard]] bool deferDispatch(ClientCommand command);
    [[nodiscard]] bool dispatchInProgress() const noexcept;
    [[nodiscard]] CommandCatalog const& commandCatalog() const;
    [[nodiscard]] CommandHandle registerCommand(CommandSpec command);
    [[nodiscard]] std::vector<CommandHandle> replaceCommandGeneration(
        std::span<CommandHandle const> retire,
        std::vector<CommandSpec> commands);
    [[nodiscard]] std::filesystem::path const& workspaceRoot() const noexcept;
    void resetKeymapToDefault();
    [[nodiscard]] CompiledKeymap const& resolveInputKeymap();
    void focusEditor();

    struct DeferredWorkCounts {
        std::uint64_t syntaxRuns = 0;
        std::uint64_t treeScans = 0;
    };
    [[nodiscard]] DeferredWorkCounts deferredWorkCounts() const;
    [[nodiscard]] static std::uint64_t liveDocumentRuntimeStateCountForTests();
    [[nodiscard]] std::string activeDocumentText() const;

    enum class DraftReopenNotice { None, Restored, Conflict };
    [[nodiscard]] DraftReopenNotice activeDraftReopenNotice() const;

    struct ResolvedPromptControls {
        std::vector<PromptControl> controls;
        std::size_t activeInput = 0;
    };

    std::filesystem::path root;
    std::filesystem::path scratchRoot;
    std::filesystem::path recoveryRoot;
    std::filesystem::path archiveRoot;
    RecoveryManager recovery;
    ScratchStore scratch;
    Workspace workspace;
    SelectionViewState selection;
    SettingsModel settings;
    std::map<std::uint64_t, DocumentRuntimeState> documentRuntimeStates;
    ClipboardRegister clipboard;
    DraftAutosaveScheduler autosave;
    FindReplaceController findReplace;
    // The document the find/replace controller last evaluated against.  Find
    // matches are byte offsets into one specific document; when the active
    // document identity or revision drifts from this, the controller is stale
    // and must be dismissed (see reconcile_find_document).
    std::optional<FileDocumentId> findDocumentId;
    StatusBar status;
    TabManager tabs;
    DiffModel diff;
    ExternalModificationFlow external;
    FollowEditsModel follow;
    TreeModel tree;
    std::shared_ptr<SyntaxParser> syntaxParser;
    // The single interaction authority: owner of the screen schema, the
    // prompt surface, panel/focus/provider truth, the interaction projection,
    // and the tree revision source. Presentation reads its projection; every
    // focus, presence, and prompt change flows through it. Declared after
    // `tree` so it is constructed first.
    ScreenState screen;
    std::unordered_map<std::string, FileDocumentId> liveDiffDocuments;
    // Read-only, in-memory "output" tabs (help, and any future
    // generated-content tab), keyed by the tab's content identity. Mirrors
    // liveDiffDocuments: a content tab does not store its document id in
    // TabState, so the document is resolved through this side map. The backing
    // documents are DocumentMode:: ReadOnly and untitled, so they are excluded
    // from autosave and cannot be saved; their content is refreshed by
    // remove+recreate, never edited in place (see openReadOnlyTab).
    std::unordered_map<std::string, FileDocumentId> readOnlyTabDocuments;
    // The user's home directory, resolved once at construction (HOME, then
    // USERPROFILE, with trailing separators stripped) so the header path
    // field's
    // "~" abbreviation is deterministic across a session rather than re-reading
    // the process environment on every presentation. Empty disables
    // abbreviation.
    std::string homeDirectory;
    // Per-document syntax language override for documents with no on-disk path
    // to infer a language from (a read-only help/output tab). refreshSyntax
    // consults this before falling back to path-derived detection, so a help
    // tab can be highlighted as e.g. Markdown despite being untitled.
    std::unordered_map<std::uint64_t, LanguageId> documentLanguageOverrides;
    SearchController search;
    std::uint64_t workspaceSearchGeneration = 0;
    NavigationHistory navigation{64};
    LspSyncViewState lspSync;
    LspFeatureViewState lspFeatures;
    KeymapViewState keymap{"default", {}};
    // Advances on every keymap mutation (keymap.bind/unbind, reset to default).
    // The catalog revision does NOT move on a rebind -- binding an existing
    // command registers nothing -- so routing-change detection needs this
    // separate counter.
    std::uint64_t keymapGeneration = 0;

private:
    std::unique_ptr<CompiledKeymap> inputKeymap;
    std::optional<std::uint64_t> inputKeymapGeneration;
    std::optional<CatalogRevision> inputCatalogRevision;

public:
    ThemeSnapshot theme{};
    // UI glyphs and dimensions, beside the theme because they are the same
    // kind of thing: presentation this runtime owns and hands to layout.
    Style style{};
    // The init.lua-composed header/footer,
    // pushed by the host after each init.lua evaluation via
    std::optional<WorkspaceReplacePreview> workspaceReplacePreview;
    std::uint64_t workspaceReplaceGeneration = 0;
    mutable std::mutex operationMutex;
    SessionTopology sessionTopology;
    CommandCatalog catalog;
    // Commands a running handler asked to dispatch, run in order once the
    // operation lock releases. The operation mutex is not reentrant, so a
    // handler cannot dispatch; this is how it asks for one.
    //
    // ONE queue, drained inside the dispatch wrapper, rather than a field per
    // caller: palette.execute, prompt.submit and a script's ssg.command all
    // want the same thing.  Separate single-slot fields each needed their own
    // early return, and a return that forgot to drain silently postponed the
    // work to some later, unrelated dispatch.
    //
    // Deferring rather than nesting keeps each command complete before the next
    // command starts.
    struct DeferredCommand {
        ClientCommand command;
    };

    // The queue, shaped so `Editor::deferDispatch` is the only way to ADD to it
    // -- by construction, not by convention.
    //
    // It was previously a bare vector that two callers pushed to directly while
    // a third went through a checked method, so the shortest way to queue a
    // command was the only unchecked one.  Hiding the vector alone would just
    // move that hole to the wrapper, so writing is private and `Editor` is the
    // only friend: a future caller cannot reach the write path at all, whereas
    // reading (which the drain needs) is harmless and stays public.
    class DeferredCommandQueue {
    public:
        // A handler that queued without limit would spin the drain loop
        // forever; refusing says so, where the alternative is an editor that
        // stops responding for no visible reason.
        static constexpr std::size_t kMaximum = 64;

        [[nodiscard]] bool empty() const noexcept { return commands_.empty(); }
        [[nodiscard]] std::size_t size() const noexcept {
            return commands_.size();
        }
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
        // Only reachable through Editor::deferDispatch, which is what enforces
        // that a dispatch is actually in progress.  Queueing outside one would
        // strand the command until some later, unrelated dispatch drained it.
        friend struct Editor;
        [[nodiscard]] bool enqueue(DeferredCommand deferred) {
            if (commands_.size() >= kMaximum) return false;
            commands_.push_back(std::move(deferred));
            return true;
        }

        std::vector<DeferredCommand> commands_;
    };

    DeferredCommandQueue deferredCommands;

    CommandResult dispatchLocked(ClientCommand const& command);
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
    PaneTopology paneTopology = PaneTopology::initial();
    DocumentPointerGesture documentPointerGesture;
    bool wordWrap = false;
    bool lineNumbers = false;
    // The active document's immutable flattened text, shared by navigation and
    // presentation until its document revision changes.
    mutable std::optional<std::uint64_t> activeTextRevision;
    mutable std::optional<FileDocumentId> activeTextDocument;
    mutable std::string activeTextCache;
    std::uint64_t nextStatusId = 1;

    // I1: Editor alone performs workspace/recovery effects for
    // close/reopen.
    [[nodiscard]] TabLifecycleResult closeTab(
        const TabState& tab, std::chrono::milliseconds durabilityTimeout,
        std::span<const TabId> alreadyClosed = {});
    [[nodiscard]] TabLifecycleResult reopenTab(
        const TabState& tab, const RecoveryRecordId& compensation);

    [[nodiscard]] WorkspaceSnapshot snapshot(std::uint64_t revision) const;
    [[nodiscard]] WorkspaceApplyResult applyWorkspaceReplace(
        const WorkspaceReplacePreview& preview);
    [[nodiscard]] std::optional<FileDocumentId> activeDocumentId() const;
    [[nodiscard]] const TabState* activeTabState() const;
    // Whether the active tab shows a live diff. A guard several command
    // handlers share (a live-diff tab is read-only for edits), read from the
    // active tab's own kind so the rule lives in one place.
    [[nodiscard]] bool activeTabIsLiveDiff() const {
        const auto* tab = activeTabState();
        return tab != nullptr && tab->kind == TabKind::LiveDiff;
    }
    [[nodiscard]] Document const* activeDocument() const;
    [[nodiscard]] Document* activeDocument();
    void ensureDocumentRuntimeState(FileDocumentId document);
    // Discards every per-document association for a document that no longer
    // exists. Normally tab close does this; delete bypasses that close
    // path (there is nothing left to flush), so it must do the same
    // cleanup or the state outlives the document.
    void discardDocumentRuntimeState(FileDocumentId document);
    [[nodiscard]] DocumentHistory& historyFor(FileDocumentId document);
    [[nodiscard]] SyntaxModel& syntaxFor(FileDocumentId document);
    [[nodiscard]] SyntaxViewState activeSyntaxView() const;
    [[nodiscard]] std::optional<WorkspaceDocumentState> activeWorkspaceState() const;
    [[nodiscard]] std::optional<DiffFileView> activeDiffFile() const;
    [[nodiscard]] std::string const& activeText() const;
    void resetSelectionForActiveDocument();
    // Collapses to a SINGLE caret at the primary's clamped position.  For a
    // document switch, where the carried selection belongs to the previous
    // document and must not survive.
    void clampSelectionToActiveDocument();
    // Clamps EVERY selection into the active document, preserving their number
    // and ranges.  For in-document edits, where a multi-cursor set must survive
    // (typing over N selections leaves N carets, Sublime-style).
    void clampSelectionsToActiveDocument();
    [[nodiscard]] UiSchema projectedUiTree() const;
    [[nodiscard]] std::optional<ResolvedPromptControls>
    resolvedPromptControls() const;
    [[nodiscard]] PromptStatusViewState promptStatusView() const;
    // The geometry-free semantic projection of the active footer-region prompt,
    // or nullopt unless a footer-region prompt is open.
    // The one draft-conflict notice resolver: the geometry-free NoticeView for
    // the active document, or nullopt unless its reopen outcome is Conflict.
    [[nodiscard]] std::optional<NoticeView> draftNotice() const;
    // The geometry-free draft-conflict notice used during presentation.
    [[nodiscard]] std::optional<NoticeView> noticeView() const;
    // Whether the active document currently raises a draft-conflict notice. The
    // notice's tree-node presence lives outside the prompt/panel transitions,
    // so the runtime reconciles this into the interaction authority after each
    // dispatch.
    [[nodiscard]] bool noticePresent() const;
    // Whether any file is externally modified (the external-modification
    // section is non-empty). Like noticePresent, reconciled into the
    // interaction authority so the external-modification node's presence tracks
    // it -- after each dispatch and in the watcher drain.
    [[nodiscard]] bool externalModificationPresent() const {
        return !external.viewState().files.empty();
    }
    // Dismiss the find/replace controller (and its prompt) when the active
    // document identity or revision no longer matches what it evaluated
    // against, so stale matches are never navigable or projected.
    void reconcileFindDocument();
    // The projected and command-bound header/footer status fields the UI tree
    // resolves its provider widgets against.
    // The status-field styling UI resolution wants: the grid path prefixes
    // the cwd with a terminal glyph; the semantic dynamic-state path takes
    // none, so a native client receives no presentation styling. A strong mode
    // (not a raw prefix) makes semantic purity a named choice at each call
    // site.
    [[nodiscard]] StatusFieldProjection uiStatusFields() const;
    [[nodiscard]] PaletteViewState paletteView() const;
    // The geometry-free tree state; GridPresenter resolves its visible window.
    [[nodiscard]] TreeViewState treeView() const;
    [[nodiscard]] CommandHandlerResult updateTabsFor(FileDocumentId document);
    [[nodiscard]] CommandHandlerResult activateDocument(FileDocumentId document);
    [[nodiscard]] DiffIngressResult applyExternalDiffBurst(
        std::vector<ExternalDiffRevision> changes);
    [[nodiscard]] DiffIngressResult applyGitDiffScan(GitDiffScan scan);
    // Records that SSG itself wrote `relativePath`, so the matching watcher
    // event is correlated as a self-save and never raises a false external
    // conflict. Ordered by the save primitive before the write is observable;
    // the library owns this, a client never participates.
    [[nodiscard]] CommandHandlerResult
    openOrFocusLiveDiffTab(const DiffFileView& file,
                           NavigationClass classification);
    // Open (or re-focus) a read-only, in-memory tab of generated text content.
    // The reusable primitive behind the help page and any future
    // generated-content tab. Opens the text as a DocumentMode::ReadOnly virtual
    // document (untitled -> never autosaved, never savable) in a tab of `kind`
    // deduped by `contentIdentity`, and activates it. Re-opening the same
    // identity REFRESHES the content by remove+recreate -- it constructs a
    // fresh read-only document rather than editing the existing one, so the
    // read-only edit chokepoint (Document::apply) is never bypassed.
    [[nodiscard]] CommandHandlerResult
    openReadOnlyTab(TabKind kind, std::string contentIdentity,
                    std::string label, std::string text,
                    LanguageId language = LanguageId::plainText());
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
    [[nodiscard]] bool
    openOrRevealFollowTargetProgrammatic(const FollowTarget& target);
    [[nodiscard]] bool revealDiffTarget(const FollowTarget& target,
                                        NavigationClass classification);
    void recordNavigation(NavigationClass classification);
    [[nodiscard]] CommandHandlerResult splitPane(SplitAxis axis);
    [[nodiscard]] CommandHandlerResult closePane();
    [[nodiscard]] CommandHandlerResult cyclePane(CycleDirection direction);
    [[nodiscard]] bool focusPane(PaneId pane);
    [[nodiscard]] bool refreshTree();
    void refreshTreeForPublication();
    // Re-assemble the authority-owned screen schema from the given UI inputs
    // and migrate the interaction over it. Takes the inputs as parameters (not
    // members) so a caller can build and migrate before adopting the new style.
    void rebuildInteractionSchema(const StyleDimensions& dimensions,
                                  std::string_view promptSigil);
    // Refresh the file picker's candidates off the authority's picker epoch: a
    // newly (re)opened File picker rebuilds synchronously, any other picker
    // state clears.
    [[nodiscard]] bool openPickerPrompt(PickerKind kind);
    // Walks the workspace into `fileCandidates`, honoring the gitignore
    // setting.
    void rebuildFileCandidates();
    void refreshSyntax(std::vector<SyntaxEdit> edits = {});
    // While `deferring_enrichment` is set (the pre-first-frame window when
    // created with defer_enrichment=true), refresh_tree and refresh_syntax
    // record that work is pending instead of running the
    // O(workspace)/O(document) scan, so the first frame is not blocked by it.
    // prime_deferred() clears the flag and runs any pending scan.  The run
    // counters exist for the startup oracle to assert no scan happened before
    // priming.
    void primeDeferred();
    // Flush drafts of open dirty documents. `flushDueAutosaveDrafts` applies
    // the debounce policy (eager first, then once per AutosaveDebounceMs);
    // called on the app's periodic tick. `flushAllAutosaveDrafts` forces every
    // dirty draft, for a clean process exit. Both return the number of drafts
    // written this call. Non-blocking: durability is the background fsync
    // thread's job.
    std::size_t flushDueAutosaveDrafts();
    std::size_t flushAllAutosaveDrafts();
    // On the first open of a saved document from disk, reconcile any dirty
    // draft recovered for its path against the current disk file (single-file
    // draft recovery, M15). Converged drafts are dropped and the clean disk
    // buffer kept; otherwise the draft is loaded as a dirty buffer and the
    // document's reopen outcome recorded (Restored when disk is unchanged,
    // Conflict when it changed externally). A no-op when there is no dirty
    // draft for the path.
    void reconcileDraftOnOpen(FileDocumentId document);
    bool deferringEnrichment = false;
    bool pendingTreeRefresh = false;
    bool pendingSyntaxRefresh = false;
    std::uint64_t treeScanCount = 0;
    std::uint64_t syntaxRunCount = 0;
    GitDiffIngress gitDiffIngress;

    void enqueueStatus(StatusPriority priority, std::string text);
    [[nodiscard]] int gitDiffWakeDescriptor() const;
};

[[nodiscard]] CommandHandlerResult success();
[[nodiscard]] CommandHandlerResult failure(std::string message);
[[nodiscard]] std::string workspaceMessage(WorkspaceResult const& result);
[[nodiscard]] std::string tabMessage(TabResult const& result);
[[nodiscard]] std::string wrongPayload(std::string_view commandId);

ClientInputResult inputLocked(Editor&, ClientInput const&);

} // namespace ssg
