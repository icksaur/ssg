#include "editor_runtime_internal.h"

#include <algorithm>

namespace ssg {
namespace {

template <typename T>
T const* payload_as(std::any const& payload) { return std::any_cast<T>(&payload); }

std::optional<std::string> string_payload(std::any const& payload) {
    if (auto const* value = payload_as<std::string>(payload)) return *value;
    if (auto const* value = payload_as<std::string_view>(payload)) return std::string{*value};
    return std::nullopt;
}

TabRecoveryBadge badge_for(ScratchDurabilityState state) {
    switch (state.kind) {
        case ScratchDurability::durable: return TabRecoveryBadge::durable;
        case ScratchDurability::pending: return TabRecoveryBadge::pending;
        case ScratchDurability::failed: return TabRecoveryBadge::failed;
    }
    return TabRecoveryBadge::none;
}

CommandHandlerResult open_document_result(EditorRuntime::Impl& runtime,
                                          WorkspaceResult const& result) {
    if (!result.accepted() || !result.document) return failure(workspace_message(result));
    return runtime.activate_document(*result.document);
}

CommandHandlerResult bind_file(EditorRuntime::Impl& runtime,
                               InvocationPrincipal const& principal,
                               FileCommand command,
                               std::any const& payload) {
    WorkspaceResult result;
    switch (command) {
        case FileCommand::open_directory: {
            auto path = string_payload(payload);
            if (!path) return failure("workspace.open_directory requires a path payload");
            result = runtime.workspace.open_directory(*path);
            if (!result.accepted()) return failure(workspace_message(result));
            runtime.root = runtime.workspace.root();
            runtime.refresh_tree();
            return success();
        }
        case FileCommand::create: {
            auto label = string_payload(payload).value_or("Untitled");
            result = runtime.workspace.new_document(label);
            return open_document_result(runtime, result);
        }
        case FileCommand::open: {
            auto path = string_payload(payload);
            if (!path) {
                (void)runtime.prompt.open(file_path_prompt(command));
                return success();
            }
            result = runtime.workspace.open_file(*path);
            return open_document_result(runtime, result);
        }
        case FileCommand::open_recent: {
            auto const* index = payload_as<std::size_t>(payload);
            if (index == nullptr) return failure("file.open_recent requires an index payload");
            result = runtime.workspace.open_recent(*index);
            return open_document_result(runtime, result);
        }
        case FileCommand::open_dropped_content: {
            auto const* dropped = payload_as<DroppedContentArguments>(payload);
            if (dropped == nullptr) return failure("file.open_dropped_content requires dropped content payload");
            result = runtime.workspace.open_dropped_content(principal, dropped->bytes, dropped->suggested_label);
            return open_document_result(runtime, result);
        }
        case FileCommand::save: {
            auto id = runtime.active_document_id();
            if (!id) return failure("no active document");
            result = runtime.workspace.save(*id);
            if (!result.accepted()) return failure(workspace_message(result));
            return runtime.update_tabs_for(*id);
        }
        case FileCommand::save_all: {
            result = runtime.workspace.save_all();
            if (!result.accepted()) return failure(workspace_message(result));
            for (auto id : runtime.workspace.documents()) (void)runtime.update_tabs_for(id);
            return success();
        }
        case FileCommand::save_as: {
            auto id = runtime.active_document_id();
            auto path = string_payload(payload);
            if (!id) return failure("no active document");
            if (!path) return failure("file.save_as requires a path payload");
            result = runtime.workspace.save_as(*id, *path);
            if (!result.accepted()) return failure(workspace_message(result));
            runtime.refresh_tree();
            return runtime.update_tabs_for(*id);
        }
        case FileCommand::reload: {
            auto id = runtime.active_document_id();
            if (!id) return failure("no active document");
            result = runtime.workspace.reload(*id);
            if (!result.accepted()) return failure(workspace_message(result));
            runtime.reset_selection_for_active_document();
            runtime.refresh_syntax();
            return runtime.update_tabs_for(*id);
        }
        case FileCommand::rename: {
            auto id = runtime.active_document_id();
            auto path = string_payload(payload);
            if (!id) return failure("no active document");
            if (!path) return failure("file.rename requires a path payload");
            result = runtime.workspace.rename_file(*id, *path);
            if (!result.accepted()) return failure(workspace_message(result));
            runtime.refresh_tree();
            return runtime.update_tabs_for(*id);
        }
        case FileCommand::remove: {
            auto id = runtime.active_document_id();
            if (!id) return failure("no active document");
            result = runtime.workspace.delete_file(*id);
            if (!result.accepted()) return failure(workspace_message(result));
            runtime.refresh_tree();
            return success();
        }
        case FileCommand::new_directory: {
            auto path = string_payload(payload);
            if (!path) return failure("file.new_directory requires a path payload");
            result = runtime.workspace.new_directory(*path);
            if (!result.accepted()) return failure(workspace_message(result));
            runtime.refresh_tree();
            return success();
        }
    }
    return failure("unknown file command");
}

CommandHandlerResult bind_tab(EditorRuntime::Impl& runtime,
                              TabCommand command,
                              std::any const& payload) {
    auto active = runtime.tabs.view_state().active;
    auto tab = payload_as<TabId>(payload) ? *payload_as<TabId>(payload) : active.value_or(TabId{0});
    TabResult result;
    switch (command) {
        case TabCommand::close: result = runtime.tabs.close(tab, std::chrono::milliseconds{100}); break;
        case TabCommand::close_others: result = runtime.tabs.close_others(tab, std::chrono::milliseconds{100}); break;
        case TabCommand::close_all: result = runtime.tabs.close_all(std::chrono::milliseconds{100}); break;
        case TabCommand::reopen_closed: result = runtime.tabs.reopen_closed(); break;
        case TabCommand::next: result = runtime.tabs.next(); break;
        case TabCommand::previous: result = runtime.tabs.previous(); break;
        case TabCommand::activate: result = runtime.tabs.activate(tab); break;
        case TabCommand::move_left: result = runtime.tabs.move_left(tab); break;
        case TabCommand::move_right: result = runtime.tabs.move_right(tab); break;
    }
    if (!result.accepted()) return failure(tab_message(result));
    runtime.clamp_selection_to_active_document();
    runtime.refresh_syntax();
    // Focus follows the pointer (M8-F): activating a tab (a tab click, or the
    // palette/lua "Tab Activate") acts on the editor, so move keyboard focus there.
    // Keyboard tab switching uses tab.next/tab.previous, which do not reach here.
    if (command == TabCommand::activate) {
        runtime.shell.focus_editor();
    }
    return success();
}

CommandHandlerResult bind_encoding(EditorRuntime::Impl& runtime,
                                    std::string_view id,
                                    std::any const& payload) {
    auto document = runtime.active_document_id();
    if (!document) return failure("no active document");
    WorkspaceResult result;
    if (id == "file.reopen_with_encoding") {
        auto const* arguments = payload_as<ReopenWithEncodingArguments>(payload);
        if (arguments == nullptr) {
            return failure("file.reopen_with_encoding requires an encoding payload");
        }
        result = runtime.workspace.reopen_with_encoding(*document, arguments->encoding);
    } else if (id == "file.set_encoding") {
        auto const* arguments = payload_as<SetEncodingArguments>(payload);
        if (arguments == nullptr) {
            return failure("file.set_encoding requires an encoding payload");
        }
        result = runtime.workspace.set_encoding(*document, arguments->encoding);
    } else if (id == "file.set_line_ending") {
        auto const* arguments = payload_as<SetLineEndingArguments>(payload);
        if (arguments == nullptr) {
            return failure("file.set_line_ending requires a line-ending payload");
        }
        result = runtime.workspace.set_line_ending(*document, arguments->line_ending);
    } else if (id == "file.set_final_newline") {
        auto const* arguments = payload_as<SetFinalNewlineArguments>(payload);
        if (arguments == nullptr) {
            return failure("file.set_final_newline requires a final-newline payload");
        }
        result = runtime.workspace.set_final_newline(*document, arguments->final_newline);
    } else {
        return failure("unknown encoding command");
    }
    if (!result.accepted()) return failure(workspace_message(result));
    runtime.refresh_syntax();
    return runtime.update_tabs_for(*document);
}

} // namespace

CommandHandlerResult EditorRuntime::Impl::update_tabs_for(FileDocumentId document) {
    auto state = workspace.state(document);
    if (!state) return failure("workspace document does not exist");
    auto result = tabs.update_document(document, workspace.document(document).mode(),
                                       state->dirty, badge_for(scratch.durability_state()));
    return result.accepted() ? success() : failure(tab_message(result));
}

CommandHandlerResult EditorRuntime::Impl::activate_document(FileDocumentId document) {
    auto state = workspace.state(document);
    if (!state) return failure("workspace document does not exist");
    auto result = tabs.open_document(document, state->key, state->display_label,
                                     workspace.document(document).mode(), state->dirty,
                                     badge_for(scratch.durability_state()));
    if (!result.accepted()) return failure(tab_message(result));
    reset_selection_for_active_document();
    refresh_syntax();
    return success();
}

void bind_runtime_files(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime) {
    auto file_commands = file_commands_command_set();
    auto tab_commands = tab_management_command_set();
    auto external_commands = external_modification_command_set();
    for (auto const& descriptor : file_commands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext& context, std::any const& payload) {
            return runtime.run_transaction([&] { return bind_file(runtime, context.principal(), descriptor.command, payload); });
        });
    }
    for (auto const& descriptor : tab_commands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const& payload) {
            return runtime.run_transaction([&] { return bind_tab(runtime, descriptor.command, payload); });
        });
    }
    for (auto const& descriptor : text_encoding_command_set.descriptors) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const& payload) {
            return runtime.run_transaction([&] { return bind_encoding(runtime, descriptor.id, payload); });
        });
    }
    for (auto const& descriptor : external_commands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const& payload) {
            return runtime.run_transaction([&] {
                auto const* id = payload_as<DiffFileId>(payload);
                if (id == nullptr) return failure("external command requires a diff file ID payload");
                std::optional<JournalDocument> document;
                ExternalModificationResult result;
                if (descriptor.action == ExternalAction::reload) result = runtime.external.reload(*id, document);
                else if (descriptor.action == ExternalAction::keep_buffer) result = runtime.external.keep_buffer(*id);
                else {
                    auto opened = runtime.external.open_diff(*id);
                    if (!opened.accepted()) return failure("external diff target is unavailable");
                    return success();
                }
                return result.accepted() ? success() : failure("external modification command failed");
            });
        });
    }
}

} // namespace ssg
