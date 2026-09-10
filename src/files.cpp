#include <ssg/Editor.h>

#include <ssg/FileCommands.h>

#include <algorithm>
#include <utility>

namespace ssg {
namespace {

enum class TabCommand {
    Close,
    CloseOthers,
    CloseAll,
    ReopenClosed,
    Next,
    Previous,
    Activate,
    MoveLeft,
    MoveRight,
};

std::optional<std::string> stringPayload(std::any const& payload) {
    if (auto const* value = payloadAs<std::string>(payload)) return *value;
    if (auto const* value = payloadAs<std::string_view>(payload)) return std::string{*value};
    return std::nullopt;
}

// The live-diff rule's classification, read from the command's own descriptor
// rather than re-derived here, so the rule and the catalog cannot disagree.
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

CommandHandlerResult bindFile(Editor& runtime,
                               FileCommand command,
                               std::any const& payload) {
    WorkspaceResult result;

    // The live-diff rule, applied once for every command it covers. A live diff
    // tab is a computed view of two revisions, so there is no file to save,
    // rename, reload or delete.
    if (mutatesTheActiveDocumentsFile(command) && runtime.activeTabIsLiveDiff()) {
        return failure("command is unavailable in live diff tabs");
    }

    // Every path-taking command prompts for its path when dispatched without
    // one, in ONE place rather than per command. A command that gains the
    // pathPrompt flag is wired by that fact alone, so the flag and the behavior
    // cannot drift apart.
    const auto& descriptors = kFileCommands;
    const auto* descriptor = std::find_if(
        descriptors.begin(), descriptors.end(),
        [&](const FileCommandDescriptor& entry) {
            return entry.command == command;
        });
    if (descriptor != descriptors.end() && descriptor->pathPrompt &&
        !stringPayload(payload)) {
        if (auto refusal = pathCommandPrecondition(runtime, command)) {
            return failure(*refusal);
        }
        auto opened =
            openGenericPrompt(runtime.screen.prompt(),
                              fileCommandPathPrompt(command));
        if (!opened.accepted()) return failure(opened.error->message);
        return success();
    }

    switch (command) {
        case FileCommand::OpenDirectory: {
            auto path = stringPayload(payload);
            if (!path) return failure("workspace.open_directory requires a path payload");
            result = runtime.workspace.openDirectory(*path);
            if (!result.accepted()) return failure(workspaceMessage(result));
            runtime.root = runtime.workspace.root();
            // Same housekeeping as at startup: opening a different workspace
            // means a different archive to expire.
            pruneArchiveReportingFailures(runtime);
            (void)runtime.refreshTree();
            return success();
        }
        case FileCommand::Create: {
            // A payload names the file to create; the file itself is not written
            // until the first save. No payload means an unnamed buffer and the
            // workspace supplies the label.
            auto path = stringPayload(payload);
            result = path && !path->empty() ? runtime.workspace.newFile(*path)
                                            : runtime.workspace.newDocument();
            return openDocumentResult(runtime, result);
        }
        case FileCommand::Open: {
            auto path = stringPayload(payload);
            if (!path) {
                auto opened = openGenericPrompt(runtime.screen.prompt(),
                    fileCommandPathPrompt(command));
                if (!opened.accepted()) return failure(opened.error->message);
                return success();
            }
            result = runtime.workspace.openFile(*path);
            return openDocumentResult(runtime, result);
        }
        case FileCommand::OpenRecent: {
            auto const* index = payloadAs<std::size_t>(payload);
            if (index == nullptr) return failure("file.open_recent requires an index payload");
            result = runtime.workspace.openRecent(*index);
            return openDocumentResult(runtime, result);
        }
        case FileCommand::OpenDroppedContent: {
            auto const* dropped = payloadAs<DroppedContentArguments>(payload);
            if (dropped == nullptr) return failure("file.open_dropped_content requires dropped content payload");
            result = runtime.workspace.openDroppedContent(dropped->bytes, dropped->suggestedLabel);
            return openDocumentResult(runtime, result);
        }
        case FileCommand::Save: {
            auto id = runtime.activeDocumentId();
            if (!id) return failure("no active document");
            // A read-only (or diff) document cannot be saved. Refuse before the
            // untitled -> Save-As redirect below, so a read-only help tab reports
            // a clean message instead of opening a naming prompt or writing.
            if (auto const* doc = runtime.workspace.tryDocument(*id);
                doc != nullptr && doc->mode() != DocumentMode::Edit) {
                return failure("this document is read-only and cannot be saved");
            }
            // A buffer that has never had a name cannot be saved over itself,
            // so saving it IS a save-as. Opening save_as's prompt (rather than
            // one of its own) keeps a single naming path: whatever the user
            // types is handled by the command that knows how to name a
            // document.
            auto const state = runtime.workspace.state(*id);
            if (state &&
                state->key.kind() != JournalDocumentKeyKind::Saved) {
                auto opened = openGenericPrompt(runtime.screen.prompt(),
                    fileCommandPathPrompt(FileCommand::SaveAs));
                if (!opened.accepted()) return failure(opened.error->message);
                return success();
            }
            result = runtime.workspace.save(*id);
            if (!result.accepted()) return failure(workspaceMessage(result));
            return runtime.updateTabsFor(*id);
        }
        case FileCommand::SaveAll: {
            result = runtime.workspace.saveAll();
            if (!result.accepted()) return failure(workspaceMessage(result));
            for (auto id : runtime.workspace.documents()) (void)runtime.updateTabsFor(id);
            return success();
        }
        case FileCommand::SaveAs: {
            auto id = runtime.activeDocumentId();
            auto path = stringPayload(payload);
            if (!id) return failure("no active document");
            if (!path) return failure("file.save_as requires a path payload");
            result = runtime.workspace.saveAs(*id, *path);
            if (!result.accepted()) return failure(workspaceMessage(result));
            (void)runtime.refreshTree();
            return runtime.updateTabsFor(*id);
        }
        case FileCommand::Reload: {
            auto id = runtime.activeDocumentId();
            if (!id) return failure("no active document");
            result = runtime.workspace.reload(*id);
            if (!result.accepted()) return failure(workspaceMessage(result));
            runtime.resetSelectionForActiveDocument();
            runtime.refreshSyntax();
            return runtime.updateTabsFor(*id);
        }
        case FileCommand::Rename: {
            auto id = runtime.activeDocumentId();
            auto path = stringPayload(payload);
            if (!id) return failure("no active document");
            if (!path) return failure("file.rename requires a path payload");
            result = runtime.workspace.renameFile(*id, *path);
            if (!result.accepted()) return failure(workspaceMessage(result));
            (void)runtime.refreshTree();
            return runtime.updateTabsFor(*id);
        }
        case FileCommand::Remove: {
            auto id = runtime.activeDocumentId();
            if (!id) return failure("no active document");
            result = runtime.workspace.deleteFile(*id);
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
        }
        case FileCommand::NewDirectory: {
            auto path = stringPayload(payload);
            if (!path) return failure("file.new_directory requires a path payload");
            result = runtime.workspace.newDirectory(*path);
            if (!result.accepted()) return failure(workspaceMessage(result));
            (void)runtime.refreshTree();
            return success();
        }
    }
    return failure("unknown file command");
}

CommandHandlerResult bindTab(Editor& runtime,
                               TabCommand command,
                               std::any const& payload) {
    auto active = runtime.tabs.viewState().active;
    auto tab = payloadAs<TabId>(payload) ? *payloadAs<TabId>(payload) : active.value_or(TabId{0});
    const auto activeTabBefore = runtime.tabs.viewState().active;
    // Remember the active document so we can reveal the caret only when the tab
    // command actually switches to a different document (activate/next/previous/
    // a close that changes the active tab / reopen). move_left/move_right and
    // close_others keep the same active document and must NOT snap the scroll.
    auto const documentBefore = runtime.activeDocumentId();
    const auto closesTab = [](const TabState& state,
                              const TabLifecycleResult& result) {
        return result.accepted() && (!state.dirty || result.durable) &&
               (result.compensation.has_value() || result.ephemeral);
    };
    std::vector<TabId> alreadyClosed;
    const auto closeOutcomeFor = [&](const TabState& state) {
        return TabCloseOutcome{
            state.id,
            runtime.closeTab(state, std::chrono::milliseconds{100},
                             alreadyClosed),
        };
    };
    TabResult result;
    switch (command) {
        case TabCommand::Close: {
            const auto found = std::find_if(
                runtime.tabs.viewState().tabs.begin(), runtime.tabs.viewState().tabs.end(),
                [&](const TabState& candidate) { return candidate.id == tab; });
            if (found == runtime.tabs.viewState().tabs.end()) {
                result = {TabError::NotFound, "tab is not open", {}, {}};
                break;
            }
            auto outcome = runtime.closeTab(*found, std::chrono::milliseconds{100});
            result = runtime.tabs.close(tab, std::move(outcome));
            break;
        }
        case TabCommand::CloseOthers: {
            const auto kept = std::find_if(
                runtime.tabs.viewState().tabs.begin(), runtime.tabs.viewState().tabs.end(),
                [&](const TabState& candidate) { return candidate.id == tab; });
            if (kept == runtime.tabs.viewState().tabs.end()) {
                result = {TabError::NotFound, "tab is not open", {}, {}};
                break;
            }
            std::vector<TabState> targets;
            for (const auto& candidate : runtime.tabs.viewState().tabs) {
                if (candidate.id != tab) {
                    targets.push_back(candidate);
                }
            }
            std::vector<TabCloseOutcome> outcomes;
            outcomes.reserve(targets.size());
            for (const auto& target : targets) {
                auto outcome = closeOutcomeFor(target);
                if (closesTab(target, outcome.result)) {
                    alreadyClosed.push_back(target.id);
                }
                outcomes.push_back(std::move(outcome));
            }
            result = runtime.tabs.closeOthers(tab, std::move(outcomes));
            break;
        }
        case TabCommand::CloseAll: {
            std::vector<TabState> targets = runtime.tabs.viewState().tabs;
            std::vector<TabCloseOutcome> outcomes;
            outcomes.reserve(targets.size());
            for (const auto& target : targets) {
                auto outcome = closeOutcomeFor(target);
                if (closesTab(target, outcome.result)) {
                    alreadyClosed.push_back(target.id);
                }
                outcomes.push_back(std::move(outcome));
            }
            result = runtime.tabs.closeAll(std::move(outcomes));
            break;
        }
        case TabCommand::ReopenClosed: {
            auto reopened = runtime.tabs.beginReopenClosed();
            if (auto* immediate = std::get_if<TabResult>(&reopened)) {
                result = std::move(*immediate);
                break;
            }
            auto request = std::get<TabReopenRequest>(reopened);
            auto outcome = runtime.reopenTab(request.tab, request.compensation);
            result = runtime.tabs.finishReopenClosed(std::move(request),
                                                     std::move(outcome));
            break;
        }
        case TabCommand::Next: result = runtime.tabs.next(); break;
        case TabCommand::Previous: result = runtime.tabs.previous(); break;
        case TabCommand::Activate: result = runtime.tabs.activate(tab); break;
        case TabCommand::MoveLeft: result = runtime.tabs.moveLeft(tab); break;
        case TabCommand::MoveRight: result = runtime.tabs.moveRight(tab); break;
    }
    if (!result.accepted()) return failure(tabMessage(result));
    runtime.clampSelectionToActiveDocument();
    // Reveal the caret when switching to a different document, so the newly
    // active tab's caret is on-screen instead of inheriting the previous tab's
    // scroll offset.
    if (runtime.activeDocumentId() != documentBefore) {
    }
    runtime.refreshSyntax();
    // Focus follows the pointer (M8-F): activating a tab (a tab click, or the
    // palette/lua "Tab Activate") acts on the editor, so move keyboard focus there.
    // Keyboard tab switching uses tab.next/tab.previous, which do not reach here.
    if (command == TabCommand::Activate) {
        runtime.screen.focusEditor();
    }
    if (runtime.tabs.viewState().active != activeTabBefore &&
        (command == TabCommand::Activate || command == TabCommand::Next ||
         command == TabCommand::Previous)) {
        runtime.recordNavigation(NavigationClass::User);
    }
    return success();
}

CommandHandlerResult bindEncoding(Editor& runtime,
                                    std::string_view id,
                                    std::any const& payload) {
    if (runtime.activeTabIsLiveDiff()) {
        return failure(std::string{id} + " is unavailable in live diff tabs");
    }
    auto document = runtime.activeDocumentId();
    if (!document) return failure("no active document");
    WorkspaceResult result;
    if (id == "file.reopen_with_encoding") {
        auto const* arguments = payloadAs<ReopenWithEncodingArguments>(payload);
        if (arguments == nullptr) {
            return failure("file.reopen_with_encoding requires an encoding payload");
        }
        result = runtime.workspace.reopenWithEncoding(*document, arguments->encoding);
    } else if (id == "file.set_encoding") {
        auto const* arguments = payloadAs<SetEncodingArguments>(payload);
        if (arguments == nullptr) {
            return failure("file.set_encoding requires an encoding payload");
        }
        result = runtime.workspace.setEncoding(*document, arguments->encoding);
    } else if (id == "file.set_line_ending") {
        auto const* arguments = payloadAs<SetLineEndingArguments>(payload);
        if (arguments == nullptr) {
            return failure("file.set_line_ending requires a line-ending payload");
        }
        result = runtime.workspace.setLineEnding(*document, arguments->lineEnding);
    } else if (id == "file.set_final_newline") {
        auto const* arguments = payloadAs<SetFinalNewlineArguments>(payload);
        if (arguments == nullptr) {
            return failure("file.set_final_newline requires a final-newline payload");
        }
        result = runtime.workspace.setFinalNewline(*document, arguments->finalNewline);
    } else {
        return failure("unknown encoding command");
    }
    if (!result.accepted()) return failure(workspaceMessage(result));
    runtime.refreshSyntax();
    return runtime.updateTabsFor(*document);
}

}  // namespace

CommandHandlerResult activateTab(Editor& runtime, TabId tabId) {
    return bindTab(runtime, TabCommand::Activate, std::any{tabId});
}

CommandHandlerResult closeTabById(Editor& runtime, TabId tabId) {
    return bindTab(runtime, TabCommand::Close, std::any{tabId});
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
    return success();
}

// How the active document is decoded and written back: its text encoding, its
// line endings, and whether it ends with a newline.
void registerEncodingCommands(CommandCatalog& catalog,
                              Editor& runtime) {
    auto declare = [](std::string id, std::string summary) {
        return CommandSpec{
            .id = std::move(id),
            .owner = "encoding-eol",
            .summary = std::move(summary),
            .effect = CommandEffect::Mutation,
            .luaApi = true,
        };
    };
    auto run = [&runtime](std::string_view id, std::any payload) {
        return bindEncoding(runtime, id, payload);
    };

    {
        auto built = declare("file.reopen_with_encoding", "Reopen With Encoding");
        built.binding = bindWireHandler<ReopenWithEncodingArguments>(
            [run](CommandContext&,
                  ReopenWithEncodingArguments const& arguments) {
                return run("file.reopen_with_encoding", std::any{arguments});
            });
        catalog.add(std::move(built));
    }
    {
        auto built = declare("file.set_encoding", "Set Encoding");
        built.binding = bindWireHandler<SetEncodingArguments>(
            [run](CommandContext&, SetEncodingArguments const& arguments) {
                return run("file.set_encoding", std::any{arguments});
            });
        catalog.add(std::move(built));
    }
    {
        auto built = declare("file.set_line_ending", "Set Line Ending");
        built.binding = bindWireHandler<SetLineEndingArguments>(
            [run](CommandContext&, SetLineEndingArguments const& arguments) {
                return run("file.set_line_ending", std::any{arguments});
            });
        catalog.add(std::move(built));
    }
    {
        auto built = declare("file.set_final_newline", "Set Final Newline");
        built.binding = bindWireHandler<SetFinalNewlineArguments>(
            [run](CommandContext&, SetFinalNewlineArguments const& arguments) {
                return run("file.set_final_newline", std::any{arguments});
            });
        catalog.add(std::move(built));
    }
}

// Opening, saving, renaming and deleting files.
//
// file.open_dropped_content is the only file command not offered to Lua:
// content dropped by a local window manager arrives outside any script's
// reach.
void registerFileCommands(CommandCatalog& catalog,
                          Editor& runtime) {
    auto spec = [](std::string id, std::string summary) {
        return CommandSpec{
            .id = std::move(id),
            .owner = "file-commands",
            .summary = std::move(summary),
            .effect = CommandEffect::Mutation,
        };
    };
    // Most of these act on a path when given one and on the active document
    // otherwise, so the path is an optional in-process argument.  A remote
    // client cannot send one: opening an arbitrary path is a local decision.
    auto declare = [&](std::string id, std::string label, std::string summary,
                       FileCommand command) {
        auto built = spec(std::move(id), std::move(summary));
        built.luaApi = true;
        built.binding = bindOptionalInProcessHandler<std::string>(
            [&runtime, command](CommandContext&,
                                std::optional<std::string> const& path) {
                return bindFile(runtime, command,
                                path ? std::any{*path} : std::any{});
            });
        if (!label.empty()) built.label = std::move(label);
        catalog.add(std::move(built));
    };

    declare("workspace.open_directory", "", "Open Directory",
            FileCommand::OpenDirectory);
    declare("file.new", "New File", "New File", FileCommand::Create);
    declare("file.open", "Open File", "Open File", FileCommand::Open);
    // Chooses from the recent list by position, not by path.
    {
        auto built = spec("file.open_recent", "Open Recent");
        built.luaApi = true;
        built.binding = bindOptionalInProcessHandler<std::size_t>(
            [&runtime](CommandContext&,
                       std::optional<std::size_t> const& index) {
                return bindFile(
                    runtime, FileCommand::OpenRecent,
                    index ? std::any{*index} : std::any{});
            });
        catalog.add(std::move(built));
    }
    declare("file.save", "Save File", "Save File", FileCommand::Save);
    declare("file.save_all", "Save All Files", "Save All Files",
            FileCommand::SaveAll);
    declare("file.save_as", "Save File As", "Save File As",
            FileCommand::SaveAs);
    declare("file.reload", "Reload File", "Reload File", FileCommand::Reload);
    declare("file.rename", "Rename File", "Rename File", FileCommand::Rename);
    declare("file.delete", "Delete File", "Delete File", FileCommand::Remove);
    declare("file.new_directory", "", "New Directory",
            FileCommand::NewDirectory);

    {
        auto built = spec("file.open_dropped_content", "Open Dropped Content");
        built.binding = bindWireHandler<DroppedContentArguments>(
            [&runtime](CommandContext&,
                       DroppedContentArguments const& arguments) {
                return bindFile(runtime, FileCommand::OpenDroppedContent,
                                std::any{arguments});
            });
        catalog.add(std::move(built));
    }
}

// Tabs.
//
// Every one of these acts on the tab you name, or on the active tab when you
// name none -- so each takes an OPTIONAL in-process tab id.  Declaring them as
// taking nothing dropped that id and made tab.activate act on whichever tab
// happened to be active.
void registerTabCommands(CommandCatalog& catalog,
                         Editor& runtime) {
    auto declare = [&](std::string id, std::string label, std::string summary,
                       TabCommand command) {
        CommandSpec built{
            .id = std::move(id),
            .owner = "tab-management",
            .summary = std::move(summary),
            .effect = CommandEffect::Mutation,
            .luaApi = true,
            .binding = bindOptionalWireHandler<TabId>(
                [&runtime, command](CommandContext&,
                                    std::optional<TabId> const& tab) {
                    return bindTab(runtime, command,
                                   tab ? std::any{*tab} : std::any{});
                }),
        };
        if (!label.empty()) built.label = std::move(label);
        catalog.add(std::move(built));
    };
    declare("tab.close", "Close Tab", "Close Tab", TabCommand::Close);
    declare("tab.close_others", "Close Other Tabs", "Close Other Tabs",
            TabCommand::CloseOthers);
    declare("tab.close_all", "Close All Tabs", "Close All Tabs",
            TabCommand::CloseAll);
    declare("tab.reopen_closed", "", "Reopen Closed",
            TabCommand::ReopenClosed);
    declare("tab.next", "Next Tab", "Next Tab", TabCommand::Next);
    declare("tab.previous", "Previous Tab", "Previous Tab",
            TabCommand::Previous);
    declare("tab.move_left", "", "Move Left", TabCommand::MoveLeft);
    declare("tab.move_right", "", "Move Right", TabCommand::MoveRight);
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
    registerEncodingCommands(catalog, runtime);
    registerDraftCommands(catalog, runtime);
}

} // namespace ssg
