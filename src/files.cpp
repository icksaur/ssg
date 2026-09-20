#include <ssg/Editor.h>

#include <ssg/FileCommands.h>

#include <algorithm>
#include <chrono>
#include <span>
#include <utility>

namespace ssg {
namespace {

bool mutatesTheActiveDocumentsFile(FileCommand command) {
    const auto& descriptors = kFileCommands;
    const auto found = std::find_if(
        descriptors.begin(), descriptors.end(),
        [&](const FileCommandDescriptor& entry) {
            return entry.command == command;
        });
    return found != descriptors.end() && found->mutatesActiveDocumentFile;
}

// Prunes the archive and surfaces a genuine housekeeping FAILURE as a
// non-fatal status. Deliberately not fatal: a corrupt archive entry must never
// stop a user opening their workspace. Retained-entry counts stay in the return
// value rather than nagging on every startup.
void pruneArchiveReportingFailures(Editor& runtime) {
    const auto report = runtime.workspace.pruneArchive();
    if (report.ok()) return;
    runtime.showStatus(
        "could not fully prune the deleted-file archive: " + report.message);
}

OperationResult openDocumentResult(Editor& runtime,
                                   WorkspaceResult const& result) {
    if (!result.accepted() || !result.document) return failure(workspaceMessage(result));
    return runtime.activateDocument(*result.document);
}

// Why a command cannot run right now, or nullopt if it can. Checked BEFORE the
// path prompt opens: prompting first and validating after would make the user
// type a name only to be told there was nothing to save, and would discard what
// they typed. The same reasons are re-checked by the command itself when the
// submitted path arrives, since state can change while the prompt is open.
std::optional<std::string> pathCommandPrecondition(
    const Editor& runtime, FileCommand command) {
    if (!mutatesTheActiveDocumentsFile(command)) return std::nullopt;

    if (runtime.activeTabIsLiveDiff()) {
        return std::string{"command is unavailable in live diff tabs"};
    }
    auto id = runtime.activeDocumentId();
    if (!id) return std::string{"no active document"};
    if (command == FileCommand::Rename) {
        auto const state = runtime.workspace.state(*id);
        // Renaming needs a file to rename. An unnamed buffer has none, and the
        // command that gives it one is save_as.
        if (!state || state->key.kind() != DocumentKeyKind::Saved) {
            return std::string{"document has no file to rename"};
        }
    }
    return std::nullopt;
}

// Shared post-operation cleanup for tab operations.
OperationResult finishTabOperation(Editor& runtime, TabResult const& result,
                                  std::optional<TabId> prevActive,
                                  bool focusEditorOnSuccess,
                                  bool trackNavigation) {
    if (!result.accepted()) return failure(tabMessage(result));
    runtime.clampSelectionToActiveDocument();
    runtime.refreshSyntax();
    if (focusEditorOnSuccess) runtime.screen.focusEditor();
    if (trackNavigation && runtime.tabs.viewState().active != prevActive)
        runtime.recordNavigation(NavigationClass::User);
    return success();
}

// Whether a tab's close lifecycle result means the tab actually closes.
bool closesTab(const TabLifecycleResult& result) {
    return result.accepted() &&
           (result.compensation.has_value() || result.ephemeral);
}

}  // namespace

OperationResult createFileByPath(Editor& runtime, std::string_view path) {
    if (path.empty()) return failure("file path must not be empty");
    auto result = runtime.workspace.newFile(std::string{path});
    return openDocumentResult(runtime, result);
}

OperationResult openStartupTarget(Editor& runtime, std::string_view path) {
    if (path.empty()) return failure("file path must not be empty");
    if (const auto restored = runtime.workspace.documentForPath(path)) {
        return runtime.activateDocument(*restored);
    }
    try {
        return statFile(runtime.workspace.root() / std::filesystem::path{path})
                   ? applyFilePathCompletion(runtime, PromptCompletion::FileOpen,
                                             path)
                   : createFileByPath(runtime, path);
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

OperationResult openDroppedContent(Editor& runtime,
                                   std::span<const std::uint8_t> bytes,
                                   std::string_view label) {
    auto result = runtime.workspace.openDroppedContent(bytes, label);
    return openDocumentResult(runtime, result);
}

OperationResult activateTab(Editor& runtime, TabId tabId) {
    const auto prevActive = runtime.tabs.viewState().active;
    auto result = runtime.tabs.activate(tabId);
    return finishTabOperation(runtime, result, prevActive, true, true);
}

OperationResult closeTabById(Editor& runtime, TabId tabId) {
    const auto& tabs = runtime.tabs.viewState().tabs;
    const auto found = std::find_if(
        tabs.begin(), tabs.end(),
        [tabId](const TabState& candidate) { return candidate.id == tabId; });
    if (found == tabs.end()) return failure("tab is not open");
    auto outcome = runtime.closeTab(*found);
    auto result = runtime.tabs.close(tabId, std::move(outcome));
    return finishTabOperation(runtime, result, std::nullopt, false, false);
}

OperationResult applyFilePathCompletion(Editor& runtime,
                                        PromptCompletion completion,
                                        std::string_view path) {
    const auto command = [&]() -> std::optional<FileCommand> {
        switch (completion) {
        case PromptCompletion::WorkspaceOpenDirectory:
            return FileCommand::OpenDirectory;
        case PromptCompletion::FileOpen:
            return FileCommand::Open;
        case PromptCompletion::FileSaveAs:
            return FileCommand::SaveAs;
        case PromptCompletion::FileRename:
            return FileCommand::Rename;
        case PromptCompletion::FileNewDirectory:
            return FileCommand::NewDirectory;
        default:
            return std::nullopt;
        }
    }();
    if (!command) return failure("unsupported path completion");
    if (auto refusal = pathCommandPrecondition(runtime, *command)) {
        return failure(*refusal);
    }

    WorkspaceResult result;
    switch (completion) {
        case PromptCompletion::WorkspaceOpenDirectory:
            result = runtime.workspace.openDirectory(path);
            if (!result.accepted()) return failure(workspaceMessage(result));
            runtime.root = runtime.workspace.root();
            pruneArchiveReportingFailures(runtime);
            (void)runtime.refreshTree();
            return success();
        case PromptCompletion::FileOpen:
            result = runtime.workspace.openFile(path);
            return openDocumentResult(runtime, result);
        case PromptCompletion::FileSaveAs: {
            auto id = runtime.activeDocumentId();
            if (!id) return failure("no active document");
            result = runtime.workspace.saveAs(*id, std::string{path});
            if (!result.accepted()) return failure(workspaceMessage(result));
            (void)runtime.refreshTree();
            runtime.refreshSyntax();
            return runtime.updateTabsFor(*id);
        }
        case PromptCompletion::FileRename: {
            auto id = runtime.activeDocumentId();
            if (!id) return failure("no active document");
            result = runtime.workspace.renameFile(*id, std::string{path});
            if (!result.accepted()) return failure(workspaceMessage(result));
            (void)runtime.refreshTree();
            runtime.refreshSyntax();
            return runtime.updateTabsFor(*id);
        }
        case PromptCompletion::FileNewDirectory:
            result = runtime.workspace.newDirectory(std::string{path});
            if (!result.accepted()) return failure(workspaceMessage(result));
            (void)runtime.refreshTree();
            return success();
        default:
            break;
    }
    return failure("unsupported path completion");
}

OperationResult Editor::updateTabsFor(FileDocumentId document) {
    auto state = workspace.state(document);
    if (!state) return failure("workspace document does not exist");
    auto const* opened = workspace.tryDocument(document);
    if (opened == nullptr) return failure("workspace document does not exist");
    auto result = tabs.updateDocument(document, state->key, state->displayLabel,
                                      opened->mode(), state->dirty);
    return result.accepted() ? success() : failure(tabMessage(result));
}

OperationResult Editor::activateDocument(FileDocumentId document) {
    auto state = workspace.state(document);
    if (!state) return failure("workspace document does not exist");
    auto const* opened = workspace.tryDocument(document);
    if (opened == nullptr) return failure("workspace document does not exist");
    // Note the startup empty buffer before opening, while it is still the only
    // tab.  Every session begins on an empty untitled buffer; opening a file
    // beside it leaves a blank tab nobody asked for and nobody will use.  It is
    // discarded only when it is the sole tab, is untitled, and holds nothing --
    // so a buffer the user typed into, or deliberately kept beside others, is
    // never taken away.
    std::optional<TabState> disposableBuffer;
    if (auto const& view = tabs.viewState(); view.tabs.size() == 1) {
        auto const& only = view.tabs.front();
        if (only.document && *only.document != document && only.documentKey &&
            only.documentKey->kind() == DocumentKeyKind::Untitled) {
            auto const* emptyDocument = workspace.tryDocument(*only.document);
            if (emptyDocument != nullptr &&
                emptyDocument->snapshot().text.empty()) {
                disposableBuffer = only;
            }
        }
    }
    ensureDocumentRuntimeState(document);
    state = workspace.state(document);
    auto result = tabs.openDocument(document, state->key, state->displayLabel,
                                    opened->mode(), state->dirty);
    if (!result.accepted()) return failure(tabMessage(result));
    // Closed only after the open succeeded, so a failed open never costs the
    // buffer the user still has. closeTab retires the workspace document and
    // TabManager consumes the completed outcome.
    if (disposableBuffer) {
        auto outcome = closeTab(*disposableBuffer);
        (void)tabs.close(disposableBuffer->id, std::move(outcome));
    }
    resetSelectionForActiveDocument();
    refreshSyntax();
    reconcileFindDocument();
    screen.refreshExternalModificationPresence(externalModificationPresent());
    return success();
}

// Opening, saving, renaming and deleting files. All registered commands are
// no-argument; callers with explicit paths use applyFilePathCompletion or the
// typed file functions directly.
void registerFileCommands(Commands& commands, Editor& runtime) {
    commands.add("workspace.open_directory", "Workspace Open Directory",
        [&runtime] {
            auto opened = openGenericPrompt(runtime.screen.prompt(),
                fileCommandPathPrompt(FileCommand::OpenDirectory));
            if (!opened.accepted()) return failure(opened.error->message);
            return success();
        });
    commands.add("file.new", "New File", [&runtime] {
            auto result = runtime.workspace.newDocument();
            return openDocumentResult(runtime, result);
        });
    commands.add("file.open", "Open File", [&runtime] {
            auto opened = openGenericPrompt(runtime.screen.prompt(),
                fileCommandPathPrompt(FileCommand::Open));
            if (!opened.accepted()) return failure(opened.error->message);
            return success();
        });
    commands.add("file.save", "Save File", [&runtime] {
            if (runtime.activeTabIsLiveDiff())
                return failure("command is unavailable in live diff tabs");
            auto id = runtime.activeDocumentId();
            if (!id) return failure("no active document");
            // A read-only document cannot be saved.
            if (auto const* doc = runtime.workspace.tryDocument(*id);
                doc != nullptr && doc->mode() != DocumentMode::Edit) {
                return failure("this document is read-only and cannot be saved");
            }
            // An untitled buffer cannot be saved over itself; redirect to save_as.
            auto const state = runtime.workspace.state(*id);
            if (state && state->key.kind() != DocumentKeyKind::Saved) {
                auto opened = openGenericPrompt(runtime.screen.prompt(),
                    fileCommandPathPrompt(FileCommand::SaveAs));
                if (!opened.accepted()) return failure(opened.error->message);
                return success();
            }
            auto result = runtime.workspace.save(*id);
            if (!result.accepted()) return failure(workspaceMessage(result));
            return runtime.updateTabsFor(*id);
        });
    commands.add("file.save_all", "Save All Files", [&runtime] {
            auto result = runtime.workspace.saveAll();
            if (!result.accepted()) return failure(workspaceMessage(result));
            for (auto id : runtime.workspace.documents()) (void)runtime.updateTabsFor(id);
            return success();
        });
    commands.add("file.save_as", "Save File As", [&runtime] {
            if (auto refusal = pathCommandPrecondition(runtime, FileCommand::SaveAs))
                return failure(*refusal);
            auto opened = openGenericPrompt(runtime.screen.prompt(),
                fileCommandPathPrompt(FileCommand::SaveAs));
            if (!opened.accepted()) return failure(opened.error->message);
            return success();
        });
    commands.add("file.reload", "Reload File", [&runtime] {
            if (runtime.activeTabIsLiveDiff())
                return failure("command is unavailable in live diff tabs");
            auto id = runtime.activeDocumentId();
            if (!id) return failure("no active document");
            auto result = runtime.workspace.reload(*id);
            if (!result.accepted()) return failure(workspaceMessage(result));
            runtime.resetSelectionForActiveDocument();
            runtime.refreshSyntax();
            return runtime.updateTabsFor(*id);
        });
    commands.add("file.rename", "Rename File", [&runtime] {
            if (auto refusal = pathCommandPrecondition(runtime, FileCommand::Rename))
                return failure(*refusal);
            auto opened = openGenericPrompt(runtime.screen.prompt(),
                fileCommandPathPrompt(FileCommand::Rename));
            if (!opened.accepted()) return failure(opened.error->message);
            return success();
        });
    commands.add("file.delete", "Delete File", [&runtime] {
            if (runtime.activeTabIsLiveDiff())
                return failure("command is unavailable in live diff tabs");
            auto id = runtime.activeDocumentId();
            if (!id) return failure("no active document");
            auto result = runtime.workspace.deleteFile(*id);
            if (!result.accepted()) return failure(workspaceMessage(result));
            // The tab's document no longer has backing bytes, so leaving it
            // open would offer editing and saving of a file that is gone.
            // Dropped rather than closed: deleteFile has already removed the
            // workspace entry, so the ordinary close path would fail on a
            // missing document and strand the tab. That path also owns the
            // per-document runtime state, so bypassing it means discarding
            // that state here or it accumulates for a document nobody can
            // reach again.
            (void)runtime.tabs.dropDocument(*id);
            runtime.discardDocumentRuntimeState(*id);
            (void)runtime.refreshTree();
            return success();
        });
    commands.add("file.new_directory", "File New Directory", [&runtime] {
            auto opened = openGenericPrompt(runtime.screen.prompt(),
                fileCommandPathPrompt(FileCommand::NewDirectory));
            if (!opened.accepted()) return failure(opened.error->message);
            return success();
        });
}

// Tabs. All registered commands operate on the active tab (no-argument);
// callers with an explicit TabId use activateTab or closeTabById directly.
void registerTabCommands(Commands& commands, Editor& runtime) {
    commands.add("tab.close", "Close Tab", [&runtime] {
            auto active = runtime.tabs.viewState().active;
            if (!active) return failure("no active tab");
            return closeTabById(runtime, *active);
        });
    commands.add("tab.close_others", "Close Other Tabs", [&runtime] {
            auto active = runtime.tabs.viewState().active;
            auto tab = active.value_or(TabId{0});
            const auto kept = std::find_if(
                runtime.tabs.viewState().tabs.begin(),
                runtime.tabs.viewState().tabs.end(),
                [tab](const TabState& candidate) { return candidate.id == tab; });
            if (kept == runtime.tabs.viewState().tabs.end())
                return failure("no active tab");
            std::vector<TabId> alreadyClosed;
            std::vector<TabState> targets;
            for (const auto& candidate : runtime.tabs.viewState().tabs) {
                if (candidate.id != tab) targets.push_back(candidate);
            }
            std::vector<TabCloseOutcome> outcomes;
            outcomes.reserve(targets.size());
            for (const auto& target : targets) {
                auto outcome = TabCloseOutcome{
                    target.id,
                    runtime.closeTab(target, alreadyClosed)};
                if (closesTab(outcome.result))
                    alreadyClosed.push_back(target.id);
                outcomes.push_back(std::move(outcome));
            }
            auto result = runtime.tabs.closeOthers(tab, std::move(outcomes));
            return finishTabOperation(runtime, result, std::nullopt, false, false);
        });
    commands.add("tab.close_all", "Close All Tabs", [&runtime] {
            std::vector<TabId> alreadyClosed;
            std::vector<TabState> targets = runtime.tabs.viewState().tabs;
            std::vector<TabCloseOutcome> outcomes;
            outcomes.reserve(targets.size());
            for (const auto& target : targets) {
                auto outcome = TabCloseOutcome{
                    target.id,
                    runtime.closeTab(target, alreadyClosed)};
                if (closesTab(outcome.result))
                    alreadyClosed.push_back(target.id);
                outcomes.push_back(std::move(outcome));
            }
            auto result = runtime.tabs.closeAll(std::move(outcomes));
            return finishTabOperation(runtime, result, std::nullopt, false, false);
        });
    commands.add("tab.reopen_closed", "Tab Reopen Closed", [&runtime] {
            const auto prevActive = runtime.tabs.viewState().active;
            auto reopened = runtime.tabs.beginReopenClosed();
            TabResult result;
            if (auto* immediate = std::get_if<TabResult>(&reopened)) {
                result = std::move(*immediate);
            } else {
                auto request = std::get<TabReopenRequest>(reopened);
                auto outcome = runtime.reopenTab(request.tab, request.compensation);
                result = runtime.tabs.finishReopenClosed(std::move(request),
                                                          std::move(outcome));
            }
            return finishTabOperation(runtime, result, prevActive, false, false);
        });
    commands.add("tab.next", "Next Tab", [&runtime] {
            const auto prevActive = runtime.tabs.viewState().active;
            auto result = runtime.tabs.next();
            return finishTabOperation(runtime, result, prevActive, false, true);
        });
    commands.add("tab.previous", "Previous Tab", [&runtime] {
            const auto prevActive = runtime.tabs.viewState().active;
            auto result = runtime.tabs.previous();
            return finishTabOperation(runtime, result, prevActive, false, true);
        });
    commands.add("tab.move_left", "Tab Move Left", [&runtime] {
            auto tab = runtime.tabs.viewState().active.value_or(TabId{0});
            auto result = runtime.tabs.moveLeft(tab);
            return finishTabOperation(runtime, result, std::nullopt, false, false);
        });
    commands.add("tab.move_right", "Tab Move Right", [&runtime] {
            auto tab = runtime.tabs.viewState().active.value_or(TabId{0});
            auto result = runtime.tabs.moveRight(tab);
            return finishTabOperation(runtime, result, std::nullopt, false, false);
        });
}

void bindRuntimeFiles(Commands& commands, Editor& runtime) {
    bindExternalModificationCommands(commands, runtime);
    registerFileCommands(commands, runtime);
    registerTabCommands(commands, runtime);
}

} // namespace ssg
