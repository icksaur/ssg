#include "editor_runtime_internal.h"

#include <algorithm>
#include <cctype>

namespace ssg {
namespace {

template <typename T>
T const* payload_as(std::any const& payload) {
    return std::any_cast<T>(&payload);
}

HistoryEditKind history_kind(TextInputCommand command) {
    switch (command) {
        case TextInputCommand::insert:
        case TextInputCommand::newline:
            return HistoryEditKind::typing;
        case TextInputCommand::delete_backward:
        case TextInputCommand::delete_word_backward:
            return HistoryEditKind::delete_backward;
        case TextInputCommand::delete_forward:
        case TextInputCommand::delete_word_forward:
            return HistoryEditKind::delete_forward;
    }
    return HistoryEditKind::other;
}

TextInputSettings text_input_settings(EditorRuntime::Impl const&) {
    return {IndentStyle::spaces, 4, true, LineEnding::lf};
}

EditCommandSettings edit_settings(EditorRuntime::Impl const&) {
    return {IndentStyle::spaces, 4, 4, LineEnding::lf, "//"};
}

CommandHandlerResult apply_transaction(EditorRuntime::Impl& runtime,
                                       EditTransaction const& transaction,
                                       SelectionSet const& selections_after,
                                       HistoryEditKind kind) {
    auto id = runtime.active_document_id();
    auto* document = runtime.active_document();
    if (!id || document == nullptr) return failure("no active document");
    auto before = runtime.selection.selections;
    auto result = runtime.history_for(*id).apply_edit(*document, transaction, before,
                                                       selections_after, kind, 0);
    if (!result.accepted()) return failure(result.message);
    runtime.selection.selections = result.selections.value_or(selections_after);
    runtime.clamp_selection_to_active_document();
    (void)runtime.update_tabs_for(*id);
    runtime.refresh_syntax();
    return success();
}

CommandHandlerResult bind_text(EditorRuntime::Impl& runtime,
                               TextInputCommand command,
                               std::string_view id,
                               std::any const& payload) {
    auto const* document = runtime.active_document();
    if (document == nullptr) return failure("no active document");
    TextInputArguments arguments;
    if (command == TextInputCommand::insert) {
        auto const* typed = payload_as<TextInputArguments>(payload);
        if (typed == nullptr) return failure(wrong_payload(id));
        arguments = *typed;
    }
    std::string inserted = arguments.text;
    auto result = apply_text_input(document->snapshot(), runtime.selection.selections,
                                   text_input_settings(runtime), command, std::move(arguments));
    if (!result.accepted() || !result.transaction || !result.selections) {
        return failure(result.message);
    }
    auto outcome = apply_transaction(runtime, *result.transaction, *result.selections,
                                     history_kind(command));
    // Keep undo word-granular: seal the current unit after a newline or after
    // inserting a whitespace/punctuation boundary, so the next word starts a
    // fresh undo step.
    if (outcome.accepted) {
        bool boundary = command == TextInputCommand::newline;
        if (command == TextInputCommand::insert && !inserted.empty()) {
            auto const last = static_cast<unsigned char>(inserted.back());
            if (last < 0x80 && (std::isspace(last) || std::ispunct(last))) {
                boundary = true;
            }
        }
        if (boundary) {
            if (auto const active = runtime.active_document_id()) {
                runtime.history_for(*active).break_coalescing();
            }
        }
    }
    return outcome;
}

CommandHandlerResult bind_selection(EditorRuntime::Impl& runtime,
                                    SelectionCommand command,
                                    std::any const& payload) {
    SelectionCommandArguments arguments;
    if (auto const* typed = payload_as<SelectionCommandArguments>(payload)) {
        arguments = *typed;
    }
    auto result = apply_selection_navigation(runtime.active_text(), runtime.selection,
                                             command, ViewportDimensions{80, 24},
                                             arguments, {}, 4);
    if (!result.accepted()) return failure(result.message);
    if (result.delta.replacement) runtime.selection = *result.delta.replacement;
    runtime.requested_first_visual_row = runtime.selection.first_visual_row;
    runtime.history_for(runtime.active_document_id().value_or(FileDocumentId{0})).break_coalescing();
    return success();
}

CommandHandlerResult bind_edit(EditorRuntime::Impl& runtime, EditCommand command) {
    auto const* document = runtime.active_document();
    if (document == nullptr) return failure("no active document");
    auto result = apply_edit_command(document->snapshot(), runtime.selection.selections,
                                     edit_settings(runtime), command);
    if (!result.accepted() || !result.transaction || !result.selections) {
        return failure(result.message);
    }
    return apply_transaction(runtime, *result.transaction, *result.selections,
                             HistoryEditKind::other);
}

CommandHandlerResult bind_history(EditorRuntime::Impl& runtime, HistoryCommand command) {
    auto id = runtime.active_document_id();
    auto* document = runtime.active_document();
    if (!id || document == nullptr) return failure("no active document");
    auto& history = runtime.history_for(*id);
    auto result = command == HistoryCommand::undo ? history.undo(*document) : history.redo(*document);
    if (!result.accepted()) return failure(result.message);
    if (result.selections) runtime.selection.selections = *result.selections;
    runtime.clamp_selection_to_active_document();
    (void)runtime.update_tabs_for(*id);
    runtime.refresh_syntax();
    return success();
}

CommandHandlerResult bind_clipboard(EditorRuntime::Impl& runtime, ClipboardCommand command) {
    auto id = runtime.active_document_id();
    auto* document = runtime.active_document();
    if (!id || document == nullptr) return failure("no active document");
    auto& history = runtime.history_for(*id);
    ClipboardResult result{ClipboardError::none, ClipboardSystemStatus::not_requested,
                           document->revision(), std::nullopt, std::nullopt, false, {}};
    switch (command) {
        case ClipboardCommand::copy:
            result = runtime.clipboard.copy(document->snapshot(), runtime.selection.selections);
            break;
        case ClipboardCommand::cut:
            result = runtime.clipboard.cut(*document, history, runtime.selection.selections, 0);
            break;
        case ClipboardCommand::paste:
            result = runtime.clipboard.paste(*document, history, runtime.selection.selections,
                                             ClipboardPasteMode::internal_only, 0);
            break;
    }
    if (!result.accepted()) return failure(result.message);
    if (result.selections) runtime.selection.selections = *result.selections;
    if (result.document_changed) {
        (void)runtime.update_tabs_for(*id);
        runtime.refresh_syntax();
    }
    return success();
}

// Move the primary selection onto the active find match and reveal it so the
// viewport scrolls to follow find navigation (find.next/previous/update_query).
void reveal_active_find_match(EditorRuntime::Impl& runtime) {
    auto const& state = runtime.find_replace.view_state();
    if (!state.open || !state.active_match ||
        *state.active_match >= state.matches.size()) {
        return;
    }
    auto const& match = state.matches[*state.active_match];
    auto text = runtime.active_text();
    auto anchor = resolve_document_position(text, match.begin);
    auto active = resolve_document_position(text, match.end);
    if (!anchor || !active) return;
    runtime.selection.selections =
        SelectionSet{std::vector<Selection>{Selection{*anchor, *active}}};
    // Reveal against the real document pane from the last snapshot, reserving the
    // find prompt's rows plus a one-row margin so the match never sits flush
    // against the prompt.  Adding back the rows that snapshot reserved yields a
    // prompt-agnostic pane height; subtracting the find prompt's rows (and the
    // margin) guarantees the match lands above the prompt whether or not it was
    // open last snapshot.
    auto const reserved = prompt_row_count(PromptKind::find) + 1;
    auto const base_rows = runtime.last_pane_content_rows +
                           runtime.last_reserved_prompt_rows;
    auto const reveal_rows = base_rows > reserved
                                 ? base_rows - reserved
                                 : std::uint32_t{1};
    ViewportDimensions reveal_viewport{runtime.last_pane_content_columns,
                                       reveal_rows};
    auto result = apply_selection_navigation(
        text, runtime.selection, SelectionCommand::view_reveal_caret,
        reveal_viewport);
    if (result.accepted() && result.delta.replacement) {
        runtime.selection = *result.delta.replacement;
    }
    runtime.requested_first_visual_row = runtime.selection.first_visual_row;
}

CommandHandlerResult bind_find_replace(EditorRuntime::Impl& runtime,
                                       Revision revision,
                                       FindReplaceCommand command,
                                       std::any const& payload) {
    auto* document = runtime.active_document();
    auto query = payload_as<std::string>(payload) ? *payload_as<std::string>(payload) : runtime.find_replace.view_state().query;
    std::optional<ByteRange> range;
    DocumentSnapshot snapshot{{}, Revision{0}, DocumentMode::edit, false};
    if (document != nullptr) {
        snapshot = document->snapshot();
        auto selected = runtime.selection.selections.primary();
        if (!selected.is_caret()) range = ByteRange{selected.lower().byte_offset, selected.upper().byte_offset};
    } else if (command != FindReplaceCommand::replace_workspace_preview &&
               command != FindReplaceCommand::replace_workspace_apply &&
               command != FindReplaceCommand::find_close &&
               command != FindReplaceCommand::find_next &&
               command != FindReplaceCommand::find_previous) {
        return failure("no active document");
    }
    switch (command) {
        case FindReplaceCommand::find_open:
            runtime.find_replace.open(snapshot, FindRequest{query, {}, range});
            runtime.find_document_id = runtime.active_document_id();
            reveal_active_find_match(runtime);
            // Open the find prompt so focus moves to it and the reserved rows
            // display the controller query (projected at snapshot time).
            (void)runtime.prompt.open(PromptRequest{
                PromptKind::find, "Find", {{"find.query", "Find query", query}},
                {}, PromptMatchCount{"find.count", "Match count", ""}});
            return success();
        case FindReplaceCommand::replace_open:
            runtime.find_replace.open_replace(snapshot, FindRequest{query, {}, range});
            runtime.find_document_id = runtime.active_document_id();
            return success();
        case FindReplaceCommand::find_close:
            runtime.find_replace.close();
            runtime.find_document_id.reset();
            // Only dismiss the prompt when it is the find prompt: a global
            // find.close must not cancel an unrelated palette/settings prompt.
            if (auto const& request = runtime.prompt.request();
                request && request->kind == PromptKind::find) {
                (void)runtime.prompt.cancel();
            }
            return success();
        case FindReplaceCommand::find_next:
            runtime.find_replace.next();
            reveal_active_find_match(runtime);
            return success();
        case FindReplaceCommand::find_previous:
            runtime.find_replace.previous();
            reveal_active_find_match(runtime);
            return success();
        case FindReplaceCommand::find_update_query: {
            auto const* arguments = payload_as<FindQueryArguments>(payload);
            if (arguments == nullptr) return failure("find.update_query requires a query payload");
            runtime.find_replace.update_query(snapshot, arguments->query, range);
            runtime.find_document_id = runtime.active_document_id();
            reveal_active_find_match(runtime);
            return success();
        }
        case FindReplaceCommand::find_toggle_case:
            runtime.find_replace.toggle_case(snapshot);
            runtime.find_document_id = runtime.active_document_id();
            return success();
        case FindReplaceCommand::find_toggle_whole_word:
            runtime.find_replace.toggle_whole_word(snapshot);
            runtime.find_document_id = runtime.active_document_id();
            return success();
        case FindReplaceCommand::find_toggle_regex:
            runtime.find_replace.toggle_regex(snapshot);
            runtime.find_document_id = runtime.active_document_id();
            return success();
        case FindReplaceCommand::find_toggle_selection:
            runtime.find_replace.toggle_selection(snapshot, range);
            runtime.find_document_id = runtime.active_document_id();
            return success();
        case FindReplaceCommand::replace_current:
        case FindReplaceCommand::replace_all: {
            auto id = runtime.active_document_id();
            if (!id) return failure("no active document");
            auto replacement = payload_as<std::string>(payload) ? *payload_as<std::string>(payload) : std::string{};
            auto before = runtime.selection.selections;
            auto after = runtime.selection.selections;
            auto result = command == FindReplaceCommand::replace_current
                ? runtime.find_replace.replace_current(*document, runtime.history_for(*id), before, after, replacement, 0)
                : runtime.find_replace.replace_all(*document, runtime.history_for(*id), before, after, replacement, 0);
            if (!result.accepted()) return failure(result.message);
            runtime.refresh_syntax();
            return runtime.update_tabs_for(*id);
        }
        case FindReplaceCommand::replace_workspace_preview:
        {
            auto const* arguments = payload_as<WorkspaceReplaceArguments>(payload);
            if (arguments == nullptr) {
                return failure("replace.workspace_preview requires a workspace replace payload");
            }
            auto result = preview_workspace_replace(runtime, revision,
                                                    arguments->request,
                                                    arguments->replacement);
            if (!result.accepted()) return failure(result.message);
            runtime.workspace_replace_preview = std::move(result.preview);
            return success();
        }
        case FindReplaceCommand::replace_workspace_apply: {
            auto const* explicit_preview = payload_as<WorkspaceReplacePreview>(payload);
            if (payload.has_value() && explicit_preview == nullptr) {
                return failure("replace.workspace_apply payload has the wrong type");
            }
            auto const* preview = runtime.workspace_replace_preview
                ? &*runtime.workspace_replace_preview
                : nullptr;
            if (preview == nullptr) {
                return failure("replace.workspace_apply requires a workspace replace preview payload");
            }
            if (explicit_preview != nullptr && *explicit_preview != *preview) {
                return failure("replace.workspace_apply payload does not match the current workspace preview");
            }
            auto result = apply_workspace_replace(runtime, *preview, runtime);
            if (!result.accepted()) return failure(result.message);
            runtime.workspace_replace_preview.reset();
            runtime.refresh_tree();
            return success();
        }
    }
    return failure("unknown find/replace command");
}

} // namespace

void bind_runtime_editing(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime) {
    auto text_commands = text_input_command_set();
    auto selection_commands = selection_navigation_command_set();
    auto history_commands = history_command_set();
    auto edit_commands = edit_command_suite_command_set();
    auto clipboard_commands = clipboard_command_set();
    auto find_replace_commands = find_replace_command_set();
    for (auto const& descriptor : text_commands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const& payload) {
            return runtime.run_transaction([&] { return bind_text(runtime, descriptor.command, descriptor.id, payload); });
        });
    }
    for (auto const& descriptor : selection_commands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const& payload) {
            return runtime.run_transaction([&] { return bind_selection(runtime, descriptor.command, payload); });
        });
    }
    for (auto const& descriptor : history_commands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const&) {
            return runtime.run_transaction([&] { return bind_history(runtime, descriptor.command); });
        });
    }
    for (auto const& descriptor : edit_commands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const&) {
            return runtime.run_transaction([&] { return bind_edit(runtime, descriptor.command); });
        });
    }
    for (auto const& descriptor : clipboard_commands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const&) {
            return runtime.run_transaction([&] { return bind_clipboard(runtime, descriptor.command); });
        });
    }
    for (auto const& descriptor : find_replace_commands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext& context, std::any const& payload) {
            return runtime.run_transaction([&] { return bind_find_replace(runtime, context.revision(), descriptor.command, payload); });
        });
    }
}

} // namespace ssg
