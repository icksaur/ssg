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

CommandHandlerResult openDocumentResult(EditorRuntime::Impl& runtime,
                                          WorkspaceResult const& result) {
    if (!result.accepted() || !result.document) return failure(workspaceMessage(result));
    return runtime.activateDocument(*result.document);
}

CommandHandlerResult bindFile(EditorRuntime::Impl& runtime,
                               InvocationPrincipal const& principal,
                               FileCommand command,
                               std::any const& payload) {
    WorkspaceResult result;
    switch (command) {
        case FileCommand::OpenDirectory: {
            auto path = stringPayload(payload);
            if (!path) return failure("workspace.open_directory requires a path payload");
            result = runtime.workspace.openDirectory(*path);
            if (!result.accepted()) return failure(workspaceMessage(result));
            runtime.root = runtime.workspace.root();
            runtime.refreshTree();
            return success();
        }
        case FileCommand::Create: {
            auto label = stringPayload(payload).value_or("Untitled");
            result = runtime.workspace.newDocument(label);
            return openDocumentResult(runtime, result);
        }
        case FileCommand::Open: {
            auto path = stringPayload(payload);
            if (!path) {
                (void)runtime.prompt.open(filePathPrompt(command));
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
                              TabCommand command,
                              std::any const& payload) {
    auto active = runtime.tabs.viewState().active;
    auto tab = payloadAs<TabId>(payload) ? *payloadAs<TabId>(payload) : active.value_or(TabId{0});
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
    return success();
}

CommandHandlerResult bindEncoding(EditorRuntime::Impl& runtime,
                                    std::string_view id,
                                    std::any const& payload) {
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

} // namespace

CommandHandlerResult EditorRuntime::Impl::updateTabsFor(FileDocumentId document) {
    auto state = workspace.state(document);
    if (!state) return failure("workspace document does not exist");
    auto result = tabs.updateDocument(document, workspace.document(document).mode(),
                                       state->dirty, badgeFor(scratch.durabilityState()));
    return result.accepted() ? success() : failure(tabMessage(result));
}

CommandHandlerResult EditorRuntime::Impl::activateDocument(FileDocumentId document) {
    auto state = workspace.state(document);
    if (!state) return failure("workspace document does not exist");
    auto result = tabs.openDocument(document, state->key, state->displayLabel,
                                     workspace.document(document).mode(), state->dirty,
                                     badgeFor(scratch.durabilityState()));
    if (!result.accepted()) return failure(tabMessage(result));
    resetSelectionForActiveDocument();
    refreshSyntax();
    return success();
}

void bindRuntimeFiles(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime) {
    auto fileCommands = fileCommandsCommandSet();
    auto tabCommands = tabManagementCommandSet();
    auto externalCommands = externalModificationCommandSet();
    for (auto const& descriptor : fileCommands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext& context, std::any const& payload) {
            return runtime.runTransaction([&] { return bindFile(runtime, context.principal(), descriptor.command, payload); });
        });
    }
    for (auto const& descriptor : tabCommands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const& payload) {
            return runtime.runTransaction([&] { return bindTab(runtime, descriptor.command, payload); });
        });
    }
    for (auto const& descriptor : kTextEncodingCommandSet.descriptors) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const& payload) {
            return runtime.runTransaction([&] { return bindEncoding(runtime, descriptor.id, payload); });
        });
    }
    for (auto const& descriptor : externalCommands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const& payload) {
            return runtime.runTransaction([&] {
                auto const* id = payloadAs<DiffFileId>(payload);
                if (id == nullptr) return failure("external command requires a diff file ID payload");
                std::optional<JournalDocument> document;
                ExternalModificationResult result;
                if (descriptor.action == ExternalAction::Reload) result = runtime.external.reload(*id, document);
                else if (descriptor.action == ExternalAction::KeepBuffer) result = runtime.external.keepBuffer(*id);
                else {
                    auto opened = runtime.external.openDiff(*id);
                    if (!opened.accepted()) return failure("external diff target is unavailable");
                    return success();
                }
                return result.accepted() ? success() : failure("external modification command failed");
            });
        });
    }
}

} // namespace ssg
