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
    const auto* found = std::find_if(
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
    runtime.enqueueStatus(
        StatusPriority::Warning,
        "could not fully prune the deleted-file archive: " + report.message);
}

CommandHandlerResult openDocumentResult(Editor& runtime,
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
        if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) {
            return std::string{"document has no file to rename"};
        }
    }
    return std::nullopt;
}

// Shared post-operation cleanup for tab operations.
CommandHandlerResult finishTabOperation(Editor& runtime,
                                         TabResult const& result,
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
bool closesTab(const TabState& state, const TabLifecycleResult& result) {
    return result.accepted() && (!state.dirty || result.durable) &&
           (result.compensation.has_value() || result.ephemeral);
}

}  // namespace

CommandHandlerResult createFileByPath(Editor& runtime, std::string_view path) {
    if (path.empty()) return failure("file path must not be empty");
    auto result = runtime.workspace.newFile(std::string{path});
    return openDocumentResult(runtime, result);
}

CommandHandlerResult openRecentFile(Editor& runtime, std::size_t index) {
    auto result = runtime.workspace.openRecent(index);
    return openDocumentResult(runtime, result);
}

CommandHandlerResult openDroppedContent(Editor& runtime,
                                         std::span<const std::uint8_t> bytes,
                                         std::string_view label) {
    auto result = runtime.workspace.openDroppedContent(bytes, label);
    return openDocumentResult(runtime, result);
}

CommandHandlerResult reopenWithEncoding(Editor& runtime, TextEncoding encoding) {
    if (runtime.activeTabIsLiveDiff())
        return failure(
            "file.reopen_with_encoding is unavailable in live diff tabs");
    auto document = runtime.activeDocumentId();
    if (!document) return failure("no active document");
    auto result = runtime.workspace.reopenWithEncoding(*document, encoding);
    if (!result.accepted()) return failure(workspaceMessage(result));
    runtime.refreshSyntax();
    return runtime.updateTabsFor(*document);
}

CommandHandlerResult setFileEncoding(Editor& runtime, TextEncoding encoding) {
    if (runtime.activeTabIsLiveDiff())
        return failure("file.set_encoding is unavailable in live diff tabs");
    auto document = runtime.activeDocumentId();
    if (!document) return failure("no active document");
    auto result = runtime.workspace.setEncoding(*document, encoding);
    if (!result.accepted()) return failure(workspaceMessage(result));
    runtime.refreshSyntax();
    return runtime.updateTabsFor(*document);
}

CommandHandlerResult setFileLineEnding(Editor& runtime, LineEnding lineEnding) {
    if (runtime.activeTabIsLiveDiff())
        return failure("file.set_line_ending is unavailable in live diff tabs");
    auto document = runtime.activeDocumentId();
    if (!document) return failure("no active document");
    auto result = runtime.workspace.setLineEnding(*document, lineEnding);
    if (!result.accepted()) return failure(workspaceMessage(result));
    runtime.refreshSyntax();
    return runtime.updateTabsFor(*document);
}

CommandHandlerResult setFileFinalNewline(Editor& runtime, bool finalNewline) {
    if (runtime.activeTabIsLiveDiff())
        return failure(
            "file.set_final_newline is unavailable in live diff tabs");
    auto document = runtime.activeDocumentId();
    if (!document) return failure("no active document");
    auto result = runtime.workspace.setFinalNewline(*document, finalNewline);
    if (!result.accepted()) return failure(workspaceMessage(result));
    runtime.refreshSyntax();
    return runtime.updateTabsFor(*document);
}

CommandHandlerResult activateTab(Editor& runtime, TabId tabId) {
    const auto prevActive = runtime.tabs.viewState().active;
    auto result = runtime.tabs.activate(tabId);
    return finishTabOperation(runtime, result, prevActive, true, true);
}

CommandHandlerResult closeTabById(Editor& runtime, TabId tabId) {
    const auto& tabs = runtime.tabs.viewState().tabs;
    const auto found = std::find_if(
        tabs.begin(), tabs.end(),
        [tabId](const TabState& candidate) { return candidate.id == tabId; });
    if (found == tabs.end()) return failure("tab is not open");
    auto outcome = runtime.closeTab(*found, std::chrono::milliseconds{100});
    auto result = runtime.tabs.close(tabId, std::move(outcome));
    return finishTabOperation(runtime, result, std::nullopt, false, false);
}

CommandHandlerResult applyFilePathCompletion(Editor& runtime,
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
            return runtime.updateTabsFor(*id);
        }
        case PromptCompletion::FileRename: {
            auto id = runtime.activeDocumentId();
            if (!id) return failure("no active document");
            result = runtime.workspace.renameFile(*id, std::string{path});
            if (!result.accepted()) return failure(workspaceMessage(result));
            (void)runtime.refreshTree();
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

CommandHandlerResult Editor::updateTabsFor(FileDocumentId document) {
    auto state = workspace.state(document);
    if (!state) return failure("workspace document does not exist");
    auto const* opened = workspace.tryDocument(document);
    if (opened == nullptr) return failure("workspace document does not exist");
    auto result = tabs.updateDocument(document, state->key, state->displayLabel,
                                       opened->mode(), state->dirty,
                                       scratch.durabilityState().kind);
    return result.accepted() ? success() : failure(tabMessage(result));
}

CommandHandlerResult Editor::activateDocument(FileDocumentId document) {
    auto state = workspace.state(document);
    if (!state) return failure("workspace document does not exist");
    auto const* opened = workspace.tryDocument(document);
    if (opened == nullptr) return failure("workspace document does not exist");
    // Note the startup scratch buffer BEFORE opening, while it is still the only
    // tab.  Every session begins on an empty untitled buffer; opening a file
    // beside it leaves a blank tab nobody asked for and nobody will use.  It is
    // discarded only when it is the sole tab, is untitled, and holds nothing --
    // so a buffer the user typed into, or deliberately kept beside others, is
    // never taken away.
    std::optional<TabState> disposableScratch;
    if (auto const& view = tabs.viewState(); view.tabs.size() == 1) {
        auto const& only = view.tabs.front();
        if (only.document && *only.document != document && only.documentKey &&
            only.documentKey->kind() == JournalDocumentKeyKind::Untitled) {
            auto const* scratchDocument = workspace.tryDocument(*only.document);
            if (scratchDocument != nullptr &&
                scratchDocument->snapshot().text.empty()) {
                disposableScratch = only;
            }
        }
    }
    // Reconcile a recovered draft only the first time a document is opened from
    // disk (fresh runtime state), never when re-activating an already-open tab —
    // that would clobber the live buffer with a stale draft.
    const bool freshlyOpened =
        documentRuntimeStates.find(document.value()) == documentRuntimeStates.end();
    ensureDocumentRuntimeState(document);
    if (freshlyOpened) reconcileDraftOnOpen(document);
    state = workspace.state(document);
    auto result = tabs.openDocument(document, state->key, state->displayLabel,
                                     opened->mode(), state->dirty,
                                     scratch.durabilityState().kind);
    if (!result.accepted()) return failure(tabMessage(result));
    // Closed only after the open succeeded, so a failed open never costs the
    // buffer the user still has. closeTab retires the workspace document and
    // TabManager consumes the completed outcome.
    if (disposableScratch) {
        auto outcome = closeTab(*disposableScratch, std::chrono::milliseconds{100});
        (void)tabs.close(disposableScratch->id, std::move(outcome));
    }
    resetSelectionForActiveDocument();
    refreshSyntax();
    reconcileFindDocument();
    screen.refreshNoticePresence(noticePresent());
    screen.refreshExternalModificationPresence(externalModificationPresent());
    screen.refreshStatusActions(status.actionNodes());
    return success();
}

// Opening, saving, renaming and deleting files. All registered commands are
// no-argument; callers with explicit paths use applyFilePathCompletion or the
// typed file functions directly.
void registerFileCommands(CommandCatalog& catalog, Editor& runtime) {
    auto spec = [](std::string id, std::string summary) {
        return CommandSpec{
            .id = std::move(id),
            .owner = "file-commands",
            .summary = std::move(summary),
            .effect = CommandEffect::Mutation,
            .luaApi = true,
        };
    };

    {
        auto built = spec("workspace.open_directory", "Open Directory");
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            auto opened = openGenericPrompt(runtime.screen.prompt(),
                fileCommandPathPrompt(FileCommand::OpenDirectory));
            if (!opened.accepted()) return failure(opened.error->message);
            return success();
        });
        catalog.add(std::move(built));
    }
    {
        auto built = spec("file.new", "New File");
        built.label = "New File";
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            auto result = runtime.workspace.newDocument();
            return openDocumentResult(runtime, result);
        });
        catalog.add(std::move(built));
    }
    {
        auto built = spec("file.open", "Open File");
        built.label = "Open File";
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            auto opened = openGenericPrompt(runtime.screen.prompt(),
                fileCommandPathPrompt(FileCommand::Open));
            if (!opened.accepted()) return failure(opened.error->message);
            return success();
        });
        catalog.add(std::move(built));
    }
    {
        auto built = spec("file.save", "Save File");
        built.label = "Save File";
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
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
            if (state && state->key.kind() != JournalDocumentKeyKind::Saved) {
                auto opened = openGenericPrompt(runtime.screen.prompt(),
                    fileCommandPathPrompt(FileCommand::SaveAs));
                if (!opened.accepted()) return failure(opened.error->message);
                return success();
            }
            auto result = runtime.workspace.save(*id);
            if (!result.accepted()) return failure(workspaceMessage(result));
            return runtime.updateTabsFor(*id);
        });
        catalog.add(std::move(built));
    }
    {
        auto built = spec("file.save_all", "Save All Files");
        built.label = "Save All Files";
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            auto result = runtime.workspace.saveAll();
            if (!result.accepted()) return failure(workspaceMessage(result));
            for (auto id : runtime.workspace.documents()) (void)runtime.updateTabsFor(id);
            return success();
        });
        catalog.add(std::move(built));
    }
    {
        auto built = spec("file.save_as", "Save File As");
        built.label = "Save File As";
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            if (auto refusal = pathCommandPrecondition(runtime, FileCommand::SaveAs))
                return failure(*refusal);
            auto opened = openGenericPrompt(runtime.screen.prompt(),
                fileCommandPathPrompt(FileCommand::SaveAs));
            if (!opened.accepted()) return failure(opened.error->message);
            return success();
        });
        catalog.add(std::move(built));
    }
    {
        auto built = spec("file.reload", "Reload File");
        built.label = "Reload File";
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
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
        catalog.add(std::move(built));
    }
    {
        auto built = spec("file.rename", "Rename File");
        built.label = "Rename File";
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            if (auto refusal = pathCommandPrecondition(runtime, FileCommand::Rename))
                return failure(*refusal);
            auto opened = openGenericPrompt(runtime.screen.prompt(),
                fileCommandPathPrompt(FileCommand::Rename));
            if (!opened.accepted()) return failure(opened.error->message);
            return success();
        });
        catalog.add(std::move(built));
    }
    {
        auto built = spec("file.delete", "Delete File");
        built.label = "Delete File";
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
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
        catalog.add(std::move(built));
    }
    {
        auto built = spec("file.new_directory", "New Directory");
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            auto opened = openGenericPrompt(runtime.screen.prompt(),
                fileCommandPathPrompt(FileCommand::NewDirectory));
            if (!opened.accepted()) return failure(opened.error->message);
            return success();
        });
        catalog.add(std::move(built));
    }
}

// Tabs. All registered commands operate on the active tab (no-argument);
// callers with an explicit TabId use activateTab or closeTabById directly.
void registerTabCommands(CommandCatalog& catalog, Editor& runtime) {
    auto tabSpec = [](std::string id, std::string summary) {
        return CommandSpec{
            .id = std::move(id),
            .owner = "tab-management",
            .summary = std::move(summary),
            .effect = CommandEffect::Mutation,
            .luaApi = true,
        };
    };

    {
        auto built = tabSpec("tab.close", "Close Tab");
        built.label = "Close Tab";
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            auto active = runtime.tabs.viewState().active;
            if (!active) return failure("no active tab");
            return closeTabById(runtime, *active);
        });
        catalog.add(std::move(built));
    }
    {
        auto built = tabSpec("tab.close_others", "Close Other Tabs");
        built.label = "Close Other Tabs";
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
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
                    runtime.closeTab(target, std::chrono::milliseconds{100},
                                     alreadyClosed)};
                if (closesTab(target, outcome.result))
                    alreadyClosed.push_back(target.id);
                outcomes.push_back(std::move(outcome));
            }
            auto result = runtime.tabs.closeOthers(tab, std::move(outcomes));
            return finishTabOperation(runtime, result, std::nullopt, false, false);
        });
        catalog.add(std::move(built));
    }
    {
        auto built = tabSpec("tab.close_all", "Close All Tabs");
        built.label = "Close All Tabs";
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            std::vector<TabId> alreadyClosed;
            std::vector<TabState> targets = runtime.tabs.viewState().tabs;
            std::vector<TabCloseOutcome> outcomes;
            outcomes.reserve(targets.size());
            for (const auto& target : targets) {
                auto outcome = TabCloseOutcome{
                    target.id,
                    runtime.closeTab(target, std::chrono::milliseconds{100},
                                     alreadyClosed)};
                if (closesTab(target, outcome.result))
                    alreadyClosed.push_back(target.id);
                outcomes.push_back(std::move(outcome));
            }
            auto result = runtime.tabs.closeAll(std::move(outcomes));
            return finishTabOperation(runtime, result, std::nullopt, false, false);
        });
        catalog.add(std::move(built));
    }
    {
        auto built = tabSpec("tab.reopen_closed", "Reopen Closed");
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
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
        catalog.add(std::move(built));
    }
    {
        auto built = tabSpec("tab.next", "Next Tab");
        built.label = "Next Tab";
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            const auto prevActive = runtime.tabs.viewState().active;
            auto result = runtime.tabs.next();
            return finishTabOperation(runtime, result, prevActive, false, true);
        });
        catalog.add(std::move(built));
    }
    {
        auto built = tabSpec("tab.previous", "Previous Tab");
        built.label = "Previous Tab";
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            const auto prevActive = runtime.tabs.viewState().active;
            auto result = runtime.tabs.previous();
            return finishTabOperation(runtime, result, prevActive, false, true);
        });
        catalog.add(std::move(built));
    }
    {
        auto built = tabSpec("tab.move_left", "Move Left");
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            auto tab = runtime.tabs.viewState().active.value_or(TabId{0});
            auto result = runtime.tabs.moveLeft(tab);
            return finishTabOperation(runtime, result, std::nullopt, false, false);
        });
        catalog.add(std::move(built));
    }
    {
        auto built = tabSpec("tab.move_right", "Move Right");
        built.binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            auto tab = runtime.tabs.viewState().active.value_or(TabId{0});
            auto result = runtime.tabs.moveRight(tab);
            return finishTabOperation(runtime, result, std::nullopt, false, false);
        });
        catalog.add(std::move(built));
    }
}

// Draft recovery commands (single-file draft recovery, M15). draft.diff opens a
// live diff of the current buffer (the draft) against its current disk content,
// so a conflict can be inspected before it is resolved. In-process only: it
// opens a live diff tab, a concept with no remote representation.
void registerDraftCommands(CommandCatalog& catalog,
                           Editor& runtime) {
    catalog.add(CommandSpec{
        .id = "draft.diff",
        .owner = "draft-recovery",
        .label = "Diff Draft Against Disk",
        .summary = "Diff Draft Against Disk",
        .effect = CommandEffect::Mutation,
        .luaApi = true,
        .binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            return runtime.openDraftDiff();
        }),
    });
    catalog.add(CommandSpec{
        .id = "draft.discard",
        .owner = "draft-recovery",
        .label = "Discard Draft (Use Disk)",
        .summary = "Discard Draft (Use Disk)",
        .effect = CommandEffect::Mutation,
        .luaApi = true,
        .binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            return runtime.discardDraft();
        }),
    });
    catalog.add(CommandSpec{
        .id = "draft.dismiss",
        .owner = "draft-recovery",
        .label = "Dismiss Draft Notice",
        .summary = "Dismiss Draft Notice",
        .effect = CommandEffect::Mutation,
        .luaApi = true,
        .binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            return runtime.dismissDraftNotice();
        }),
    });
}

void bindRuntimeFiles(CommandCatalog& catalog, Editor& runtime) {
    bindExternalModificationCommands(catalog, runtime);
    registerFileCommands(catalog, runtime);
    registerTabCommands(catalog, runtime);
    registerDraftCommands(catalog, runtime);
}

} // namespace ssg
