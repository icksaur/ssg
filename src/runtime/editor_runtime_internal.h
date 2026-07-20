#pragma once

#include <ssg/clipboard.h>
#include <ssg/diff_model.h>
#include <ssg/edit_commands.h>
#include <ssg/editor_runtime.h>
#include <ssg/editor_session_builder.h>
#include <ssg/external_modification_flow.h>
#include <ssg/file_commands.h>
#include <ssg/find_replace.h>
#include <ssg/follow_edits.h>
#include <ssg/history.h>
#include <ssg/keymap.h>
#include <ssg/lsp_feature_controller.h>
#include <ssg/lsp_workspace_edit_controller.h>
#include <ssg/lua.h>
#include <ssg/prompt.h>
#include <ssg/search.h>
#include <ssg/settings.h>
#include <ssg/status_queue.h>
#include <ssg/syntax_model.h>
#include <ssg/tab_manager.h>
#include <ssg/tree_model.h>
#include <ssg/ui_layout.h>
#include <ssg/workspace.h>

#include <any>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

namespace ssg {

void bindRuntimeEditing(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime);
void bindRuntimeFiles(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime);
void bindRuntimePresentation(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime);
void bindRuntimeNavigation(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime);
void bindRuntimeLanguageServices(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime);

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
         bool deferEnrichment = false);

    std::filesystem::path root;
    std::filesystem::path scratchRoot;
    std::filesystem::path recoveryRoot;
    RecoveryActions recovery;
    ScratchStore scratch;
    Workspace workspace;
    SelectionViewState selection;
    std::map<std::uint64_t, DocumentHistory> histories;
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
    TreeModel tree;
    SyntaxModel syntax;
    SearchController search;
    NavigationHistory navigation{64};
    LspSyncViewState lspSync;
    LspFeatureViewState lspFeatures;
    KeymapViewState keymap{"default", {}};
    ThemeSnapshot theme{};
    std::optional<WorkspaceReplacePreview> workspaceReplacePreview;
    std::unique_ptr<EditorSession> session;
    // Set by palette.execute after validating the selected candidate; the
    // EditorRuntime dispatch wrapper runs it through the registry once the
    // palette.execute transaction's session lock releases (the session mutex is
    // non-reentrant, so a handler cannot re-enter dispatch).
    std::optional<std::string> pendingPaletteTarget;
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
    [[nodiscard]] Document const* activeDocument() const;
    [[nodiscard]] Document* activeDocument();
    [[nodiscard]] DocumentHistory& historyFor(FileDocumentId document);
    [[nodiscard]] std::optional<WorkspaceDocumentState> activeWorkspaceState() const;
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
                                                   KeySequence const& leaderPending = {},
                                                   PaletteReport const& paletteReport = {}) const;
    [[nodiscard]] PromptStatusViewState promptStatusView(ViewportDimensions dimensions) const;
    void projectFindReplacePrompt(PromptViewState& promptView) const;
    // Dismiss the find/replace controller (and its prompt) when the active
    // document identity or revision no longer matches what it evaluated against,
    // so stale matches are never navigable or projected.
    void reconcileFindDocument();
    [[nodiscard]] ShellViewState shellView(ViewportDimensions dimensions,
                                            KeySequence const& leaderPending = {},
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
    void refreshTree();
    void reconcilePromptFocus();
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
    void enqueueStatus(StatusPriority priority, std::string text);
};

[[nodiscard]] CommandHandlerResult success();
[[nodiscard]] CommandHandlerResult failure(std::string message);
[[nodiscard]] std::string workspaceMessage(WorkspaceResult const& result);
[[nodiscard]] std::string tabMessage(TabResult const& result);
[[nodiscard]] std::string wrongPayload(std::string_view commandId);

} // namespace ssg
