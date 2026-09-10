#include <ssg/DraftReopenClassifier.h>
#include <ssg/Editor.h>
#include <ssg/FilesystemWatcher.h>
#include <ssg/GraphemeLayout.h>
#include <ssg/ScreenLayout.h>
#include <ssg/Style.h>
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

constexpr std::size_t kFirstFrameSyntaxMaxBytes = 2 * 1024 * 1024;

std::string liveDiffTabLabelForPath(const std::filesystem::path& path) {
    const auto filename = path.filename().string();
    return filename.empty() ? path.generic_string() : filename;
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

std::string liveDiffTextWithoutRemovedRows(const DiffFileView& file) {
    return file.currentContent;
}

std::string uniqueDiscardedDraftArchiveName(std::string_view savedPath) {
    std::string basename =
        std::filesystem::path{std::string{savedPath}}.filename().string();
    if (basename.empty()) basename = "draft";
    if (basename.size() > 64) basename.resize(64);
    const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return basename + "." + std::to_string(fastContentHash(savedPath)) + "." +
           std::to_string(stamp) + ".draft";
}

} // namespace

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
    bind(seq({"Mod+Shift+KeyD"}), "draft.discard", "editor");

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

std::span<const std::byte> asByteSpan(std::string_view text) noexcept {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::optional<std::string> readFileText(std::filesystem::path const& path) {
    auto result = readFile(path);
    if (!result.ok()) return std::nullopt;
    return std::string{reinterpret_cast<const char*>(result.bytes.data()),
                       result.bytes.size()};
}

std::unordered_map<std::uint64_t, std::uint64_t>
documentRevisions(const Workspace& workspace) {
    std::unordered_map<std::uint64_t, std::uint64_t> revisions;
    for (auto const id : workspace.documents()) {
        revisions.emplace(id.value(), workspace.document(id).revision());
    }
    return revisions;
}

bool existingDocumentMutated(
    const std::unordered_map<std::uint64_t, std::uint64_t>& before,
    const Workspace& workspace) {
    for (auto const id : workspace.documents()) {
        const auto found = before.find(id.value());
        if (found == before.end()) {
            continue;
        }
        if (workspace.document(id).revision() != found->second) {
            return true;
        }
    }
    return false;
}

void reconcileAfterOperation(
    Editor& editor,
    const std::unordered_map<std::uint64_t, std::uint64_t>& revisionsBefore,
    bool accepted) {
    editor.reconcileFindDocument();
    editor.screen.refreshNoticePresence(editor.noticePresent());
    editor.screen.refreshExternalModificationPresence(
        editor.externalModificationPresent());
    editor.screen.refreshStatusActions(editor.status.actionNodes());
    if (accepted &&
        existingDocumentMutated(revisionsBefore, editor.workspace)) {
        (void)editor.follow.notifyLocalEdit();
    }
}

bool pathContains(std::filesystem::path const& root,
                   std::filesystem::path const& candidate) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *rootIt != *candidateIt) {
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path> workspaceChangePath(
    std::filesystem::path const& root, std::string_view rawPath,
    std::string& message) {
    auto supplied = std::filesystem::path{rawPath};
    if (rawPath.empty() || supplied.is_absolute()) {
        message = "workspace replacement path must be relative";
        return std::nullopt;
    }
    for (auto const& part : supplied) {
        if (part == "..") {
            message = "workspace replacement path must not traverse";
            return std::nullopt;
        }
    }
    std::error_code code;
    auto candidate = weaklyCanonicalPath(root / supplied, code);
    if (code) {
        message = code.message();
        return std::nullopt;
    }
    if (!pathContains(root, candidate)) {
        message = "workspace replacement path resolves outside the workspace";
        return std::nullopt;
    }
    return candidate;
}

PromptRoutingState inputPromptState(Editor const& editor) {
    PromptRoutingState routing;
    routing.focus = editor.screen.effectiveFocus();
    auto const promptStatus = editor.promptStatusView();
    if (promptStatus.activeKind == PromptKind::Palette) {
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
            routing.currentValue = control.value;
            break;
        }
    }
    return routing;
}

InputRoutingSnapshot inputRoutingSnapshot(Editor& editor) {
    auto const* document = editor.activeDocument();
    auto const* tab = editor.activeTabState();
    auto notice = editor.noticeView();
    std::optional<std::vector<InputRoutingNoticeAction>> noticeActions;
    if (notice) {
        noticeActions.emplace();
        noticeActions->reserve(notice->actions.size());
        for (auto const& action : notice->actions) {
            noticeActions->push_back({action.id, action.commandId});
        }
    }
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
        .noticeActions = std::move(noticeActions),
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
        auto result = applyFindQuery(editor, std::move(update->query));
        if (!result.accepted()) return result.message;
        return std::nullopt;
    }
    if (auto* update = std::get_if<UpdateReplacement>(&*mutation)) {
        auto result = applyReplacement(editor, std::move(update->replacement));
        if (!result.accepted()) return result.message;
        return std::nullopt;
    }

    const auto change = std::get<SearchQueryChange>(*mutation);
    const auto binding = editor.tree.activeProviderBinding();
    if (!binding || binding->kind != TreeProviderKind::Search) {
        return "search query target changed before execution";
    }
    auto state = editor.tree.searchState(binding->id);
    if (!state) return "search provider state is unavailable";
    switch (change.kind) {
    case SearchQueryChange::Kind::Append:
        state->query += change.text;
        state->editing = true;
        break;
    case SearchQueryChange::Kind::DeleteGraphemeBack:
        state->query = applyPromptTextEdit(
            state->query,
            {PromptTextEdit::Kind::DeleteGraphemeBack, {}});
        state->editing = true;
        break;
    case SearchQueryChange::Kind::MoveFirst:
    case SearchQueryChange::Kind::MoveLast: {
        const auto view = editor.tree.viewState();
        const auto* provider = activeTreeProvider(view);
        if (provider == nullptr || provider->nodes.empty()) {
            return std::nullopt;
        }
        const auto& node =
            change.kind == SearchQueryChange::Kind::MoveFirst
                ? provider->nodes.front()
                : provider->nodes.back();
        if (!editor.tree.select(node.node.id)) {
            return "search result target changed before execution";
        }
        state->editing = false;
        break;
    }
    case SearchQueryChange::Kind::Submit:
        editor.search.cancelWorkspaceSearch();
        editor.workspaceSearchState.reset();
        editor.workspaceSearchCorpus.reset();
        editor.panelWorkspaceSearchGeneration.reset();
        editor.tree.replaceProvider(TreeProviderSnapshot{
            binding->id, TreeProviderKind::Search, {}});
        state->submittedQuery.reset();
        state->searching = false;
        if (!state->query.empty()) {
            state->submittedQuery = state->query;
            state->searching = true;
            const auto sourceGeneration = ++editor.workspaceSearchGeneration;
            editor.startWorkspaceSearch(
                ParsedSearchQuery{.mode = SearchMode::Text,
                                  .text = state->query},
                sourceGeneration);
        }
        break;
    case SearchQueryChange::Kind::Focus:
        if (!editor.screen.focusPanel()) {
            return "search query sidebar is unavailable";
        }
        state->editing = true;
        break;
    }
    if (!editor.tree.setSearchState(binding->id, std::move(*state))) {
        return "search provider state changed before execution";
    }
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
    const auto revisionsBefore = documentRevisions(editor.workspace);
    editor.screen.refreshExternalModificationPresence(
        editor.externalModificationPresent());
    auto error = applyInputMutation(editor, std::move(route.mutation));
    editor.reconcileFindDocument();
    editor.screen.refreshNoticePresence(editor.noticePresent());
    editor.screen.refreshExternalModificationPresence(
        editor.externalModificationPresent());
    editor.screen.refreshStatusActions(editor.status.actionNodes());
    if (!error && existingDocumentMutated(revisionsBefore, editor.workspace)) {
        (void)editor.follow.notifyLocalEdit();
    }
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
    const auto revisionsBefore = documentRevisions(editor.workspace);
    editor.screen.refreshExternalModificationPresence(
        editor.externalModificationPresent());
    auto result = invokeExternalAction(editor, route.invocation);
    editor.reconcileFindDocument();
    editor.screen.refreshNoticePresence(editor.noticePresent());
    editor.screen.refreshExternalModificationPresence(
        editor.externalModificationPresent());
    editor.screen.refreshStatusActions(editor.status.actionNodes());
    if (result.accepted &&
        existingDocumentMutated(revisionsBefore, editor.workspace)) {
        (void)editor.follow.notifyLocalEdit();
    }
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
    const auto revisionsBefore = documentRevisions(editor.workspace);
    editor.screen.refreshExternalModificationPresence(
        editor.externalModificationPresent());
    auto result = applyUiNodeActivation(editor, route.nodeId);
    editor.reconcileFindDocument();
    editor.screen.refreshNoticePresence(editor.noticePresent());
    editor.screen.refreshExternalModificationPresence(
        editor.externalModificationPresent());
    editor.screen.refreshStatusActions(editor.status.actionNodes());
    if (result.accepted &&
        existingDocumentMutated(revisionsBefore, editor.workspace)) {
        (void)editor.follow.notifyLocalEdit();
    }
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
    const auto revisionsBefore = documentRevisions(editor.workspace);
    editor.screen.refreshExternalModificationPresence(
        editor.externalModificationPresent());
    auto result = activateTreeNode(editor, route.nodeId);
    editor.reconcileFindDocument();
    editor.screen.refreshNoticePresence(editor.noticePresent());
    editor.screen.refreshExternalModificationPresence(
        editor.externalModificationPresent());
    editor.screen.refreshStatusActions(editor.status.actionNodes());
    if (result.accepted &&
        existingDocumentMutated(revisionsBefore, editor.workspace)) {
        (void)editor.follow.notifyLocalEdit();
    }
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
            const auto revisionsBeforeClose =
                documentRevisions(editor.workspace);
            (void)editor.screen.closeFinder();
            reconcileAfterOperation(editor, revisionsBeforeClose, true);
        }
        auto const activation = editor.screen.openPickerActivation();
        const auto outcome =
            result.viewAction ? ClientInputOutcome::ViewOwned
                              : ClientInputOutcome::Dispatched;
        return {outcome, std::nullopt, std::move(result), activation};
    }
    if (route.activation.mode == SearchMode::File) {
        const auto revisionsBefore = documentRevisions(editor.workspace);
        editor.screen.refreshExternalModificationPresence(
            editor.externalModificationPresent());
        auto result = applyFilePathCompletion(editor, PromptCompletion::FileOpen,
                                              route.candidateId);
        reconcileAfterOperation(editor, revisionsBefore, result.accepted);
        if (!result.accepted) {
            return {ClientInputOutcome::Rejected, std::nullopt,
                    CommandResult{CommandError::HandlerFailed,
                                  std::move(result.message), {}},
                    std::nullopt};
        }
        if (editor.screen.openPickerActivation() == route.activation) {
            const auto revisionsBeforeClose =
                documentRevisions(editor.workspace);
            (void)editor.screen.closeFinder();
            reconcileAfterOperation(editor, revisionsBeforeClose, true);
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

std::string workspaceMessage(WorkspaceResult const& result) {
    return result.message.empty() ? "workspace operation failed" : result.message;
}

std::string tabMessage(TabResult const& result) {
    return result.message.empty() ? "tab operation failed" : result.message;
}

Editor::Editor(std::filesystem::path canonicalCwd,
               std::filesystem::path scratchRoot,
               std::filesystem::path recoveryRoot,
               std::filesystem::path archiveRoot,
               bool deferEnrichment,
               std::shared_ptr<SyntaxParser> parser,
               bool enableGitDiffWorker,
               bool enableFilesystemWatcher)
    : root{std::move(canonicalCwd)},
      workspaceIgnore{makePlatformGitIgnoreMatcher(root)},
      scratchRoot{weaklyCanonicalPath(scratchRoot)},
      recoveryRoot{weaklyCanonicalPath(recoveryRoot)},
      archiveRoot{weaklyCanonicalPath(archiveRoot)},
      recovery{RecoveryManager::create(recoveryRoot)},
      scratch{ScratchStore::create(scratchRoot, root)},
      workspace{Workspace::create(root, recovery, this->archiveRoot)},
      selection{initialSelection()}, clipboard{4}, tabs{},
      external{workspace, diff}, syntaxParser{std::move(parser)},
      screen{assembleScreen("help.open", StyleDimensions{},
                            Style{}.inputLineSigil),
             tree},
      theme{defaultTheme()}, deferringEnrichment{deferEnrichment},
      gitDiffIngress{*this, root, enableGitDiffWorker,
                     enableFilesystemWatcher} {
    homeDirectory = resolveHomeDirectory();
    workspace.setSaveObserver(
        [this](const std::filesystem::path& relativePath) {
            external.registerSaveExpectation(relativePath);
    });
    (void)refreshTree();
    refreshSyntax();
}

Editor::~Editor() = default;

int Editor::gitDiffWakeDescriptor() const {
    return gitDiffIngress.worker.wakeDescriptor();
}


TabLifecycleResult Editor::closeTab(
    const TabState& tab, std::chrono::milliseconds durabilityTimeout,
    std::span<const TabId> alreadyClosed) {
    if (!tab.document) {
        if (tab.kind == TabKind::ReadOnlyOutput) {
            // Read-only output tabs have no recovery record.
            const auto mapped = readOnlyTabDocuments.find(tab.contentIdentity);
            if (mapped != readOnlyTabDocuments.end()) {
                const auto document = mapped->second;
                readOnlyTabDocuments.erase(mapped);
                documentRuntimeStates.erase(document.value());
                documentLanguageOverrides.erase(document.value());
                (void)workspace.removeDocument(document);
            }
            return {TabError::None, {}, std::nullopt, std::nullopt, std::nullopt,
                    false, true};
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
                    std::optional<JournalDocument> journal;
                    if (auto const* current = workspace.tryDocument(document);
                        current != nullptr) {
                        journal = JournalDocument{state->key, current->mode(),
                                                  state->dirty,
                                                  current->snapshot().text,
                                                  workspace.baselineFor(document)};
                    }
                    auto closed =
                        recovery.closeDocument(journal, scratch, durabilityTimeout);
                    if (!closed.accepted()) {
                        return {TabError::LifecycleFailed, closed.error->message,
                                std::nullopt, std::nullopt, std::nullopt, false};
                    }
                    if (journal) scratch.removeDocument(state->key);
                    auto removed = workspace.removeDocument(document);
                    if (!removed.accepted()) {
                        return {TabError::LifecycleFailed, workspaceMessage(removed),
                                std::nullopt, std::nullopt, std::nullopt, false};
                    }
                    documentRuntimeStates.erase(document.value());
                    liveDiffDocuments.erase(mapped);
                    return {TabError::None, {}, closed.compensation, std::nullopt,
                            std::nullopt,
                            scratch.waitUntilDurable(durabilityTimeout)};
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
    std::optional<JournalDocument> document;
    if (auto const* current = workspace.tryDocument(*tab.document); current != nullptr) {
        document = JournalDocument{state->key, current->mode(), state->dirty,
                                   current->snapshot().text,
                                   workspace.baselineFor(*tab.document)};
    }
    auto closed = recovery.closeDocument(document, scratch, durabilityTimeout);
    if (!closed.accepted()) {
        return {TabError::LifecycleFailed, closed.error->message, std::nullopt,
                std::nullopt, std::nullopt, false};
    }
    if (document) scratch.removeDocument(state->key);
    auto removed = workspace.removeDocument(*tab.document);
    if (!removed.accepted()) {
        return {TabError::LifecycleFailed, workspaceMessage(removed),
                std::nullopt, std::nullopt, std::nullopt, false};
    }
    documentRuntimeStates.erase(tab.document->value());
    autosave.forget(*tab.document);
    return {TabError::None,      {},
            closed.compensation, std::nullopt,
            std::nullopt,        scratch.waitUntilDurable(durabilityTimeout)};
}

TabLifecycleResult Editor::reopenTab(
    const TabState& tab, const RecoveryRecordId& compensation) {
    std::optional<JournalDocument> restoredDocument;
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
    } else if (restoredDocument->key.kind() == JournalDocumentKeyKind::Saved) {
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
        if (!state || state->key.kind() != JournalDocumentKeyKind::Saved ||
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
    options.excludedDirectories = {scratchRoot, recoveryRoot, archiveRoot};
    return WorkspaceCorpus{root, std::move(buffers), *workspaceIgnore, readFile,
                           std::move(options)};
}

WorkspaceSnapshot Editor::snapshot(std::uint64_t revision) const {
    WorkspaceSnapshot result;
    result.revision = revision;
    auto corpus = workspaceCorpus();
    result.files.reserve(corpus.paths().size());
    for (std::size_t index = 0; index < corpus.paths().size(); ++index) {
        auto file = corpus.read(index);
        if (!file) continue;
        result.files.push_back(
            {std::move(file->path), std::move(file->text)});
    }
    return result;
}

void Editor::startWorkspaceSearch(std::string query,
                                  std::uint64_t sourceRevision) {
    panelWorkspaceSearchGeneration.reset();
    workspaceSearchState =
        search.beginWorkspaceSearch(std::move(query), sourceRevision);
    workspaceSearchCorpus = workspaceCorpus();
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

void Editor::advanceWorkspaceSearch() {
    if (!workspaceSearchCorpus || !workspaceSearchState) return;
    auto batch = search.evaluate(*workspaceSearchState,
                                 *workspaceSearchCorpus,
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

WorkspaceApplyResult Editor::applyWorkspaceReplace(
    const WorkspaceReplacePreview& preview) {
    std::vector<std::filesystem::path> paths;
    std::vector<std::string> normalizedPaths;
    paths.reserve(preview.changes.size());
    normalizedPaths.reserve(preview.changes.size());
    for (auto const& change : preview.changes) {
        std::string message;
        auto path = workspaceChangePath(root, change.path, message);
        if (!path) {
            return {FindReplaceError::WorkspaceRejected,
                    preview.sourceRevision, std::move(message)};
        }
        if (pathContains(scratchRoot, *path) ||
            pathContains(recoveryRoot, *path) ||
            pathContains(archiveRoot, *path)) {
            return {FindReplaceError::WorkspaceRejected,
                    preview.sourceRevision,
                    "workspace replacement path targets runtime state"};
        }
        auto normalized =
            std::filesystem::path{change.path}.lexically_normal().generic_string();
        std::string current;
        bool foundOpenDocument = false;
        for (auto const id : workspace.documents()) {
            auto state = workspace.state(id);
            if (!state || state->key.kind() != JournalDocumentKeyKind::Saved ||
                state->key.savedPath() != normalized) {
                continue;
            }
            current = workspace.document(id).snapshot().text;
            foundOpenDocument = true;
            break;
        }
        if (!foundOpenDocument) {
            auto content = readFileText(*path);
            if (!content) {
                return {FindReplaceError::StaleRevision, preview.sourceRevision,
                        "workspace replacement target cannot be read"};
            }
            current = std::move(*content);
        }
        if (current != change.before) {
            return {FindReplaceError::StaleRevision, preview.sourceRevision,
                    "workspace replacement preview is stale"};
        }
        paths.push_back(std::move(*path));
        normalizedPaths.push_back(std::move(normalized));
    }
    for (std::size_t index = 0; index < preview.changes.size(); ++index) {
        auto const& change = preview.changes[index];
        try {
            replaceFileAtomically(paths[index], asByteSpan(change.after));
        } catch (const std::exception&) {
            return {FindReplaceError::WorkspaceRejected, preview.sourceRevision, "failed to write workspace file"};
        }
    }
    for (std::size_t index = 0; index < preview.changes.size(); ++index) {
        for (auto const id : workspace.documents()) {
            auto state = workspace.state(id);
            if (!state || state->key.kind() != JournalDocumentKeyKind::Saved ||
                state->key.savedPath() != normalizedPaths[index]) {
                continue;
            }
            auto reloaded = workspace.reload(id);
            if (!reloaded.accepted()) {
                return {FindReplaceError::WorkspaceRejected,
                        preview.sourceRevision, workspaceMessage(reloaded)};
            }
            (void)updateTabsFor(id);
        }
    }
    return {FindReplaceError::None,
            std::uint64_t{preview.sourceRevision + 1}, {}};
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
    const auto diffText = liveDiffTextWithoutRemovedRows(file);
    std::optional<FileDocumentId> document;
    auto mapped = liveDiffDocuments.find(target.id.value());
    if (mapped != liveDiffDocuments.end()) {
        if (const auto* opened = workspace.tryDocument(mapped->second);
            opened != nullptr &&
            opened->snapshot().text == diffText) {
            document = mapped->second;
        } else {
            documentRuntimeStates.erase(mapped->second.value());
            auto removed = workspace.removeDocument(mapped->second);
            liveDiffDocuments.erase(mapped);
            if (!removed.accepted()) {
                return failure(workspaceMessage(removed));
            }
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
    ensureDocumentRuntimeState(*replacement.document);
    documentLanguageOverrides.insert_or_assign(replacement.document->value(),
                                                std::move(language));
    auto mapped = readOnlyTabDocuments.find(contentIdentity);
    if (mapped != readOnlyTabDocuments.end()) {
        const auto previous = mapped->second;
        documentRuntimeStates.erase(previous.value());
        documentLanguageOverrides.erase(previous.value());
        (void)workspace.removeDocument(previous);
    }
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

OperationResult Editor::openDraftDiff() {
    const auto id = activeDocumentId();
    if (!id) return failure("no active document");
    const auto state = workspace.state(*id);
    if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) {
        return failure("draft.diff needs a saved file");
    }
    const auto* opened = workspace.tryDocument(*id);
    if (opened == nullptr) return failure("no active document");
    const std::string draft = opened->snapshot().text;

    const auto absolute =
        workspace.root() / std::filesystem::path{state->key.savedPath()};
    std::string disk;
    if (const auto read = readFile(absolute); read.ok()) {
        disk.assign(reinterpret_cast<const char*>(read.bytes.data()),
                    read.bytes.size());
    }

    const DiffFileId diffId{"draft:" + state->key.savedPath()};
    const std::uint64_t revision{diff.viewState().revision + 1};
    const auto applied = diff.applyNonGitEvent(
        NonGitDiffEvent{NonGitDiffEventKind::Create, diffId,
                        std::filesystem::path{state->key.savedPath()},
                        std::nullopt, disk, draft},
        revision);
    if (!applied.accepted()) return failure("draft diff could not be computed");

    const auto file = diff.file(diffId);
    if (!file.has_value()) return failure("draft diff is unavailable");
    return openOrFocusLiveDiffTab(file->get(), NavigationClass::Programmatic);
}

bool Editor::archiveDiscardedDraft(std::string_view savedPath,
                                                std::string_view content) {
    const auto archiveDir = scratchRoot.parent_path() / "draft-archive";
    const auto created = ensureDirectory(archiveDir);
    if (!created.ok()) {
        return false;
    }

    const auto target =
        archiveDir / uniqueDiscardedDraftArchiveName(savedPath);
    const std::span<const std::byte> bytes{
        reinterpret_cast<const std::byte*>(content.data()), content.size()};
    return createFileExclusively(target, bytes).ok();
}

OperationResult Editor::discardDraft() {
    const auto id = activeDocumentId();
    if (!id) return failure("no active document");
    const auto state = workspace.state(*id);
    if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) {
        return failure("draft.discard needs a saved file");
    }
    if (!state->dirty) return failure("no unsaved edits to discard");
    const auto* opened = workspace.tryDocument(*id);
    if (opened == nullptr) return failure("no active document");
    const std::string draftText = opened->snapshot().text;

    if (!archiveDiscardedDraft(state->key.savedPath(), draftText)) {
        return failure("could not archive the draft before discarding");
    }

    const auto reloaded = workspace.reload(*id);
    if (!reloaded.accepted()) return failure("could not load the file from disk");

    scratch.removeDocument(state->key);
    (void)scratch.waitUntilDurable(std::chrono::milliseconds{100});
    if (const auto found = documentRuntimeStates.find(id->value());
        found != documentRuntimeStates.end()) {
        found->second.reopen = DraftReopenOutcome::None;
    }
    resetSelectionForActiveDocument();
    refreshSyntax();
    return updateTabsFor(*id);
}

OperationResult Editor::dismissDraftNotice() {
    const auto id = activeDocumentId();
    if (!id) return failure("no active document");
    const auto found = documentRuntimeStates.find(id->value());
    if (found == documentRuntimeStates.end() ||
        found->second.reopen != DraftReopenOutcome::Conflict) {
        return failure("no draft notice to dismiss");
    }
    found->second.reopen = DraftReopenOutcome::None;
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
    return gitDiffIngress.revealCurrentDiffTarget(
        target, NavigationClass::Programmatic);
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
    documentRuntimeStates.erase(document.value());
    documentLanguageOverrides.erase(document.value());
    autosave.forget(document);
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

SyntaxViewState Editor::activeSyntaxView() const {
    if (auto id = activeDocumentId()) {
        if (auto it = documentRuntimeStates.find(id->value());
            it != documentRuntimeStates.end()) {
            return it->second.syntax.viewState();
        }
    }
    const auto* document = activeDocument();
    const auto text = document ? document->snapshot().text : std::string{};
    const auto revision = document ? document->revision() : std::uint64_t{0};
    return SyntaxViewState::plainText(revision, LanguageId::plainText(), text, 4);
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
        document->snapshot().revision != findReplace.viewState().sourceRevision;
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
    auto language = LanguageId::plainText();
    if (auto const override = documentLanguageOverrides.find(id->value());
        override != documentLanguageOverrides.end()) {
        language = override->second;
    } else if (auto state = activeWorkspaceState();
               state && state->key.kind() == JournalDocumentKeyKind::Saved) {
        language = LanguageId::fromPath(state->key.savedPath());
    }
    if (deferringEnrichment) {
        const bool canEagerlyParse =
            document != nullptr && model.hasGrammar(language) &&
            text.size() <= kFirstFrameSyntaxMaxBytes;
        if (!canEagerlyParse) {
            pendingSyntaxRefresh = true;
            return;
        }
    }
    if (!model.canIncrementallyParse(language)) edits.clear();
    (void)model.parse(revision, std::move(language), std::move(text),
                      std::move(edits));
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
    if (pendingSyntaxRefresh) {
        pendingSyntaxRefresh = false;
        refreshSyntax();
        ran = true;
    }
    (void)ran;
}

void Editor::enqueueStatus(StatusPriority priority, std::string text) {
    auto value = nextStatusId++;
    if (status
            .enqueue(StatusItem{StatusId{value}, priority, std::move(text), {}})
            .accepted) {
        screen.refreshStatusActions(status.actionNodes());
    }
}

void Editor::reconcileDraftOnOpen(FileDocumentId document) {
    auto state = workspace.state(document);
    if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) return;

    const auto drafts = scratch.recovery().documents;
    const auto draft = std::find_if(
        drafts.begin(), drafts.end(), [&](const JournalDocument& candidate) {
            return candidate.dirty && candidate.key == state->key;
        });
    if (draft == drafts.end()) return;

    auto const* opened = workspace.tryDocument(document);
    auto rawDisk = workspace.rawDiskContent(document);
    if (opened == nullptr || !rawDisk) return;
    const DraftDiskState disk{std::move(*rawDisk), opened->snapshot().text};

    auto& runtimeState = documentRuntimeStates.at(document.value());
    switch (classifyDraftReopen(draft->baseline,
                                             draft->utf8Content, disk)) {
        case DraftReopenClass::Converged:
            scratch.removeDocument(draft->key);
            runtimeState.reopen = DraftReopenOutcome::None;
            return;
        case DraftReopenClass::Unchanged:
            if (workspace.restoreDraft(document, draft->utf8Content)) {
                runtimeState.reopen = DraftReopenOutcome::Restored;
            } else {
                runtimeState.reopen = DraftReopenOutcome::Conflict;
            }
            return;
        case DraftReopenClass::Conflict:
            (void)workspace.restoreDraft(document, draft->utf8Content);
            runtimeState.reopen = DraftReopenOutcome::Conflict;
            return;
        case DraftReopenClass::Missing:
            // Unreachable via file.open (the file was just read from disk), so a
            // missing disk file here means the classifier's contract changed;
            // leave the clean buffer rather than guess.
            return;
    }
}

std::size_t Editor::flushDueAutosaveDrafts() {
    std::lock_guard operationLock{operationMutex};
    return autosave.flushDueDrafts(*this);
}

std::size_t Editor::flushAllAutosaveDrafts() {
    std::lock_guard operationLock{operationMutex};
    return autosave.flushAllDrafts(*this);
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

bool Editor::revealDiffTarget(const FollowTarget& target,
                              NavigationClass classification) {
    if (target.deleted) {
        return false;
    }
    const auto opened = workspace.openFile(target.path.generic_string());
    if (!opened.accepted() || !opened.document ||
        !activateDocument(*opened.document).accepted) {
        return false;
    }
    return gitDiffIngress.revealCurrentDiffTarget(target, classification);
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

WorkspacePreviewResult Editor::workspacePreview(WorkspaceReplaceArguments args) {
    std::lock_guard g{operationMutex};
    const auto revisionsBefore = documentRevisions(workspace);
    screen.refreshExternalModificationPresence(externalModificationPresent());
    auto r = previewWorkspaceReplace(
        snapshot(++workspaceReplaceGeneration), args.request, args.replacement);
    if (r.accepted()) workspaceReplacePreview = r.preview;
    reconcileFindDocument();
    screen.refreshNoticePresence(noticePresent());
    screen.refreshExternalModificationPresence(externalModificationPresent());
    screen.refreshStatusActions(status.actionNodes());
    if (r.accepted() && existingDocumentMutated(revisionsBefore, workspace)) {
        (void)follow.notifyLocalEdit();
    }
    return r;
}

WorkspaceApplyResult Editor::workspaceApply(
    std::optional<WorkspaceReplacePreview> expected) {
    std::lock_guard g{operationMutex};
    const auto revisionsBefore = documentRevisions(workspace);
    screen.refreshExternalModificationPresence(externalModificationPresent());
    WorkspaceApplyResult result = [&] {
        auto const* preview = workspaceReplacePreview
            ? &*workspaceReplacePreview : nullptr;
        if (preview == nullptr) {
            return WorkspaceApplyResult{FindReplaceError::WorkspaceRejected, 0,
                "workspace apply requires a workspace replace preview",
                };
        }
        if (expected && *expected != *preview) {
            return WorkspaceApplyResult{FindReplaceError::WorkspaceRejected, 0,
                "workspace apply input does not match the current preview",
                };
        }
        auto r = applyWorkspaceReplace(*preview);
        if (!r.accepted()) {
            return r;
        }
        workspaceReplacePreview.reset();
        (void)refreshTree();
        return r;
    }();
    reconcileFindDocument();
    screen.refreshNoticePresence(noticePresent());
    screen.refreshExternalModificationPresence(externalModificationPresent());
    screen.refreshStatusActions(status.actionNodes());
    if (result.accepted() && existingDocumentMutated(revisionsBefore, workspace)) {
        (void)follow.notifyLocalEdit();
    }
    return result;
}

WorkspaceSearchState Editor::workspaceSearch(std::string query) {
    std::lock_guard g{operationMutex};
    const auto sourceGeneration = ++workspaceSearchGeneration;
    startWorkspaceSearch(std::move(query), sourceGeneration);
    return *workspaceSearchState;
}

FindReplaceOperationResult Editor::updateFindQuery(std::string query) {
    std::lock_guard g{operationMutex};
    auto r = applyFindQuery(*this, std::move(query));
    return r;
}

FindReplaceOperationResult Editor::updateReplacement(std::string replacement) {
    std::lock_guard g{operationMutex};
    auto r = applyReplacement(*this, std::move(replacement));
    return r;
}

EditorCreateResult createEditor(EditorConfig config) {
    try {
        auto cwd = canonicalDirectory(config.cwd);
        if (config.scratchRoot.empty()) config.scratchRoot = cwd / ".ssg" / "scratch";
        if (config.recoveryRoot.empty()) config.recoveryRoot = cwd / ".ssg" / "recovery";
        if (config.archiveRoot.empty()) config.archiveRoot = cwd / ".ssg" / "archive";
        const auto scratchCreated = ensureDirectory(config.scratchRoot);
        if (!scratchCreated.ok()) {
            throw std::runtime_error(scratchCreated.message);
        }
        const auto recoveryCreated = ensureDirectory(config.recoveryRoot);
        if (!recoveryCreated.ok()) {
            throw std::runtime_error(recoveryCreated.message);
        }
        auto editor = std::unique_ptr<Editor>{new Editor{
            cwd, config.scratchRoot, config.recoveryRoot, config.archiveRoot,
            config.deferEnrichment, std::move(config.syntaxParser),
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
    return {gitDiffIngress.drainGitDiffWorker()};
}

bool Editor::dispatchInProgress() const noexcept {
    return commands.dispatchInProgress();
}

bool Editor::deferDispatch(std::string commandId) {
    if (!commands.dispatchInProgress()) return false;
    return deferredCommands.enqueue({std::move(commandId)});
}

CommandResult Editor::dispatchLocked(std::string_view commandId) {
    if (workspaceSearchPending() && commandId != "search.workspace") {
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
    const auto dispatchAndReconcile = [&](std::string_view dispatched) {
        const auto revisionsBefore = documentRevisions(workspace);
        screen.refreshExternalModificationPresence(
            externalModificationPresent());
        auto result = commands.dispatch(dispatched);
        reconcileFindDocument();
        screen.refreshNoticePresence(noticePresent());
        screen.refreshExternalModificationPresence(
            externalModificationPresent());
        screen.refreshStatusActions(status.actionNodes());
        if (result.accepted() &&
            existingDocumentMutated(revisionsBefore, workspace)) {
            (void)follow.notifyLocalEdit();
        }
        return result;
    };
    const auto dispatchAndDrain = [&](std::string_view dispatched) {
        auto outcome = dispatchAndReconcile(dispatched);
        if (!outcome.accepted()) {
            deferredCommands.clear();
            return outcome;
        }
        while (!deferredCommands.empty()) {
            auto deferred = deferredCommands.takeFront();
            auto deferredResult = dispatchAndReconcile(deferred.id);
            if (!deferredResult.accepted()) {
                deferredCommands.clear();
                return CommandResult{
                    deferredResult.error,
                    deferred.id + ": " +
                        deferredResult.message, std::nullopt};
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
    return std::visit(
        [&](auto route) {
            return executeInputRoute(*this, std::move(route),
                                     std::move(routed));
        },
        std::move(routed.action));
}

CommandResult Editor::dispatchById(std::string_view commandId) {
    // A handler must be refused before taking the non-recursive aggregate lock.
    if (commands.dispatchInProgress()) {
        return {CommandError::HandlerFailed,
                std::string{kNestedDispatchRefusal}};
    }
    std::lock_guard operationLock{operationMutex};
    return dispatchLocked(commandId);
}

CommandResult Editor::dispatch(std::string_view commandId) {
    return dispatchById(commandId);
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
