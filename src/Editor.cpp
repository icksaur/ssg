#include <ssg/CommandCatalog.h>
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

    bind(seq({"Alt+KeyS"}), "file.save", "*");
    bind(seq({"Alt+KeyN"}), "file.new", "*");
    bind(seq({"Alt+KeyZ"}), "edit.undo", "*");
    bind(seq({"Alt+Shift+KeyZ"}), "edit.redo", "*");
    bind(seq({"Alt+KeyP"}), "file_finder.open", "*");
    bind(seq({"Alt+Shift+KeyP"}), "palette.open", "*");
    bind(seq({"Alt+KeyB"}), "panel.toggle", "*");
    bind(seq({"Alt+KeyH"}), "help.open", "*");
    bind(seq({"Alt+KeyO"}), "panel.focus", "*");
    // Tab cycling: Alt+BracketRight/Left cannot be used -- ESC ] / ESC [ are
    // the OSC / CSI introducers -- so the brackets give way to
    // Alt+Period/Comma.
    bind(seq({"Alt+Period"}), "tab.next", "*");
    bind(seq({"Alt+Comma"}), "tab.previous", "*");
    bind(seq({"Alt+KeyW"}), "tab.close", "*");
    bind(seq({"Alt+Shift+KeyT"}), "settings.open", "*");
    bind(seq({"Alt+KeyA"}), "select.all", "*");
    bind(seq({"Alt+KeyD"}), "select.add_next_occurrence", "*");
    bind(seq({"Alt+KeyI"}), "select.split_into_lines", "*");
    bind(seq({"Alt+KeyK"}), "select.add_cursor_up", "*");
    bind(seq({"Alt+KeyJ"}), "select.add_cursor_down", "*");
    bind(seq({"Alt+Slash"}), "find.open", "*");
    bind(seq({"Alt+Digit8"}), "find.word_under_cursor", "editor");
    bind(seq({"Alt+KeyR"}), "replace.open", "*");
    bind(seq({"Alt+Shift+KeyD"}), "draft.discard", "editor");

    bind(seq({"Alt+KeyX"}), "clipboard.cut", "editor");
    bind(seq({"Alt+KeyC"}), "clipboard.copy", "editor");
    bind(seq({"Alt+KeyV"}), "clipboard.paste", "editor");
    // Prompts have no selection to cut or copy.
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
    bind(seq({"Alt+Home"}), "cursor.document_start", "editor");
    bind(seq({"Alt+End"}), "cursor.document_end", "editor");
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
    bind(seq({"Alt+Backspace"}), "text.delete_word_backward", "editor");
    bind(seq({"Alt+ArrowLeft"}), "cursor.word_left", "editor");
    bind(seq({"Alt+ArrowRight"}), "cursor.word_right", "editor");
    bind(seq({"Alt+Shift+ArrowLeft"}), "select.word_left", "editor");
    bind(seq({"Alt+Shift+ArrowRight"}), "select.word_right", "editor");

    bind(seq({"ArrowDown"}), "tree.select_next", "panel");
    bind(seq({"ArrowUp"}), "tree.select_previous", "panel");
    bind(seq({"Enter"}), "tree.activate", "panel");

    bind(seq({"Enter"}), "prompt.submit", "prompt");
    bind(seq({"Escape"}), "prompt.cancel", "prompt");
    bind(seq({"ArrowDown"}), "prompt.next", "prompt");
    bind(seq({"ArrowUp"}), "prompt.previous", "prompt");
    bind(seq({"Tab"}), "prompt.focus_next_control", "prompt");
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

Editor::Editor(std::filesystem::path canonicalCwd,
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
      selection{initialSelection()}, clipboard{4}, tabs{},
      external{workspace, diff}, syntaxParser{std::move(parser)},
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

WorkspaceSnapshot Editor::snapshot(std::uint64_t revision) const {
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

CommandHandlerResult Editor::openOrFocusLiveDiffTab(
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

CommandHandlerResult Editor::openReadOnlyTab(
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

CommandHandlerResult Editor::openDraftDiff() {
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
    std::error_code code;
    std::filesystem::create_directories(archiveDir, code);
    if (code) return false;

    const auto target =
        archiveDir / uniqueDiscardedDraftArchiveName(savedPath);
    const std::span<const std::byte> bytes{
        reinterpret_cast<const std::byte*>(content.data()), content.size()};
    return createFileExclusively(target, bytes).ok();
}

CommandHandlerResult Editor::discardDraft() {
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

CommandHandlerResult Editor::dismissDraftNotice() {
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
    ++treeScanCount;
    tree.replaceProvider(TreeProviderSnapshot::fromFilesystem(
        TreeProviderId{"filesystem"}, root));
    rebuildFileCandidates();
    return true;
}

void Editor::refreshTreeForPublication() {
    (void)refreshTree();
}

void Editor::rebuildInteractionSchema(
    const StyleDimensions& dimensions,
    std::string_view promptSigil) {
    (void)screen.updateComposition(
        assembleScreen("help.open", dimensions, promptSigil));
}

bool Editor::openPickerPrompt(PickerKind kind) {
    return screen.openFinder(kind);
}

// The index opens its OWN repository handle rather than sharing the git-diff
// worker's: that one is owned by its thread, and libgit2 handles are not safe
// to use from two threads.
void Editor::rebuildFileCandidates() {
    auto matcher = makePlatformGitIgnoreMatcher(root);
    WorkspaceFileIndexOptions options;
    options.respectGitignore =
        boolSetting(settings, SettingKey::FileFinderRespectGitignore, true);
    fileCandidates =
        std::move(buildWorkspaceFileIndex(root, *matcher, options).candidates);
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
    ++syntaxRunCount;
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

CommandHandlerResult Editor::splitPane(SplitAxis axis) {
    (void)paneTopology.splitActive(axis);
    return success();
}

CommandHandlerResult Editor::closePane() {
    if (!paneTopology.closeActive()) {
        return failure("the only pane cannot be closed");
    }
    return success();
}

CommandHandlerResult Editor::cyclePane(
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

void Editor::focusEditor() {
    std::lock_guard operationLock{operationMutex};
    screen.focusEditor();
}

EditorCreateResult createEditor(EditorConfig config) {
    try {
        auto cwd = canonicalDirectory(config.cwd);
        if (config.scratchRoot.empty()) config.scratchRoot = cwd / ".ssg" / "scratch";
        if (config.recoveryRoot.empty()) config.recoveryRoot = cwd / ".ssg" / "recovery";
        if (config.archiveRoot.empty()) config.archiveRoot = cwd / ".ssg" / "archive";
        std::filesystem::create_directories(config.scratchRoot);
        std::filesystem::create_directories(config.recoveryRoot);
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
        registerAllCommands(editor->catalog, *editor);
        return {std::move(editor), {}};
    } catch (std::exception const& exception) {
        return {nullptr, exception.what()};
    }
}

PumpResult Editor::pump() {
    if (catalog.dispatchInProgress()) {
        throw std::logic_error{"worker results cannot be pumped during dispatch"};
    }
    std::lock_guard operationLock{operationMutex};
    return {gitDiffIngress.drainGitDiffWorker()};
}

Editor::DeferredWorkCounts Editor::deferredWorkCounts() const {
    std::lock_guard operationLock{operationMutex};
    return {syntaxRunCount, treeScanCount};
}

std::uint64_t Editor::liveDocumentRuntimeStateCountForTests() {
    return DocumentRuntimeState::liveInstances();
}

bool Editor::dispatchInProgress() const noexcept {
    return catalog.dispatchInProgress();
}

bool Editor::deferDispatch(ClientCommand command) {
    if (!catalog.dispatchInProgress()) return false;
    return deferredCommands.enqueue({std::move(command)});
}

CommandResult Editor::dispatchLocked(ClientCommand const& command) {
    const auto dispatchAndReconcile = [&](const ClientCommand& dispatched) {
        const auto revisionsBefore = documentRevisions(workspace);
        screen.refreshExternalModificationPresence(
            externalModificationPresent());
        auto result = catalog.dispatch(dispatched);
        if (result.activeWorkspace) {
            sessionTopology.activeWorkspace = result.activeWorkspace;
        }
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
    const auto dispatchAndDrain = [&](const ClientCommand& dispatched) {
        const auto* requested = dispatched.id.handle().valid()
                                    ? catalog.find(dispatched.id.handle())
                                    : catalog.find(dispatched.id.name());
        const bool routing =
            requested && requested->effect == CommandEffect::Routing;
        auto outcome = dispatchAndReconcile(dispatched);
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
            auto const deferredResult =
                dispatchAndReconcile(deferred.command);
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
    if (result.accepted() && screen.openPicker() == PickerKind::File &&
        command.id == "file.open") {
        (void)screen.prompt().cancel();
    }
    return {result.error, std::move(result.message),
            std::move(result.viewAction)};
}

ClientInputResult Editor::input(ClientInput const& input) {
    if (catalog.dispatchInProgress()) {
        return {ClientInputOutcome::Rejected, std::nullopt,
                CommandResult{CommandError::HandlerFailed,
                              std::string{kNestedDispatchRefusal},
                              {}}};
    }
    std::lock_guard operationLock{operationMutex};
    return inputLocked(*this, input);
}

CommandResult Editor::dispatch(ClientCommand const& command) {
    // A handler must be refused before taking the non-recursive aggregate lock.
    if (catalog.dispatchInProgress()) {
        return {CommandError::HandlerFailed,
                std::string{kNestedDispatchRefusal}};
    }
    std::lock_guard operationLock{operationMutex};
    return dispatchLocked(command);
}

SessionTopology Editor::topology() const {
    std::lock_guard operationLock{operationMutex};
    return sessionTopology;
}

CommandCatalog const& Editor::commandCatalog() const {
    return catalog;
}

CommandHandle Editor::registerCommand(CommandSpec command) {
    if (dispatchInProgress()) {
        throw std::logic_error{"commands cannot be registered during dispatch"};
    }
    std::lock_guard operationLock{operationMutex};
    return catalog.add(std::move(command));
}

std::vector<CommandHandle> Editor::replaceCommandGeneration(
    std::span<CommandHandle const> retire,
    std::vector<CommandSpec> commands) {
    if (dispatchInProgress()) {
        throw std::logic_error{
            "command generations cannot be replaced during dispatch"};
    }
    std::lock_guard operationLock{operationMutex};
    return catalog.replaceGeneration(retire, std::move(commands));
}

std::filesystem::path const& Editor::workspaceRoot() const noexcept {
    return root;
}

DiffIngressResult Editor::applyGitDiffScan(GitDiffScan scan) {
    std::lock_guard operationLock{operationMutex};
    return gitDiffIngress.applyGitDiffScanLocked(std::move(scan));
}

std::string Editor::activeDocumentText() const {
    std::lock_guard operationLock{operationMutex};
    return activeText();
}

Editor::DraftReopenNotice Editor::activeDraftReopenNotice() const {
    std::lock_guard operationLock{operationMutex};
    const auto id = activeDocumentId();
    if (!id) return DraftReopenNotice::None;
    const auto found = documentRuntimeStates.find(id->value());
    if (found == documentRuntimeStates.end()) return DraftReopenNotice::None;
    switch (found->second.reopen) {
        case DraftReopenOutcome::None: return DraftReopenNotice::None;
        case DraftReopenOutcome::Restored: return DraftReopenNotice::Restored;
        case DraftReopenOutcome::Conflict: return DraftReopenNotice::Conflict;
    }
    return DraftReopenNotice::None;
}

} // namespace ssg
