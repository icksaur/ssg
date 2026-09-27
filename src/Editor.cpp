#include <ssg/Editor.h>
#include <ssg/FilesystemWatcher.h>
#include <ssg/GitDiffScanStage.h>
#include <ssg/GraphemeLayout.h>
#include <ssg/ScreenLayout.h>
#include <ssg/Selection.h>
#include <ssg/SessionSnapshot.h>
#include <ssg/Style.h>
#include <ssg/TextCodec.h>
#include <ssg/WorkspaceFileIndex.h>
#include <ssg/platform_files.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <unordered_map>

namespace ssg {
namespace {

std::string liveDiffTabLabelForPath(const std::filesystem::path& path) {
    const auto filename = path.filename().string();
    return filename.empty() ? path.generic_string() : filename;
}

std::vector<GitTreeRecord> gitTreeRecordsFromScan(
    const std::vector<GitDiffFile>& files) {
    std::vector<GitTreeRecord> records;
    records.reserve(files.size());
    for (const auto& file : files) {
        records.push_back(
            {.workspacePath = file.path.generic_string(),
             .label = file.path.generic_string(),
             .status = file.status(),
             .commands = {}});
    }
    return records;
}

std::size_t lineStartOffset(std::string_view text, std::size_t line) {
    std::size_t offset = 0;
    for (std::size_t current = 0; current < line && offset < text.size();
         ++current) {
        const auto newline = text.find('\n', offset);
        if (newline == std::string_view::npos) return text.size();
        offset = newline + 1;
    }
    return offset;
}

std::string resolveHomeDirectory() {
    std::string home;
    if (const char* value = std::getenv("HOME"); value != nullptr) {
        home = value;
    } else if (const char* profile = std::getenv("USERPROFILE");
               profile != nullptr) {
        home = profile;
    }
    while (!home.empty() && (home.back() == '/' || home.back() == '\\')) {
        home.pop_back();
    }
    for (auto& ch : home) {
        if (ch == '\\') ch = '/';
    }
    return home;
}

} // namespace

Editor::OperationScope::OperationScope(Editor& editor, RevisionScope scope)
    : editor_{editor}, scope_{scope} {
    if (editor_.activeOperation_ != nullptr) {
        throw std::logic_error{"editor operation is already active"};
    }
    if (scope == RevisionScope::ActiveDocument) {
        if (const auto id = editor_.activeDocumentId()) {
            if (const auto* document = editor_.workspace.tryDocument(*id)) {
                activeRevision_ = std::pair{*id, document->revision()};
            }
        }
    } else if (scope == RevisionScope::Workspace) {
        for (const auto id : editor_.workspace.documents()) {
            revisions_.emplace(id.value(), editor_.workspace.document(id).revision());
        }
    }
    externalPresentBefore_ = editor_.externalModificationPresent();
    editor_.screen.refreshExternalModificationPresence(externalPresentBefore_);
    editor_.activeOperation_ = this;
}

void Editor::OperationScope::checkpoint(bool accepted) {
    bool acceptedEdit = false;
    if (scope_ == RevisionScope::ActiveDocument) {
        if (activeRevision_) {
            const auto* document =
                editor_.workspace.tryDocument(activeRevision_->first);
            acceptedEdit = accepted && document != nullptr &&
                           document->revision() != activeRevision_->second;
        }
        if (const auto id = editor_.activeDocumentId()) {
            if (const auto* active = editor_.workspace.tryDocument(*id)) {
                activeRevision_ = std::pair{*id, active->revision()};
            } else {
                activeRevision_.reset();
            }
        } else {
            activeRevision_.reset();
        }
    } else if (scope_ == RevisionScope::Workspace) {
        for (const auto id : editor_.workspace.documents()) {
            const auto revision = editor_.workspace.document(id).revision();
            const auto previous = revisions_.find(id.value());
            acceptedEdit |= accepted && previous != revisions_.end() &&
                            previous->second != revision;
            revisions_.insert_or_assign(id.value(), revision);
        }
    }
    editor_.reconcileFindDocument();
    const bool externalPresentAfter = editor_.externalModificationPresent();
    if (externalPresentAfter != externalPresentBefore_) {
        editor_.screen.refreshExternalModificationPresence(externalPresentAfter);
    }
    externalPresentBefore_ = externalPresentAfter;
    if (acceptedEdit) (void)editor_.follow.notifyLocalEdit();
    pending_ = false;
}

Editor::OperationScope::~OperationScope() {
    if (pending_) {
        editor_.reconcileFindDocument();
        const bool externalPresentAfter = editor_.externalModificationPresent();
        if (externalPresentAfter != externalPresentBefore_) {
            editor_.screen.refreshExternalModificationPresence(externalPresentAfter);
        }
    }
    editor_.activeOperation_ = nullptr;
}

KeymapViewState defaultTerminalKeymap() {
    auto seq = [](std::initializer_list<std::string_view> strokes) {
        auto parsed = parseKeySequence(strokes);
        if (!parsed) throw std::logic_error{"curated keymap has an invalid stroke"};
        return *parsed;
    };
    KeymapViewState keymap{"default", {}};
    auto bind = [&](KeySequence sequence, std::string command,
                    std::string context) {
        keymap.bindings.push_back(
            {std::move(sequence), std::move(command), std::move(context)});
    };

    bind(seq({"Mod+KeyS"}), "file.save", "*");
    bind(seq({"Mod+KeyN"}), "file.new", "*");
    bind(seq({"Mod+KeyZ"}), "edit.undo", "*");
    bind(seq({"Mod+Shift+KeyZ"}), "edit.redo", "*");
    bind(seq({"Mod+KeyP"}), "file_finder.open", "*");
    bind(seq({"Mod+Shift+KeyP"}), "palette.open", "*");
    bind(seq({"Mod+Shift+KeyF"}), "panel.show_search", "*");
    bind(seq({"Mod+KeyB"}), "panel.toggle", "*");
    bind(seq({"Mod+KeyH"}), "help.open", "*");
    bind(seq({"Mod+KeyO"}), "panel.toggle_focus", "*");
    bind(seq({"Mod+BracketLeft"}), "panel.shrink", "*");
    bind(seq({"Mod+BracketRight"}), "panel.grow", "*");
    bind(seq({"Mod+Period"}), "tab.next", "*");
    bind(seq({"Mod+Comma"}), "tab.previous", "*");
    bind(seq({"Mod+KeyW"}), "tab.close", "*");
    bind(seq({"Mod+Shift+KeyT"}), "settings.open", "*");
    bind(seq({"Mod+KeyA"}), "select.all", "*");
    bind(seq({"Mod+KeyD"}), "select.add_next_occurrence", "*");
    bind(seq({"Mod+KeyI"}), "select.split_into_lines", "*");
    bind(seq({"Mod+KeyK"}), "select.add_cursor_up", "*");
    bind(seq({"Mod+KeyJ"}), "select.add_cursor_down", "*");
    bind(seq({"Mod+Slash"}), "find.open", "*");
    bind(seq({"Mod+Digit8"}), "find.word_under_cursor", "editor");
    bind(seq({"Mod+KeyR"}), "replace.open", "*");

    bind(seq({"Mod+KeyX"}), "clipboard.cut", "editor");
    bind(seq({"Mod+KeyC"}), "clipboard.copy", "editor");
    bind(seq({"Mod+KeyV"}), "clipboard.paste", "editor");
    // Prompts have no selection to cut or copy.
    bind(seq({"Mod+KeyV"}), "clipboard.paste", "prompt");
    bind(seq({"Mod+KeyV"}), "clipboard.paste", "panel");

    bind(seq({"ArrowDown"}), "cursor.line_down", "editor");
    bind(seq({"ArrowUp"}), "cursor.line_up", "editor");
    bind(seq({"ArrowLeft"}), "cursor.left", "editor");
    bind(seq({"ArrowRight"}), "cursor.right", "editor");
    bind(seq({"Shift+ArrowLeft"}), "select.left", "editor");
    bind(seq({"Shift+ArrowRight"}), "select.right", "editor");
    bind(seq({"Shift+ArrowUp"}), "select.line_up", "editor");
    bind(seq({"Shift+ArrowDown"}), "select.line_down", "editor");
    bind(seq({"Home"}), "cursor.line_start", "editor");
    bind(seq({"End"}), "cursor.line_end", "editor");
    bind(seq({"Shift+Home"}), "select.line_start", "editor");
    bind(seq({"Shift+End"}), "select.line_end", "editor");
    bind(seq({"Mod+Home"}), "cursor.document_start", "editor");
    bind(seq({"Mod+End"}), "cursor.document_end", "editor");
    bind(seq({"Mod+Shift+KeyG"}), "goto.line", "editor");
    bind(seq({"Mod+Shift+Home"}), "select.document_start", "editor");
    bind(seq({"Mod+Shift+End"}), "select.document_end", "editor");
    bind(seq({"PageUp"}), "cursor.page_up", "editor");
    bind(seq({"PageDown"}), "cursor.page_down", "editor");
    bind(seq({"Shift+PageUp"}), "select.page_up", "editor");
    bind(seq({"Shift+PageDown"}), "select.page_down", "editor");
    bind(seq({"Enter"}), "text.newline", "editor");
    bind(seq({"Tab"}), "text.tab", "editor");
    bind(seq({"Backspace"}), "text.delete_backward", "editor");
    bind(seq({"Delete"}), "text.delete_forward", "editor");
    bind(seq({"Mod+Backspace"}), "text.delete_word_backward", "editor");
    bind(seq({"Mod+ArrowLeft"}), "cursor.word_left", "editor");
    bind(seq({"Mod+ArrowRight"}), "cursor.word_right", "editor");
    bind(seq({"Mod+Shift+ArrowLeft"}), "select.word_left", "editor");
    bind(seq({"Mod+Shift+ArrowRight"}), "select.word_right", "editor");

    bind(seq({"ArrowDown"}), "tree.select_next", "panel");
    bind(seq({"ArrowUp"}), "tree.select_previous", "panel");
    bind(seq({"Enter"}), "tree.activate", "panel");

    bind(seq({"Enter"}), "prompt.submit", "prompt");
    bind(seq({"Escape"}), "prompt.cancel", "prompt");
    bind(seq({"ArrowDown"}), "prompt.next", "prompt");
    bind(seq({"ArrowUp"}), "prompt.previous", "prompt");
    bind(seq({"Tab"}), "prompt.focus_next_control", "prompt");
    bind(seq({"Mod+KeyE"}), "external.focus", "*");
    bind(seq({"ArrowDown"}), "external.select_next", "external");
    bind(seq({"ArrowUp"}), "external.select_previous", "external");
    bind(seq({"Enter"}), "external.reload", "external");
    bind(seq({"KeyK"}), "external.keep_buffer", "external");
    bind(seq({"KeyD"}), "external.open_diff", "external");
    bind(seq({"Escape"}), "external.focus_return", "external");

    return keymap;
}

namespace {

DocumentPosition zeroPosition() {
    return {ByteOffset{0}, LineIndex{0}, CellIndex{0}};
}

SelectionViewState initialSelection() {
    auto zero = zeroPosition();
    return {SelectionSet{std::vector<Selection>{Selection{zero, zero}}}, 0, 0, std::nullopt};
}

std::filesystem::path canonicalDirectory(std::filesystem::path const& path) {
    std::error_code code;
    auto canonical = canonicalPath(path, code);
    const auto status = code ? std::optional<FileStat>{} : statFile(canonical);
    if (code || !status || status->kind != FileKind::Directory) {
        throw std::invalid_argument{"workspace root must be an existing directory"};
    }
    return canonical;
}

PromptRoutingState inputPromptState(Editor const& editor) {
    PromptRoutingState routing;
    routing.focus = editor.screen.effectiveFocus();
    auto const prompt = editor.promptView();
    if (prompt.activeKind == PromptKind::Palette) {
        routing.prompt = ActivePrompt::Palette;
        return routing;
    }
    auto const controls = editor.resolvedPromptControls();
    if (!controls) return routing;

    switch (editor.screen.prompt().request()->kind) {
    case PromptKind::Find:
        routing.prompt = ActivePrompt::Find;
        break;
    case PromptKind::Replace:
        routing.prompt = ActivePrompt::Replace;
        break;
    case PromptKind::Path:
    case PromptKind::Settings:
    case PromptKind::CommandArgument:
        routing.prompt = ActivePrompt::TextPrompt;
        break;
    case PromptKind::Palette:
        break;
    }
    routing.activeInput = controls->activeInput;
    std::size_t inputIndex = 0;
    for (auto const& control : controls->controls) {
        if (control.kind != PromptControlKind::Input) continue;
        if (inputIndex++ == controls->activeInput) {
            routing.current = PromptEditState{
                control.value, control.cursor.value_or(control.value.size())};
            break;
        }
    }
    return routing;
}

InputRoutingSnapshot inputRoutingSnapshot(Editor& editor) {
    auto const* document = editor.activeDocument();
    auto const* tab = editor.activeTabState();
    const auto treeProvider = editor.tree.activeProviderBinding();
    const auto searchState =
        treeProvider ? editor.tree.searchState(treeProvider->id) : std::nullopt;
    return {
        .keymap = std::cref(editor.resolveInputKeymap()),
        .prompt = inputPromptState(editor),
        .clipboardText = editor.clipboard.plainText(),
        .activeText = editor.activeText(),
        .activeDocument = editor.activeDocumentId(),
        .documentRevision = document ? document->revision() : 0,
        .activeTab = tab ? std::optional{tab->id} : std::nullopt,
        .activeTabKind = tab ? std::optional{tab->kind} : std::nullopt,
        .diffRevision = editor.diff.viewState().revision,
        .selections = std::cref(editor.selection.selections),
        .followMode = editor.follow.viewState().mode,
        .panes = editor.paneTopology.panes(),
        .gesture = editor.documentPointerGesture,
        .activeTreeProvider =
            treeProvider
                ? std::optional<TreeProviderKind>{treeProvider->kind}
                : std::nullopt,
        .searchEditing = searchState && searchState->editing,
    };
}

void applyGesture(Editor& editor,
                  std::optional<DocumentPointerGesture> gesture) {
    if (!gesture) return;
    editor.documentPointerGesture = std::move(*gesture);
}

std::optional<std::string> applyInputMutation(
    Editor& editor, std::optional<EditorMutation> mutation) {
    if (!mutation) return std::nullopt;
    auto messageIfRejected = [](OperationResult result)
        -> std::optional<std::string> {
        if (result.accepted) return std::nullopt;
        return std::move(result.message);
    };
    if (auto* text = std::get_if<ApplyTextInput>(&*mutation)) {
        return messageIfRejected(
            applyEditorTextInput(editor, text->command,
                                 std::move(text->arguments)));
    }
    if (auto* selections = std::get_if<ApplySelections>(&*mutation)) {
        return messageIfRejected(
            applyEditorSelections(editor, std::move(*selections)));
    }
    if (auto* tab = std::get_if<ActivateTab>(&*mutation)) {
        return messageIfRejected(activateTab(editor, tab->tabId));
    }
    if (auto* tab = std::get_if<CloseTab>(&*mutation)) {
        return messageIfRejected(closeTabById(editor, tab->tabId));
    }
    if (auto* pane = std::get_if<FocusPane>(&*mutation)) {
        if (!editor.focusPane(pane->pane)) {
            return "editor pane focus target changed before execution";
        }
        if (editor.screen.effectiveFocus() != FocusTarget::Editor) {
            editor.screen.focusEditor();
        }
        editor.recordNavigation(NavigationClass::User);
        return std::nullopt;
    }
    if (auto* args = std::get_if<PromptValueArguments>(&*mutation)) {
        auto result = editor.screen.prompt().updateValue(args->index, args->value);
        if (!result.accepted()) return result.error->message;
        return std::nullopt;
    }
    if (auto* update = std::get_if<UpdateFindQuery>(&*mutation)) {
        auto result = applyFindQuery(editor, update->query);
        if (!result.accepted()) return result.message;
        return std::nullopt;
    }
    if (auto* update = std::get_if<UpdateReplacement>(&*mutation)) {
        auto result = applyReplacement(editor, update->replacement);
        if (!result.accepted()) return result.message;
        return std::nullopt;
    }

    auto result =
        editor.applySearchQueryChange(std::get<SearchQueryChange>(*mutation));
    if (!result.accepted) return std::move(result.message);
    return std::nullopt;
}

ClientInputResult executeInputRoute(Editor&, RouteUnhandled,
                                    RoutedInput const&) {
    return {ClientInputOutcome::Unhandled, std::nullopt, std::nullopt,
            std::nullopt};
}

ClientInputResult executeInputRoute(Editor& editor, RouteRejected route,
                                    RoutedInput const& routed) {
    if (routed.clearGestureOnRejection) {
        editor.documentPointerGesture.clear();
    }
    return {
        ClientInputOutcome::Rejected, std::nullopt,
        CommandResult{CommandError::HandlerFailed, std::move(route.message), {}},
        std::nullopt};
}

ClientInputResult executeInputRoute(Editor& editor, RouteAccepted route,
                                    RoutedInput routed) {
    auto error = applyInputMutation(editor, std::move(route.mutation));
    if (error) {
        if (routed.clearGestureOnRejection) {
            editor.documentPointerGesture.clear();
        }
        return {
            ClientInputOutcome::Rejected, std::nullopt,
            CommandResult{CommandError::HandlerFailed, std::move(*error), {}},
            std::nullopt};
    }
    applyGesture(editor, std::move(routed.gestureOnAccepted));
    return {ClientInputOutcome::Dispatched, std::nullopt,
            CommandResult{CommandError::None, {}, {}}, std::nullopt};
}

ClientInputResult executeInputRoute(Editor& editor, RouteClientOwned route,
                                    RoutedInput routed) {
    applyGesture(editor, std::move(routed.gestureOnAccepted));
    return {ClientInputOutcome::ClientOwned, std::move(route.input),
            std::nullopt, std::nullopt};
}

ClientInputResult executeInputRoute(Editor& editor, RouteViewAction route,
                                    RoutedInput routed) {
    applyGesture(editor, std::move(routed.gestureOnAccepted));
    return {
        ClientInputOutcome::ViewOwned, std::nullopt,
        CommandResult{CommandError::None, {}, std::move(route.action)},
        std::nullopt};
}

ClientInputResult executeInputRoute(Editor& editor, RouteDispatch route,
                                    RoutedInput routed) {
    auto result = editor.dispatchLocked(route.commandId);
    if (result.accepted()) {
        applyGesture(editor, std::move(routed.gestureOnAccepted));
    } else if (routed.clearGestureOnRejection) {
        editor.documentPointerGesture.clear();
    }
    auto const activation =
        result.accepted() ? editor.screen.openPickerActivation() : std::nullopt;
    auto const outcome =
        !result.accepted() ? ClientInputOutcome::Rejected
        : result.viewAction ? ClientInputOutcome::ViewOwned
                            : ClientInputOutcome::Dispatched;
    return {outcome, std::nullopt, std::move(result), activation};
}

ClientInputResult executeInputRoute(Editor& editor, InvokeExternalAction route,
                                    RoutedInput routed) {
    auto result = invokeExternalAction(editor, route.invocation);
    if (!result.accepted) {
        if (routed.clearGestureOnRejection) {
            editor.documentPointerGesture.clear();
        }
        return {ClientInputOutcome::Rejected, std::nullopt,
                CommandResult{CommandError::HandlerFailed,
                              std::move(result.message), {}},
                std::nullopt};
    }
    applyGesture(editor, std::move(routed.gestureOnAccepted));
    auto const activation = editor.screen.openPickerActivation();
    const auto outcome =
        result.viewAction ? ClientInputOutcome::ViewOwned
                          : ClientInputOutcome::Dispatched;
    return {outcome, std::nullopt,
            CommandResult{CommandError::None, {}, std::move(result.viewAction)},
            activation};
}

ClientInputResult executeInputRoute(Editor& editor, ActivateUiNode route,
                                    RoutedInput routed) {
    auto result = applyUiNodeActivation(editor, route.nodeId);
    if (!result.accepted) {
        return {ClientInputOutcome::Rejected, std::nullopt,
                CommandResult{CommandError::HandlerFailed,
                              std::move(result.message), {}},
                std::nullopt};
    }
    applyGesture(editor, std::move(routed.gestureOnAccepted));
    auto const activation = editor.screen.openPickerActivation();
    const auto outcome =
        result.viewAction ? ClientInputOutcome::ViewOwned
                          : ClientInputOutcome::Dispatched;
    return {outcome, std::nullopt,
            CommandResult{CommandError::None, {}, std::move(result.viewAction)},
            activation};
}

ClientInputResult executeInputRoute(Editor& editor, ActivateTreeNode route,
                                    RoutedInput routed) {
    auto result = activateTreeNode(editor, route.nodeId);
    if (!result.accepted) {
        if (routed.clearGestureOnRejection) {
            editor.documentPointerGesture.clear();
        }
        return {ClientInputOutcome::Rejected, std::nullopt,
                CommandResult{CommandError::HandlerFailed,
                              std::move(result.message), {}},
                std::nullopt};
    }
    applyGesture(editor, std::move(routed.gestureOnAccepted));
    auto const activation = editor.screen.openPickerActivation();
    const auto outcome =
        result.viewAction ? ClientInputOutcome::ViewOwned
                          : ClientInputOutcome::Dispatched;
    return {outcome, std::nullopt,
            CommandResult{CommandError::None, {}, std::move(result.viewAction)},
            activation};
}

ClientInputResult executeInputRoute(Editor& editor, SubmitPicker route,
                                    RoutedInput) {
    auto const palette = editor.paletteView();
    const auto* candidates = palette.candidatesFor(route.activation.mode);
    if (candidates == nullptr) {
        return {ClientInputOutcome::Rejected, std::nullopt,
                CommandResult{CommandError::HandlerFailed,
                              "picker mode has no candidate inventory", {}},
                std::nullopt};
    }
    auto const published =
        std::find_if(candidates->begin(), candidates->end(),
                     [&](auto const& c) { return c.id == route.candidateId; });
    if (published == candidates->end()) {
        return {ClientInputOutcome::Rejected, std::nullopt,
                CommandResult{CommandError::HandlerFailed,
                              "candidate is not in the picker inventory", {}},
                std::nullopt};
    }
    if (editor.screen.openPickerActivation() != route.activation) {
        return {ClientInputOutcome::Rejected, std::nullopt,
                CommandResult{CommandError::HandlerFailed,
                              "SubmitPicker requires a matching open picker",
                              {}},
                std::nullopt};
    }

    if (route.activation.mode == SearchMode::Command) {
        auto result = editor.dispatchLocked(route.candidateId);
        if (!result.accepted()) {
            return {ClientInputOutcome::Rejected, std::nullopt,
                    CommandResult{result.error, std::move(result.message), {}},
                    std::nullopt};
        }
        if (editor.screen.openPickerActivation() == route.activation) {
            (void)editor.screen.closeFinder();
        }
        auto const activation = editor.screen.openPickerActivation();
        const auto outcome =
            result.viewAction ? ClientInputOutcome::ViewOwned
                              : ClientInputOutcome::Dispatched;
        return {outcome, std::nullopt, std::move(result), activation};
    }
    if (route.activation.mode == SearchMode::File) {
        auto result = applyFilePathCompletion(editor, PromptCompletion::FileOpen,
                                              route.candidateId);
        if (!result.accepted) {
            return {ClientInputOutcome::Rejected, std::nullopt,
                    CommandResult{CommandError::HandlerFailed,
                                  std::move(result.message), {}},
                    std::nullopt};
        }
        if (editor.screen.openPickerActivation() == route.activation) {
            (void)editor.screen.closeFinder();
        }
        const auto outcome =
            result.viewAction ? ClientInputOutcome::ViewOwned
                              : ClientInputOutcome::Dispatched;
        return {outcome, std::nullopt,
                CommandResult{CommandError::None, {},
                              std::move(result.viewAction)},
                editor.screen.openPickerActivation()};
    }
    return {ClientInputOutcome::Rejected, std::nullopt,
            CommandResult{CommandError::HandlerFailed,
                          "open picker has no submit action", {}},
            std::nullopt};
}

} // namespace

OperationResult success() { return {}; }
OperationResult failure(std::string message) {
    return {false, std::move(message), std::nullopt};
}

OperationResult Editor::applySearchQueryChange(SearchQueryChange change) {
    const auto binding = tree.activeProviderBinding();
    if (!binding || binding->kind != TreeProviderKind::Search) {
        return failure("search query target changed before execution");
    }
    auto state = tree.searchState(binding->id);
    if (!state) return failure("search provider state is unavailable");
    switch (change.kind) {
    case SearchQueryChange::Kind::Edit:
        state->query = applyPromptTextEdit(state->query, change.edit);
        state->editing = true;
        break;
    case SearchQueryChange::Kind::MoveFirst:
    case SearchQueryChange::Kind::MoveLast: {
        const auto view = tree.viewState();
        const auto* provider = activeTreeProvider(view);
        if (provider == nullptr || provider->nodes.empty()) {
            return success();
        }
        const auto& node =
            change.kind == SearchQueryChange::Kind::MoveFirst
                ? provider->nodes.front()
                : provider->nodes.back();
        if (!tree.select(node.node.id)) {
            return failure("search result target changed before execution");
        }
        state->editing = false;
        break;
    }
    case SearchQueryChange::Kind::Submit:
        cancelWorkspaceSearch();
        state->submittedQuery.reset();
        state->searching = false;
        if (!state->query.text().empty()) {
            state->submittedQuery = state->query.text();
            state->searching = true;
            const auto sourceGeneration = ++workspaceSearchGeneration;
            startWorkspaceSearch(
                ParsedSearchQuery{.mode = SearchMode::Text,
                                  .text = state->query.text()},
                sourceGeneration);
        }
        break;
    case SearchQueryChange::Kind::Focus:
        if (!screen.focusPanel()) {
            return failure("search query sidebar is unavailable");
        }
        state->editing = true;
        break;
    }
    if (!tree.setSearchState(binding->id, std::move(*state))) {
        return failure("search provider state changed before execution");
    }
    return success();
}

std::string workspaceMessage(WorkspaceResult const& result) {
    return result.message.empty() ? "workspace operation failed" : result.message;
}

std::string tabMessage(TabResult const& result) {
    return result.message.empty() ? "tab operation failed" : result.message;
}

Editor::Editor(std::filesystem::path canonicalCwd,
               std::filesystem::path recoveryRoot,
               std::filesystem::path archiveRoot,
               std::filesystem::path snapshotPath,
               std::string snapshotIdentity,
               bool deferEnrichment,
               std::shared_ptr<SyntaxParser> parser,
               bool enableGitDiffWorker,
               bool enableFilesystemWatcher)
    : root{std::move(canonicalCwd)},
      workspaceIgnore{makePlatformGitIgnoreMatcher(root)},
      recoveryRoot{weaklyCanonicalPath(recoveryRoot)},
      archiveRoot{weaklyCanonicalPath(archiveRoot)},
      snapshotPath{std::move(snapshotPath)},
      snapshotIdentity{std::move(snapshotIdentity)},
      recovery{RecoveryManager::create(recoveryRoot)},
      workspace{Workspace::create(root, recovery, this->archiveRoot)},
      selection{initialSelection()}, clipboard{4}, tabs{},
      external{workspace, diff}, syntaxParser{std::move(parser)},
      syntaxWorker{syntaxParser},
      screen{assembleScreen("help.open", StyleDimensions{},
                            Style{}.inputLineSigil),
             tree},
      theme{defaultTheme()}, deferringEnrichment{deferEnrichment},
      gitDiffIngress{root, enableGitDiffWorker, enableFilesystemWatcher} {
    homeDirectory = resolveHomeDirectory();
    workspace.setSaveObserver(
        [this](const std::filesystem::path& relativePath) {
            external.registerSaveExpectation(relativePath);
    });
    (void)refreshTree();
    refreshSyntax();
}

Editor::~Editor() = default;

const PlatformWake* Editor::gitDiffWake() const noexcept {
    return gitDiffIngress.wake();
}

const PlatformWake* Editor::syntaxWake() const noexcept {
    return syntaxWorker.wake();
}


TabLifecycleResult Editor::closeTab(
    const TabState& tab, std::span<const TabId> alreadyClosed) {
    if (!tab.document) {
        if (tab.kind == TabKind::ReadOnlyOutput) {
            // Read-only output tabs have no recovery record.
            const auto mapped = readOnlyTabDocuments.find(tab.contentIdentity);
            if (mapped != readOnlyTabDocuments.end()) {
                const auto document = mapped->second;
                auto removed = workspace.removeDocument(document);
                if (!removed.accepted()) {
                    return {TabError::LifecycleFailed,
                            workspaceMessage(removed), std::nullopt,
                            std::nullopt, std::nullopt, false};
                }
                discardDocumentRuntimeState(document);
            }
            return {TabError::None, {}, std::nullopt, std::nullopt, std::nullopt,
                    true};
        }
        if (tab.kind == TabKind::LiveDiff) {
            const auto mapped = liveDiffDocuments.find(tab.contentIdentity);
            if (mapped != liveDiffDocuments.end()) {
                const auto document = mapped->second;
                const bool stillReferenced = std::any_of(
                    tabs.viewState().tabs.begin(), tabs.viewState().tabs.end(),
                    [&](const TabState& candidate) {
                        return candidate.id != tab.id &&
                               std::find(alreadyClosed.begin(),
                                         alreadyClosed.end(),
                                         candidate.id) == alreadyClosed.end() &&
                               candidate.document == document;
                    });
                if (!stillReferenced) {
                    auto state = workspace.state(document);
                    if (!state) {
                        return {TabError::NotFound,
                                "live diff document state does not exist",
                                std::nullopt, std::nullopt, std::nullopt, false};
                    }
                    std::optional<ClosedDocumentSnapshot> snapshot;
                    if (auto const* current = workspace.tryDocument(document);
                        current != nullptr) {
                        snapshot = ClosedDocumentSnapshot{
                            state->key, current->mode(), state->dirty,
                            current->snapshot().text};
                    }
                    auto closed = recovery.closeDocument(snapshot);
                    if (!closed.accepted()) {
                        return {TabError::LifecycleFailed, closed.error->message,
                                std::nullopt, std::nullopt, std::nullopt, false};
                    }
                    auto removed = workspace.removeDocument(document);
                    if (!removed.accepted()) {
                        return {TabError::LifecycleFailed, workspaceMessage(removed),
                                std::nullopt, std::nullopt, std::nullopt, false};
                    }
                    discardDocumentRuntimeState(document);
                    return {TabError::None, {}, closed.compensation, std::nullopt,
                            std::nullopt};
                }
                liveDiffDocuments.erase(mapped);
            }
        }
        return {};
    }
    const auto isAlreadyClosed = [&](TabId id) {
        return std::find(alreadyClosed.begin(), alreadyClosed.end(), id) !=
               alreadyClosed.end();
    };
    const bool sharedByDocumentTab = std::any_of(
        tabs.viewState().tabs.begin(), tabs.viewState().tabs.end(),
        [&](const TabState& candidate) {
            return candidate.id != tab.id && !isAlreadyClosed(candidate.id) &&
                   candidate.document == tab.document;
        });
    const bool sharedByLiveDiffTab = std::any_of(
        tabs.viewState().tabs.begin(), tabs.viewState().tabs.end(),
        [&](const TabState& candidate) {
            if (candidate.id == tab.id || candidate.kind != TabKind::LiveDiff ||
                isAlreadyClosed(candidate.id)) {
                return false;
            }
            const auto mapped = liveDiffDocuments.find(candidate.contentIdentity);
            return mapped != liveDiffDocuments.end() &&
                   mapped->second == *tab.document;
        });
    if (sharedByDocumentTab || sharedByLiveDiffTab) {
        return {};
    }
    auto state = workspace.state(*tab.document);
    if (!state) return {TabError::NotFound, "tab document does not exist",
                        std::nullopt, std::nullopt, std::nullopt, false};
    std::optional<ClosedDocumentSnapshot> document;
    if (auto const* current = workspace.tryDocument(*tab.document); current != nullptr) {
        document = ClosedDocumentSnapshot{
            state->key, current->mode(), state->dirty,
            current->snapshot().text};
    }
    auto closed = recovery.closeDocument(document);
    if (!closed.accepted()) {
        return {TabError::LifecycleFailed, closed.error->message, std::nullopt,
                std::nullopt, std::nullopt, false};
    }
    auto removed = workspace.removeDocument(*tab.document);
    if (!removed.accepted()) {
        return {TabError::LifecycleFailed, workspaceMessage(removed),
                std::nullopt, std::nullopt, std::nullopt, false};
    }
    discardDocumentRuntimeState(*tab.document);
    return {TabError::None,      {},
            closed.compensation, std::nullopt,
            std::nullopt};
}

TabLifecycleResult Editor::reopenTab(
    const TabState& tab, const RecoveryRecordId& compensation) {
    std::optional<ClosedDocumentSnapshot> restoredDocument;
    auto restored = recovery.restoreDocument(compensation, restoredDocument);
    if (!restored.accepted()) {
        return {TabError::LifecycleFailed, restored.error->message, std::nullopt,
                std::nullopt, std::nullopt, false};
    }
    if (!restoredDocument) {
        return {TabError::LifecycleFailed, "recovery record had no document",
                std::nullopt, std::nullopt, std::nullopt, false};
    }

    WorkspaceResult opened;
    if (tab.kind == TabKind::LiveDiff) {
        opened = workspace.openVirtualDocument(
            tab.label, restoredDocument->utf8Content, restoredDocument->mode);
    } else if (restoredDocument->key.kind() == DocumentKeyKind::Saved) {
        opened = workspace.openFile(restoredDocument->key.savedPath());
    } else {
        opened = workspace.newDocument();
    }
    if (!opened.accepted() || !opened.document) {
        return {TabError::LifecycleFailed, workspaceMessage(opened), std::nullopt,
                std::nullopt, std::nullopt, false};
    }
    auto* reopenedDocument = const_cast<Document*>(workspace.tryDocument(*opened.document));
    if (!reopenedDocument) {
        return {TabError::LifecycleFailed,
                "reopened document was not available in workspace",
                std::nullopt, std::nullopt, std::nullopt, false};
    }
    const auto current = reopenedDocument->snapshot();
    if (current.text != restoredDocument->utf8Content) {
        auto replace = workspace.apply(
            *opened.document,
            {current.revision,
             {{ByteOffset{0}, current.text.size(), restoredDocument->utf8Content}}});
        if (!replace.accepted()) {
            return {TabError::LifecycleFailed, replace.message, std::nullopt,
                    std::nullopt, std::nullopt, false};
        }
    }
    ensureDocumentRuntimeState(*opened.document);
    auto reopenedState = workspace.state(*opened.document);
    if (!reopenedState) {
        return {TabError::LifecycleFailed,
                "reopened document state was not available in workspace",
                std::nullopt, std::nullopt, std::nullopt, false};
    }
    if (tab.kind == TabKind::LiveDiff) {
        liveDiffDocuments[tab.contentIdentity] = *opened.document;
    }
    return {TabError::None, {}, std::nullopt, *opened.document,
            reopenedState->key, true};
}

WorkspaceCorpus Editor::workspaceCorpus() const {
    std::vector<WorkspaceCorpusBuffer> buffers;
    buffers.reserve(workspace.documents().size());
    for (auto const id : workspace.documents()) {
        auto state = workspace.state(id);
        if (!state || state->key.kind() != DocumentKeyKind::Saved ||
            state->contentKind != FileContentKind::Text) {
            continue;
        }
        buffers.push_back(
            {std::optional<std::string>{state->key.savedPath()},
             [this, id]() -> std::optional<std::string> {
                 const auto current = workspace.state(id);
                 if (!current ||
                     current->contentKind != FileContentKind::Text) {
                     return std::nullopt;
                 }
                 return workspace.document(id).snapshot().text;
             }});
    }

    WorkspaceCorpusOptions options;
    options.excludedDirectories = {recoveryRoot, archiveRoot};
    return WorkspaceCorpus{root, std::move(buffers), *workspaceIgnore, readFile,
                           std::move(options)};
}

void Editor::startWorkspaceSearch(std::string query,
                                  std::uint64_t sourceRevision) {
    panelWorkspaceSearchGeneration.reset();
    workspaceSearchState =
        search.beginWorkspaceSearch(std::move(query), sourceRevision);
    workspaceSearchCorpus = workspaceCorpus();
}

void Editor::openWorkspaceSearch() {
    startWorkspaceSearch(std::string{}, ++workspaceSearchGeneration);
}

void Editor::startWorkspaceSearch(ParsedSearchQuery query,
                                  std::uint64_t sourceRevision) {
    workspaceSearchState =
        search.beginWorkspaceSearch(std::move(query), sourceRevision);
    panelWorkspaceSearchGeneration =
        workspaceSearchState->request.generation;
    workspaceSearchCorpus = workspaceCorpus();
}

bool Editor::workspaceSearchPending() const noexcept {
    return workspaceSearchState.has_value() &&
           !workspaceSearchState->finished;
}

void Editor::cancelWorkspaceSearch() {
    search.cancelWorkspaceSearch();
    workspaceSearchState.reset();
    workspaceSearchCorpus.reset();
    panelWorkspaceSearchGeneration.reset();
    const TreeProviderId searchProvider{"search"};
    if (auto state = tree.searchState(searchProvider)) {
        state->submittedQuery.reset();
        state->searching = false;
        tree.replaceProvider(TreeProviderSnapshot{
            searchProvider, TreeProviderKind::Search, {}});
        (void)tree.setSearchState(searchProvider, std::move(*state));
    }
}

void Editor::advanceWorkspaceSearch() {
    if (!workspaceSearchCorpus || !workspaceSearchState) return;
    auto batch = evaluateWorkspaceSearch(
        *workspaceSearchCorpus, *workspaceSearchState,
        kDefaultFindWorkBudget);
    if (!batch.finished) return;
    const auto publishResult = search.publish(
        batch, workspaceSearchState->request.sourceRevision);
    const bool panelSearch =
        panelWorkspaceSearchGeneration &&
        *panelWorkspaceSearchGeneration == batch.generation;
    if (panelSearch && publishResult == SearchPublishResult::Accepted) {
        const TreeProviderId searchProvider{"search"};
        auto panelState = tree.searchState(searchProvider);
        if (panelState && panelState->submittedQuery) {
            std::vector<SearchTreeRecord> records;
            records.reserve(batch.results.size());
            for (const auto& result : batch.results) {
                if (!result.line) continue;
                records.push_back(
                    {.workspacePath = result.path,
                     .label = result.label,
                     .sourceLine = result.line->value(),
                     .sourceColumn = result.column});
            }
            tree.replaceProvider(TreeProviderSnapshot::fromSearch(
                searchProvider, std::move(records)));
            panelState->searching = false;
            (void)tree.setSearchState(searchProvider,
                                     std::move(*panelState));
        }
    } else if (panelSearch) {
        const TreeProviderId searchProvider{"search"};
        if (auto panelState = tree.searchState(searchProvider)) {
            panelState->submittedQuery.reset();
            panelState->searching = false;
            (void)tree.setSearchState(searchProvider,
                                     std::move(*panelState));
        }
    }
    workspaceSearchState.reset();
    workspaceSearchCorpus.reset();
    panelWorkspaceSearchGeneration.reset();
}

std::optional<FileDocumentId> Editor::activeDocumentId() const {
    auto const& view = tabs.viewState();
    if (!view.active) return std::nullopt;
    auto found = std::find_if(view.tabs.begin(), view.tabs.end(), [&](TabState const& tab) {
        return tab.id == *view.active;
    });
    if (found == view.tabs.end()) return std::nullopt;
    if (found->document) return found->document;
    if (found->kind == TabKind::LiveDiff) {
        const auto mapped = liveDiffDocuments.find(found->contentIdentity);
        if (mapped != liveDiffDocuments.end()) {
            return mapped->second;
        }
    }
    if (found->kind == TabKind::ReadOnlyOutput) {
        const auto mapped = readOnlyTabDocuments.find(found->contentIdentity);
        if (mapped != readOnlyTabDocuments.end()) {
            return mapped->second;
        }
    }
    return std::nullopt;
}

const TabState* Editor::activeTabState() const {
    auto const& view = tabs.viewState();
    if (!view.active) return nullptr;
    auto found = std::find_if(view.tabs.begin(), view.tabs.end(),
                              [&](const TabState& tab) {
                                  return tab.id == *view.active;
                              });
    if (found == view.tabs.end()) return nullptr;
    return &*found;
}

OperationResult Editor::openOrFocusLiveDiffTab(
    const DiffFileView& file, NavigationClass classification) {
    const auto target = diffOpenFile(file);
    const auto diffText = file.currentContent;
    std::optional<FileDocumentId> document;
    auto mapped = liveDiffDocuments.find(target.id.value());
    if (mapped != liveDiffDocuments.end()) {
        if (const auto* opened = workspace.tryDocument(mapped->second);
            opened != nullptr &&
            opened->snapshot().text == diffText) {
            document = mapped->second;
        } else {
            auto removed = workspace.removeDocument(mapped->second);
            if (!removed.accepted()) {
                return failure(workspaceMessage(removed));
            }
            discardDocumentRuntimeState(mapped->second);
        }
    }
    if (!document) {
        auto created = workspace.openVirtualDocument(
            liveDiffTabLabelForPath(target.path), diffText,
            DocumentMode::Diff);
        if (!created.accepted() || !created.document) {
            return failure(workspaceMessage(created));
        }
        document = *created.document;
    }

    ensureDocumentRuntimeState(*document);
    liveDiffDocuments[target.id.value()] = *document;
    auto opened = tabs.openContent(TabKind::LiveDiff, target.id.value(),
                                   liveDiffTabLabelForPath(target.path),
                                   DocumentMode::Diff);
    if (!opened.accepted()) {
        return failure(tabMessage(opened));
    }
    if (classification == NavigationClass::User) {
        recordNavigation(classification);
    }
    screen.focusEditor();
    return success();
}

OperationResult Editor::openReadOnlyTab(
    TabKind kind, std::string contentIdentity, std::string label,
    std::string text, LanguageId language) {
    auto replacement =
        workspace.openVirtualDocument(label, text, DocumentMode::ReadOnly);
    if (!replacement.accepted() || !replacement.document) {
        return failure(workspaceMessage(replacement));
    }
    auto mapped = readOnlyTabDocuments.find(contentIdentity);
    if (mapped != readOnlyTabDocuments.end()) {
        const auto previous = mapped->second;
        auto removed = workspace.removeDocument(previous);
        if (!removed.accepted()) {
            (void)workspace.removeDocument(*replacement.document);
            return failure(workspaceMessage(removed));
        }
        discardDocumentRuntimeState(previous);
    }
    ensureDocumentRuntimeState(*replacement.document);
    documentLanguageOverrides.insert_or_assign(replacement.document->value(),
                                                std::move(language));
    readOnlyTabDocuments[contentIdentity] = *replacement.document;
    auto opened =
        tabs.openContent(kind, contentIdentity, label, DocumentMode::ReadOnly);
    if (!opened.accepted()) {
        return failure(tabMessage(opened));
    }
    screen.focusEditor();
    refreshSyntax();
    return success();
}

bool Editor::openOrRevealFollowTargetProgrammatic(const FollowTarget& target) {
    const auto file = diff.file(target.id);
    if (!file.has_value()) {
        return false;
    }
    if (!openOrFocusLiveDiffTab(file->get(), NavigationClass::Programmatic)
             .accepted) {
        return false;
    }
    if (target.deleted) {
        return true;
    }
    return revealCurrentDiffTarget(target);
}

bool Editor::revealCurrentDiffTarget(const FollowTarget& target) {
    const auto& text = activeText();
    const auto offset = lineStartOffset(text, target.newestHunkLine);
    const auto position = resolveSelectionPosition(text, ByteOffset{offset});
    if (!position) {
        return false;
    }
    selection.selections =
        SelectionSet{std::vector<Selection>{Selection{*position, *position}}};
    screen.focusEditor();
    return true;
}

void Editor::refreshLiveDiffDocuments(const DiffViewState& diffView) {
    const std::vector<std::pair<std::string, FileDocumentId>> trackedDocuments(
        liveDiffDocuments.begin(), liveDiffDocuments.end());
    for (const auto& [identity, document] : trackedDocuments) {
        const auto mapped = liveDiffDocuments.find(identity);
        if (mapped == liveDiffDocuments.end() || mapped->second != document) {
            continue;
        }
        const auto id = DiffFileId{identity};
        auto file = std::find_if(
            diffView.files.begin(), diffView.files.end(),
            [&](const DiffFileView& candidate) { return candidate.id == id; });
        const auto desired =
            file == diffView.files.end() ? std::string{} : file->currentContent;
        const auto* opened = workspace.tryDocument(document);
        if (opened == nullptr) {
            discardDocumentRuntimeState(document);
            continue;
        }
        if (opened->snapshot().text == desired) {
            continue;
        }
        auto state = workspace.state(document);
        const auto label =
            state ? state->displayLabel : std::string{"LiveDiff"};
        auto replacement = workspace.openVirtualDocument(
            label, desired, DocumentMode::Diff);
        if (!replacement.accepted() || !replacement.document) {
            continue;
        }
        auto removed = workspace.removeDocument(document);
        if (!removed.accepted()) {
            (void)workspace.removeDocument(*replacement.document);
            continue;
        }
        discardDocumentRuntimeState(document);
        ensureDocumentRuntimeState(*replacement.document);
        liveDiffDocuments[identity] = *replacement.document;
    }
}

Document const* Editor::activeDocument() const {
    auto id = activeDocumentId();
    return id ? workspace.tryDocument(*id) : nullptr;
}

Document* Editor::activeDocument() {
    auto id = activeDocumentId();
    return id ? const_cast<Document*>(workspace.tryDocument(*id)) : nullptr;
}

void Editor::ensureDocumentRuntimeState(FileDocumentId document) {
    documentRuntimeStates.try_emplace(
        document.value(),
        DocumentRuntimeState{settings, syntaxParser});
}

void Editor::discardDocumentRuntimeState(FileDocumentId document) {
    syntaxWorker.cancel(document);
    documentRuntimeStates.erase(document.value());
    documentLanguageOverrides.erase(document.value());
    if (findDocumentId == document) findDocumentId.reset();
    for (auto it = liveDiffDocuments.begin(); it != liveDiffDocuments.end();) {
        it = it->second == document ? liveDiffDocuments.erase(it)
                                    : std::next(it);
    }
    for (auto it = readOnlyTabDocuments.begin();
         it != readOnlyTabDocuments.end();) {
        it = it->second == document ? readOnlyTabDocuments.erase(it)
                                    : std::next(it);
    }
}

DocumentHistory& Editor::historyFor(FileDocumentId document) {
    auto it = documentRuntimeStates.find(document.value());
    if (it == documentRuntimeStates.end()) {
        throw std::logic_error{"document history was requested before document "
                               "runtime state existed"};
    }
    return it->second.history;
}

SyntaxModel& Editor::syntaxFor(FileDocumentId document) {
    auto it = documentRuntimeStates.find(document.value());
    if (it == documentRuntimeStates.end()) {
        throw std::logic_error{"document syntax was requested before document "
                               "runtime state existed"};
    }
    return it->second.syntax;
}

std::shared_ptr<const SyntaxViewState> Editor::activeSyntaxView() const {
    if (auto id = activeDocumentId()) {
        if (auto it = documentRuntimeStates.find(id->value());
            it != documentRuntimeStates.end()) {
            return it->second.syntax.sharedViewState();
        }
    }
    const auto* document = activeDocument();
    const auto text = document ? document->snapshot().text : std::string{};
    const auto revision = document ? document->revision() : std::uint64_t{0};
    return std::make_shared<const SyntaxViewState>(
        SyntaxViewState::plainText(
            revision, LanguageId::plainText(), text, 4));
}

LanguageId Editor::languageFor(FileDocumentId document) const {
    if (auto override = documentLanguageOverrides.find(document.value());
        override != documentLanguageOverrides.end()) {
        return override->second;
    }
    if (auto state = workspace.state(document);
        state && state->key.kind() == DocumentKeyKind::Saved) {
        return LanguageId::fromPath(state->key.savedPath());
    }
    return LanguageId::plainText();
}

LanguageId Editor::activeSyntaxLanguage() const {
    auto document = activeDocumentId();
    return document ? languageFor(*document) : LanguageId::plainText();
}

std::optional<WorkspaceDocumentState> Editor::activeWorkspaceState() const {
    auto id = activeDocumentId();
    return id ? workspace.state(*id) : std::nullopt;
}

std::optional<DiffFileView> Editor::activeDiffFile() const {
    const auto diffState = diff.viewState();
    std::optional<std::string> identity;
    if (const auto* tab = activeTabState();
        tab && tab->kind == TabKind::LiveDiff &&
        !tab->contentIdentity.empty()) {
        identity = tab->contentIdentity;
    }
    const auto file = diffState.fileForIdentity(identity);
    return file ? std::optional<DiffFileView>{file->get()} : std::nullopt;
}

std::string const& Editor::activeText() const {
    static const std::string empty;
    const auto documentId = activeDocumentId();
    auto const* document = activeDocument();
    if (!documentId || document == nullptr) return empty;
    const auto revision = document->revision();
    if (activeTextDocument != documentId || activeTextRevision != revision) {
        activeTextCache = document->snapshot().text;
        activeTextDocument = documentId;
        activeTextRevision = revision;
    }
    return activeTextCache;
}

void Editor::resetSelectionForActiveDocument() {
    selection = initialSelection();
}

void Editor::clampSelectionToActiveDocument() {
    auto const& text = activeText();
    auto offset = selection.selections.primary().active.byteOffset.value();
    if (offset > text.size()) offset = text.size();
    auto position = ssg::resolveSelectionPosition(text, ByteOffset{offset}).value_or(zeroPosition());
    selection.selections = SelectionSet{std::vector<Selection>{Selection{position, position}}};
}

void Editor::clampSelectionsToActiveDocument() {
    auto const& text = activeText();
    auto snapDownToGraphemeBoundary = [&](DocumentPosition const& position) {
        auto offset = position.byteOffset.value();
        if (offset > text.size()) offset = text.size();
        for (;;) {
            if (auto at = ssg::resolveSelectionPosition(
                    text, ByteOffset{offset})) {
                return *at;
            }
            if (offset == 0) break;
            --offset;
        }
        return zeroPosition();
    };
    std::vector<Selection> clamped;
    clamped.reserve(selection.selections.items().size());
    for (auto const& sel : selection.selections.items()) {
        clamped.push_back(Selection{
            snapDownToGraphemeBoundary(sel.anchor),
            snapDownToGraphemeBoundary(sel.active)});
    }
    if (clamped.empty()) {
        clamped.push_back(Selection{zeroPosition(), zeroPosition()});
    }
    selection.selections = SelectionSet{std::move(clamped)};
}

bool Editor::refreshTree() {
    if (deferringEnrichment) {
        pendingTreeRefresh = true;
        return false;
    }
    tree.replaceProvider(TreeProviderSnapshot::fromFilesystemDirectories(
        TreeProviderId{"filesystem"}, root, loadedFilesystemDirectories));
    if (screen.openPicker() == PickerKind::File) rebuildFileCandidates();
    return true;
}

void Editor::refreshTreeForPublication() {
    (void)refreshTree();
}

OperationResult Editor::toggleTreeExpanded(
    const TreeProviderId& providerId, const TreeNodeId& nodeId) {
    const bool expanding = !tree.isExpanded(providerId, nodeId);
    if (!tree.toggleExpanded(providerId, nodeId)) {
        return failure("tree node is not expandable");
    }
    if (!expanding || providerId != TreeProviderId{"filesystem"}) {
        return success();
    }

    const auto loadedBefore = loadedFilesystemDirectories;
    try {
        const auto view = tree.viewState();
        const auto provider = std::ranges::find(
            view.providers, providerId,
            [](const TreeProviderView& candidate) {
                return candidate.providerId;
            });
        if (provider != view.providers.end()) {
            for (const auto& node : provider->nodes) {
                if (node.node.kind == TreeNodeKind::Directory &&
                    node.node.parentId == nodeId &&
                    node.node.workspacePath) {
                    loadedFilesystemDirectories.push_back(
                        *node.node.workspacePath);
                }
            }
            std::ranges::sort(loadedFilesystemDirectories);
            const auto unique = std::ranges::unique(
                loadedFilesystemDirectories);
            loadedFilesystemDirectories.erase(unique.begin(),
                                               unique.end());
        }
        (void)refreshTree();
        return success();
    } catch (const std::exception& exception) {
        loadedFilesystemDirectories = loadedBefore;
        (void)tree.toggleExpanded(providerId, nodeId);
        return failure("failed to expand filesystem tree: " +
                       std::string{exception.what()});
    }
}

void Editor::rebuildInteractionSchema(
    const StyleDimensions& dimensions,
    std::string_view promptSigil) {
    (void)screen.updateComposition(
        assembleScreen("help.open", dimensions, promptSigil));
}

bool Editor::openPickerPrompt(PickerKind kind) {
    if (kind == PickerKind::File) rebuildFileCandidates();
    return screen.openFinder(kind);
}

// Main-thread workspace walks share one matcher. The git-diff worker owns a
// separate repository handle because libgit2 handles are not cross-thread safe.
void Editor::rebuildFileCandidates() {
    workspaceIgnore = makePlatformGitIgnoreMatcher(root);
    WorkspaceFileIndexOptions options;
    options.respectGitignore =
        boolSetting(settings, SettingKey::FileFinderRespectGitignore, true);
    fileCandidates = std::move(
        buildWorkspaceFileIndex(root, *workspaceIgnore, options).candidates);
}

void Editor::reconcileFindDocument() {
    if (!findReplace.viewState().open) {
        findDocumentId.reset();
        return;
    }
    auto const active = activeDocumentId();
    auto const* document = activeDocument();
    bool const stale =
        !active || active != findDocumentId || document == nullptr ||
        document->revision() != findReplace.viewState().sourceRevision;
    if (!stale) return;
    findReplace.close();
    if (auto const& request = screen.prompt().request();
        request && (request->kind == PromptKind::Find ||
                    request->kind == PromptKind::Replace)) {
        (void)screen.prompt().cancel();
    }
    findDocumentId.reset();
}

void Editor::refreshSyntax(std::vector<SyntaxEdit> edits) {
    auto id = activeDocumentId();
    if (!id) return;
    auto& model = syntaxFor(*id);
    auto const* document = activeDocument();
    auto text = document ? document->snapshot().text : std::string{};
    auto revision = document ? document->revision() : std::uint64_t{0};
    auto language = languageFor(*id);
    if (!model.hasParser()) {
        (void)model.parse(revision, std::move(language), std::move(text),
                          std::move(edits));
        return;
    }
    auto prepared = model.request(revision, std::move(language), std::move(text),
                                  std::move(edits));
    if (prepared.accepted()) {
        syntaxWorker.submit({*id, std::move(prepared.request)});
    }
}

void Editor::primeDeferred() {
    std::lock_guard operationLock{operationMutex};
    if (!deferringEnrichment) return;
    deferringEnrichment = false;
    bool ran = false;
    if (pendingTreeRefresh) {
        pendingTreeRefresh = false;
        (void)refreshTree();
        ran = true;
    }
    (void)ran;
}

void Editor::showStatus(std::string text) {
    statusText = std::move(text);
}

DiffIngressResult Editor::applyExternalDiffBurst(
    std::vector<ExternalDiffRevision> changes) {
    std::lock_guard operationLock{operationMutex};
    if (changes.empty()) {
        return {DiffIngressError::EmptyBurst};
    }

    auto stagedDiff = diff;
    std::vector<FollowDiffChange> followChanges;
    followChanges.reserve(changes.size());
    for (auto& change : changes) {
        const auto id = change.event.id;
        std::vector<DiffHunk> priorHunks;
        if (const auto prior = stagedDiff.file(id)) {
            priorHunks = prior->get().hunks;
        }
        const auto applied =
            stagedDiff.applyNonGitEvent(std::move(change.event), change.revision);
        if (!applied.accepted()) {
            return {DiffIngressError::DiffRejected};
        }
        const auto changedFile = stagedDiff.file(id);
        if (!changedFile) {
            return {DiffIngressError::DiffRejected};
        }
        followChanges.push_back(
            {changedFile->get(), std::move(priorHunks), change.revision});
    }

    auto stagedFollow = follow;
    const auto followed =
        stagedFollow.acceptExternalChanges(std::move(followChanges));
    if (!followed.accepted()) {
        return {DiffIngressError::FollowRejected};
    }

    const auto previousTarget = follow.viewState().activeTarget;
    diff = std::move(stagedDiff);
    follow = std::move(stagedFollow);
    const auto next = follow.viewState();
    if (next.mode == FollowMode::Following && next.activeTarget &&
        next.activeTarget != previousTarget) {
        (void)openOrRevealFollowTargetProgrammatic(*next.activeTarget);
    }
    return {};
}

void Editor::recordNavigation(NavigationClass classification) {
    (void)follow.applyNavigation({.classification = classification});
}

OperationResult Editor::splitPane(SplitAxis axis) {
    (void)paneTopology.splitActive(axis);
    return success();
}

OperationResult Editor::closePane() {
    if (!paneTopology.closeActive()) {
        return failure("the only editor pane cannot be closed");
    }
    return success();
}

OperationResult Editor::cyclePane(
    CycleDirection direction) {
    paneTopology.cycle(direction);
    if (follow.viewState().mode == FollowMode::Following) {
        (void)follow.pause();
    }
    recordNavigation(NavigationClass::User);
    return success();
}

bool Editor::focusPane(PaneId pane) {
    return paneTopology.focus(pane);
}

void Editor::resetKeymapToDefault() {
    std::lock_guard operationLock{operationMutex};
    keymap = defaultTerminalKeymap();
    ++keymapGeneration;
}

CompiledKeymap const& Editor::resolveInputKeymap() {
    if (!inputKeymap || inputKeymapGeneration != keymapGeneration) {
        inputKeymap = std::make_unique<CompiledKeymap>(keymap);
        inputKeymapGeneration = keymapGeneration;
    }
    return *inputKeymap;
}

void Editor::focusEditor() {
    std::lock_guard operationLock{operationMutex};
    screen.focusEditor();
}

WorkspaceSearchState Editor::workspaceSearch(std::string query) {
    std::lock_guard g{operationMutex};
    const auto sourceGeneration = ++workspaceSearchGeneration;
    startWorkspaceSearch(std::move(query), sourceGeneration);
    return *workspaceSearchState;
}

FindReplaceOperationResult Editor::updateFindQuery(PromptEditState query) {
    std::lock_guard g{operationMutex};
    return applyFindQuery(*this, std::move(query));
}

FindReplaceOperationResult Editor::updateReplacement(
    PromptEditState replacement) {
    std::lock_guard g{operationMutex};
    return applyReplacement(*this, std::move(replacement));
}

OperationResult Editor::saveSession() {
    std::lock_guard lock{operationMutex};
    if (snapshotPath.empty()) return success();

    SessionSnapshot snapshot;
    snapshot.identity = snapshotIdentity;
    const auto& view = tabs.viewState();
    for (const auto& tab : view.tabs) {
        if (tab.kind != TabKind::Document || tab.mode != DocumentMode::Edit ||
            !tab.document) {
            continue;
        }
        const auto state = workspace.state(*tab.document);
        if (!state || !state->dirty) continue;
        const auto persistence = workspace.persistenceState(*tab.document);
        const auto* document = workspace.tryDocument(*tab.document);
        if (!persistence || !document || !tab.documentKey) {
            return failure("could not snapshot an incomplete document tab");
        }

        SessionSnapshotTab saved;
        saved.label = state->displayLabel;
        saved.mode = document->mode();
        saved.draft = document->snapshot().text;
        saved.active = view.active == tab.id;
        if (state->key.kind() == DocumentKeyKind::Untitled) {
            saved.backing = SessionBackingKind::Untitled;
        } else {
            saved.path = state->key.savedPath();
            if (persistence->persisted) {
                saved.backing = SessionBackingKind::PersistedPath;
                saved.baseline = persistence->baseline;
            } else {
                saved.backing = SessionBackingKind::NeverCreatedPath;
            }
        }
        snapshot.tabs.push_back(std::move(saved));
    }

    const auto written = writeSessionSnapshot(snapshotPath, snapshot);
    return written.accepted() ? success() : failure(written.message);
}

OperationResult Editor::restoreSession() {
    if (snapshotPath.empty()) return success();
    const auto read = readSessionSnapshot(snapshotPath, snapshotIdentity);
    if (!read.accepted()) return failure(read.message);
    if (!read.snapshot) return success();

    std::optional<TabId> requestedActive;
    std::size_t conflicts = 0;
    try {
        for (const auto& saved : read.snapshot->tabs) {
        WorkspaceResult restored;
        bool recoverUntitled = false;
        if (saved.backing == SessionBackingKind::Untitled) {
            restored = workspace.restoreUntitled(
                saved.label, saved.draft, saved.mode);
        } else {
            const auto absolute = workspace.root() / saved.path;
            std::optional<FileStat> status;
            try {
                status = statFile(absolute);
            } catch (const std::exception&) {
                recoverUntitled = true;
            }

            if (!recoverUntitled &&
                saved.backing == SessionBackingKind::NeverCreatedPath) {
                if (!status) {
                    restored = workspace.restorePathBound(
                        saved.path, saved.label, saved.draft, saved.mode);
                    if (!restored.accepted() &&
                        restored.error == WorkspaceError::AlreadyOpen) {
                        recoverUntitled = true;
                    }
                } else {
                    recoverUntitled = true;
                }
            } else if (!recoverUntitled &&
                       saved.backing == SessionBackingKind::PersistedPath) {
                if (!status) {
                    restored = workspace.restorePathBound(
                        saved.path, saved.label, saved.draft, saved.mode);
                    if (!restored.accepted() &&
                        restored.error == WorkspaceError::AlreadyOpen) {
                        recoverUntitled = true;
                    }
                } else if (status->kind != FileKind::Regular) {
                    recoverUntitled = true;
                } else {
                    const auto current = readFile(absolute);
                    if (current.status == FileIoStatus::NotFound) {
                        restored = workspace.restorePathBound(
                            saved.path, saved.label, saved.draft, saved.mode);
                        if (!restored.accepted()) recoverUntitled = true;
                    } else if (!current.ok()) {
                        recoverUntitled = true;
                    } else {
                        const auto decoded = decodeText(current.bytes);
                        if ((decoded.accepted() &&
                             decoded.text->utf8 == saved.draft) ||
                            current.bytes == saved.baseline) {
                            restored = workspace.restorePersisted(
                                saved.path, saved.label, current.bytes,
                                saved.draft, saved.mode);
                        } else {
                            recoverUntitled = true;
                        }
                    }
                }
            }
        }

        if (recoverUntitled) {
            ++conflicts;
            restored = workspace.restoreUntitled(
                saved.path + " (recovered)", saved.draft, saved.mode);
        }
        if (!restored.accepted() || !restored.document) {
            return failure(
                "session snapshot '" + snapshotPath.string() +
                "' could not be restored: " + workspaceMessage(restored) +
                "; move or delete it to start without recovery");
        }
        const auto activated = activateDocument(*restored.document);
        if (!activated.accepted) return activated;
        if (saved.active) requestedActive = tabs.viewState().active;
        }

        if (requestedActive) {
            const auto activated = tabs.activate(*requestedActive);
            if (!activated.accepted()) return failure(tabMessage(activated));
            clampSelectionToActiveDocument();
            refreshSyntax();
        }
        if (conflicts != 0) {
            showStatus(
                "Recovered " + std::to_string(conflicts) +
                (conflicts == 1 ? " session draft" : " session drafts") +
                " as untitled copies because their disk paths changed");
        }
    } catch (const std::exception& error) {
        return failure(
            "session snapshot '" + snapshotPath.string() +
            "' could not be restored: " + error.what() +
            "; move or delete it to start without recovery");
    }
    return success();
}

EditorCreateResult createEditor(EditorConfig config) {
    try {
        auto cwd = canonicalDirectory(config.cwd);
        if (config.recoveryRoot.empty()) config.recoveryRoot = cwd / ".ssg" / "recovery";
        if (config.archiveRoot.empty()) config.archiveRoot = cwd / ".ssg" / "archive";
        const auto recoveryCreated = ensureDirectory(config.recoveryRoot);
        if (!recoveryCreated.ok()) {
            throw std::runtime_error(recoveryCreated.message);
        }
        auto editor = std::unique_ptr<Editor>{new Editor{
            cwd, config.recoveryRoot, config.archiveRoot, config.snapshotPath,
            std::move(config.snapshotIdentity), config.deferEnrichment,
            std::move(config.syntaxParser),
            config.enableGitDiffWorker, config.enableFilesystemWatcher}};
        (void)editor->workspace.pruneArchive();
        editor->keymap = defaultTerminalKeymap();
        if (auto errors = KeymapMatcher{editor->keymap}.validate(); !errors.empty()) {
            return {nullptr, "default keymap is invalid: " + errors.front().message};
        }
        if (!KeymapMatcher{editor->keymap}.hasGlobalBinding("settings.open")) {
            return {nullptr,
                    "default keymap lacks a global settings.open escape hatch"};
        }
        registerAllCommands(editor->commands, *editor);
        const auto restored = editor->restoreSession();
        if (!restored.accepted) return {nullptr, restored.message};
        return {std::move(editor), {}};
    } catch (std::exception const& exception) {
        return {nullptr, exception.what()};
    }
}

PumpResult Editor::pump() {
    if (commands.dispatchInProgress()) {
        throw std::logic_error{"worker results cannot be pumped during dispatch"};
    }
    std::lock_guard operationLock{operationMutex};
    auto batch = gitDiffIngress.drain();
    if (batch.watchEvents.empty() && !batch.fullReconcile) {
        const bool findOpen = findReplace.viewState().open;
        std::optional<std::pair<FileDocumentId, std::uint64_t>> activeBefore;
        if (findOpen) {
            if (const auto id = activeDocumentId()) {
                if (const auto* document = workspace.tryDocument(*id)) {
                    activeBefore = std::pair{*id, document->revision()};
                }
            }
        }
        auto accepted = adoptGitDiffWorkerDrainLocked(std::move(batch));
        std::optional<std::pair<FileDocumentId, std::uint64_t>> activeAfter;
        if (findOpen) {
            if (const auto id = activeDocumentId()) {
                if (const auto* document = workspace.tryDocument(*id)) {
                    activeAfter = std::pair{*id, document->revision()};
                }
            }
        }
        if (activeBefore != activeAfter) {
            reconcileFindDocument();
        }
        return {accepted};
    }
    OperationScope operation{*this, OperationScope::RevisionScope::None};
    return {adoptGitDiffWorkerDrainLocked(std::move(batch))};
}

bool Editor::adoptGitDiffWorkerDrainLocked(GitDiffWorkerDrain batch) {
    bool accepted = batch.watcherAvailabilityChanged || !batch.scans.empty() ||
                    !batch.watchEvents.empty() || batch.fullReconcile;
    bool externalAdvanced = false;
    for (auto& scan : batch.scans) {
        (void)applyGitDiffScanLocked(std::move(scan));
    }
    if (!batch.watchEvents.empty()) {
        externalAdvanced = external.ingest(std::vector<WatchEvent>{
            batch.watchEvents.begin(), batch.watchEvents.end()});
        const bool inventoryChanged = std::any_of(
            batch.watchEvents.begin(), batch.watchEvents.end(),
            [](const WatchEvent& event) {
                return event.kind != WatchEventKind::Modify ||
                       event.path.filename() == ".gitignore";
            });
        if (inventoryChanged) {
            refreshTreeForPublication();
        }
    }
    if (batch.fullReconcile) {
        externalAdvanced |= external.reconcileAllOpenDocumentsAgainstDisk();
        refreshTreeForPublication();
    }
    if (externalAdvanced) {
        refreshSyntax();
        for (const auto document : workspace.documents()) {
            (void)updateTabsFor(document);
        }
        if (activeOperation_ == nullptr) {
            reconcileFindDocument();
            screen.refreshExternalModificationPresence(
                externalModificationPresent());
        }
    }
    return accepted;
}

DiffIngressResult Editor::applyGitDiffScanLocked(GitDiffScan scan) {
    if (scan.revision == 0) {
        currentGitBranch = scan.currentBranch;
        return {};
    }
    if (scan.revision <= lastGitScanRevision) {
        return {DiffIngressError::DiffRejected};
    }
    currentGitBranch = scan.currentBranch;
    auto gitRecords = gitTreeRecordsFromScan(scan.files);
    const auto revision = scan.revision;
    auto staged = stageGitDiffScan(std::move(scan), diff, follow);
    if (!staged.accepted()) {
        return staged.result;
    }
    if (staged.mutated) {
        const auto previousTarget = follow.viewState().activeTarget;
        diff = std::move(staged.diff);
        follow = std::move(staged.follow);
        for (const auto& id : staged.statusOnlyIds) {
            std::optional<TabId> liveTab;
            for (const auto& tab : tabs.viewState().tabs) {
                if (tab.kind == TabKind::LiveDiff &&
                    tab.contentIdentity == id.value()) {
                    liveTab = tab.id;
                    break;
                }
            }
            if (liveTab) {
                const auto currentTabs = tabs.viewState();
                const auto found = std::find_if(
                    currentTabs.tabs.begin(), currentTabs.tabs.end(),
                    [&](const TabState& tab) { return tab.id == *liveTab; });
                if (found != currentTabs.tabs.end()) {
                    auto outcome = closeTab(*found);
                    (void)tabs.close(*liveTab, std::move(outcome));
                }
            }
        }
        refreshLiveDiffDocuments(diff.viewState());
        const auto next = follow.viewState();
        if (next.mode == FollowMode::Following && next.activeTarget &&
            next.activeTarget != previousTarget) {
            (void)openOrRevealFollowTargetProgrammatic(*next.activeTarget);
        }
    }
    tree.replaceProvider(TreeProviderSnapshot::fromGit(
        TreeProviderId{"git"}, std::move(gitRecords)));
    lastGitScanRevision = revision;
    return {};
}

bool Editor::pumpSyntax() {
    if (commands.dispatchInProgress()) {
        throw std::logic_error{"worker results cannot be pumped during dispatch"};
    }
    std::lock_guard operationLock{operationMutex};
    bool accepted = false;
    for (auto& completion : syntaxWorker.drain()) {
        auto state = documentRuntimeStates.find(completion.document.value());
        auto const* document = workspace.tryDocument(completion.document);
        if (state == documentRuntimeStates.end() || document == nullptr) {
            continue;
        }
        auto language = languageFor(completion.document);
        if (document->revision() != completion.request->revision() ||
            language != completion.request->language()) {
            continue;
        }
        accepted =
            state->second.syntax
                .accept(completion.request, completion.output)
                .accepted() ||
            accepted;
    }
    return accepted;
}

bool Editor::dispatchInProgress() const noexcept {
    return commands.dispatchInProgress();
}

bool Editor::deferDispatch(std::string commandId) {
    if (!commands.dispatchInProgress()) return false;
    return deferredCommands.enqueue(std::move(commandId));
}

CommandResult Editor::dispatchLocked(std::string_view commandId) {
    std::optional<OperationScope> standaloneOperation;
    if (activeOperation_ == nullptr) {
        standaloneOperation.emplace(*this, OperationScope::RevisionScope::Workspace);
    }
    if (workspaceSearchPending() && commandId != "search.workspace") {
        cancelWorkspaceSearch();
    }
    const auto dispatchCommand = [&](std::string_view dispatched) {
        auto result = commands.dispatch(dispatched);
        activeOperation_->checkpoint(result.accepted());
        return result;
    };
    const auto dispatchAndDrain = [&](std::string_view dispatched) {
        auto outcome = dispatchCommand(dispatched);
        if (!outcome.accepted()) {
            deferredCommands.clear();
            return outcome;
        }
        while (!deferredCommands.empty()) {
            auto deferred = deferredCommands.takeFront();
            auto deferredResult = dispatchCommand(deferred);
            if (!deferredResult.accepted()) {
                deferredCommands.clear();
                return CommandResult{
                    deferredResult.error,
                    deferred + ": " + deferredResult.message, std::nullopt};
            }
            if (!deferredResult.viewAction) {
                deferredResult.viewAction = std::move(outcome.viewAction);
            }
            outcome = deferredResult;
        }
        return outcome;
    };
    auto result = dispatchAndDrain(commandId);
    return {result.error, std::move(result.message),
            std::move(result.viewAction)};
}

ClientInputResult Editor::input(ClientInput const& input) {
    if (commands.dispatchInProgress()) {
        return {ClientInputOutcome::Rejected, std::nullopt,
                CommandResult{CommandError::HandlerFailed,
                              std::string{kNestedDispatchRefusal},
                              {}}};
    }
    std::lock_guard operationLock{operationMutex};
    auto routed = routeInput(inputRoutingSnapshot(*this), input);
    const bool activeDocumentOnly =
        std::holds_alternative<RouteAccepted>(routed.action);
    const auto* picker = std::get_if<SubmitPicker>(&routed.action);
    const bool commandPicker =
        picker && picker->activation.mode == SearchMode::Command;
    const bool needsReconciliation =
        activeDocumentOnly ||
        std::holds_alternative<InvokeExternalAction>(routed.action) ||
        std::holds_alternative<ActivateUiNode>(routed.action) ||
        std::holds_alternative<ActivateTreeNode>(routed.action) ||
        (std::holds_alternative<SubmitPicker>(routed.action) && !commandPicker);
    std::optional<OperationScope> operation;
    if (needsReconciliation) {
        auto scope = OperationScope::RevisionScope::Workspace;
        if (activeDocumentOnly) {
            scope = OperationScope::RevisionScope::ActiveDocument;
        } else if (std::holds_alternative<ActivateTreeNode>(routed.action)) {
            scope = OperationScope::RevisionScope::None;
        } else if (picker && picker->activation.mode == SearchMode::File) {
            scope = OperationScope::RevisionScope::None;
        }
        operation.emplace(*this, scope);
    }
    auto result = std::visit(
        [&](auto route) {
            return executeInputRoute(*this, std::move(route),
                                     std::move(routed));
        },
        std::move(routed.action));
    if (operation && result.command) {
        operation->checkpoint(result.command->accepted());
    }
    return result;
}

OperationResult Editor::applyTextInput(TextInputCommand command,
                                       TextInputArguments arguments) {
    if (commands.dispatchInProgress()) {
        return failure(std::string{kNestedDispatchRefusal});
    }
    std::lock_guard operationLock{operationMutex};
    OperationScope operation{*this, OperationScope::RevisionScope::ActiveDocument};
    auto result = applyEditorTextInput(*this, command, std::move(arguments));
    operation.checkpoint(result.accepted);
    return result;
}

CommandResult Editor::dispatch(std::string_view commandId) {
    // A handler must be refused before taking the non-recursive aggregate lock.
    if (commands.dispatchInProgress()) {
        return {CommandError::HandlerFailed,
                std::string{kNestedDispatchRefusal}};
    }
    std::lock_guard operationLock{operationMutex};
    return dispatchLocked(commandId);
}

Commands const& Editor::commandRegistry() const {
    return commands;
}

void Editor::addCommand(std::string id, std::string label,
                        std::function<CommandResult()> handler) {
    if (dispatchInProgress()) {
        throw std::logic_error{
            "commands cannot be registered during dispatch"};
    }
    std::lock_guard operationLock{operationMutex};
    commands.add(std::move(id), std::move(label), std::move(handler));
}

void Editor::replaceCommands(std::span<std::string const> oldIds,
                             Commands::Replacements replacements) {
    if (dispatchInProgress()) {
        throw std::logic_error{
            "commands cannot be replaced during dispatch"};
    }
    std::lock_guard operationLock{operationMutex};
    commands.replace(oldIds, std::move(replacements));
}

} // namespace ssg
