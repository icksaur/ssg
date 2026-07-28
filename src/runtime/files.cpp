#include "editor_runtime_internal.h"

#include <algorithm>

namespace ssg {
namespace {

template <typename T>
T const* payloadAs(std::any const& payload) { return std::any_cast<T>(&payload); }

std::optional<std::string> stringPayload(std::any const& payload) {
    if (auto const* value = payloadAs<std::string>(payload)) return *value;
    if (auto const* value = payloadAs<std::string_view>(payload)) return std::string{*value};
    return std::nullopt;
}

TabRecoveryBadge badgeFor(ScratchDurabilityState state) {
    switch (state.kind) {
        case ScratchDurability::Durable: return TabRecoveryBadge::Durable;
        case ScratchDurability::Pending: return TabRecoveryBadge::Pending;
        case ScratchDurability::Failed: return TabRecoveryBadge::Failed;
    }
    return TabRecoveryBadge::None;
}

bool activeLiveDiffTab(const EditorRuntime::Impl& runtime) {
    const auto* tab = runtime.activeTabState();
    return tab != nullptr && tab->kind == TabKind::LiveDiff;
}

// The live-diff rule's classification, read from the command's own descriptor
// rather than re-derived here, so the rule and the catalog cannot disagree.
bool mutatesTheActiveDocumentsFile(FileCommand command) {
    const auto& descriptors = fileCommandsCommandSet().descriptors();
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
void pruneArchiveReportingFailures(EditorRuntime::Impl& runtime) {
    const auto report = runtime.workspace.pruneArchive();
    if (report.ok()) return;
    runtime.enqueueStatus(
        StatusPriority::Warning,
        "could not fully prune the deleted-file archive: " + report.message);
}

CommandHandlerResult openDocumentResult(EditorRuntime::Impl& runtime,
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
    const EditorRuntime::Impl& runtime, FileCommand command) {
    if (!mutatesTheActiveDocumentsFile(command)) return std::nullopt;

    if (activeLiveDiffTab(runtime)) {
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

CommandHandlerResult bindFile(EditorRuntime::Impl& runtime,
                               InvocationPrincipal const& principal,
                               FileCommand command,
                               std::any const& payload) {
    WorkspaceResult result;

    // The live-diff rule, applied once for every command it covers. A live diff
    // tab is a computed view of two revisions, so there is no file to save,
    // rename, reload or delete.
    if (mutatesTheActiveDocumentsFile(command) && activeLiveDiffTab(runtime)) {
        return failure("command is unavailable in live diff tabs");
    }

    // Every path-taking command prompts for its path when dispatched without
    // one, in ONE place rather than per command. A command that gains the
    // pathPrompt flag is wired by that fact alone, so the flag and the behavior
    // cannot drift apart.
    const auto& descriptors = fileCommandsCommandSet().descriptors();
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
            runtime.prompt.open(fileCommandsCommandSet().pathPrompt(command));
        if (!opened.accepted()) return failure(opened.error->message);
        runtime.reconcilePromptFocus();
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
            runtime.refreshTree();
            return success();
        }
        case FileCommand::Create: {
            // No payload means an unnamed buffer; the workspace supplies the label.
            auto label = stringPayload(payload).value_or(std::string{});
            result = runtime.workspace.newDocument(label);
            return openDocumentResult(runtime, result);
        }
        case FileCommand::Open: {
            auto path = stringPayload(payload);
            if (!path) {
                (void)runtime.prompt.open(
                    fileCommandsCommandSet().pathPrompt(command));
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
            result = runtime.workspace.openDroppedContent(principal, dropped->bytes, dropped->suggestedLabel);
            return openDocumentResult(runtime, result);
        }
        case FileCommand::Save: {
            auto id = runtime.activeDocumentId();
            if (!id) return failure("no active document");
            // A buffer that has never had a name cannot be saved over itself,
            // so saving it IS a save-as. Opening save_as's prompt (rather than
            // one of its own) keeps a single naming path: whatever the user
            // types is handled by the command that knows how to name a
            // document.
            auto const state = runtime.workspace.state(*id);
            if (state &&
                state->key.kind() != JournalDocumentKeyKind::Saved) {
                auto opened = runtime.prompt.open(
                    fileCommandsCommandSet().pathPrompt(FileCommand::SaveAs));
                if (!opened.accepted()) return failure(opened.error->message);
                runtime.reconcilePromptFocus();
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
            runtime.refreshTree();
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
            runtime.refreshTree();
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
            // workspace entry, so the close lifecycle would fail on a missing
            // document and strand the tab. That lifecycle also owns the
            // per-document runtime state, so bypassing it means discarding
            // that state here or it accumulates for a document nobody can
            // reach again.
            (void)runtime.tabs.dropDocument(*id);
            runtime.discardDocumentRuntimeState(*id);
            runtime.refreshTree();
            return success();
        }
        case FileCommand::NewDirectory: {
            auto path = stringPayload(payload);
            if (!path) return failure("file.new_directory requires a path payload");
            result = runtime.workspace.newDirectory(*path);
            if (!result.accepted()) return failure(workspaceMessage(result));
            runtime.refreshTree();
            return success();
        }
    }
    return failure("unknown file command");
}

CommandHandlerResult bindTab(EditorRuntime::Impl& runtime,
                              ClientId client,
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
    TabResult result;
    switch (command) {
        case TabCommand::Close: result = runtime.tabs.close(tab, std::chrono::milliseconds{100}); break;
        case TabCommand::CloseOthers: result = runtime.tabs.closeOthers(tab, std::chrono::milliseconds{100}); break;
        case TabCommand::CloseAll: result = runtime.tabs.closeAll(std::chrono::milliseconds{100}); break;
        case TabCommand::ReopenClosed: result = runtime.tabs.reopenClosed(); break;
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
    // scroll offset (doc/spec-scroll.md reveal policy).
    if (runtime.activeDocumentId() != documentBefore) {
        runtime.revealPrimaryCaret();
    }
    runtime.refreshSyntax();
    // Focus follows the pointer (M8-F): activating a tab (a tab click, or the
    // palette/lua "Tab Activate") acts on the editor, so move keyboard focus there.
    // Keyboard tab switching uses tab.next/tab.previous, which do not reach here.
    if (command == TabCommand::Activate) {
        runtime.shell.focusEditor();
    }
    if (runtime.tabs.viewState().active != activeTabBefore &&
        (command == TabCommand::Activate || command == TabCommand::Next ||
         command == TabCommand::Previous)) {
        runtime.recordNavigation(client, NavigationClass::User);
    }
    return success();
}

CommandHandlerResult bindEncoding(EditorRuntime::Impl& runtime,
                                    std::string_view id,
                                    std::any const& payload) {
    if (activeLiveDiffTab(runtime)) {
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

CommandHandlerResult EditorRuntime::Impl::updateTabsFor(FileDocumentId document) {
    auto state = workspace.state(document);
    if (!state) return failure("workspace document does not exist");
    auto const* opened = workspace.tryDocument(document);
    if (opened == nullptr) return failure("workspace document does not exist");
    auto result = tabs.updateDocument(document, state->key, state->displayLabel,
                                       opened->mode(), state->dirty,
                                       badgeFor(scratch.durabilityState()));
    return result.accepted() ? success() : failure(tabMessage(result));
}

CommandHandlerResult EditorRuntime::Impl::activateDocument(FileDocumentId document) {
    auto state = workspace.state(document);
    if (!state) return failure("workspace document does not exist");
    auto const* opened = workspace.tryDocument(document);
    if (opened == nullptr) return failure("workspace document does not exist");
    ensureDocumentRuntimeState(document);
    auto result = tabs.openDocument(document, state->key, state->displayLabel,
                                     opened->mode(), state->dirty,
                                     badgeFor(scratch.durabilityState()));
    if (!result.accepted()) return failure(tabMessage(result));
    resetSelectionForActiveDocument();
    refreshSyntax();
    return success();
}

// What to do about a file that changed on disk under an open buffer.
//
// Each takes a live diff-file id, which means nothing to a remote client, so
// these are in-process only.
void registerExternalModificationCommands(EditorSessionBuilder& builder,
                                          EditorRuntime::Impl& runtime) {
    auto declare = [&](std::string id, std::string summary,
                       ExternalAction action) {
        builder.add(
            CommandSpecBuilder{std::move(id)}
                .owner("external-modification-flow")
                .summary(std::move(summary))
                .mutates()
                .lua()
                .inProcessHandler<DiffFileId>(
                    [&runtime, action](CommandContext&,
                                       DiffFileId const& file) {
                        return runtime.runTransaction([&]() -> CommandHandlerResult {
                            std::optional<JournalDocument> document;
                            ExternalModificationResult result;
                            if (action == ExternalAction::Reload) {
                                result = runtime.external.reload(file, document);
                            } else if (action == ExternalAction::KeepBuffer) {
                                result = runtime.external.keepBuffer(file);
                            } else {
                                auto opened = runtime.external.openDiff(file);
                                if (!opened.accepted()) {
                                    return failure(
                                        "external diff target is unavailable");
                                }
                                return success();
                            }
                            return result.accepted()
                                       ? success()
                                       : failure(
                                             "external modification command failed");
                        });
                    }));
    };
    declare("external.reload", "Reload", ExternalAction::Reload);
    declare("external.keep_buffer", "Keep Buffer", ExternalAction::KeepBuffer);
    declare("external.open_diff", "Open Diff", ExternalAction::OpenDiff);
}

void bindRuntimeFiles(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime) {
    registerExternalModificationCommands(builder, runtime);
    auto fileCommands = fileCommandsCommandSet();
    auto tabCommands = tabManagementCommandSet();
    for (auto const& descriptor : fileCommands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext& context, std::any const& payload) {
            return runtime.runTransaction([&] { return bindFile(runtime, context.principal(), descriptor.command, payload); });
        });
    }
    for (auto const& descriptor : tabCommands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext& context, std::any const& payload) {
            return runtime.runTransaction([&] {
                return bindTab(runtime, context.principal().clientId(),
                               descriptor.command, payload);
            });
        });
    }
    for (auto const& descriptor : kTextEncodingCommandSet.descriptors) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const& payload) {
            return runtime.runTransaction([&] { return bindEncoding(runtime, descriptor.id, payload); });
        });
    }
}

} // namespace ssg
