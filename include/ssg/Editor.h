#pragma once

#include <ssg/ClientInput.h>
#include <ssg/ClipboardRegister.h>
#include <ssg/Command.h>
#include <ssg/CompiledKeymap.h>
#include <ssg/DiffIngressResult.h>
#include <ssg/DiffModel.h>
#include <ssg/DocumentHistory.h>
#include <ssg/DocumentPointerGesture.h>
#include <ssg/EditorFrameState.h>
#include <ssg/EditCommands.h>
#include <ssg/ExternalModificationFlow.h>
#include <ssg/FindReplace.h>
#include <ssg/FollowEditsModel.h>
#include <ssg/GitDiffIngress.h>
#include <ssg/InputRouting.h>
#include <ssg/Keymap.h>
#include <ssg/LspState.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/PaneTopology.h>
#include <ssg/Picker.h>
#include <ssg/PromptSurface.h>
#include <ssg/ScreenState.h>
#include <ssg/Search.h>
#include <ssg/Settings.h>
#include <ssg/StatusFields.h>
#include <ssg/Style.h>
#include <ssg/SyntaxModel.h>
#include <ssg/SyntaxWorker.h>
#include <ssg/TabManager.h>
#include <ssg/Theme.h>
#include <ssg/TreeModel.h>
#include <ssg/UiTree.h>
#include <ssg/Workspace.h>

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
#include <utility>
#include <vector>

namespace ssg {

struct EditorConfig {
    std::filesystem::path cwd;
    std::filesystem::path recoveryRoot;
    std::filesystem::path archiveRoot;
    std::filesystem::path snapshotPath;
    std::string snapshotIdentity;
    bool deferEnrichment = false;
    std::shared_ptr<SyntaxParser> syntaxParser;
    bool enableGitDiffWorker = true;
    bool enableFilesystemWatcher = true;
};

struct Editor;
namespace test { struct EditorAccess; }

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
[[nodiscard]] OperationResult activateTab(Editor& runtime, TabId tabId);
[[nodiscard]] OperationResult closeTabById(Editor& runtime, TabId tabId);
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
    friend OperationResult applyKeymapBind(
        Editor& runtime, KeymapBindArguments arguments);
    friend OperationResult applyKeymapUnbind(
        Editor& runtime, KeymapUnbindArguments arguments);

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

    Editor(std::filesystem::path canonicalCwd,
           std::filesystem::path recoveryRoot,
           std::filesystem::path archiveRoot,
           std::filesystem::path snapshotPath,
           std::string snapshotIdentity,
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
    [[nodiscard]] EditorFrameState captureFrameState() const;
    // Runtime-thread scan adoption; the caller holds operationMutex.
    [[nodiscard]] DiffIngressResult applyGitDiffScanLocked(GitDiffScan scan);
    // Applies a drained batch; the caller holds operationMutex as in pump.
    [[nodiscard]] bool adoptGitDiffWorkerDrainLocked(GitDiffWorkerDrain batch);
    [[nodiscard]] bool pumpSyntax();
    [[nodiscard]] const PlatformWake* syntaxWake() const noexcept;
    // CMD-5: no command handler runs while another handler is executing.
    [[nodiscard]] CommandResult dispatch(std::string_view commandId);
    // CMD-6: payload-bearing client operations remain typed through application.
    [[nodiscard]] ClientInputResult input(ClientInput const& input);
    // Programmatic text edits bypass key routing but share its locked maintenance.
    [[nodiscard]] OperationResult applyTextInput(
        TextInputCommand command, TextInputArguments arguments = {});
    [[nodiscard]] bool deferDispatch(std::string commandId);
    [[nodiscard]] bool dispatchInProgress() const noexcept;
    [[nodiscard]] Commands const& commandRegistry() const;
    void addCommand(std::string id, std::string label,
                    std::function<CommandResult()> handler);
    void replaceCommands(std::span<std::string const> oldIds,
                         Commands::Replacements replacements);
    void openWorkspaceSearch();
    [[nodiscard]] bool workspaceSearchPending() const noexcept;
    void advanceWorkspaceSearch();
    [[nodiscard]] OperationResult applySearchQueryChange(
        SearchQueryChange change);
    void resetKeymapToDefault();
    [[nodiscard]] CompiledKeymap const& resolveInputKeymap();
    [[nodiscard]] KeymapViewState const& keymapView() const noexcept {
        return keymap;
    }
    void focusEditor();

    [[nodiscard]] WorkspaceSearchState workspaceSearch(std::string query);
    [[nodiscard]] FindReplaceOperationResult updateFindQuery(
        PromptEditState query);
    [[nodiscard]] FindReplaceOperationResult updateReplacement(
        PromptEditState replacement);
    // These operations are called by dispatch/input while operationMutex is held.
    [[nodiscard]] OperationResult executeFindReplaceCommand(
        FindReplaceCommand command);
    [[nodiscard]] FindReplaceOperationResult applyFindQueryLocked(
        PromptEditState query);
    [[nodiscard]] FindReplaceOperationResult applyReplacementLocked(
        PromptEditState replacement);
    [[nodiscard]] OperationResult executeExternalAction(
        ExternalActionInvocation const& invocation);
    [[nodiscard]] OperationResult executeExternalAction(ExternalAction action);
    [[nodiscard]] OperationResult activateTreeNode(TreeNodeId nodeId);
    [[nodiscard]] FindReplaceViewState const& findView() const noexcept {
        return findReplace.viewState();
    }
    [[nodiscard]] OperationResult saveSession();
    [[nodiscard]] OperationResult deleteActiveFile();

    struct ResolvedPromptControls {
        std::vector<PromptControl> controls;
        std::size_t activeInput = 0;
    };

    std::filesystem::path root;
private:
    std::unique_ptr<GitIgnoreMatcher> workspaceIgnore;
    std::filesystem::path recoveryRoot;
    std::filesystem::path archiveRoot;
    std::filesystem::path snapshotPath;
    std::string snapshotIdentity;
    RecoveryManager recovery;
public:
    Workspace workspace;
    SelectionViewState selection;
    SettingsModel settings;
private:
    friend struct test::EditorAccess;
    std::map<std::uint64_t, DocumentRuntimeState> documentRuntimeStates;
public:
    ClipboardRegister clipboard;
private:
    // Accepted evaluations and replacements adopt the document ID alongside
    // their byte offsets and revision; rejected prompt updates adopt neither.
    FindReplaceController findReplace;
    std::optional<FileDocumentId> findDocumentId;
    void closeFind();
    void revealActiveFindMatch();
public:
    std::string statusText;
    TabManager tabs;
    DiffModel diff;
    ExternalModificationFlow external;
    FollowEditsModel follow;
    TreeModel tree;
private:
    std::shared_ptr<SyntaxParser> syntaxParser;
    SyntaxWorker syntaxWorker;
public:
    // The single interaction authority: owner of the screen schema, the
    // prompt surface, panel/focus/provider truth, the interaction projection,
    // and the tree revision source. Presentation reads its projection; every
    // focus, presence, and prompt change flows through it. Declared after
    // `tree` so it is constructed first.
    ScreenState screen;
private:
    std::unordered_map<std::string, FileDocumentId> liveDiffDocuments;
    // Read-only, in-memory "output" tabs (help, and any future
    // generated-content tab), keyed by the tab's content identity. Mirrors
    // liveDiffDocuments: a content tab does not store its document id in
    // TabState, so the document is resolved through this side map. The backing
    // documents are DocumentMode:: ReadOnly and untitled, so they are excluded
    // from autosave and cannot be saved; their content is refreshed by
    // remove+recreate, never edited in place (see openReadOnlyTab).
    std::unordered_map<std::string, FileDocumentId> readOnlyTabDocuments;
    // Resolved once so the header path abbreviation is stable for the session.
    std::string homeDirectory;
    // Per-document syntax language override for documents with no on-disk path
    // to infer a language from (a read-only help/output tab). refreshSyntax
    // consults this before falling back to path-derived detection, so a help
    // tab can be highlighted as e.g. Markdown despite being untitled.
    std::unordered_map<std::uint64_t, LanguageId> documentLanguageOverrides;
public:
    SearchController search;
private:
    std::uint64_t workspaceSearchGeneration = 0;
public:
    NavigationHistory navigation{64};
    LspSyncViewState lspSync;
    LspFeatureViewState lspFeatures;
private:
    KeymapViewState keymap{"default", {}};
    // Advances on every keymap mutation (keymap.bind/unbind, reset to default).
    // The catalog revision does NOT move on a rebind -- binding an existing
    // command registers nothing -- so routing-change detection needs this
    // separate counter.
    std::uint64_t keymapGeneration = 0;
    std::unique_ptr<CompiledKeymap> inputKeymap;
    std::optional<std::uint64_t> inputKeymapGeneration;
    void adoptKeymap(KeymapViewState replacement);

public:
    ThemeSnapshot theme{};
    // UI glyphs and dimensions, beside the theme because they are the same
    // kind of thing: presentation this runtime owns and hands to layout.
    Style style{};
    mutable std::mutex operationMutex;
    Commands commands;
    CommandResult dispatchLocked(std::string_view commandId);
private:
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

private:
    // Owns post-operation maintenance while the editor operation lock is held.
    // Deferred commands checkpoint separately so each observes reconciled state.
    class OperationScope {
    public:
        // None is for worker adoption and activation paths without local edits.
        enum class RevisionScope { None, ActiveDocument, Workspace };
        OperationScope(Editor& editor, RevisionScope revisions);
        ~OperationScope();
        void checkpoint(bool accepted);

    private:
        Editor& editor_;
        RevisionScope scope_;
        std::optional<std::pair<FileDocumentId, std::uint64_t>> activeRevision_;
        std::unordered_map<std::uint64_t, std::uint64_t> revisions_;
        bool externalPresentBefore_ = false;
        bool pending_ = true;
    };
    OperationScope* activeOperation_ = nullptr;

public:
    // The open file picker's candidate set, built when the picker opens and
    // rebuilt on filesystem refresh only while that picker remains open.
    std::vector<PaletteCandidate> fileCandidates;
private:
    std::vector<std::string> loadedFilesystemDirectories;
    std::optional<WorkspaceCorpus> workspaceSearchCorpus;
    std::optional<WorkspaceSearchState> workspaceSearchState;
    std::optional<std::uint64_t> panelWorkspaceSearchGeneration;
public:
    PaneTopology paneTopology = PaneTopology::initial();
    DocumentPointerGesture documentPointerGesture;
    bool wordWrap = false;
    bool lineNumbers = false;
private:
    // The active document's immutable flattened text, shared by navigation and
    // presentation until its document revision changes.
    mutable std::optional<std::uint64_t> activeTextRevision;
    mutable std::optional<FileDocumentId> activeTextDocument;
    mutable std::string activeTextCache;
    void startWorkspaceSearch(std::string query, std::uint64_t sourceRevision);
    void startWorkspaceSearch(ParsedSearchQuery query,
                              std::uint64_t sourceRevision);
public:
    // I1: Editor alone performs workspace/recovery effects for
    // close/reopen.
    [[nodiscard]] TabLifecycleResult closeTab(
        const TabState& tab, std::span<const TabId> alreadyClosed = {});
    [[nodiscard]] TabLifecycleResult reopenTab(
        const TabState& tab, const RecoveryRecordId& compensation);
    [[nodiscard]] OperationResult restoreSession();

private:
    [[nodiscard]] WorkspaceCorpus workspaceCorpus() const;
    void cancelWorkspaceSearch();
public:
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
private:
    // After workspace removal, erase all associations; safe to repeat.
    void discardDocumentRuntimeState(FileDocumentId document);
public:
    [[nodiscard]] DocumentHistory& historyFor(FileDocumentId document);
    [[nodiscard]] SyntaxModel& syntaxFor(FileDocumentId document);
    [[nodiscard]] std::shared_ptr<const SyntaxViewState>
    activeSyntaxView() const;
    [[nodiscard]] LanguageId languageFor(FileDocumentId document) const;
    [[nodiscard]] LanguageId activeSyntaxLanguage() const;
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
        return external.hasPending();
    }
private:
    [[nodiscard]] OperationResult executeExternalAction(
        ExternalAction action, std::optional<DiffFileId> requestedFile);
    void refreshDocumentSyntax(FileDocumentId document,
                               std::vector<SyntaxEdit> edits = {});
    void reconcileFindDocument();
public:
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
private:
    void refreshTreeForPublication();
public:
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
private:
    void refreshLiveDiffDocuments(const DiffViewState& view);
public:
    [[nodiscard]] bool revealCurrentDiffTarget(const FollowTarget& target);
    // While `deferringEnrichment` is set, workspace tree scans wait until
    // `primeDeferred`; syntax requests are always non-blocking.
    void primeDeferred();
private:
    bool deferringEnrichment = false;
    bool pendingTreeRefresh = false;
    std::uint64_t lastGitScanRevision = 0;
    std::optional<std::string> currentGitBranch;
    GitDiffIngress gitDiffIngress;

public:
    void showStatus(std::string text);
    [[nodiscard]] const PlatformWake* gitDiffWake() const noexcept;
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
