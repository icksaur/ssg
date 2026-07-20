#include "runtime/editor_runtime_internal.h"

#include <ssg/layout.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace ssg {
namespace {

ThemeSnapshot defaultTheme() {
    // Readable dark theme derived from the VSCode-style palette in
    // caco/public/themes/dark.css.  Low indices are dark fills, high indices
    // are light text, hues sit in the middle.  Role assignments keep every
    // co_visible_role_pairs member on a distinct palette index.
    ThemeSnapshot snapshot{};
    constexpr std::array<std::array<std::uint8_t, 3>, kThemePaletteSize>
        palette{{
            {30, 30, 30},
            {212, 212, 212},
            {62, 62, 66},
            {133, 133, 133},
            {77, 170, 252},
            {229, 192, 123},
            {239, 74, 74},
            {76, 175, 80},
            {171, 71, 188},
            {38, 192, 192},
            {212, 149, 106},
            {209, 109, 158},
            {187, 187, 187},
            {106, 106, 106},
            {232, 232, 232},
            {255, 255, 255},
        }};
    for (std::size_t index = 0; index < snapshot.palette.size(); ++index) {
        snapshot.palette[index] = SrgbColor::fromSerializedChannels(
            palette[index][0], palette[index][1], palette[index][2]);
    }

    auto role = [&](SemanticRole which, std::uint8_t index) {
        snapshot.semanticIndices[static_cast<std::size_t>(which)] = index;
    };
    role(SemanticRole::Foreground, 1);
    role(SemanticRole::Background, 0);
    role(SemanticRole::Caret, 15);
    role(SemanticRole::Selection, 4);
    role(SemanticRole::DiagnosticError, 6);
    role(SemanticRole::DiagnosticWarning, 5);
    role(SemanticRole::DiagnosticInfo, 9);
    role(SemanticRole::DiagnosticHint, 8);
    role(SemanticRole::GitAdded, 7);
    role(SemanticRole::GitModified, 10);
    role(SemanticRole::GitDeleted, 6);
    role(SemanticRole::GitConflict, 11);
    role(SemanticRole::TreeBackground, 2);
    role(SemanticRole::TreeFocus, 4);
    role(SemanticRole::TabActive, 4);
    role(SemanticRole::TabInactive, 13);
    role(SemanticRole::PanelActive, 9);
    role(SemanticRole::PanelInactive, 3);
    role(SemanticRole::Header, 12);
    role(SemanticRole::Footer, 12);
    role(SemanticRole::StatusInfo, 9);
    role(SemanticRole::StatusWarning, 5);
    role(SemanticRole::StatusError, 6);
    role(SemanticRole::LineNumber, 3);
    role(SemanticRole::ActiveLineNumber, 14);
    role(SemanticRole::SearchMatch, 10);
    role(SemanticRole::Prompt, 8);
    role(SemanticRole::ScrollbarTrack, 2);
    role(SemanticRole::ScrollbarThumb, 13);
    role(SemanticRole::DiffAdded, 7);
    role(SemanticRole::DiffRemoved, 6);
    role(SemanticRole::DiffModified, 10);

    auto syntax = [&](SyntaxScope scope, std::uint8_t index) {
        snapshot.syntaxIndices[static_cast<std::size_t>(scope)] = index;
    };
    syntax(SyntaxScope::PlainText, 1);
    syntax(SyntaxScope::Comment, 3);
    syntax(SyntaxScope::Keyword, 8);
    syntax(SyntaxScope::String, 7);
    syntax(SyntaxScope::Number, 10);
    syntax(SyntaxScope::Type, 9);
    syntax(SyntaxScope::Function, 4);
    syntax(SyntaxScope::Variable, 1);
    syntax(SyntaxScope::OperatorToken, 5);
    syntax(SyntaxScope::Punctuation, 14);
    syntax(SyntaxScope::Invalid, 6);
    return snapshot;
}

// The curated terminal runtime keymap (doc/spec-keymap.md K2): a small set of
// argument-free bindings the TUI drives, plus the context-divergent navigation
// keys.  Only argument-free-usable commands are bound (a bare chord dispatches
// with no payload); exhaustive reachability is the palette's job.  Global (*)
// chords are Escape-led and prefix-free; single strokes differ per focus.
KeymapViewState defaultTerminalKeymap() {
    auto seq = [](std::initializer_list<std::string_view> strokes) {
        auto parsed = KeyCodec{}.parseSequence(strokes);
        if (!parsed) throw std::logic_error{"curated keymap has an invalid stroke"};
        return *parsed;
    };
    KeymapViewState keymap{"default", {}};
    auto bind = [&](KeySequence sequence, std::string command,
                    std::string context) {
        keymap.bindings.push_back(
            {std::move(sequence), std::move(command), std::move(context)});
    };

    bind(seq({"Escape", "KeyS"}), "file.save", "*");
    bind(seq({"Escape", "KeyZ"}), "edit.undo", "*");
    bind(seq({"Escape", "Shift+KeyZ"}), "edit.redo", "*");
    bind(seq({"Escape", "KeyP"}), "palette.open", "*");
    bind(seq({"Escape", "KeyB"}), "panel.toggle", "*");
    bind(seq({"Escape", "KeyO"}), "panel.focus", "*");
    bind(seq({"Escape", "BracketRight"}), "tab.next", "*");
    bind(seq({"Escape", "BracketLeft"}), "tab.previous", "*");
    bind(seq({"Escape", "KeyW"}), "tab.close", "*");
    bind(seq({"Escape", "KeyF", "KeyT"}), "settings.open", "*");
    bind(seq({"Escape", "KeyA"}), "select.all", "*");
    bind(seq({"Escape", "KeyD"}), "select.add_next_occurrence", "*");
    bind(seq({"Escape", "KeyI"}), "select.split_into_lines", "*");
    bind(seq({"Escape", "KeyK"}), "select.add_cursor_up", "*");
    bind(seq({"Escape", "KeyJ"}), "select.add_cursor_down", "*");
    bind(seq({"Escape", "Slash"}), "find.open", "*");
    bind(seq({"Escape", "KeyR"}), "replace.open", "*");

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
    bind(seq({"Ctrl+Home"}), "cursor.document_start", "editor");
    bind(seq({"Ctrl+End"}), "cursor.document_end", "editor");
    bind(seq({"Ctrl+Shift+Home"}), "select.document_start", "editor");
    bind(seq({"Ctrl+Shift+End"}), "select.document_end", "editor");
    bind(seq({"PageUp"}), "cursor.page_up", "editor");
    bind(seq({"PageDown"}), "cursor.page_down", "editor");
    bind(seq({"Shift+PageUp"}), "select.page_up", "editor");
    bind(seq({"Shift+PageDown"}), "select.page_down", "editor");
    bind(seq({"Enter"}), "text.newline", "editor");
    bind(seq({"Backspace"}), "text.delete_backward", "editor");

    bind(seq({"ArrowDown"}), "tree.select_next", "panel");
    bind(seq({"ArrowUp"}), "tree.select_previous", "panel");
    bind(seq({"Enter"}), "tree.activate", "panel");

    bind(seq({"Enter"}), "prompt.submit", "prompt");
    bind(seq({"Escape", "Escape"}), "prompt.cancel", "prompt");
    bind(seq({"ArrowDown"}), "palette.next", "prompt");
    bind(seq({"ArrowUp"}), "palette.previous", "prompt");
    // Find/replace option toggles and replace-all, reachable while a find or
    // replace prompt is focused.  KeyC/KeyG/KeyE/KeyL are not in the `*` chord
    // set, so these Escape-prefixed chords stay prefix-free.  The handlers are
    // benign no-ops unless a find/replace prompt is active.
    bind(seq({"Escape", "KeyC"}), "find.toggle_case", "prompt");
    bind(seq({"Escape", "KeyG"}), "find.toggle_whole_word", "prompt");
    bind(seq({"Escape", "KeyE"}), "find.toggle_regex", "prompt");
    bind(seq({"Escape", "KeyL"}), "replace.all", "prompt");

    return keymap;
}

DocumentPosition zeroPosition() {
    return {ByteOffset{0}, LineIndex{0}, CellIndex{0}};
}

SelectionViewState initialSelection() {
    auto zero = zeroPosition();
    return {SelectionSet{std::vector<Selection>{Selection{zero, zero}}}, 0, 0, std::nullopt};
}

std::filesystem::path canonicalDirectory(std::filesystem::path const& path) {
    std::error_code code;
    auto canonical = std::filesystem::canonical(path, code);
    if (code || !std::filesystem::is_directory(canonical)) {
        throw std::invalid_argument{"workspace root must be an existing directory"};
    }
    return canonical;
}

std::string readFileText(std::filesystem::path const& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::optional<std::filesystem::path> pathFromUri(std::string_view uri) {
    constexpr std::string_view prefix{"file://"};
    if (uri.rfind(prefix, 0) != 0) return std::nullopt;
    return std::filesystem::path{std::string{uri.substr(prefix.size())}};
}

std::string uriFromPath(std::filesystem::path const& path) {
    return "file://" + path.generic_string();
}

std::optional<std::string> relativeToRoot(std::filesystem::path const& root,
                                            std::filesystem::path const& path) {
    auto relative = path.lexically_relative(root);
    if (relative.empty()) return std::nullopt;
    for (auto const& part : relative) {
        if (part == "..") return std::nullopt;
    }
    return relative.generic_string();
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
    auto candidate = std::filesystem::weakly_canonical(root / supplied, code);
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

} // namespace

CommandHandlerResult success() { return CommandHandlerResult::success(); }
CommandHandlerResult failure(std::string message) {
    return CommandHandlerResult::failure(std::move(message));
}

std::string wrongPayload(std::string_view commandId) {
    return std::string{commandId} + " payload has the wrong type";
}

std::string workspaceMessage(WorkspaceResult const& result) {
    return result.message.empty() ? "workspace operation failed" : result.message;
}

std::string tabMessage(TabResult const& result) {
    return result.message.empty() ? "tab operation failed" : result.message;
}

EditorRuntime::Impl::Impl(std::filesystem::path canonicalCwd,
                          std::filesystem::path scratchRoot,
                          std::filesystem::path recoveryRoot,
                          bool deferEnrichment)
    : root{std::move(canonicalCwd)},
      scratchRoot{std::filesystem::weakly_canonical(scratchRoot)},
      recoveryRoot{std::filesystem::weakly_canonical(recoveryRoot)},
      recovery{RecoveryActions::create(recoveryRoot)},
      scratch{ScratchStore::create(scratchRoot, root)},
      workspace{Workspace::create(root, recovery)},
      selection{initialSelection()},
      clipboard{4},
      shell{{"Files", "Git", "Symbols"}},
      tabs{*this},
      external{recovery, diff},
      syntax{},
      search{*this, *this},
      theme{defaultTheme()},
      deferringEnrichment{deferEnrichment} {
    refreshTree();
    refreshSyntax();
}

CommandHandlerResult EditorRuntime::Impl::runTransaction(
    std::function<CommandHandlerResult()> operation) {
    return operation();
}

std::any& EditorRuntime::Impl::featureStateValue(std::type_index) {
    throw std::logic_error{"EditorRuntime exposes feature state through snapshots"};
}

void EditorRuntime::Impl::publishStatusValue(std::type_index, std::any statusValue) {
    if (auto const* item = std::any_cast<StatusItem>(&statusValue)) {
        (void)status.enqueue(*item);
    }
}

void EditorRuntime::Impl::publishDeltaValue(std::type_index, std::any) {}

TabLifecycleResult EditorRuntime::Impl::close(
    const TabState& tab, std::chrono::milliseconds durabilityTimeout) {
    if (!tab.document) return {};
    auto state = workspace.state(*tab.document);
    if (!state) return {TabError::NotFound, "tab document does not exist", std::nullopt, false};
    std::optional<JournalDocument> document;
    if (auto const* current = activeDocument(); current != nullptr) {
        document = JournalDocument{state->key, current->mode(), state->dirty,
                                   current->snapshot().text};
    }
    auto closed = recovery.closeDocument(document, scratch, durabilityTimeout);
    if (!closed.accepted()) {
        return {TabError::LifecycleFailed, closed.error->message, std::nullopt, false};
    }
    if (document) scratch.removeDocument(state->key);
    return {TabError::None, {}, closed.compensation, scratch.waitUntilDurable(durabilityTimeout)};
}

TabLifecycleResult EditorRuntime::Impl::reopen(
    const TabState&, const RecoveryRecordId& compensation) {
    auto restored = workspace.restore(compensation);
    if (!restored.accepted()) {
        return {TabError::LifecycleFailed, workspaceMessage(restored), std::nullopt, false};
    }
    return {};
}

WorkspaceSnapshot EditorRuntime::Impl::snapshot(Revision revision) const {
    WorkspaceSnapshot result;
    result.revision = revision;
    for (auto const id : workspace.documents()) {
        auto state = workspace.state(id);
        if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) continue;
        result.files.push_back({state->key.savedPath(), workspace.document(id).snapshot().text});
    }
    std::filesystem::recursive_directory_iterator it{root};
    std::filesystem::recursive_directory_iterator end;
    for (; it != end; ++it) {
        auto const& entry = *it;
        if (entry.is_directory() &&
            (pathContains(scratchRoot, entry.path()) ||
             pathContains(recoveryRoot, entry.path()))) {
            it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file()) continue;
        auto relative = relativeToRoot(root, entry.path());
        if (!relative) continue;
        if (std::find_if(result.files.begin(), result.files.end(), [&](WorkspaceFile const& file) {
                return file.path == *relative;
            }) != result.files.end()) {
            continue;
        }
        result.files.push_back({*relative, readFileText(entry.path())});
    }
    return result;
}

std::vector<SearchCommandDescriptor> EditorRuntime::Impl::descriptors() const {
    std::vector<SearchCommandDescriptor> result;
    for (auto const& descriptor : p0CommandDescriptors()) {
        result.push_back({descriptor.id, descriptor.id});
    }
    return result;
}

PaletteExecutionResult EditorRuntime::Impl::execute(std::string_view commandId) {
    auto descriptors = p0CommandDescriptors();
    return {std::find_if(descriptors.begin(), descriptors.end(),
                         [&](CommandDescriptor const& descriptor) { return descriptor.id == commandId; }) !=
                descriptors.end(),
            {}};
}

WorkspaceApplyResult EditorRuntime::Impl::apply(
    const WorkspaceReplacePreview& preview, WorkspaceRecoverySink& recoverySink) {
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
            pathContains(recoveryRoot, *path)) {
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
            current = readFileText(*path);
        }
        if (current != change.before) {
            return {FindReplaceError::StaleRevision, preview.sourceRevision,
                    "workspace replacement preview is stale"};
        }
        paths.push_back(std::move(*path));
        normalizedPaths.push_back(std::move(normalized));
    }
    WorkspaceRecoveryRecord record{preview.sourceRevision, Revision{preview.sourceRevision.value() + 1}, preview.changes};
    if (!recoverySink.store(record)) {
        return {FindReplaceError::RecoveryRejected, preview.sourceRevision, "workspace replacement recovery rejected"};
    }
    for (std::size_t index = 0; index < preview.changes.size(); ++index) {
        auto const& change = preview.changes[index];
        std::ofstream output{paths[index], std::ios::binary | std::ios::trunc};
        if (!output) return {FindReplaceError::WorkspaceRejected, preview.sourceRevision, "failed to write workspace file"};
        output << change.after;
    }
    for (std::size_t index = 0; index < preview.changes.size(); ++index) {
        auto const& change = preview.changes[index];
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
    return {FindReplaceError::None, record.appliedRevision, {}};
}

WorkspaceApplyResult EditorRuntime::Impl::recover(const WorkspaceRecoveryRecord& record) {
    for (auto const& change : record.changes) {
        std::ofstream output{root / change.path, std::ios::binary | std::ios::trunc};
        if (!output) return {FindReplaceError::WorkspaceRejected, record.appliedRevision, "failed to recover workspace file"};
        output << change.before;
    }
    return {FindReplaceError::None, record.appliedRevision, {}};
}

bool EditorRuntime::Impl::store(const WorkspaceRecoveryRecord&) { return true; }

std::optional<LspDocumentSnapshot> EditorRuntime::Impl::snapshot(std::string_view uri) const {
    auto path = pathFromUri(uri);
    if (!path) return std::nullopt;
    for (auto const id : workspace.documents()) {
        auto state = workspace.state(id);
        if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) continue;
        if (uriFromPath(root / state->key.savedPath()) == uri) {
            return LspDocumentSnapshot{std::string{uri}, workspace.document(id).revision(), 1,
                                       workspace.document(id).snapshot().text};
        }
    }
    return std::nullopt;
}

LspWorkspaceDocumentWriteResult EditorRuntime::Impl::apply(
    std::string uri, Revision expectedRevision, std::string text) {
    for (auto const id : workspace.documents()) {
        auto state = workspace.state(id);
        if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) continue;
        if (uriFromPath(root / state->key.savedPath()) != uri) continue;
        auto& document = const_cast<Document&>(workspace.document(id));
        if (document.revision() != expectedRevision) {
            return {document.revision(), LspWorkspaceDocumentError::StaleRevision, "document revision is stale"};
        }
        auto snapshot = document.snapshot();
        auto result = document.apply({snapshot.revision, {{ByteOffset{0}, snapshot.text.size(), std::move(text)}}});
        if (!result.accepted()) return {document.revision(), LspWorkspaceDocumentError::WriteFailed, result.message};
        return {result.revision, LspWorkspaceDocumentError::None, {}};
    }
    return {Revision{0}, LspWorkspaceDocumentError::UnknownDocument, "document URI is not open"};
}

LspWorkspaceFileResult EditorRuntime::Impl::snapshot(std::string_view uri, LspWorkspaceFileNode& node) const {
    auto path = pathFromUri(uri);
    if (!path) return {LspWorkspaceFileError::NotFound, "URI is not a file URI"};
    if (!std::filesystem::exists(*path)) {
        node.kind = LspWorkspaceFileNodeKind::Missing;
    } else if (std::filesystem::is_directory(*path)) {
        node.kind = LspWorkspaceFileNodeKind::Directory;
    } else {
        node.kind = LspWorkspaceFileNodeKind::File;
        node.content = readFileText(*path);
    }
    return {};
}

LspWorkspaceFileResult EditorRuntime::Impl::createFile(std::string uri, bool overwrite) {
    auto path = pathFromUri(uri);
    if (!path) return {LspWorkspaceFileError::IoError, "URI is not a file URI"};
    if (std::filesystem::exists(*path) && !overwrite) return {LspWorkspaceFileError::AlreadyExists, "file already exists"};
    std::ofstream output{*path, std::ios::binary | std::ios::trunc};
    return output ? LspWorkspaceFileResult{} : LspWorkspaceFileResult{LspWorkspaceFileError::IoError, "failed to create file"};
}

LspWorkspaceFileResult EditorRuntime::Impl::writeFile(std::string uri, std::string content) {
    auto path = pathFromUri(uri);
    if (!path) return {LspWorkspaceFileError::IoError, "URI is not a file URI"};
    std::ofstream output{*path, std::ios::binary | std::ios::trunc};
    if (!output) return {LspWorkspaceFileError::IoError, "failed to write file"};
    output << content;
    return {};
}

LspWorkspaceFileResult EditorRuntime::Impl::renamePath(std::string oldUri, std::string newUri, bool overwrite) {
    auto oldPath = pathFromUri(oldUri);
    auto newPath = pathFromUri(newUri);
    if (!oldPath || !newPath) return {LspWorkspaceFileError::IoError, "URI is not a file URI"};
    if (std::filesystem::exists(*newPath) && !overwrite) return {LspWorkspaceFileError::AlreadyExists, "destination exists"};
    std::error_code code;
    std::filesystem::rename(*oldPath, *newPath, code);
    return code ? LspWorkspaceFileResult{LspWorkspaceFileError::IoError, code.message()} : LspWorkspaceFileResult{};
}

LspWorkspaceFileResult EditorRuntime::Impl::deletePath(std::string uri, bool recursive) {
    auto path = pathFromUri(uri);
    if (!path) return {LspWorkspaceFileError::IoError, "URI is not a file URI"};
    std::error_code code;
    if (recursive) std::filesystem::remove_all(*path, code);
    else std::filesystem::remove(*path, code);
    return code ? LspWorkspaceFileResult{LspWorkspaceFileError::IoError, code.message()} : LspWorkspaceFileResult{};
}

LspWorkspaceFileResult EditorRuntime::Impl::restorePath(std::string uri, const LspWorkspaceFileNode& node) {
    auto path = pathFromUri(uri);
    if (!path) return {LspWorkspaceFileError::IoError, "URI is not a file URI"};
    if (node.kind == LspWorkspaceFileNodeKind::Missing) {
        std::error_code code;
        std::filesystem::remove_all(*path, code);
        return code ? LspWorkspaceFileResult{LspWorkspaceFileError::IoError, code.message()} : LspWorkspaceFileResult{};
    }
    if (node.kind == LspWorkspaceFileNodeKind::Directory) {
        std::error_code code;
        std::filesystem::create_directories(*path, code);
        return code ? LspWorkspaceFileResult{LspWorkspaceFileError::IoError, code.message()} : LspWorkspaceFileResult{};
    }
    return writeFile(std::move(uri), node.content);
}

std::optional<FileDocumentId> EditorRuntime::Impl::activeDocumentId() const {
    // The active tab is the single source of truth for the active editor
    // document.  With no active tab (e.g. the last tab was closed) there is no
    // active document and the shell renders its empty state; the editor view
    // never shows a document that has no tab.
    auto const& view = tabs.viewState();
    if (!view.active) return std::nullopt;
    auto found = std::find_if(view.tabs.begin(), view.tabs.end(), [&](TabState const& tab) {
        return tab.id == *view.active;
    });
    if (found != view.tabs.end()) return found->document;
    return std::nullopt;
}

Document const* EditorRuntime::Impl::activeDocument() const {
    auto id = activeDocumentId();
    return id ? &workspace.document(*id) : nullptr;
}

Document* EditorRuntime::Impl::activeDocument() {
    auto id = activeDocumentId();
    return id ? const_cast<Document*>(&workspace.document(*id)) : nullptr;
}

DocumentHistory& EditorRuntime::Impl::historyFor(FileDocumentId document) {
    auto [it, inserted] = histories.try_emplace(document.value(), HistoryConfig::defaults());
    return it->second;
}

std::optional<WorkspaceDocumentState> EditorRuntime::Impl::activeWorkspaceState() const {
    auto id = activeDocumentId();
    return id ? workspace.state(*id) : std::nullopt;
}

std::string EditorRuntime::Impl::activeText() const {
    auto const* document = activeDocument();
    return document ? document->snapshot().text : std::string{};
}

void EditorRuntime::Impl::resetSelectionForActiveDocument() {
    selection = initialSelection();
    requestedFirstVisualRow = 0;
}

void EditorRuntime::Impl::clampSelectionToActiveDocument() {
    auto text = activeText();
    auto offset = selection.selections.primary().active.byteOffset.value();
    if (offset > text.size()) offset = text.size();
    auto position = resolveDocumentPosition(text, ByteOffset{offset}).value_or(zeroPosition());
    selection.selections = SelectionSet{std::vector<Selection>{Selection{position, position}}};
}

std::vector<CellRun> EditorRuntime::Impl::activeCellRuns() const {
    std::vector<CellRun> runs;
    std::string const text = activeText();
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string::npos ? end : end - start);
        runs.push_back(computeCellRun(line, 4));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (runs.empty()) runs.push_back(computeCellRun("", 4));
    return runs;
}

ViewportViewState EditorRuntime::Impl::computeEditorViewport(
    ViewportDimensions dimensions, std::uint32_t firstRow,
    std::uint32_t firstColumn) const {
    if (wordWrap) {
        auto runs = activeCellRuns();
        return computeViewport(runs, dimensions, firstRow);
    }
    // Word wrap off (default): one logical line is one visual row; only the
    // visible lines are segmented, so this is O(visible rows), not O(document).
    return computeViewportUnwrapped(activeText(), dimensions, firstRow,
                                      firstColumn, 4);
}

ViewportViewState EditorRuntime::Impl::viewport(ViewportDimensions dimensions) const {
    return computeEditorViewport(dimensions, requestedFirstVisualRow,
                                   requestedFirstVisualColumn);
}

void EditorRuntime::Impl::refreshTree() {
    if (deferringEnrichment) {
        pendingTreeRefresh = true;
        return;
    }
    ++treeScanCount;
    tree.replaceProvider(filesystemTreeSnapshot(
        TreeProviderId{"filesystem"}, root, TreeRevision{nextTreeRevision++}));
}

void EditorRuntime::Impl::reconcilePromptFocus() {
    if (prompt.active() && shell.focus() != FocusTarget::Prompt) {
        shell.enterPromptFocus();
    } else if (!prompt.active() && shell.focus() == FocusTarget::Prompt) {
        shell.exitPromptFocus();
    }
}

void EditorRuntime::Impl::reconcileFindDocument() {
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
    // The document the find evaluated against is gone, changed, or was edited:
    // close the controller and dismiss its prompt so no stale match is navigable.
    findReplace.close();
    if (auto const& request = prompt.request();
        request && (request->kind == PromptKind::Find ||
                    request->kind == PromptKind::Replace)) {
        (void)prompt.cancel();
    }
    findDocumentId.reset();
}

void EditorRuntime::Impl::refreshSyntax() {
    if (deferringEnrichment) {
        pendingSyntaxRefresh = true;
        return;
    }
    ++syntaxRunCount;
    auto const* document = activeDocument();
    auto text = document ? document->snapshot().text : std::string{};
    auto revision = document ? document->revision() : Revision{0};
    auto request = syntax.request(revision, LanguageId::plainText(), std::move(text));
    if (request.accepted()) {
        auto output = syntax.run(*request.request);
        (void)syntax.accept(request.request, output);
    }
}

void EditorRuntime::Impl::primeDeferred() {
    if (!deferringEnrichment) return;
    deferringEnrichment = false;
    // Run whichever scans were requested while deferring, now that the first
    // frame is drawn.  Order: tree then syntax (independent; both publish through
    // the normal snapshot channel on the next snapshot).
    bool ran = false;
    if (pendingTreeRefresh) {
        pendingTreeRefresh = false;
        refreshTree();
        ran = true;
    }
    if (pendingSyntaxRefresh) {
        pendingSyntaxRefresh = false;
        refreshSyntax();
        ran = true;
    }
    // Advance the session revision so delta-based clients observe the primed
    // enrichment; a same-revision snapshot pair yields no delta (derive_session_
    // delta rejects it), so without this a WebSocket client would miss it.
    if (ran && session) session->advanceRevision();
}

void EditorRuntime::Impl::enqueueStatus(StatusPriority priority, std::string text) {
    auto value = nextStatusId++;
    (void)status.enqueue(StatusItem{StatusId{value}, priority, std::move(text), {}});
}

EditorRuntime::EditorRuntime(std::unique_ptr<Impl> implementation) noexcept
    : impl_{std::move(implementation)} {}
EditorRuntime::~EditorRuntime() = default;

EditorRuntimeCreateResult EditorRuntime::create(EditorRuntimeConfig config) {
    try {
        auto cwd = canonicalDirectory(config.cwd);
        if (config.scratchRoot.empty()) config.scratchRoot = cwd / ".ssg" / "scratch";
        if (config.recoveryRoot.empty()) config.recoveryRoot = cwd / ".ssg" / "recovery";
        std::filesystem::create_directories(config.scratchRoot);
        std::filesystem::create_directories(config.recoveryRoot);
        auto impl = std::make_unique<Impl>(cwd, config.scratchRoot,
                                           config.recoveryRoot,
                                           config.deferEnrichment);
        impl->keymap = defaultTerminalKeymap();
        if (auto errors = KeymapMatcher{impl->keymap}.validate({}); !errors.empty()) {
            return {nullptr, "default keymap is invalid: " + errors.front().message};
        }
        if (!KeymapMatcher{impl->keymap}.hasGlobalBinding("settings.open", {})) {
            return {nullptr,
                    "default keymap lacks a global settings.open escape hatch"};
        }
        EditorSessionBuilder builder;
        builder.services(*impl);
        bindRuntimeEditing(builder, *impl);
        bindRuntimeFiles(builder, *impl);
        bindRuntimePresentation(builder, *impl);
        bindRuntimeNavigation(builder, *impl);
        bindRuntimeLanguageServices(builder, *impl);
        impl->session = builder.build();
        return {std::unique_ptr<EditorRuntime>{new EditorRuntime{std::move(impl)}}, {}};
    } catch (std::exception const& exception) {
        return {nullptr, exception.what()};
    }
}

AttachResult EditorRuntime::attach(InvocationPrincipal principal, ViewId viewId) {
    auto clientId = principal.clientId();
    auto result = impl_->session->attach(std::move(principal), viewId);
    if (result.accepted()) {
        (void)impl_->follow.attachClient(clientId, ViewportDimensions{80, 24});
    }
    return result;
}

bool EditorRuntime::detach(ClientId clientId) {
    (void)impl_->follow.detachClient(clientId);
    return impl_->session->detach(clientId);
}

void EditorRuntime::primeDeferred() { impl_->primeDeferred(); }

EditorRuntime::DeferredWorkCounts EditorRuntime::deferredWorkCounts() const {
    return {impl_->syntaxRunCount, impl_->treeScanCount};
}

CommandResult EditorRuntime::dispatch(ClientId clientId, ClientCommand const& command) {
    auto result = impl_->session->dispatch(clientId, command);
    impl_->reconcileFindDocument();
    impl_->reconcilePromptFocus();
    // palette.execute validates the selected candidate then defers execution to
    // here so the target runs through the registry (with its own capability and
    // revision checks) outside the non-reentrant session lock.
    if (result.accepted() && impl_->pendingPaletteTarget) {
        auto target = std::move(*impl_->pendingPaletteTarget);
        impl_->pendingPaletteTarget.reset();
        auto targetResult = impl_->session->dispatch(
            clientId, {target, impl_->session->revision(), {}});
        impl_->reconcileFindDocument();
        impl_->reconcilePromptFocus();
        return targetResult;
    }
    return result;
}

Revision EditorRuntime::revision() const { return impl_->session->revision(); }
std::filesystem::path const& EditorRuntime::workspaceRoot() const noexcept { return impl_->root; }
std::optional<SessionSnapshot> EditorRuntime::snapshot(ClientId clientId, ViewportDimensions dimensions,
                                                       KeySequence leaderPending,
                                                       PaletteReport paletteReport) const {
    auto client = impl_->session->attachedClient(clientId);
    if (!client) return std::nullopt;
    return assembleSessionSnapshot(impl_->session->revision(), impl_->session->topology(),
                                     client->principal, client->viewId,
                                     impl_->viewport(dimensions),
                                     impl_->sections(dimensions, leaderPending, paletteReport));
}

std::string EditorRuntime::activeDocumentText() const { return impl_->activeText(); }

} // namespace ssg
