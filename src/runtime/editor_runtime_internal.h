#pragma once

#include <ssg/ClipboardRegister.h>
#include <ssg/DiffModel.h>
#include <ssg/EditCommands.h>
#include <ssg/EditorRuntime.h>
#include <ssg/EditorSessionBuilder.h>
#include <ssg/ExternalModificationFlow.h>
#include <ssg/FileCommands.h>
#include <ssg/FindReplace.h>
#include <ssg/FollowEditsModel.h>
#include <ssg/DocumentHistory.h>
#include <ssg/Keymap.h>
#include <ssg/LspFeatureController.h>
#include <ssg/LspWorkspaceEditController.h>
#include <ssg/LuaCommandHost.h>
#include <ssg/Picker.h>
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

namespace ssg {

// Resolves a boolean setting, falling back when the stored value is not a bool.
// Shared by the presentation commands and the runtime's own reads.
inline bool boolSetting(SettingsModel const& settings, SettingKey key,
                        bool fallback) {
    auto value = settings.resolve(key).value;
    if (auto const* typed = std::get_if<bool>(&value)) return *typed;
    return fallback;
}

struct GitDiffRefreshWorkerState;

struct DocumentRuntimeState {
    explicit DocumentRuntimeState(
        HistoryConfig historyConfig = HistoryConfig::defaults(),
        std::shared_ptr<SyntaxParser> parser = nullptr)
        : history{historyConfig}, syntax{std::move(parser)} {
        ++liveCount;
    }

    DocumentRuntimeState(DocumentRuntimeState&& other) noexcept
        : history{std::move(other.history)}, syntax{std::move(other.syntax)} {
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

private:
    inline static std::atomic<std::uint64_t> liveCount{0};
};

void bindRuntimeEditing(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime);
void bindRuntimeFiles(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime);
void bindRuntimePresentation(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime);
void bindRuntimeNavigation(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime);
void bindRuntimeLanguageServices(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime);
[[nodiscard]] CommandHandlerResult executeFindReplaceCommand(
    EditorRuntime::Impl& runtime, Revision revision, FindReplaceCommand command,
    std::any const& payload);

struct EditorRuntime::Impl final : CommandServices,
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
         bool enableGitDiffWorker = true);
    ~Impl();

    std::filesystem::path root;
    std::filesystem::path scratchRoot;
    std::filesystem::path recoveryRoot;
    std::filesystem::path archiveRoot;
    RecoveryActions recovery;
    ScratchStore scratch;
    Workspace workspace;
    SelectionViewState selection;
    std::map<std::uint64_t, DocumentRuntimeState> documentRuntimeStates;
    ClipboardRegister clipboard;
    SettingsModel settings;
    FindReplaceController findReplace;
    // The document the find/replace controller last evaluated against.  Find
    // matches are byte offsets into one specific document; when the active
    // document identity or revision drifts from this, the controller is stale and
    // must be dismissed (see reconcile_find_document).
    std::optional<FileDocumentId> findDocumentId;
    PromptSurface prompt;
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
    std::unordered_map<std::string, StatusFieldProvider> statusFieldProviders;
    std::unordered_map<std::string, FileDocumentId> liveDiffDocuments;
    SearchController search;
    NavigationHistory navigation{64};
    LspSyncViewState lspSync;
    LspFeatureViewState lspFeatures;
    KeymapViewState keymap{"default", {}};
    ThemeSnapshot theme{};
    // Chrome glyphs and dimensions, beside the theme because they are the same
    // kind of thing: presentation this runtime owns and hands to layout.  See
    // doc/spec-style.md.
    Style style{};
    std::optional<WorkspaceReplacePreview> workspaceReplacePreview;
    std::unique_ptr<EditorSession> session;
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
        friend struct EditorRuntime::Impl;
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
    // Which picker the active PromptKind::Palette prompt belongs to, and the
    // sole source of the published palette mode and candidate set.  Maintained
    // as an invariant (set iff such a prompt is active) by
    // reconcileOpenPicker() rather than cleared at each close path, so a stale
    // kind cannot leak into the next open.
    std::optional<PickerKind> openPicker;
    // The open file picker's candidate set, built when the picker opens and
    // discarded when it closes: the walk stays off the per-keystroke and
    // per-frame paths, at the cost of not reflecting files created while the
    // picker is open (reopening picks them up).
    std::vector<PaletteCandidate> fileCandidates;
    std::uint32_t requestedFirstVisualRow = 0;
    // Horizontal scroll offset in cells (word wrap OFF only; VP-H). Reveal and the
    // horizontal scroll command update it; the viewport path passes it through.
    std::uint32_t requestedFirstVisualColumn = 0;
    // The document pane geometry from the most recent snapshot, plus the prompt
    // rows that snapshot reserved.  Used to reveal find matches against the real
    // pane height (not a fixed 24) so a match never lands behind the prompt rows.
    // Adding the reserved rows back yields a prompt-agnostic pane height, from
    // which the reveal subtracts the find prompt's rows deterministically.
    mutable std::uint32_t lastPaneContentRows = 24;
    mutable std::uint32_t lastPaneContentColumns = 80;
    mutable std::uint32_t lastReservedPromptRows = 0;
    // The side-panel (tree) content height from the most recent snapshot (a
    // read-only layout cache, like last_pane_content_rows), and the server-owned
    // tree scroll offset. The offset is written on the command path only
    // (reveal_tree_selection after a selection/expansion change, or tree.scroll
    // for a wheel) using the last cached height, so snapshot generation never
    // mutates it — one client's snapshot cannot move another client's scroll
    // (see doc/spec-scroll.md R2).
    mutable std::uint32_t lastPanelContentRows = 0;
    std::uint32_t treeFirstVisible = 0;
    bool wordWrap = false;
    std::uint64_t nextStatusId = 1;
    std::uint64_t nextTreeRevision = 1;

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
    [[nodiscard]] std::string activeText() const;
    void resetSelectionForActiveDocument();
    void clampSelectionToActiveDocument();
    [[nodiscard]] std::vector<CellRun> activeCellRuns() const;
    // The editor viewport, gated on word wrap: exact wrapped geometry when word
    // wrap is on; O(visible rows) unwrapped projection (compute_viewport_unwrapped)
    // when off, so a large document's first frame is viewport-bounded (M12).
    [[nodiscard]] ViewportViewState computeEditorViewport(
        ViewportDimensions dimensions, std::uint32_t firstRow,
        std::uint32_t firstColumn) const;
    [[nodiscard]] ViewportViewState viewport(ViewportDimensions dimensions) const;
    [[nodiscard]] SessionSnapshotSections sections(ViewportDimensions dimensions,
                                                   PaletteReport const& paletteReport = {}) const;
    [[nodiscard]] PromptStatusViewState promptStatusView(ViewportDimensions dimensions) const;
    void projectFindReplacePrompt(PromptViewState& promptView) const;
    // Dismiss the find/replace controller (and its prompt) when the active
    // document identity or revision no longer matches what it evaluated against,
    // so stale matches are never navigable or projected.
    void reconcileFindDocument();
    [[nodiscard]] ShellViewState shellView(ViewportDimensions dimensions,
                                            PaletteReport const& paletteReport = {}) const;
    [[nodiscard]] PaletteViewState paletteView() const;
    // The tree view state with its scroll offset, scrollbar, and visible-window
    // hit map resolved against the last panel height (keep-selection-visible).
    [[nodiscard]] TreeViewState treeView() const;
    // Scroll the tree so the selected node is visible, using the last cached
    // panel height. Called on the command path after a selection/expansion change
    // (never during snapshot generation), so it cannot perturb another client.
    void revealTreeSelection();
    // Scroll the tree viewport by `rows` (wheel), adjusting the server-owned
    // offset clamped to [0, maximum_first_row] WITHOUT moving the selection --
    // the tree analog of the editor's view.scroll_lines.
    void scrollTree(std::int64_t rows);
    void scrollTreeToFraction(std::uint32_t numerator,
                              std::uint32_t denominator);
    // Scroll the editor viewport minimally so the PRIMARY caret is visible, using
    // the last cached pane dimensions. Called on the command path after any edit
    // moves the caret (typing, delete, undo/redo, paste), so the view follows the
    // caret instead of leaving the user typing off-screen (see doc/spec-scroll.md
    // R5). The plain-caret analog of reveal_active_find_match.
    void revealPrimaryCaret();
    [[nodiscard]] TextEncodingViewState textEncodingView() const;
    [[nodiscard]] DocumentViewState documentView() const;
    [[nodiscard]] std::string currentPathLabel() const;
    [[nodiscard]] CommandHandlerResult updateTabsFor(FileDocumentId document);
    [[nodiscard]] CommandHandlerResult activateDocument(FileDocumentId document);
    [[nodiscard]] ExternalDiffBurstResult applyExternalDiffBurst(
        std::vector<ExternalDiffRevision> changes);
    [[nodiscard]] GitDiffScanResult applyGitDiffScan(GitDiffScan scan);
    [[nodiscard]] CommandHandlerResult openOrFocusLiveDiffTab(
        const DiffFileView& file, NavigationClass classification,
        std::optional<ClientId> userClient);
    void refreshLiveDiffDocuments(const DiffViewState& view);
    [[nodiscard]] bool openOrRevealFollowTargetProgrammatic(
        const FollowTarget& target);
    [[nodiscard]] bool revealCurrentDiffTarget(
        const FollowTarget& target, NavigationClass classification);
    [[nodiscard]] bool revealDiffTarget(
        const FollowTarget& target, NavigationClass classification);
    void recordNavigation(ClientId client, NavigationClass classification);
    void refreshTree();
    void reconcilePromptFocus();
    void reconcileOpenPicker();
    // The only way to open a picker.  Setting the kind and opening its prompt
    // together is what makes the "openPicker is set whenever a Palette prompt is
    // active" half of the invariant true by construction, leaving
    // reconcileOpenPicker() responsible only for the clearing half.
    [[nodiscard]] bool openPickerPrompt(PickerKind kind);
    // Walks the workspace into `fileCandidates`, honoring the gitignore setting.
    void rebuildFileCandidates();
    void refreshSyntax();
    // M10 fast startup deferral (doc/spec-fast-startup.md M10-3/M10-4).  While
    // `deferring_enrichment` is set (the pre-first-frame window when created with
    // defer_enrichment=true), refresh_tree and refresh_syntax record that work is
    // pending instead of running the O(workspace)/O(document) scan, so the first
    // frame is not blocked by it.  prime_deferred() clears the flag and runs any
    // pending scan.  The run counters exist for the startup oracle to assert no
    // scan happened before priming.
    void primeDeferred();
    bool deferringEnrichment = false;
    bool pendingTreeRefresh = false;
    bool pendingSyntaxRefresh = false;
    std::uint64_t treeScanCount = 0;
    std::uint64_t syntaxRunCount = 0;
    std::unique_ptr<GitDiffRefreshWorkerState> gitDiffWorker;

    void enqueueStatus(StatusPriority priority, std::string text);
    void startGitDiffWorker(bool enable);
    void stopGitDiffWorker();
    void drainGitDiffScans();
    [[nodiscard]] int gitDiffWakeDescriptor() const;
};

[[nodiscard]] CommandHandlerResult success();
[[nodiscard]] CommandHandlerResult failure(std::string message);
[[nodiscard]] std::string workspaceMessage(WorkspaceResult const& result);
[[nodiscard]] std::string tabMessage(TabResult const& result);
[[nodiscard]] std::string wrongPayload(std::string_view commandId);

} // namespace ssg
