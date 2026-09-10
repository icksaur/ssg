#pragma once

#include <ssg/ClientInput.h>
#include <ssg/ClipboardRegister.h>
#include <ssg/Command.h>
#include <ssg/CompiledKeymap.h>
#include <ssg/DiffModel.h>
#include <ssg/DocumentHistory.h>
#include <ssg/DocumentPointerGesture.h>
#include <ssg/EditCommands.h>
#include <ssg/ExternalModificationFlow.h>
#include <ssg/FileCommands.h>
#include <ssg/FindReplace.h>
#include <ssg/FollowEditsModel.h>
#include <ssg/GitDiffIngress.h>
#include <ssg/InputRouting.h>
#include <ssg/Keymap.h>
#include <ssg/LineLayoutCache.h>
#include <ssg/LspState.h>
#include <ssg/LuaCommandHost.h>
#include <ssg/PaneTopology.h>
#include <ssg/Picker.h>
#include <ssg/PromptSurface.h>
#include <ssg/ScreenState.h>
#include <ssg/Search.h>
#include <ssg/SessionSnapshot.h>
#include <ssg/Settings.h>
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

struct EditorConfig {
    std::filesystem::path cwd;
    std::filesystem::path recoveryRoot;
    std::filesystem::path archiveRoot;
    std::filesystem::path snapshotPath;
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

struct OperationResult {
    bool accepted = true;
    std::string message;
    std::optional<ViewAction> viewAction;

    operator CommandResult() const {
        return {accepted ? CommandError::None : CommandError::HandlerFailed,
                message, viewAction};
    }
};

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

struct DocumentRuntimeState {
    explicit DocumentRuntimeState(
        const SettingsModel& settings,
        std::shared_ptr<SyntaxParser> parser = nullptr)
        : history{settings}, syntax{std::move(parser)} {}

    DocumentRuntimeState(const DocumentRuntimeState&) = delete;
    DocumentRuntimeState& operator=(const DocumentRuntimeState&) = delete;
    DocumentRuntimeState(DocumentRuntimeState&&) noexcept = default;
    DocumentRuntimeState& operator=(DocumentRuntimeState&&) = default;

    DocumentHistory history;
    SyntaxModel syntax;
};

void bindRuntimeEditing(Commands& commands, Editor& runtime);
void bindRuntimeFiles(Commands& commands, Editor& runtime);
void bindRuntimePresentation(Commands& commands, Editor& runtime);
void bindRuntimeNavigation(Commands& commands, Editor& runtime);
void bindRuntimeLanguageServices(Commands& commands, Editor& runtime);
void bindRuntimeHelp(Commands& commands, Editor& runtime);
void registerAllCommands(Commands& commands, Editor& runtime);
[[nodiscard]] OperationResult applyEditorSelections(
    Editor& runtime, ApplySelections mutation);
[[nodiscard]] OperationResult applyEditorTextInput(
    Editor& runtime, TextInputCommand command,
    TextInputArguments arguments = {});
[[nodiscard]] OperationResult executeFindReplaceCommand(
    Editor& runtime, FindReplaceCommand command);
[[nodiscard]] FindReplaceOperationResult applyFindQuery(
    Editor& runtime, std::string query);
[[nodiscard]] FindReplaceOperationResult applyReplacement(
    Editor& runtime, std::string replacement);
[[nodiscard]] OperationResult activateTab(Editor& runtime, TabId tabId);
[[nodiscard]] OperationResult closeTabById(Editor& runtime, TabId tabId);
[[nodiscard]] OperationResult activateTreeNode(Editor& runtime,
                                                TreeNodeId nodeId);
[[nodiscard]] OperationResult invokeExternalAction(
    Editor& runtime, ExternalActionInvocation const& invocation);
[[nodiscard]] OperationResult applyUiNodeActivation(Editor& runtime,
                                                    UiNodeId const& nodeId);
[[nodiscard]] OperationResult applyThemeSet(Editor& runtime,
                                            ThemeSetArguments arguments);
[[nodiscard]] OperationResult applyStyleDefine(Editor& runtime,
                                               StyleDefineArguments arguments);
[[nodiscard]] OperationResult applyKeymapBind(Editor& runtime,
                                              KeymapBindArguments arguments);
[[nodiscard]] OperationResult applyKeymapUnbind(Editor& runtime,
                                                KeymapUnbindArguments arguments);

struct Editor final {
private:
    friend EditorCreateResult createEditor(EditorConfig config);

    Editor(std::filesystem::path canonicalCwd,
           std::filesystem::path recoveryRoot,
           std::filesystem::path archiveRoot,
           std::filesystem::path snapshotPath,
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
    // CMD-5: no command handler runs while another handler is executing.
    [[nodiscard]] CommandResult dispatch(std::string_view commandId);
    // CMD-6: payload-bearing client operations remain typed through application.
    [[nodiscard]] ClientInputResult input(ClientInput const& input);
    [[nodiscard]] bool deferDispatch(std::string commandId);
    [[nodiscard]] bool dispatchInProgress() const noexcept;
    [[nodiscard]] Commands const& commandRegistry() const;
    void addCommand(std::string id, std::string label,
                    std::function<CommandResult()> handler);
    void replaceCommands(std::span<std::string const> oldIds,
                         Commands::Replacements replacements);
    void startWorkspaceSearch(std::string query, std::uint64_t sourceRevision);
    void startWorkspaceSearch(ParsedSearchQuery query,
                              std::uint64_t sourceRevision);
    [[nodiscard]] bool workspaceSearchPending() const noexcept;
    void advanceWorkspaceSearch();
    void resetKeymapToDefault();
    [[nodiscard]] CompiledKeymap const& resolveInputKeymap();
    void focusEditor();

    [[nodiscard]] WorkspaceSearchState workspaceSearch(std::string query);
    [[nodiscard]] FindReplaceOperationResult updateFindQuery(std::string query);
    [[nodiscard]] FindReplaceOperationResult updateReplacement(
        std::string replacement);
    [[nodiscard]] OperationResult saveSession();

    struct ResolvedPromptControls {
        std::vector<PromptControl> controls;
        std::size_t activeInput = 0;
    };

    std::filesystem::path root;
    std::unique_ptr<GitIgnoreMatcher> workspaceIgnore;
    std::filesystem::path recoveryRoot;
    std::filesystem::path archiveRoot;
    std::filesystem::path snapshotPath;
    RecoveryManager recovery;
    Workspace workspace;
    SelectionViewState selection;
    SettingsModel settings;
    std::map<std::uint64_t, DocumentRuntimeState> documentRuntimeStates;
    ClipboardRegister clipboard;
    FindReplaceController findReplace;
    // The document the find/replace controller last evaluated against.  Find
    // matches are byte offsets into one specific document; when the active
    // document identity or revision drifts from this, the controller is stale
    // and must be dismissed (see reconcile_find_document).
    std::optional<FileDocumentId> findDocumentId;
    std::string statusText;
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

public:
    ThemeSnapshot theme{};
    // UI glyphs and dimensions, beside the theme because they are the same
    // kind of thing: presentation this runtime owns and hands to layout.
    Style style{};
    // The init.lua-composed header/footer,
    // pushed by the host after each init.lua evaluation via
    mutable std::mutex operationMutex;
    Commands commands;
    // Holds command IDs requested by the active handler until it finishes.
    // Only Editor can enqueue, so work cannot be stranded outside dispatch.
    class DeferredCommandQueue {
    public:
        static constexpr std::size_t kMaximum = 64;

        [[nodiscard]] bool empty() const noexcept { return ids_.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }
        [[nodiscard]] bool contains(std::string_view commandId) const noexcept {
            for (const auto& id : ids_) {
                if (id == commandId) return true;
            }
            return false;
        }
        [[nodiscard]] std::string takeFront() {
            auto front = std::move(ids_.front());
            ids_.erase(ids_.begin());
            return front;
        }
        void clear() noexcept { ids_.clear(); }

    private:
        friend struct Editor;
        [[nodiscard]] bool enqueue(std::string id) {
            if (ids_.size() >= kMaximum) return false;
            ids_.push_back(std::move(id));
            return true;
        }

        std::vector<std::string> ids_;
    };

    // Nested requests queue by ID and drain after the current handler,
    // stopping at the first failure.
    DeferredCommandQueue deferredCommands;

    CommandResult dispatchLocked(std::string_view commandId);
    // The open file picker's candidate set, built when the picker opens and
    // rebuilt on filesystem refresh only while that picker remains open.
    std::vector<PaletteCandidate> fileCandidates;
    std::vector<std::string> loadedFilesystemDirectories;
    std::optional<WorkspaceCorpus> workspaceSearchCorpus;
    std::optional<WorkspaceSearchState> workspaceSearchState;
    std::optional<std::uint64_t> panelWorkspaceSearchGeneration;
    PaneTopology paneTopology = PaneTopology::initial();
    DocumentPointerGesture documentPointerGesture;
    bool wordWrap = false;
    bool lineNumbers = false;
    // The active document's immutable flattened text, shared by navigation and
    // presentation until its document revision changes.
    mutable std::optional<std::uint64_t> activeTextRevision;
    mutable std::optional<FileDocumentId> activeTextDocument;
    mutable std::string activeTextCache;
    // I1: Editor alone performs workspace/recovery effects for
    // close/reopen.
    [[nodiscard]] TabLifecycleResult closeTab(
        const TabState& tab, std::span<const TabId> alreadyClosed = {});
    [[nodiscard]] TabLifecycleResult reopenTab(
        const TabState& tab, const RecoveryRecordId& compensation);
    [[nodiscard]] OperationResult restoreSession();

    [[nodiscard]] WorkspaceCorpus workspaceCorpus() const;
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
    [[nodiscard]] PromptViewState promptView() const;
    // Whether any file is externally modified.
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
    [[nodiscard]] OperationResult updateTabsFor(FileDocumentId document);
    [[nodiscard]] OperationResult activateDocument(FileDocumentId document);
    [[nodiscard]] DiffIngressResult applyExternalDiffBurst(
        std::vector<ExternalDiffRevision> changes);
    // Records that SSG itself wrote `relativePath`, so the matching watcher
    // event is correlated as a self-save and never raises a false external
    // conflict. Ordered by the save primitive before the write is observable;
    // the library owns this, a client never participates.
    [[nodiscard]] OperationResult
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
    [[nodiscard]] OperationResult
    openReadOnlyTab(TabKind kind, std::string contentIdentity,
                    std::string label, std::string text,
                    LanguageId language = LanguageId::plainText());
    [[nodiscard]] bool
    openOrRevealFollowTargetProgrammatic(const FollowTarget& target);
    void recordNavigation(NavigationClass classification);
    [[nodiscard]] OperationResult splitPane(SplitAxis axis);
    [[nodiscard]] OperationResult closePane();
    [[nodiscard]] OperationResult cyclePane(CycleDirection direction);
    [[nodiscard]] bool focusPane(PaneId pane);
    [[nodiscard]] bool refreshTree();
    void refreshTreeForPublication();
    [[nodiscard]] OperationResult toggleTreeExpanded(
        const TreeProviderId& providerId, const TreeNodeId& nodeId);
    // Re-assemble the authority-owned screen schema from the given UI inputs
    // and migrate the interaction over it. Takes the inputs as parameters (not
    // members) so a caller can build and migrate before adopting the new style.
    void rebuildInteractionSchema(const StyleDimensions& dimensions,
                                  std::string_view promptSigil);
    // A newly (re)opened File picker rebuilds synchronously before publication.
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
    bool deferringEnrichment = false;
    bool pendingTreeRefresh = false;
    bool pendingSyntaxRefresh = false;
    GitDiffIngress gitDiffIngress;

    void showStatus(std::string text);
    [[nodiscard]] int gitDiffWakeDescriptor() const;
};

[[nodiscard]] OperationResult success();
[[nodiscard]] OperationResult failure(std::string message);
[[nodiscard]] std::string workspaceMessage(WorkspaceResult const& result);
[[nodiscard]] std::string tabMessage(TabResult const& result);
[[nodiscard]] OperationResult createFileByPath(Editor& runtime,
                                                std::string_view path);
[[nodiscard]] OperationResult openStartupTarget(Editor& runtime,
                                                 std::string_view path);
[[nodiscard]] OperationResult openDroppedContent(
    Editor& runtime, std::span<const std::uint8_t> bytes,
    std::string_view label);
[[nodiscard]] OperationResult applyFilePathCompletion(
    Editor& runtime, PromptCompletion completion, std::string_view path);
[[nodiscard]] OperationResult applyGotoLine(Editor& runtime,
                                            std::string_view lineText);
[[nodiscard]] OperationResult navigateTo(
    Editor& runtime, NavigationTarget target,
    NavigationOrigin origin = NavigationOrigin::User);
} // namespace ssg
