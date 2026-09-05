#include <ssg/EditorSessionImpl.h>
#include <ssg/CommandCatalog.h>
#include <ssg/DraftReopenClassifier.h>
#include <ssg/FilesystemWatcher.h>
#include <ssg/GraphemeLayout.h>
#include <ssg/Style.h>
#include <ssg/ScreenLayout.h>
#include <ssg/platform_files.h>

#include <algorithm>
#include <array>
#include <span>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <limits>
#include <iterator>
#include <stdexcept>
#include <unordered_map>

namespace ssg {
namespace {

// First-frame syntax should be ready when it's cheap: parsing a small file is
// comfortably within startup budget, while multi-MB input can exceed it and is
// deferred until primeDeferred().
constexpr std::size_t kEagerSyntaxMaxBytes = 2 * 1024 * 1024;

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

std::string liveDiffTabLabelForPath(const std::filesystem::path& path) {
    const auto filename = path.filename().string();
    return filename.empty() ? path.generic_string() : filename;
}

// The user's home directory for the header path field's "~" abbreviation.
// HOME first (POSIX), then USERPROFILE (Windows); trailing separators are
// stripped so a home value like "/home/user/" still matches "/home/user/repo".
// Empty when unknown, which disables the abbreviation.
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
    // The path field compares against workspaceRoot.generic_string() ('/'
    // separators on every platform), so normalize a Windows USERPROFILE's
    // backslashes to match.
    for (auto& ch : home) {
        if (ch == '\\') ch = '/';
    }
    return home;
}

std::string liveDiffDocumentText(const DiffFileView& file) {
    // A deleted file's whole content is represented as Removed phantom rows
    // (see Viewport.cpp's removedBlocks/phantom-row projection), never as
    // real document text -- currentContent is already empty for a deleted
    // file (DiffModel::updateGitFile sets it from workingContent, which is
    // absent when deleted). Synthesizing baseline content as the "current"
    // text here would duplicate every removed line: once as a real row from
    // this text, and again as the phantom row the viewport already inserts
    // for the same baseline line.
    return file.currentContent;
}

// The curated terminal runtime keymap: a small set of
// argument-free bindings the TUI drives, plus the context-divergent navigation
// keys.  Only argument-free-usable commands are bound (a bare stroke dispatches
// with no payload); exhaustive reachability is the palette's job.  Every binding
// is a single stroke: global (*) actions are Alt chords, navigation differs per
// focus, and Escape is a plain cancel.
} // namespace

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

    // Frequent actions are single Alt+<key> chords.  In a terminal Alt+X
    // transmits as the bytes ESC X, which decode_input coalesces into one
    // alt=true stroke, so these are the same keys the user already presses -- the
    // former Escape leader is gone, and Escape is now a plain cancel key.
    bind(seq({"Alt+KeyS"}), "file.save", "*");
    bind(seq({"Alt+KeyN"}), "file.new", "*");
    bind(seq({"Alt+KeyZ"}), "edit.undo", "*");
    bind(seq({"Alt+Shift+KeyZ"}), "edit.redo", "*");
    // Alt+p opens the file picker (the frequent action) and Alt+Shift+P the
    // command palette, matching the convention users arrive with.
    bind(seq({"Alt+KeyP"}), "file_finder.open", "*");
    bind(seq({"Alt+Shift+KeyP"}), "palette.open", "*");
    bind(seq({"Alt+KeyB"}), "panel.toggle", "*");
    bind(seq({"Alt+KeyH"}), "help.open", "*");
    bind(seq({"Alt+KeyO"}), "panel.focus", "*");
    // Tab cycling: Alt+BracketRight/Left cannot be used -- ESC ] / ESC [ are the
    // OSC / CSI introducers -- so the brackets give way to Alt+Period/Comma.
    bind(seq({"Alt+Period"}), "tab.next", "*");
    bind(seq({"Alt+Comma"}), "tab.previous", "*");
    bind(seq({"Alt+KeyW"}), "tab.close", "*");
    // The Settings escape hatch (protected: a settings.open binding must always
    // exist) moves from the former three-stroke chord to a single Alt+Shift+T.
    bind(seq({"Alt+Shift+KeyT"}), "settings.open", "*");
    bind(seq({"Alt+KeyA"}), "select.all", "*");
    bind(seq({"Alt+KeyD"}), "select.add_next_occurrence", "*");
    bind(seq({"Alt+KeyI"}), "select.split_into_lines", "*");
    bind(seq({"Alt+KeyK"}), "select.add_cursor_up", "*");
    bind(seq({"Alt+KeyJ"}), "select.add_cursor_down", "*");
    bind(seq({"Alt+Slash"}), "find.open", "*");
    // Alt+8 seeds find with the word under the caret.  Editor-context: it acts on
    // the caret and document.
    bind(seq({"Alt+Digit8"}), "find.word_under_cursor", "editor");
    bind(seq({"Alt+KeyR"}), "replace.open", "*");
    // Draft recovery's "Use disk": discard unsaved edits back to the disk
    // version (the draft is archived first, so this is reversible).
    bind(seq({"Alt+Shift+KeyD"}), "draft.discard", "editor");

    // Cut/copy/paste act on the editor's selection, so they are bound in the
    // editor context; paste is additionally bound in the prompt so a prompt's
    // value can be pasted into.
    bind(seq({"Alt+KeyX"}), "clipboard.cut", "editor");
    bind(seq({"Alt+KeyC"}), "clipboard.copy", "editor");
    bind(seq({"Alt+KeyV"}), "clipboard.paste", "editor");
    // Paste also works while a prompt owns the keyboard -- find, replace, a path,
    // the palette query.  The client fulfils it against the prompt's own text
    // rather than the document, the same way typing into a prompt is routed.
    // Cut and copy are deliberately absent: a prompt's value is client-owned and
    // there is no selection within it to take.
    bind(seq({"Alt+KeyV"}), "clipboard.paste", "prompt");

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
    // Alt+Home/End also jump to the document extremes: the physical Home/End keys
    // are natural for "top/bottom of file", and Alt is the modifier the rest of
    // the editor uses.
    bind(seq({"Alt+Home"}), "cursor.document_start", "editor");
    bind(seq({"Alt+End"}), "cursor.document_end", "editor");
    // Alt+Shift+G opens a prompt for a line number and jumps there (clamped).
    bind(seq({"Alt+Shift+KeyG"}), "goto.line", "editor");
    bind(seq({"Ctrl+Shift+Home"}), "select.document_start", "editor");
    bind(seq({"Ctrl+Shift+End"}), "select.document_end", "editor");
    bind(seq({"PageUp"}), "cursor.page_up", "editor");
    bind(seq({"PageDown"}), "cursor.page_down", "editor");
    bind(seq({"Shift+PageUp"}), "select.page_up", "editor");
    bind(seq({"Shift+PageDown"}), "select.page_down", "editor");
    bind(seq({"Enter"}), "text.newline", "editor");
    bind(seq({"Backspace"}), "text.delete_backward", "editor");
    bind(seq({"Delete"}), "text.delete_forward", "editor");
    // Alt+Backspace deletes the word to the left.  Alt+Backspace transmits as
    // the bytes ESC 0x7f, which decode_input coalesces into one Alt+Backspace
    // stroke.
    bind(seq({"Alt+Backspace"}), "text.delete_word_backward", "editor");
    // Word navigation: Alt+Left/Right (and Shift to extend).  Arrow keys use the
    // CSI modifier-parameter form, which decode_input parses into a single
    // alt=true stroke.
    bind(seq({"Alt+ArrowLeft"}), "cursor.word_left", "editor");
    bind(seq({"Alt+ArrowRight"}), "cursor.word_right", "editor");
    bind(seq({"Alt+Shift+ArrowLeft"}), "select.word_left", "editor");
    bind(seq({"Alt+Shift+ArrowRight"}), "select.word_right", "editor");

    bind(seq({"ArrowDown"}), "tree.select_next", "panel");
    bind(seq({"ArrowUp"}), "tree.select_previous", "panel");
    bind(seq({"Enter"}), "tree.activate", "panel");

    bind(seq({"Enter"}), "prompt.submit", "prompt");
    // A single Escape cancels a focused prompt (find, replace, path, palette).
    // With the leader gone Escape is no longer a chord prefix, so one press is
    // unambiguous.
    bind(seq({"Escape"}), "prompt.cancel", "prompt");
    bind(seq({"ArrowDown"}), "prompt.next", "prompt");
    bind(seq({"ArrowUp"}), "prompt.previous", "prompt");
    // Tab advances the keyboard among a multi-input prompt's inputs (replace's
    // query and replacement); a single-input prompt stays put.
    bind(seq({"Tab"}), "prompt.focus_next_control", "prompt");
    // The find/replace option toggles (find.toggle_case/whole_word/regex,
    // replace.all) are reachable through the command palette; they do not earn a
    // dedicated key and are left unbound.

    // The external-modification bar. Alt+E focuses it from any state (global,
    // present-gated by the command); within the external context the arrows move
    // the selection and Enter/K/D run the offered action on it, mirroring the
    // panel's navigation, and Escape returns focus without touching prompt
    // lifecycle.
    bind(seq({"Alt+KeyE"}), "external.focus", "*");
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
    auto canonical = std::filesystem::canonical(path, code);
    if (code || !std::filesystem::is_directory(canonical)) {
        throw std::invalid_argument{"workspace root must be an existing directory"};
    }
    return canonical;
}

std::span<const std::byte> asByteSpan(std::string_view text) noexcept {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// std::nullopt for a file that could not be read, so an unreadable file can
// never be mistaken for an empty one. That mistake is destructive here: these
// results feed staleness comparisons and rollback snapshots, where fake empty
// content would overwrite or restore nothing over something.
std::optional<std::string> readFileText(std::filesystem::path const& path) {
    auto result = readFile(path);
    if (!result.ok()) return std::nullopt;
    return std::string{reinterpret_cast<const char*>(result.bytes.data()),
                       result.bytes.size()};
}

std::size_t lineStartOffset(std::string_view text, std::size_t line) {
    std::size_t offset = 0;
    while (line > 0 && offset < text.size()) {
        const auto newline = text.find('\n', offset);
        if (newline == std::string_view::npos) {
            return text.size();
        }
        offset = newline + 1;
        --line;
    }
    return offset;
}

std::unordered_map<std::uint64_t, std::uint64_t> documentRevisions(
    const Workspace& workspace) {
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

EditorSession::Impl::Impl(std::filesystem::path canonicalCwd,
                          std::filesystem::path scratchRoot,
                          std::filesystem::path recoveryRoot,
                          std::filesystem::path archiveRoot,
                          bool deferEnrichment,
                          std::shared_ptr<SyntaxParser> parser,
                          bool enableGitDiffWorker,
                          bool enableFilesystemWatcher)
    : root{std::move(canonicalCwd)},
      scratchRoot{std::filesystem::weakly_canonical(scratchRoot)},
      recoveryRoot{std::filesystem::weakly_canonical(recoveryRoot)},
      archiveRoot{std::filesystem::weakly_canonical(archiveRoot)},
      recovery{RecoveryManager::create(recoveryRoot)},
      scratch{ScratchStore::create(scratchRoot, root)},
      workspace{Workspace::create(root, recovery, this->archiveRoot)},
      selection{initialSelection()},
      clipboard{4},
      tabs{},
      external{recovery, diff},
      syntaxParser{std::move(parser)},
      screen{assembleScreen("help.open", StyleDimensions{},
                            Style{}.inputLineSigil),
             tree},
      search{SearchCommands{
          .descriptors = [this] {
              std::vector<SearchCommandDescriptor> result;
              for (auto const* command : catalog.commands()) {
                  result.push_back({command->id, command->id});
              }
              return result;
          },
          .execute = [this](std::string_view commandId) {
              return PaletteExecutionResult{
                  catalog.find(commandId) != nullptr, {}};
          },
      }},
      theme{defaultTheme()},
      deferringEnrichment{deferEnrichment},
      gitDiffWorker{root, enableGitDiffWorker, enableFilesystemWatcher} {
    homeDirectory = resolveHomeDirectory();
    workspace.setSaveObserver([this](const std::filesystem::path& relativePath) {
        registerExternalSaveExpectation(relativePath);
    });
    (void)refreshTree();
    refreshSyntax();
}

EditorSession::Impl::~Impl() = default;

bool EditorSession::Impl::drainGitDiffWorker() {
    auto batch = gitDiffWorker.drain();
    bool accepted = batch.watcherAvailabilityChanged || !batch.scans.empty() ||
                    !batch.watchEvents.empty() || batch.fullReconcile;
    for (auto& scan : batch.scans) {
        (void)applyGitDiffScan(std::move(scan));
    }
    // Git scans first, then the external reconcile once over the whole queue, so a
    // burst of git scans never starves external ingress and both draw revisions
    // from the one shared DiffModel in order (Decision 10).
    if (!batch.watchEvents.empty()) {
        reconcileExternalWatchEvents(std::vector<WatchEvent>{
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
    // After ordinary ingress, recover any events the watcher dropped on overflow by
    // re-scanning every open document against disk (a full external resync).
    if (batch.fullReconcile) {
        reconcileAllOpenDocumentsAgainstDisk();
        refreshTreeForPublication();
    }
    return accepted;
}

int EditorSession::Impl::gitDiffWakeDescriptor() const {
    return gitDiffWorker.wakeDescriptor();
}


TabLifecycleResult EditorSession::Impl::closeTab(
    const TabState& tab, std::chrono::milliseconds durabilityTimeout,
    std::span<const TabId> alreadyClosed) {
    if (!tab.document) {
        if (tab.kind == TabKind::ReadOnlyOutput) {
            // A read-only output tab (help, generated content) is ephemeral and
            // regenerable: it is never journaled for reopen and never persists.
            // Drop its backing document and map entry directly, skipping the
            // recovery/scratch path entirely, and signal ephemeral so closeAt
            // accepts the missing compensation record.
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
    return {TabError::None, {}, closed.compensation, std::nullopt, std::nullopt,
            scratch.waitUntilDurable(durabilityTimeout)};
}

TabLifecycleResult EditorSession::Impl::reopenTab(
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

WorkspaceSnapshot EditorSession::Impl::snapshot(std::uint64_t revision) const {
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
             pathContains(recoveryRoot, entry.path()) ||
             pathContains(archiveRoot, entry.path()))) {
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
        auto content = readFileText(entry.path());
        if (!content) continue;
        result.files.push_back({*relative, std::move(*content)});
    }
    return result;
}

WorkspaceApplyResult EditorSession::Impl::applyWorkspaceReplace(
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
            // Unreadable is not "unchanged": refusing here is what stops a
            // replacement being applied to a file whose current state is
            // unknown.
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
        // Atomic replace, not truncate-then-stream: a replace-across-files run
        // interrupted part way through must leave each file either wholly old
        // or wholly new. A truncating write turns an interruption into a
        // truncated source file.
        try {
            replaceFileAtomically(paths[index], asByteSpan(change.after));
        } catch (const std::exception&) {
            return {FindReplaceError::WorkspaceRejected, preview.sourceRevision, "failed to write workspace file"};
        }
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
    return {FindReplaceError::None,
            std::uint64_t{preview.sourceRevision + 1}, {}};
}

std::optional<FileDocumentId> EditorSession::Impl::activeDocumentId() const {
    // The active tab is the single source of truth for the active editor
    // document.  With no active tab (e.g. the last tab was closed) there is no
    // active document and the shell renders its empty state; the editor view
    // never shows a document that has no tab.
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

const TabState* EditorSession::Impl::activeTabState() const {
    auto const& view = tabs.viewState();
    if (!view.active) return nullptr;
    auto found = std::find_if(view.tabs.begin(), view.tabs.end(),
                              [&](const TabState& tab) {
                                  return tab.id == *view.active;
                              });
    if (found == view.tabs.end()) return nullptr;
    return &*found;
}

CommandHandlerResult EditorSession::Impl::openOrFocusLiveDiffTab(
    const DiffFileView& file, NavigationClass classification) {
    const auto target = diffOpenFile(file);
    const auto diffText = liveDiffDocumentText(file);
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

CommandHandlerResult EditorSession::Impl::openReadOnlyTab(
    TabKind kind, std::string contentIdentity, std::string label,
    std::string text, LanguageId language) {
    // Build the replacement document FIRST, then swap: a ReadOnly document
    // rejects Document::apply, so content is refreshed by remove+recreate (never
    // an in-place edit) -- and creating before removing keeps a refresh failure
    // non-destructive, so a failed rebuild leaves the existing tab intact.
    auto created =
        workspace.openVirtualDocument(label, text, DocumentMode::ReadOnly);
    if (!created.accepted() || !created.document) {
        return failure(workspaceMessage(created));
    }
    ensureDocumentRuntimeState(*created.document);
    documentLanguageOverrides.insert_or_assign(created.document->value(),
                                                std::move(language));
    auto mapped = readOnlyTabDocuments.find(contentIdentity);
    if (mapped != readOnlyTabDocuments.end()) {
        const auto previous = mapped->second;
        documentRuntimeStates.erase(previous.value());
        documentLanguageOverrides.erase(previous.value());
        (void)workspace.removeDocument(previous);
    }
    readOnlyTabDocuments[contentIdentity] = *created.document;
    auto opened =
        tabs.openContent(kind, contentIdentity, label, DocumentMode::ReadOnly);
    if (!opened.accepted()) {
        return failure(tabMessage(opened));
    }
    screen.focusEditor();
    // Untitled documents get no language from a path, so highlight the override
    // language (e.g. Markdown) now that this tab is active.
    refreshSyntax();
    return success();
}

CommandHandlerResult EditorSession::Impl::openDraftDiff() {
    const auto id = activeDocumentId();
    if (!id) return failure("no active document");
    const auto state = workspace.state(*id);
    if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) {
        // A live diff tab's document (and an untitled buffer) is not a saved
        // file, so it has no on-disk side to diff the draft against.
        return failure("draft.diff needs a saved file");
    }
    const auto* opened = workspace.tryDocument(*id);
    if (opened == nullptr) return failure("no active document");
    const std::string draft = opened->snapshot().text;

    // The baseline is the file's CURRENT disk content, read now (not the
    // open-time bytes) so the diff reflects any external change. A missing or
    // unreadable file diffs the draft against empty, matching a deleted-file
    // conflict where the draft would recreate the file on save.
    const auto absolute =
        workspace.root() / std::filesystem::path{state->key.savedPath()};
    std::string disk;
    if (const auto read = readFile(absolute); read.ok()) {
        disk.assign(reinterpret_cast<const char*>(read.bytes.data()),
                    read.bytes.size());
    }

    const DiffFileId diffId{"draft:" + state->key.savedPath()};
    // Non-git entries share the DiffModel's monotonic revision line; one past
    // the current revision is always fresh. Create both seeds and updates the
    // entry (an existing non-git entry is updated in place), so re-running
    // draft.diff on the same file refreshes its tab.
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

DiffFileId EditorSession::Impl::externalDiffFileId(std::string_view savedPath) {
    return DiffFileId{"external:" + std::string{savedPath}};
}

std::optional<std::string> EditorSession::Impl::savedPathFromExternalDiffId(
    const DiffFileId& id) {
    static constexpr std::string_view prefix{"external:"};
    const auto& value = id.value();
    if (std::string_view{value}.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    return value.substr(prefix.size());
}

std::optional<FileDocumentId> EditorSession::Impl::resolveExternalDocument(
    const DiffFileId& id) const {
    const auto savedPath = savedPathFromExternalDiffId(id);
    if (!savedPath) return std::nullopt;
    for (const auto documentId : workspace.documents()) {
        const auto state = workspace.state(documentId);
        if (state && state->key.kind() == JournalDocumentKeyKind::Saved &&
            state->key.savedPath() == *savedPath) {
            return documentId;
        }
    }
    return std::nullopt;
}

std::optional<FileDocumentId>
EditorSession::Impl::resolveOpenSavedDocumentByPath(
    const std::filesystem::path& relativePath) const {
    const auto normalized = relativePath.generic_string();
    for (const auto documentId : workspace.documents()) {
        const auto state = workspace.state(documentId);
        if (state && state->key.kind() == JournalDocumentKeyKind::Saved &&
            state->key.savedPath() == normalized) {
            return documentId;
        }
    }
    return std::nullopt;
}

void EditorSession::Impl::registerExternalSaveExpectation(
    const std::filesystem::path& relativePath) {
    const auto observed =
        WatchFileState::observe(workspace.root() / relativePath);
    if (!observed) return;
    SaveExpectation expectation{relativePath, *observed};
    {
        std::lock_guard lock(externalSaveMutex);
        pendingSaveExpectations.push_back(expectation);
        // Bound the deque: a save the watcher never reports back must not
        // accumulate forever.
        constexpr std::size_t kMaxSaveExpectations = 256;
        while (pendingSaveExpectations.size() > kMaxSaveExpectations) {
            pendingSaveExpectations.pop_front();
        }
    }
    // Also register with the real watcher's normalizer so a genuine save is stamped
    // at source (Decision 9); handed to the worker thread, which owns the watcher,
    // so the main thread never touches it. Bounded so a save the worker never drains
    // cannot grow without limit.
    gitDiffWorker.registerSavedPath(std::move(expectation));
}

void EditorSession::Impl::reconcileExternalWatchEvents(
    std::vector<WatchEvent> events, bool resync) {
    const auto flowRevisionBefore = external.viewState().revision;
    const auto diffRevisionBefore = diff.viewState().revision;
    for (auto& event : events) {
        if (event.kind == WatchEventKind::Overflow) {
            continue;
        }
        // Correlate SSG's own writes so a save never reads as an external change.
        // A self-save resolves the external state: consume exactly its expectation
        // and clear any pending conflict for the file, so a stale expectation can
        // never accumulate to suppress a later genuine external edit.
        {
            std::optional<WatchFileState> observed;
            if (event.identity && event.size && event.modificationTime) {
                observed = WatchFileState{*event.identity, *event.size,
                                          *event.modificationTime};
            } else {
                observed =
                    WatchFileState::observe(workspace.root() / event.path);
            }
            bool selfSave = false;
            if (observed) {
                std::lock_guard lock(externalSaveMutex);
                const auto found = std::find_if(
                    pendingSaveExpectations.begin(),
                    pendingSaveExpectations.end(),
                    [&](const SaveExpectation& expectation) {
                        return expectation.path == event.path &&
                               expectation.state == *observed;
                    });
                if (found != pendingSaveExpectations.end()) {
                    pendingSaveExpectations.erase(found);
                    selfSave = true;
                }
            }
            if (selfSave) {
                // The save already advanced the workspace baseline to the written
                // bytes; clearing any pending conflict needs no further cross-store
                // commit, so the dismissal commit is a no-op.
                (void)external.keepBuffer(
                    externalDiffFileId(event.path.generic_string()),
                    [](bool, const std::optional<std::string>&) { return true; });
                continue;
            }
        }

        const std::filesystem::path& lookupPath =
            (event.kind == WatchEventKind::Rename && event.previousPath)
                ? *event.previousPath
                : event.path;
        const auto documentId = resolveOpenSavedDocumentByPath(lookupPath);
        if (!documentId) {
            // A change to a file no open document corresponds to is ignored by this
            // flow; the tree/git refresh already covers it.
            continue;
        }
        const auto state = workspace.state(*documentId);
        const auto* document = workspace.tryDocument(*documentId);
        if (!state || document == nullptr) {
            continue;
        }

        const std::string savedPath = event.path.generic_string();
        const DiffFileId id = externalDiffFileId(savedPath);
        const std::string baseline = document->snapshot().text;

        std::optional<std::string> diskContent;
        bool unknownObservation = false;
        if (event.kind != WatchEventKind::Remove) {
            diskContent = readFileText(workspace.root() / event.path);
            if (!diskContent) {
                // Decision 2a: an unreadable/non-regular path where a file was is
                // Unknown. Never silently skip it -- raise it as a removal conflict,
                // so the buffer now orphaned from any regular file surfaces.
                unknownObservation = true;
                event.kind = WatchEventKind::Remove;
            }
        } else {
            // A queued ordinary Remove carries only the fact "removed" and is never
            // re-observed by the watcher. Between the emit and this processing the
            // path may have reappeared as a directory, an unreadable file, or a
            // regular file. Re-observe before trusting the Remove so a stale one
            // cannot match a Missing baseline and be silently skipped: a status
            // error or a present-but-non-regular/unreadable entry is Unknown (raise
            // through the same chokepoint), a present regular file is a real change
            // (raise as Modify), and only a still-genuine absence stays a Remove that
            // a Missing baseline suppresses. Runtime thread only.
            std::error_code linkCode;
            const auto linkStatus =
                std::filesystem::symlink_status(workspace.root() / event.path,
                                                linkCode);
            const bool statusError =
                linkStatus.type() == std::filesystem::file_type::none;
            if (statusError) {
                unknownObservation = true;
            } else if (std::filesystem::exists(linkStatus)) {
                auto reobserved = readFileText(workspace.root() / event.path);
                if (reobserved) {
                    diskContent = std::move(reobserved);
                    event.kind = WatchEventKind::Modify;
                } else {
                    unknownObservation = true;
                }
            }
        }

        // Decision 4: an event whose observed disk state equals the document's
        // external baseline is a change already adopted or dismissed (keep_buffer);
        // skip it so a duplicate/coalesced ordinary event does not re-raise a
        // dismissed conflict. A rename changes identity (handled by adoption) and an
        // Unknown observation never matches, so both fall through to processing. The
        // diff CONTENT stays buffer-vs-disk; only this raise/skip decision consults
        // the baseline.
        if (!unknownObservation && event.kind != WatchEventKind::Rename &&
            workspace.matchesExternalBaseline(*documentId, diskContent)) {
            continue;
        }

        // Seed the non-git entry the first time this file is observed, so openDiff
        // finds a file and applyNonGitEvent is not rejected (Decision 5). Its
        // revision, like the event's, is allocated from the shared DiffModel.
        bool seededHere = false;
        if (!diff.file(id).has_value()) {
            const std::uint64_t seedRevision{diff.viewState().revision + 1};
            (void)diff.seedNonGit({{id, event.path, baseline}}, seedRevision);
            seededHere = true;
        }
        const std::uint64_t diffRevision{diff.viewState().revision + 1};

        std::optional<JournalDocument> journal{JournalDocument{
            state->key, document->mode(), state->dirty, document->snapshot().text}};

        // The clean auto-reload and rename-adoption paths commit to the workspace
        // BEFORE the flow publishes the cleared/updated state (Decision 11): the
        // flow calls this and only adopts when it succeeds, so a failed workspace
        // commit leaves the prior published conflict rather than a stale buffer.
        // The commit decodes the RAW disk bytes through the document's encoding.
        bool committed = false;
        std::function<bool()> commitClean;
        std::function<bool()> commitConflictRename;
        if (event.kind == WatchEventKind::Rename) {
            commitClean = [&]() {
                const bool ok =
                    workspace
                        .adoptExternalRename(*documentId, savedPath,
                                             diskContent.value_or(std::string{}),
                                             /*replaceBuffer=*/true)
                        .accepted();
                committed = ok;
                return ok;
            };
            // A dirty rename keeps its buffer, but the document must still adopt
            // the new path's disk identity and baseline BEFORE the flow publishes
            // the conflict under the new-path id. The flow calls this and refuses
            // to publish when it fails, so a failed adoption never leaves an action
            // referencing a path no document owns.
            commitConflictRename = [&]() {
                const bool ok =
                    workspace
                        .adoptExternalRename(*documentId, savedPath,
                                             diskContent.value_or(std::string{}),
                                             /*replaceBuffer=*/false)
                        .accepted();
                committed = ok;
                return ok;
            };
        } else if (event.kind != WatchEventKind::Remove) {
            commitClean = [&]() {
                const bool ok =
                    workspace
                        .reloadWithContent(*documentId,
                                           diskContent.value_or(std::string{}))
                        .accepted();
                committed = ok;
                return ok;
            };
        }

        ExternalEventInput input{event, id, baseline, diskContent};
        if (event.kind == WatchEventKind::Rename && event.previousPath) {
            input.previousId =
                externalDiffFileId(event.previousPath->generic_string());
        }
        const auto result =
            resync ? external.processResyncEvent(std::move(input), diffRevision,
                                                 journal, commitClean,
                                                 commitConflictRename)
                   : external.processEvent(std::move(input), diffRevision,
                                           journal, commitClean,
                                           commitConflictRename);
        if (!result.accepted()) {
            // A rejected event must leave no diff entry behind. When this iteration
            // seeded the new-path entry (so openDiff would have a file), roll it back
            // so a failed rename-adoption -- or any rejected event -- never orphans a
            // diff entry keyed to a path no pending action owns.
            if (seededHere && diff.file(id).has_value()) {
                const std::uint64_t removalRevision{diff.viewState().revision +
                                               1};
                (void)diff.removeFile(id, removalRevision);
            }
            continue;
        }

        // A rename changes the namespaced id; retire the stale diff entry keyed by
        // the old path so a second, orphaned entry is not left behind (Decision 10).
        if (event.kind == WatchEventKind::Rename && event.previousPath) {
            const DiffFileId previousId =
                externalDiffFileId(event.previousPath->generic_string());
            if (previousId != id && diff.file(previousId).has_value()) {
                const std::uint64_t removalRevision{diff.viewState().revision +
                                               1};
                (void)diff.removeFile(previousId, removalRevision);
            }
        }

        if (committed) {
            (void)updateTabsFor(*documentId);
        }
    }
    // External state changes here in the watcher drain, not only on a command
    // dispatch: reconcile the section's presence into the screen whenever the
    // flow's view advanced, so the node appears/updates
    // without waiting for an unrelated command.
    if (external.viewState().revision != flowRevisionBefore) {
        screen.refreshExternalModificationPresence(
            externalModificationPresent());
    }
}

bool EditorSession::Impl::commitExternalDismissal(
    FileDocumentId document, bool removed,
    const std::optional<std::string>& dismissedContent) {
    return workspace.commitExternalDismissal(
        document, removed, dismissedContent,
        [&](const std::optional<DraftBaseline>& newBaseline) -> bool {
            // Decision 5: an already-persisted draft record still carries the
            // pre-dismissal baseline; refresh it to the dismissed state so a
            // crash-reopen classifies Unchanged instead of resurrecting the
            // conflict via draft recovery. No persisted record: nothing to do.
            const auto state = workspace.state(document);
            if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) {
                return true;
            }
            const auto drafts = scratch.recovery().documents;
            const auto draft = std::find_if(
                drafts.begin(), drafts.end(),
                [&](const JournalDocument& candidate) {
                    return candidate.dirty && candidate.key == state->key;
                });
            if (draft == drafts.end()) return true;
            scratch.updateDocument(JournalDocument{draft->key, draft->mode,
                                                   draft->dirty,
                                                   draft->utf8Content,
                                                   newBaseline});
            return true;
        });
}

void EditorSession::Impl::reconcileAllOpenDocumentsAgainstDisk() {
    std::vector<WatchEvent> synthesized;
    std::uint64_t sequence = 0;
    for (const auto documentId : workspace.documents()) {
        const auto state = workspace.state(documentId);
        if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) {
            continue;
        }
        const std::filesystem::path relative{state->key.savedPath()};
        const auto absolute = workspace.root() / relative;
        std::error_code linkCode;
        const auto linkStatus = std::filesystem::symlink_status(absolute, linkCode);
        WatchEvent event;
        event.path = relative;
        event.origin = WatchEventOrigin::External;
        event.sequence = ++sequence;
        // file_type::none is an indeterminate status (permission denied, I/O error);
        // file_type::not_found is a determinate clean absence. Only the former is a
        // status error.
        if (linkStatus.type() == std::filesystem::file_type::none) {
            // Decision 2a: a status/stat error is Unknown, not a clean absence -- it
            // must never become a Missing-matchable Remove the resync could suppress.
            // Synthesize a Modify so the shared reconcile re-reads, fails, and marks
            // the observation Unknown, which always raises. This converges on the one
            // Unknown-detection chokepoint.
            event.kind = WatchEventKind::Modify;
            synthesized.push_back(std::move(event));
            continue;
        }
        // The path entry itself is present (a regular file, a directory, or even a
        // broken symlink) -- distinct from exists(), which follows the link and is
        // false for a broken symlink, indistinguishable there from a true absence.
        const bool entryPresent = std::filesystem::exists(linkStatus);
        std::error_code code;
        const bool exists = std::filesystem::exists(absolute, code) && !code;
        if (!entryPresent) {
            // A genuine absence. A dismissed removal (keep_buffer set the baseline
            // Missing) must not be re-raised by the resync; only a still-differing
            // absence raises.
            if (workspace.matchesExternalBaseline(documentId, std::nullopt)) {
                continue;
            }
            event.kind = WatchEventKind::Remove;
            synthesized.push_back(std::move(event));
            continue;
        }
        // The entry is present. If it is a readable regular file whose content
        // matches the baseline, it did not change during the overflow window and
        // needs no event (and a dirty document must not be told its unchanged disk
        // file was modified). Otherwise -- a changed file OR an Unknown observation
        // (unreadable, non-regular, or broken symlink) -- synthesize a Modify. The
        // shared reconcile re-reads it; on a failed read it marks the observation
        // Unknown, which ALWAYS raises and never matches a Missing baseline, so an
        // Unknown never collapses into a Remove the ordinary reconcile could
        // suppress. Both the ordinary and overflow paths thus converge on the one
        // Unknown-detection chokepoint (Decision 2a).
        const auto disk = exists ? readFileText(absolute) : std::nullopt;
        if (disk && workspace.matchesExternalBaseline(documentId, *disk)) {
            continue;
        }
        event.kind = WatchEventKind::Modify;
        synthesized.push_back(std::move(event));
    }
    if (!synthesized.empty()) {
        reconcileExternalWatchEvents(std::move(synthesized), /*resync=*/true);
    }
}

bool EditorSession::Impl::archiveDiscardedDraft(std::string_view savedPath,
                                                std::string_view content) {
    // Beside the scratch store (not the workspace deleted-file archive, which
    // the housekeeping pruner owns), so a discarded draft is never pruned as a
    // stale deleted file.
    const auto archiveDir = scratchRoot.parent_path() / "draft-archive";
    std::error_code code;
    std::filesystem::create_directories(archiveDir, code);
    if (code) return false;

    // Name by a HASH of the workspace-relative path rather than the flattened
    // path itself: a legal deep path can exceed a filesystem's per-component
    // name limit (255 bytes on Linux), which would make discard fail for a valid
    // file. A short basename prefix stays for humans browsing the archive; the
    // hash disambiguates two files sharing a basename, and the nanosecond stamp
    // keeps repeated discards of one file distinct.
    std::string basename =
        std::filesystem::path{std::string{savedPath}}.filename().string();
    if (basename.empty()) basename = "draft";
    if (basename.size() > 64) basename.resize(64);
    const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const auto target =
        archiveDir / (basename + "." + std::to_string(fastContentHash(savedPath)) +
                      "." + std::to_string(stamp) + ".draft");
    const std::span<const std::byte> bytes{
        reinterpret_cast<const std::byte*>(content.data()), content.size()};
    return createFileExclusively(target, bytes).ok();
}

CommandHandlerResult EditorSession::Impl::discardDraft() {
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

    // Archive the discarded edits FIRST, before anything is removed or the
    // buffer is reloaded: even if the reload fails, the draft survives here and
    // in the scratch store, so a mis-click is always recoverable.
    if (!archiveDiscardedDraft(state->key.savedPath(), draftText)) {
        return failure("could not archive the draft before discarding");
    }

    // Reload the on-disk content, which refreshes the baseline and clears
    // dirty. A missing or unreadable file leaves the draft untouched.
    const auto reloaded = workspace.reload(*id);
    if (!reloaded.accepted()) return failure("could not load the file from disk");

    scratch.removeDocument(state->key);
    // "Use disk" is meant to be final. removeDocument is queued to the async
    // durability thread, so wait briefly (as tab close does) to shrink the
    // window where a crash could replay the just-discarded draft on next launch.
    // Best-effort: the archived copy already makes a lost race recoverable.
    (void)scratch.waitUntilDurable(std::chrono::milliseconds{100});
    if (const auto found = documentRuntimeStates.find(id->value());
        found != documentRuntimeStates.end()) {
        found->second.reopen = DraftReopenOutcome::None;
    }
    resetSelectionForActiveDocument();
    refreshSyntax();
    return updateTabsFor(*id);
}

CommandHandlerResult EditorSession::Impl::dismissDraftNotice() {
    const auto id = activeDocumentId();
    if (!id) return failure("no active document");
    const auto found = documentRuntimeStates.find(id->value());
    if (found == documentRuntimeStates.end() ||
        found->second.reopen != DraftReopenOutcome::Conflict) {
        // Only a Conflict raises the notice; Restored/None show nothing to
        // dismiss, so dismissing them would silently mutate non-notice state.
        return failure("no draft notice to dismiss");
    }
    found->second.reopen = DraftReopenOutcome::None;
    return success();
}

void EditorSession::Impl::refreshLiveDiffDocuments(const DiffViewState& diffView) {
    for (auto it = liveDiffDocuments.begin(); it != liveDiffDocuments.end();) {
        const auto id = DiffFileId{it->first};
        auto file = std::find_if(
            diffView.files.begin(), diffView.files.end(),
            [&](const DiffFileView& candidate) { return candidate.id == id; });
        const auto desired =
            file == diffView.files.end() ? std::string{}
                                         : liveDiffDocumentText(*file);
        const auto document = it->second;
        const auto* opened = workspace.tryDocument(document);
        if (opened == nullptr) {
            it = liveDiffDocuments.erase(it);
            continue;
        }
        if (opened->snapshot().text == desired) {
            ++it;
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
        it->second = *replacement.document;
        ensureDocumentRuntimeState(*replacement.document);
        auto removed = workspace.removeDocument(document);
        if (removed.accepted()) {
            documentRuntimeStates.erase(document.value());
        }
        ++it;
    }
}

bool EditorSession::Impl::openOrRevealFollowTargetProgrammatic(
    const FollowTarget& target) {
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
    return revealCurrentDiffTarget(target, NavigationClass::Programmatic);
}

Document const* EditorSession::Impl::activeDocument() const {
    auto id = activeDocumentId();
    return id ? workspace.tryDocument(*id) : nullptr;
}

Document* EditorSession::Impl::activeDocument() {
    auto id = activeDocumentId();
    return id ? const_cast<Document*>(workspace.tryDocument(*id)) : nullptr;
}

void EditorSession::Impl::ensureDocumentRuntimeState(FileDocumentId document) {
    documentRuntimeStates.try_emplace(
        document.value(),
        DocumentRuntimeState{settings, syntaxParser});
}

void EditorSession::Impl::discardDocumentRuntimeState(FileDocumentId document) {
    documentRuntimeStates.erase(document.value());
    documentLanguageOverrides.erase(document.value());
    autosave.forget(document);
    // A find that was scoped to this document no longer has a subject.
    if (findDocumentId == document) findDocumentId.reset();
    // Any live diff tab mapped to it is equally orphaned.
    for (auto it = liveDiffDocuments.begin(); it != liveDiffDocuments.end();) {
        it = it->second == document ? liveDiffDocuments.erase(it)
                                    : std::next(it);
    }
    // Same for a read-only output (help) tab mapped to it.
    for (auto it = readOnlyTabDocuments.begin();
         it != readOnlyTabDocuments.end();) {
        it = it->second == document ? readOnlyTabDocuments.erase(it)
                                    : std::next(it);
    }
}

DocumentHistory& EditorSession::Impl::historyFor(FileDocumentId document) {
    auto it = documentRuntimeStates.find(document.value());
    if (it == documentRuntimeStates.end()) {
        throw std::logic_error{
            "document history was requested before document runtime state existed"};
    }
    return it->second.history;
}

SyntaxModel& EditorSession::Impl::syntaxFor(FileDocumentId document) {
    auto it = documentRuntimeStates.find(document.value());
    if (it == documentRuntimeStates.end()) {
        throw std::logic_error{
            "document syntax was requested before document runtime state existed"};
    }
    return it->second.syntax;
}

SyntaxViewState EditorSession::Impl::activeSyntaxView() const {
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

std::optional<WorkspaceDocumentState> EditorSession::Impl::activeWorkspaceState() const {
    auto id = activeDocumentId();
    return id ? workspace.state(*id) : std::nullopt;
}

std::optional<DiffFileView> EditorSession::Impl::activeDiffFile() const {
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

std::string const& EditorSession::Impl::activeText() const {
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

void EditorSession::Impl::resetSelectionForActiveDocument() {
    selection = initialSelection();
}

void EditorSession::Impl::clampSelectionToActiveDocument() {
    auto const& text = activeText();
    auto offset = selection.selections.primary().active.byteOffset.value();
    if (offset > text.size()) offset = text.size();
    auto position = ssg::SelectionNavigator::resolvePosition(text, ByteOffset{offset}).value_or(zeroPosition());
    selection.selections = SelectionSet{std::vector<Selection>{Selection{position, position}}};
}

void EditorSession::Impl::clampSelectionsToActiveDocument() {
    auto const& text = activeText();
    auto clampPosition = [&](DocumentPosition const& p) {
        auto offset = p.byteOffset.value();
        if (offset > text.size()) offset = text.size();
        // Prefer the exact offset; if it is not a grapheme boundary (only
        // possible for a selection carried from a differently-shaped document,
        // not for the edit paths this serves), snap DOWN to the nearest boundary
        // at or below it rather than teleporting to the document end.
        for (;;) {
            if (auto at = ssg::SelectionNavigator::resolvePosition(
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
        clamped.push_back(
            Selection{clampPosition(sel.anchor), clampPosition(sel.active)});
    }
    if (clamped.empty()) {
        clamped.push_back(Selection{zeroPosition(), zeroPosition()});
    }
    selection.selections = SelectionSet{std::move(clamped)};
}

bool EditorSession::Impl::refreshTree() {
    if (deferringEnrichment) {
        pendingTreeRefresh = true;
        return false;
    }
    ++treeScanCount;
    tree.replaceProvider(TreeProviderSnapshot::fromFilesystem(
        TreeProviderId{"filesystem"}, root));
    rebuildFileCandidates();
    return true;
}

void EditorSession::Impl::refreshTreeForPublication() {
    (void)refreshTree();
}

void EditorSession::Impl::rebuildInteractionSchema(
    const StyleDimensions& dimensions,
    std::string_view promptSigil) {
    (void)screen.updateComposition(
        assembleScreen("help.open", dimensions, promptSigil));
}

bool EditorSession::Impl::openPickerPrompt(PickerKind kind) {
    return screen.openFinder(kind);
}

// The index opens its OWN repository handle rather than sharing the git-diff
// worker's: that one is owned by its thread, and libgit2 handles are not safe to
// use from two threads.
void EditorSession::Impl::rebuildFileCandidates() {
    auto matcher = makePlatformGitIgnoreMatcher(root);
    WorkspaceFileIndexOptions options;
    options.respectGitignore =
        boolSetting(settings, SettingKey::FileFinderRespectGitignore, true);
    fileCandidates =
        std::move(WorkspaceFileIndex{}.build(root, *matcher, options).candidates);
}

void EditorSession::Impl::reconcileFindDocument() {
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
    if (auto const& request = screen.prompt().request();
        request && (request->kind == PromptKind::Find ||
                    request->kind == PromptKind::Replace)) {
        (void)screen.prompt().cancel();
    }
    findDocumentId.reset();
}

void EditorSession::Impl::refreshSyntax(std::vector<SyntaxEdit> edits) {
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
            text.size() <= kEagerSyntaxMaxBytes;
        if (!canEagerlyParse) {
            pendingSyntaxRefresh = true;
            return;
        }
    }
    ++syntaxRunCount;
    if (!model.canIncrementallyParse(language)) edits.clear();
    (void)model.parse(revision, std::move(language), std::move(text),
                      std::move(edits));
}

void EditorSession::Impl::primeDeferred() {
    if (!deferringEnrichment) return;
    deferringEnrichment = false;
    // Run whichever scans were requested while deferring, now that the first
    // frame is drawn. Order: tree then syntax.
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

void EditorSession::Impl::enqueueStatus(StatusPriority priority, std::string text) {
    auto value = nextStatusId++;
    if (status
            .enqueue(StatusItem{StatusId{value}, priority, std::move(text), {}})
            .accepted) {
        screen.refreshStatusActions(status.actionNodes());
    }
}

namespace {

std::vector<AutosaveCandidate> autosaveCandidates(const Workspace& workspace) {
    std::vector<AutosaveCandidate> candidates;
    for (const auto id : workspace.documents()) {
        auto state = workspace.state(id);
        if (!state) continue;
        auto const* current = workspace.tryDocument(id);
        if (current == nullptr) continue;
        // Only an editable document can hold unsaved user edits worth a draft. A
        // live-diff tab's virtual document (DocumentMode::Diff) is a derived view
        // that is untitled and non-empty, so it would otherwise read as a dirty
        // untitled buffer and be persisted as a spurious scratch draft.
        if (current->mode() != DocumentMode::Edit) continue;
        // Only a dirty document is a flush candidate, so only a dirty document
        // pays for a text snapshot + hash. A clean one still appears (hash 0) so
        // the scheduler can drop any debounce state it held — cheap, no copy.
        std::uint64_t contentHash = 0;
        if (state->dirty) {
            contentHash = fastContentHash(current->snapshot().text);
        }
        candidates.push_back(AutosaveCandidate{id, state->dirty, contentHash});
    }
    return candidates;
}

} // namespace

std::size_t EditorSession::Impl::persistAutosaveDraft(FileDocumentId document) {
    auto state = workspace.state(document);
    auto const* current = workspace.tryDocument(document);
    if (!state || current == nullptr) return 0;

    // A buffer too large to draft gets NO draft (and thus no crash-safety),
    // reported rather than silently written: a giant draft would blow the
    // scratch quota and stall fsync, and a stale partial draft would be false
    // reassurance. Remove any earlier draft for the key so the on-disk state is
    // honestly "no draft", and warn once.
    const auto& text = current->snapshot().text;
    if (text.size() > autosaveDraftByteCap) {
        // Report and drop any prior draft exactly ONCE per over-cap episode:
        // the reported flag gates the whole block so a long oversized edit
        // session does not enqueue a no-op journal remove on every flush tick.
        if (const auto found = documentRuntimeStates.find(document.value());
            found != documentRuntimeStates.end() &&
            !found->second.autosaveOversizeReported) {
            found->second.autosaveOversizeReported = true;
            scratch.removeDocument(state->key);
            enqueueStatus(StatusPriority::Warning,
                          "file is too large to autosave a draft; unsaved edits "
                          "are not crash-protected until saved");
        }
        return 0;
    }
    if (const auto found = documentRuntimeStates.find(document.value());
        found != documentRuntimeStates.end()) {
        found->second.autosaveOversizeReported = false;
    }

    // Identical JournalDocument to the tab-close path, minus the blocking
    // durability wait: autosave leaves fsync to the background thread so a tick
    // never stalls the UI (the recovery badge still reports pending/durable).
    scratch.updateDocument(JournalDocument{state->key, current->mode(),
                                           state->dirty, text,
                                           workspace.baselineFor(document)});
    return 1;
}

void EditorSession::Impl::reconcileDraftOnOpen(FileDocumentId document) {
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
    switch (DraftReopenClassifier{}.classify(draft->baseline,
                                             draft->utf8Content, disk)) {
        case DraftReopenClass::Converged:
            // The edits equal disk (or were undone): nothing to recover. Drop the
            // draft and keep the clean disk buffer already open.
            scratch.removeDocument(draft->key);
            runtimeState.reopen = DraftReopenOutcome::None;
            return;
        case DraftReopenClass::Unchanged:
            // Disk is unchanged since the edits branched: load the draft dirty.
            // If it cannot be loaded (disk is now binary/undecodable, so the
            // buffer is read-only), do NOT drop it silently -- keep it in scratch
            // and raise the conflict notice so the user is warned, never falsely
            // reassured that nothing needs attention.
            if (workspace.restoreDraft(document, draft->utf8Content)) {
                runtimeState.reopen = DraftReopenOutcome::Restored;
            } else {
                runtimeState.reopen = DraftReopenOutcome::Conflict;
            }
            return;
        case DraftReopenClass::Conflict:
            // Best-effort load; the draft stays in scratch whether or not the
            // buffer can hold it (a binary/undecodable disk file yields a
            // read-only buffer). Either way the conflict notice is raised, so the
            // draft is never silently lost.
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

std::size_t EditorSession::Impl::flushDueAutosaveDrafts() {
    autosave.setInterval(std::chrono::milliseconds{
        uint32Setting(settings, SettingKey::AutosaveDebounceMs, 10000)});
    const auto candidates = autosaveCandidates(workspace);
    std::size_t flushed = 0;
    for (const auto id :
         autosave.due(std::chrono::steady_clock::now(), candidates)) {
        flushed += persistAutosaveDraft(id);
    }
    return flushed;
}

std::size_t EditorSession::Impl::flushAllAutosaveDrafts() {
    const auto candidates = autosaveCandidates(workspace);
    std::size_t flushed = 0;
    for (const auto id :
         autosave.flushAll(std::chrono::steady_clock::now(), candidates)) {
        flushed += persistAutosaveDraft(id);
    }
    return flushed;
}

DiffIngressResult EditorSession::Impl::applyExternalDiffBurst(
    std::vector<ExternalDiffRevision> changes) {
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

DiffIngressResult EditorSession::Impl::applyGitDiffScan(GitDiffScan scan) {
    if (scan.revision == 0) {
        auto const previousBranch = currentGitBranch;
        currentGitBranch = scan.currentBranch;
        (void)previousBranch;
        return {};
    }
    if (scan.revision <= lastGitScanRevision) {
        return {DiffIngressError::DiffRejected};
    }
    currentGitBranch = scan.currentBranch;
    auto gitRecords = gitTreeRecordsFromScan(scan.files);
    auto stagedDiff = diff;
    auto stagedFollow = follow;
    std::vector<FollowDiffChange> followChanges;
    followChanges.reserve(scan.files.size() + stagedDiff.viewState().files.size());
    bool mutated = false;
    std::vector<DiffFileId> statusOnlyIds;

    std::uint64_t nextRevision = std::uint64_t{stagedDiff.viewState().revision + 1};
    const auto nextMutationRevision = [&nextRevision]() {
        auto current = nextRevision;
        nextRevision = std::uint64_t{nextRevision + 1};
        return current;
    };
    const auto removeDetailedFile =
        [&](const DiffFileId& id) -> DiffIngressResult {
        const auto prior = stagedDiff.file(id);
        if (!prior || !stagedDiff.isGitFile(id)) {
            return {};
        }
        auto removedFile = prior->get();
        auto priorHunks = removedFile.hunks;
        const auto revision = nextMutationRevision();
        const auto removed = stagedDiff.removeFile(id, revision);
        if (!removed.accepted()) {
            return {DiffIngressError::DiffRejected};
        }
        mutated = true;
        removedFile.deleted = true;
        removedFile.currentContent.clear();
        removedFile.hunks.clear();
        removedFile.changedLines.clear();
        followChanges.push_back(
            {std::move(removedFile), std::move(priorHunks), revision});
        return {};
    };

    std::vector<DiffFileId> scannedIds;
    scannedIds.reserve(scan.files.size());
    for (auto& file : scan.files) {
        scannedIds.push_back(file.id);
        std::vector<DiffHunk> priorHunks;
        if (const auto prior = stagedDiff.file(file.id)) {
            priorHunks = prior->get().hunks;
        }
        const auto revision = nextMutationRevision();
        const auto applied = stagedDiff.updateGitFile(
            {.id = file.id,
             .path = file.path,
             .previousPath = file.previousPath,
             .baselineContent = std::move(file.baselineContent),
             .workingContent = std::move(file.workingContent)},
            scan.baselineIdentity, revision);
        if (!applied.accepted()) {
            if (applied.error != DiffError::WorkLimitExceeded) {
                return {DiffIngressError::DiffRejected};
            }
            statusOnlyIds.push_back(file.id);
            if (auto removed = removeDetailedFile(file.id);
                !removed.accepted()) {
                return removed;
            }
            continue;
        }
        const auto changedFile = stagedDiff.file(file.id);
        if (!changedFile) {
            return {DiffIngressError::DiffRejected};
        }
        mutated = true;
        followChanges.push_back(
            {changedFile->get(), std::move(priorHunks), revision});
    }

    const auto stagedView = stagedDiff.viewState();
    for (const auto& file : stagedView.files) {
        if (std::find(scannedIds.begin(), scannedIds.end(), file.id) !=
            scannedIds.end()) {
            continue;
        }
        // A git rescan reconciles only git-source entries. A non-git entry (a
        // draft-vs-disk diff, or an external-modification view) is owned by a
        // different flow and must survive a scan that simply does not mention
        // it, rather than being evicted as "no longer changed".
        if (!stagedDiff.isGitFile(file.id)) {
            continue;
        }
        if (auto removed = removeDetailedFile(file.id); !removed.accepted()) {
            return removed;
        }
    }

    if (mutated) {
        const auto followed =
            stagedFollow.acceptExternalChanges(std::move(followChanges));
        if (!followed.accepted()) {
            return {DiffIngressError::FollowRejected};
        }

        const auto previousTarget = follow.viewState().activeTarget;
        diff = std::move(stagedDiff);
        follow = std::move(stagedFollow);
        for (const auto& id : statusOnlyIds) {
            std::optional<TabId> liveTab;
            for (const auto& tab : tabs.viewState().tabs) {
                if (tab.kind == TabKind::LiveDiff &&
                    tab.contentIdentity == id.value()) {
                    liveTab = tab.id;
                    break;
                }
            }
            if (liveTab) {
                const auto found = std::find_if(
                    tabs.viewState().tabs.begin(), tabs.viewState().tabs.end(),
                    [&](const TabState& tab) { return tab.id == *liveTab; });
                if (found != tabs.viewState().tabs.end()) {
                    auto outcome = closeTab(*found, std::chrono::milliseconds{100});
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
    lastGitScanRevision = scan.revision;
    return {};
}

bool EditorSession::Impl::revealCurrentDiffTarget(
    const FollowTarget& target, NavigationClass classification) {
    (void)classification;
    const auto& text = activeText();
    const auto offset = lineStartOffset(text, target.newestHunkLine);
    const auto position =
        SelectionNavigator::resolvePosition(text, ByteOffset{offset});
    if (!position) {
        return false;
    }
    selection.selections =
        SelectionSet{std::vector<Selection>{Selection{*position, *position}}};
    screen.focusEditor();
    return true;
}

bool EditorSession::Impl::revealDiffTarget(
    const FollowTarget& target, NavigationClass classification) {
    if (target.deleted) {
        return false;
    }
    const auto opened = workspace.openFile(target.path.generic_string());
    if (!opened.accepted() || !opened.document ||
        !activateDocument(*opened.document).accepted) {
        return false;
    }
    return revealCurrentDiffTarget(target, classification);
}

void EditorSession::Impl::recordNavigation(NavigationClass classification) {
    (void)follow.applyNavigation({.classification = classification});
}

CommandHandlerResult EditorSession::Impl::splitPane(SplitAxis axis) {
    (void)paneTopology.splitActive(axis);
    return success();
}

CommandHandlerResult EditorSession::Impl::closePane() {
    if (!paneTopology.closeActive()) {
        return failure("the only pane cannot be closed");
    }
    return success();
}

CommandHandlerResult EditorSession::Impl::cyclePane(
    CycleDirection direction) {
    paneTopology.cycle(direction);
    if (follow.viewState().mode == FollowMode::Following) {
        (void)follow.pause();
    }
    recordNavigation(NavigationClass::User);
    return success();
}

bool EditorSession::Impl::focusPane(PaneId pane) {
    return paneTopology.focus(pane);
}

EditorSession::EditorSession(std::unique_ptr<Impl> implementation) noexcept
    : impl_{std::move(implementation)} {}
EditorSession::~EditorSession() = default;

void EditorSession::resetKeymapToDefault() {
    std::lock_guard operationLock{impl_->operationMutex};
    // defaultTerminalKeymap() is a fixed, already-construction-time-
    // validated value (see create() above), so no re-validation is needed
    // here -- resetting to it can never fail.
    impl_->keymap = defaultTerminalKeymap();
    ++impl_->keymapGeneration;
}

void EditorSession::focusEditor() {
    std::lock_guard operationLock{impl_->operationMutex};
    impl_->screen.focusEditor();
}

EditorSessionCreateResult EditorSession::create(EditorSessionConfig config) {
    try {
        auto cwd = canonicalDirectory(config.cwd);
        if (config.scratchRoot.empty()) config.scratchRoot = cwd / ".ssg" / "scratch";
        if (config.recoveryRoot.empty()) config.recoveryRoot = cwd / ".ssg" / "recovery";
        if (config.archiveRoot.empty()) config.archiveRoot = cwd / ".ssg" / "archive";
        std::filesystem::create_directories(config.scratchRoot);
        std::filesystem::create_directories(config.recoveryRoot);
        // The archive root is deliberately NOT created here. Creating it eagerly
        // would materialise a `.ssg/` directory inside every workspace merely
        // for being opened -- visible in the file tree, and pointless for a
        // session that never deletes anything. FileArchive creates it on the
        // first delete instead.
        auto impl = std::make_unique<Impl>(cwd, config.scratchRoot,
                                           config.recoveryRoot,
                                           config.archiveRoot,
                                           config.deferEnrichment,
                                           std::move(config.syntaxParser),
                                           config.enableGitDiffWorker,
                                           config.enableFilesystemWatcher);
        // Housekeeping at workspace open rather than on a timer, so it is
        // deterministic and testable. Its result is deliberately ignored: a
        // corrupt archive entry must never stop a user opening their workspace.
        (void)impl->workspace.pruneArchive();
        impl->keymap = defaultTerminalKeymap();
        if (auto errors = KeymapMatcher{impl->keymap}.validate(); !errors.empty()) {
            return {nullptr, "default keymap is invalid: " + errors.front().message};
        }
        if (!KeymapMatcher{impl->keymap}.hasGlobalBinding("settings.open")) {
            return {nullptr,
                    "default keymap lacks a global settings.open escape hatch"};
        }
        registerAllCommands(impl->catalog, *impl);
        return {std::unique_ptr<EditorSession>{new EditorSession{std::move(impl)}}, {}};
    } catch (std::exception const& exception) {
        return {nullptr, exception.what()};
    }
}

PumpResult EditorSession::pump() {
    if (impl_->catalog.dispatchInProgress()) {
        throw std::logic_error{"worker results cannot be pumped during dispatch"};
    }
    std::lock_guard operationLock{impl_->operationMutex};
    return {impl_->drainGitDiffWorker()};
}

void EditorSession::primeDeferred() {
    std::lock_guard operationLock{impl_->operationMutex};
    impl_->primeDeferred();
}
std::size_t EditorSession::flushDueAutosaveDrafts() {
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->flushDueAutosaveDrafts();
}
std::size_t EditorSession::flushAllAutosaveDrafts() {
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->flushAllAutosaveDrafts();
}

EditorSession::DeferredWorkCounts EditorSession::deferredWorkCounts() const {
    std::lock_guard operationLock{impl_->operationMutex};
    return {impl_->syntaxRunCount, impl_->treeScanCount};
}

std::uint64_t EditorSession::liveDocumentRuntimeStateCountForTests() {
    return DocumentRuntimeState::liveInstances();
}

void EditorSession::setAutosaveDraftByteCapForTests(std::uint64_t cap) {
    std::lock_guard operationLock{impl_->operationMutex};
    impl_->autosaveDraftByteCap = cap;
}

void EditorSession::reconcileExternalWatchEventsForTest(
    std::vector<WatchEvent> events) {
    std::lock_guard operationLock{impl_->operationMutex};
    // Mirror the runtime drain: an Overflow in the batch triggers the full
    // open-document-vs-disk resync (the worker would signal it out of band), the
    // ordinary events reconcile normally.
    bool overflowed = false;
    std::vector<WatchEvent> ordinary;
    for (auto& event : events) {
        if (event.kind == WatchEventKind::Overflow) {
            overflowed = true;
        } else {
            ordinary.push_back(std::move(event));
        }
    }
    if (!ordinary.empty()) {
        impl_->reconcileExternalWatchEvents(std::move(ordinary));
    }
    if (overflowed) {
        impl_->reconcileAllOpenDocumentsAgainstDisk();
    }
}

bool EditorSession::diffModelHasFileForTest(const DiffFileId& id) const {
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->diff.file(id).has_value();
}

void EditorSession::reportWatcherAvailabilityForTest(bool available) {
    std::lock_guard operationLock{impl_->operationMutex};
    impl_->gitDiffWorker.setAvailabilityForTest(available);
}

void EditorSession::refreshFilesystemForTest() {
    std::lock_guard operationLock{impl_->operationMutex};
    impl_->refreshTreeForPublication();
}

bool EditorSession::dispatchInProgress() const noexcept {
    return impl_->catalog.dispatchInProgress();
}

bool EditorSession::Impl::defer(ClientCommand command) {
    if (!catalog.dispatchInProgress()) return false;
    return deferredCommands.enqueue({std::move(command)});
}

bool EditorSession::deferDispatch(ClientCommand command) {
    return impl_->defer(std::move(command));
}

CommandResult EditorSession::Impl::dispatchLocked(ClientCommand const& command) {
    // Every direct dispatch -- keystroke, palette, or script -- is local: there
    // is one trusted caller, so a mutation always counts toward the local-edit
    // follow pause.
    const auto dispatchAs = [&](const ClientCommand& dispatched) {
        const auto revisionsBefore = documentRevisions(workspace);
        auto result = catalog.dispatch(dispatched);
        if (result.activeWorkspace) {
            topology.activeWorkspace = result.activeWorkspace;
        }
        reconcileFindDocument();
        // The draft-conflict notice's presence lives in per-document runtime state,
        // outside the prompt/panel transitions, so reconcile it into the screen
        // here where every state change (open, reopen, tab switch, discard,
        // dismiss) has settled -- the notice region then shows/hides in the presence
        // section this dispatch publishes.
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
    // Dispatches a command AND runs whatever its handler asked to invoke,
    // before returning.  The drain lives here rather than at the end of this
    // function so that no path can reach a `return` with requests still
    // queued: the palette and prompt paths below return early, and a command
    // run through either of them may queue just as any other can.
    //
    // Requests run once the session lock has released, in the order asked for.
    const auto dispatchAndDrain = [&](const ClientCommand& dispatched) {
        const auto* requested = dispatched.id.handle().valid()
                                    ? catalog.find(dispatched.id.handle())
                                    : catalog.find(dispatched.id.name());
        const bool routing =
            requested && requested->effect == CommandEffect::Routing;
        auto outcome = dispatchAs(dispatched);
        // A handler that FAILED does not get its requests performed: it may
        // have queued half a sequence before giving up, and running that half
        // is worse than running none of it.  Its success would also overwrite
        // the failure being reported.
        if (!outcome.accepted()) {
            deferredCommands.clear();
            return outcome;
        }
        if (routing && deferredCommands.size() != 1) {
            deferredCommands.clear();
            return CatalogDispatchResult{
                CommandError::HandlerFailed,
                "a routing command must queue exactly one target",
                std::nullopt};
        }
        if (outcome.viewAction && !deferredCommands.empty()) {
            deferredCommands.clear();
            return CatalogDispatchResult{
                CommandError::HandlerFailed,
                "a view-action command cannot defer another command",
                std::nullopt};
        }
        bool directRoutingTarget = routing;
        while (!deferredCommands.empty()) {
            auto deferred = deferredCommands.takeFront();
            if (directRoutingTarget) {
                const auto* target = deferred.command.id.handle().valid()
                                         ? catalog.find(
                                               deferred.command.id.handle())
                                         : catalog.find(
                                               deferred.command.id.name());
                if (target && target->effect == CommandEffect::Routing) {
                    deferredCommands.clear();
                    return CatalogDispatchResult{
                        CommandError::HandlerFailed,
                        "a routing command cannot target another routing "
                        "command",
                        std::nullopt};
                }
            }
            directRoutingTarget = false;
            auto const deferredResult = dispatchAs(deferred.command);
            // The first failure is reported, naming the command that failed,
            // and the rest are abandoned: continuing would run the remainder of
            // a sequence whose earlier step did not happen.
            if (!deferredResult.accepted()) {
                deferredCommands.clear();
                return CatalogDispatchResult{
                    deferredResult.error,
                    std::string{deferred.command.id.name()} + ": " +
                        deferredResult.message, std::nullopt};
            }
            if (deferredResult.viewAction &&
                !deferredCommands.empty()) {
                deferredCommands.clear();
                return CatalogDispatchResult{
                    CommandError::HandlerFailed,
                    "a deferred view-action command cannot precede another "
                    "command",
                    std::nullopt};
            }
            outcome = deferredResult;
        }
        return outcome;
    };
    auto result = dispatchAndDrain(command);
    // The file picker's submit is file.open, which (unlike palette.execute) has
    // no prompt side effects of its own.  Closing it here rather than in the
    // client keeps close-on-success semantics identical for keyboard and
    // pointer submits: a rejected open -- the file was removed between the walk
    // and the submit -- leaves the picker open with its query intact.
    if (result.accepted() && screen.openPicker() == PickerKind::File &&
        command.id == "file.open") {
        (void)screen.prompt().cancel();
    }
    return {result.error, std::move(result.message),
            std::move(result.viewAction)};
}

ClientInputResult EditorSession::input(ClientInput const& input) {
    if (impl_->catalog.dispatchInProgress()) {
        return {ClientInputOutcome::Rejected, std::nullopt,
                CommandResult{CommandError::HandlerFailed,
                              std::string{kNestedDispatchRefusal},
                              {}}};
    }
    std::lock_guard operationLock{impl_->operationMutex};
    return inputLocked(*impl_, input);
}

CommandResult EditorSession::dispatch(ClientCommand const& command) {
    // A handler must be refused before taking the non-recursive aggregate lock.
    if (impl_->catalog.dispatchInProgress()) {
        return {CommandError::HandlerFailed,
                std::string{kNestedDispatchRefusal}};
    }
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->dispatchLocked(command);
}

SessionTopology EditorSession::topology() const {
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->topology;
}

CommandCatalog const& EditorSession::commandCatalog() const {
    return impl_->catalog;
}

CommandHandle EditorSession::registerCommand(CommandSpec command) {
    if (dispatchInProgress()) {
        throw std::logic_error{"commands cannot be registered during dispatch"};
    }
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->catalog.add(std::move(command));
}

std::vector<CommandHandle> EditorSession::replaceCommandGeneration(
    std::span<CommandHandle const> retire,
    std::vector<CommandSpec> commands) {
    if (dispatchInProgress()) {
        throw std::logic_error{
            "command generations cannot be replaced during dispatch"};
    }
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->catalog.replaceGeneration(retire, std::move(commands));
}

std::uint64_t EditorSession::gitFullRefreshCountForTest() const {
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->gitDiffWorker.fullRefreshCount();
}

std::filesystem::path const& EditorSession::workspaceRoot() const noexcept { return impl_->root; }
DiffIngressResult EditorSession::applyExternalDiffBurst(
    std::vector<ExternalDiffRevision> changes) {
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->applyExternalDiffBurst(std::move(changes));
}
DiffIngressResult EditorSession::applyGitDiffScan(GitDiffScan scan) {
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->applyGitDiffScan(std::move(scan));
}
int EditorSession::gitDiffWakeDescriptor() const {
    return impl_->gitDiffWakeDescriptor();
}

std::string EditorSession::activeDocumentText() const {
    std::lock_guard operationLock{impl_->operationMutex};
    return impl_->activeText();
}

EditorSession::DraftReopenNotice EditorSession::activeDraftReopenNotice() const {
    std::lock_guard operationLock{impl_->operationMutex};
    const auto id = impl_->activeDocumentId();
    if (!id) return DraftReopenNotice::None;
    const auto found = impl_->documentRuntimeStates.find(id->value());
    if (found == impl_->documentRuntimeStates.end()) return DraftReopenNotice::None;
    switch (found->second.reopen) {
        case DraftReopenOutcome::None: return DraftReopenNotice::None;
        case DraftReopenOutcome::Restored: return DraftReopenNotice::Restored;
        case DraftReopenOutcome::Conflict: return DraftReopenNotice::Conflict;
    }
    return DraftReopenNotice::None;
}

} // namespace ssg
