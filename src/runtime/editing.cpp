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
        case TextInputCommand::Insert:
        case TextInputCommand::Newline:
            return HistoryEditKind::Typing;
        case TextInputCommand::DeleteBackward:
        case TextInputCommand::DeleteWordBackward:
            return HistoryEditKind::DeleteBackward;
        case TextInputCommand::DeleteForward:
        case TextInputCommand::DeleteWordForward:
            return HistoryEditKind::DeleteForward;
    }
    return HistoryEditKind::Other;
}

TextInputSettings text_input_settings(EditorRuntime::Impl const&) {
    return {IndentStyle::Spaces, 4, true, LineEnding::Lf};
}

EditCommandSettings edit_settings(EditorRuntime::Impl const&) {
    return {IndentStyle::Spaces, 4, 4, LineEnding::Lf, "//"};
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
    runtime.reveal_primary_caret();
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
    if (command == TextInputCommand::Insert) {
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
        bool boundary = command == TextInputCommand::Newline;
        if (command == TextInputCommand::Insert && !inserted.empty()) {
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
    // Navigate and reveal against the REAL editor pane cached from the last
    // snapshot, not a fake {80, 24}: page motion advances by the real height and
    // the built-in caret reveal uses the real height/width (so a far-right column
    // on a wide line is not clamped at column 80). See doc/spec-scroll.md R6.
    ViewportDimensions const viewport{
        std::max<std::uint32_t>(runtime.last_pane_content_columns, 1),
        std::max<std::uint32_t>(runtime.last_pane_content_rows, 1)};
    auto result = apply_selection_navigation(runtime.active_text(), runtime.selection,
                                             command, viewport,
                                             arguments, {}, 4, runtime.word_wrap);
    if (!result.accepted()) return failure(result.message);
    if (result.delta.replacement) runtime.selection = *result.delta.replacement;
    runtime.requested_first_visual_row = runtime.selection.first_visual_row;
    runtime.requested_first_visual_column = runtime.selection.first_visual_column;
    runtime.history_for(runtime.active_document_id().value_or(FileDocumentId{0})).break_coalescing();
    // Focus follows the pointer (M8-F): a click-to-caret / drag-select acts on the
    // editor, so it moves the authoritative keyboard focus there. Gated on the two
    // pointer-driven selection commands; keyboard caret motion is a different
    // SelectionCommand and never reaches here.
    if (command == SelectionCommand::CursorSetPosition ||
        command == SelectionCommand::SelectSetRange) {
        runtime.shell.focus_editor();
    }
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
                             HistoryEditKind::Other);
}

CommandHandlerResult bind_history(EditorRuntime::Impl& runtime, HistoryCommand command) {
    auto id = runtime.active_document_id();
    auto* document = runtime.active_document();
    if (!id || document == nullptr) return failure("no active document");
    auto& history = runtime.history_for(*id);
    auto result = command == HistoryCommand::Undo ? history.undo(*document) : history.redo(*document);
    if (!result.accepted()) return failure(result.message);
    if (result.selections) runtime.selection.selections = *result.selections;
    runtime.clamp_selection_to_active_document();
    runtime.reveal_primary_caret();
    (void)runtime.update_tabs_for(*id);
    runtime.refresh_syntax();
    return success();
}

CommandHandlerResult bind_clipboard(EditorRuntime::Impl& runtime, ClipboardCommand command) {
    auto id = runtime.active_document_id();
    auto* document = runtime.active_document();
    if (!id || document == nullptr) return failure("no active document");
    auto& history = runtime.history_for(*id);
    ClipboardResult result{ClipboardError::None, ClipboardSystemStatus::NotRequested,
                           document->revision(), std::nullopt, std::nullopt, false, {}};
    switch (command) {
        case ClipboardCommand::Copy:
            result = runtime.clipboard.copy(document->snapshot(), runtime.selection.selections);
            break;
        case ClipboardCommand::Cut:
            result = runtime.clipboard.cut(*document, history, runtime.selection.selections, 0);
            break;
        case ClipboardCommand::Paste:
            result = runtime.clipboard.paste(*document, history, runtime.selection.selections,
                                             ClipboardPasteMode::InternalOnly, 0);
            break;
    }
    if (!result.accepted()) return failure(result.message);
    if (result.selections) runtime.selection.selections = *result.selections;
    if (result.document_changed) {
        // Reveal the primary caret (a pure viewport op that preserves the whole
        // selection set) so a cut/paste with the caret off-screen scrolls into
        // view. Do NOT clamp here: clamp_selection_to_active_document collapses
        // the set to a single caret and would discard a multi-cursor cut/paste.
        runtime.reveal_primary_caret();
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
    auto const reserved = prompt_row_count(PromptKind::Find) + 1;
    auto const base_rows = runtime.last_pane_content_rows +
                           runtime.last_reserved_prompt_rows;
    auto const reveal_rows = base_rows > reserved
                                 ? base_rows - reserved
                                 : std::uint32_t{1};
    ViewportDimensions reveal_viewport{runtime.last_pane_content_columns,
                                       reveal_rows};
    auto result = apply_selection_navigation(
        text, runtime.selection, SelectionCommand::ViewRevealCaret,
        reveal_viewport, {}, {}, 4, runtime.word_wrap);
    if (result.accepted() && result.delta.replacement) {
        runtime.selection = *result.delta.replacement;
    }
    runtime.requested_first_visual_row = runtime.selection.first_visual_row;
    runtime.requested_first_visual_column = runtime.selection.first_visual_column;
}

// A replace prompt is active when the controller is open in replace mode AND
// the active prompt is the replace prompt.  The replacement-editing and
// destructive replace commands guard on this so a stray prompt-context chord
// from an unrelated (palette/settings) prompt cannot mutate hidden state (an
// open-only guard leaks because find can stay open behind another prompt).
bool replace_prompt_active(EditorRuntime::Impl& runtime) {
    auto const& state = runtime.find_replace.view_state();
    auto const& request = runtime.prompt.request();
    return state.open && state.replace_mode && request &&
           request->kind == PromptKind::Replace;
}

// A find or replace prompt is the active prompt over an open controller.  The
// toggle/next/previous chords guard on this so a stray prompt-context chord from
// an unrelated (palette/settings) prompt cannot mutate hidden find state (find
// can remain open behind another prompt).
bool find_or_replace_prompt_active(EditorRuntime::Impl& runtime) {
    auto const& state = runtime.find_replace.view_state();
    auto const& request = runtime.prompt.request();
    return state.open && request &&
           (request->kind == PromptKind::Find ||
            request->kind == PromptKind::Replace);
}

// The find/replace option indicators shared by the find and replace prompts,
// seeded from the current options; project_find_replace_prompt refreshes their
// `checked` state from the authoritative options at snapshot time.
std::vector<PromptToggle> find_option_toggles(EditorRuntime::Impl& runtime) {
    auto const& options = runtime.find_replace.view_state().options;
    return {{"find.toggle_case", "Case", options.case_sensitive, 9},
            {"find.toggle_whole_word", "Word", options.whole_word, 9},
            {"find.toggle_regex", "Regex", options.regex, 10}};
}

CommandHandlerResult bind_find_replace(EditorRuntime::Impl& runtime,
                                       Revision revision,
                                       FindReplaceCommand command,
                                       std::any const& payload) {
    auto* document = runtime.active_document();
    auto query = payload_as<std::string>(payload) ? *payload_as<std::string>(payload) : runtime.find_replace.view_state().query;
    std::optional<ByteRange> range;
    DocumentSnapshot snapshot{{}, Revision{0}, DocumentMode::Edit, false};
    if (document != nullptr) {
        snapshot = document->snapshot();
        auto selected = runtime.selection.selections.primary();
        if (!selected.is_caret()) range = ByteRange{selected.lower().byte_offset, selected.upper().byte_offset};
    } else if (command != FindReplaceCommand::ReplaceWorkspacePreview &&
               command != FindReplaceCommand::ReplaceWorkspaceApply &&
               command != FindReplaceCommand::FindClose &&
               command != FindReplaceCommand::FindNext &&
               command != FindReplaceCommand::FindPrevious) {
        return failure("no active document");
    }
    switch (command) {
        case FindReplaceCommand::FindOpen:
            runtime.find_replace.open(snapshot, FindRequest{query, runtime.find_replace.view_state().options, range});
            runtime.find_document_id = runtime.active_document_id();
            reveal_active_find_match(runtime);
            // Open the find prompt so focus moves to it and the reserved rows
            // display the controller query (projected at snapshot time).
            (void)runtime.prompt.open(PromptRequest{
                PromptKind::Find, "Find", {{"find.query", "Find query", query}},
                find_option_toggles(runtime),
                PromptMatchCount{"find.count", "Match count", ""}});
            return success();
        case FindReplaceCommand::ReplaceOpen:
            runtime.find_replace.open_replace(snapshot, FindRequest{query, runtime.find_replace.view_state().options, range});
            runtime.find_document_id = runtime.active_document_id();
            reveal_active_find_match(runtime);
            // Three-row replace prompt: query (row 0, display-only, seeded from
            // the current find query), replacement (row 1, editable), and the
            // option/match-count row.  The client edits only the replacement.
            (void)runtime.prompt.open(PromptRequest{
                PromptKind::Replace, "Replace",
                {{"find.query", "Find query", query},
                 {"replace.replacement", "Replace with",
                  runtime.find_replace.view_state().replacement}},
                find_option_toggles(runtime),
                PromptMatchCount{"find.count", "Match count", ""}});
            return success();
        case FindReplaceCommand::ReplaceUpdateReplacement: {
            if (!replace_prompt_active(runtime)) return success();
            auto const* arguments = payload_as<FindQueryArguments>(payload);
            if (arguments == nullptr) return failure("replace.update_replacement requires a replacement payload");
            runtime.find_replace.update_replacement(arguments->query);
            runtime.find_document_id = runtime.active_document_id();
            return success();
        }
        case FindReplaceCommand::FindClose:
            runtime.find_replace.close();
            runtime.find_document_id.reset();
            // Dismiss the find or replace prompt (both belong to this
            // controller); a global find.close must not cancel an unrelated
            // palette/settings prompt.
            if (auto const& request = runtime.prompt.request();
                request && (request->kind == PromptKind::Find ||
                            request->kind == PromptKind::Replace)) {
                (void)runtime.prompt.cancel();
            }
            return success();
        case FindReplaceCommand::FindNext:
            if (!find_or_replace_prompt_active(runtime)) return success();
            runtime.find_replace.next();
            reveal_active_find_match(runtime);
            return success();
        case FindReplaceCommand::FindPrevious:
            if (!find_or_replace_prompt_active(runtime)) return success();
            runtime.find_replace.previous();
            reveal_active_find_match(runtime);
            return success();
        case FindReplaceCommand::FindUpdateQuery: {
            auto const* arguments = payload_as<FindQueryArguments>(payload);
            if (arguments == nullptr) return failure("find.update_query requires a query payload");
            runtime.find_replace.update_query(snapshot, arguments->query, range);
            runtime.find_document_id = runtime.active_document_id();
            reveal_active_find_match(runtime);
            return success();
        }
        case FindReplaceCommand::FindToggleCase:
            if (!find_or_replace_prompt_active(runtime)) return success();
            runtime.find_replace.toggle_case(snapshot);
            runtime.find_document_id = runtime.active_document_id();
            return success();
        case FindReplaceCommand::FindToggleWholeWord:
            if (!find_or_replace_prompt_active(runtime)) return success();
            runtime.find_replace.toggle_whole_word(snapshot);
            runtime.find_document_id = runtime.active_document_id();
            return success();
        case FindReplaceCommand::FindToggleRegex:
            if (!find_or_replace_prompt_active(runtime)) return success();
            runtime.find_replace.toggle_regex(snapshot);
            runtime.find_document_id = runtime.active_document_id();
            return success();
        case FindReplaceCommand::FindToggleSelection:
            runtime.find_replace.toggle_selection(snapshot, range);
            runtime.find_document_id = runtime.active_document_id();
            return success();
        case FindReplaceCommand::ReplaceCurrent:
        case FindReplaceCommand::ReplaceAll: {
            if (!replace_prompt_active(runtime)) return success();
            auto id = runtime.active_document_id();
            if (!id) return failure("no active document");
            auto replacement = runtime.find_replace.view_state().replacement;
            auto before = runtime.selection.selections;
            auto after = runtime.selection.selections;
            auto result = command == FindReplaceCommand::ReplaceCurrent
                ? runtime.find_replace.replace_current(*document, runtime.history_for(*id), before, after, replacement, 0)
                : runtime.find_replace.replace_all(*document, runtime.history_for(*id), before, after, replacement, 0);
            if (!result.accepted()) return failure(result.message);
            runtime.refresh_syntax();
            auto tabs_result = runtime.update_tabs_for(*id);
            reveal_active_find_match(runtime);
            // If no match remains to reveal (common after replace.all), still
            // reveal the primary caret so a replace with the caret off-screen
            // scrolls into view, per the edits-reveal policy (doc/spec-scroll.md
            // R5). When a match does remain, reveal_active_find_match already
            // revealed it above the prompt; don't override that.
            auto const& fr = runtime.find_replace.view_state();
            if (!fr.open || !fr.active_match ||
                *fr.active_match >= fr.matches.size()) {
                runtime.reveal_primary_caret();
            }
            return tabs_result;
        }
        case FindReplaceCommand::ReplaceWorkspacePreview:
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
        case FindReplaceCommand::ReplaceWorkspaceApply: {
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

void EditorRuntime::Impl::reveal_primary_caret() {
    // Reveal against the real editor pane cached from the last snapshot: the
    // content rows/columns already exclude any reserved prompt rows, so no prompt
    // adjustment is needed (unlike reveal_active_find_match, which runs while the
    // find prompt is open). The offset is re-clamped in compute_viewport, so a
    // one-frame-stale cache can never place it out of range.
    ViewportDimensions reveal_viewport{
        std::max<std::uint32_t>(last_pane_content_columns, 1),
        std::max<std::uint32_t>(last_pane_content_rows, 1)};
    auto result = apply_selection_navigation(
        active_text(), selection, SelectionCommand::ViewRevealCaret,
        reveal_viewport, {}, {}, 4, word_wrap);
    if (result.accepted() && result.delta.replacement) {
        selection = *result.delta.replacement;
    }
    requested_first_visual_row = selection.first_visual_row;
    requested_first_visual_column = selection.first_visual_column;
}

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
