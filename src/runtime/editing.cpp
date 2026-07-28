#include "editor_runtime_internal.h"

#include <algorithm>
#include <cctype>

namespace ssg {
namespace {

template <typename T>
T const* payloadAs(std::any const& payload) {
    return std::any_cast<T>(&payload);
}

HistoryEditKind historyKind(TextInputCommand command) {
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

TextInputSettings textInputSettings(EditorRuntime::Impl const&) {
    return {IndentStyle::Spaces, 4, true, LineEnding::Lf};
}

EditCommandSettings editSettings(EditorRuntime::Impl const&) {
    return {IndentStyle::Spaces, 4, 4, LineEnding::Lf, "//"};
}

bool activeLiveDiffTab(const EditorRuntime::Impl& runtime) {
    const auto* tab = runtime.activeTabState();
    return tab != nullptr && tab->kind == TabKind::LiveDiff;
}

CommandHandlerResult applyTransaction(EditorRuntime::Impl& runtime,
                                       EditTransaction const& transaction,
                                       SelectionSet const& selectionsAfter,
                                       HistoryEditKind kind) {
    auto id = runtime.activeDocumentId();
    auto* document = runtime.activeDocument();
    if (!id || document == nullptr) return failure("no active document");
    auto before = runtime.selection.selections;
    auto result = runtime.historyFor(*id).applyEdit(*document, transaction, before,
                                                       selectionsAfter, kind, 0);
    if (!result.accepted()) return failure(result.message);
    runtime.selection.selections = result.selections.value_or(selectionsAfter);
    runtime.clampSelectionToActiveDocument();
    runtime.revealPrimaryCaret();
    (void)runtime.updateTabsFor(*id);
    runtime.refreshSyntax();
    return success();
}

CommandHandlerResult bindText(EditorRuntime::Impl& runtime,
                               TextInputCommand command,
                               TextInputArguments arguments) {
    if (activeLiveDiffTab(runtime)) {
        return failure("text input is unavailable in diff mode");
    }
    auto const* document = runtime.activeDocument();
    if (document == nullptr) return failure("no active document");
    // The payload arrives already typed: the handler declared what it consumes,
    // so there is no cast to fail here.
    if (command != TextInputCommand::Insert) arguments = {};
    std::string inserted = arguments.text;
    auto result = TextInputInterpreter{}.apply(document->snapshot(), runtime.selection.selections,
                                   textInputSettings(runtime), command, std::move(arguments));
    if (!result.accepted() || !result.transaction || !result.selections) {
        return failure(result.message);
    }
    auto outcome = applyTransaction(runtime, *result.transaction, *result.selections,
                                     historyKind(command));
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
            if (auto const active = runtime.activeDocumentId()) {
                runtime.historyFor(*active).breakCoalescing();
            }
        }
    }
    return outcome;
}

CommandHandlerResult bindSelection(EditorRuntime::Impl& runtime,
                                    ClientId client,
                                    SelectionCommand command,
                                    std::any const& payload) {
    SelectionCommandArguments arguments;
    if (auto const* typed = payloadAs<SelectionCommandArguments>(payload)) {
        arguments = *typed;
    }
    // Navigate and reveal against the REAL editor pane cached from the last
    // snapshot, not a fake {80, 24}: page motion advances by the real height and
    // the built-in caret reveal uses the real height/width (so a far-right column
    // on a wide line is not clamped at column 80). See doc/spec-scroll.md R6.
    ViewportDimensions const viewport{
        std::max<std::uint32_t>(runtime.lastPaneContentColumns, 1),
        std::max<std::uint32_t>(runtime.lastPaneContentRows, 1)};
    const auto diffFile = runtime.activeDiffFile();
    auto result = ssg::SelectionNavigator{}.apply(runtime.activeText(), runtime.selection,
                                             command, viewport,
                                             arguments, {}, 4,
                                             runtime.wordWrap,
                                             diffFile ? &*diffFile : nullptr);
    if (!result.accepted()) return failure(result.message);
    if (result.delta.replacement) runtime.selection = *result.delta.replacement;
    runtime.requestedFirstVisualRow = runtime.selection.firstVisualRow;
    runtime.requestedFirstVisualColumn = runtime.selection.firstVisualColumn;
    if (auto active = runtime.activeDocumentId()) {
        runtime.historyFor(*active).breakCoalescing();
    }
    // Focus follows the pointer (M8-F): a click-to-caret / drag-select acts on the
    // editor, so it moves the authoritative keyboard focus there. Gated on the two
    // pointer-driven selection commands; keyboard caret motion is a different
    // SelectionCommand and never reaches here.
    if (command == SelectionCommand::CursorSetPosition ||
        command == SelectionCommand::SelectSetRange) {
        runtime.shell.focusEditor();
    }
    runtime.recordNavigation(client, NavigationClass::User);
    return success();
}

CommandHandlerResult bindEdit(EditorRuntime::Impl& runtime, EditCommand command) {
    if (activeLiveDiffTab(runtime)) {
        return failure("edit command cannot mutate a diff document");
    }
    auto const* document = runtime.activeDocument();
    if (document == nullptr) return failure("no active document");
    auto result = EditInterpreter{}.apply(document->snapshot(), runtime.selection.selections,
                                     editSettings(runtime), command);
    if (!result.accepted() || !result.transaction || !result.selections) {
        return failure(result.message);
    }
    return applyTransaction(runtime, *result.transaction, *result.selections,
                             HistoryEditKind::Other);
}

CommandHandlerResult bindHistory(EditorRuntime::Impl& runtime, HistoryCommand command) {
    if (activeLiveDiffTab(runtime)) {
        return failure("history command cannot mutate a diff document");
    }
    auto id = runtime.activeDocumentId();
    auto* document = runtime.activeDocument();
    if (!id || document == nullptr) return failure("no active document");
    auto& history = runtime.historyFor(*id);
    auto result = command == HistoryCommand::Undo ? history.undo(*document) : history.redo(*document);
    if (!result.accepted()) return failure(result.message);
    if (result.selections) runtime.selection.selections = *result.selections;
    runtime.clampSelectionToActiveDocument();
    runtime.revealPrimaryCaret();
    (void)runtime.updateTabsFor(*id);
    runtime.refreshSyntax();
    return success();
}

CommandHandlerResult bindClipboard(EditorRuntime::Impl& runtime, ClipboardCommand command) {
    if (activeLiveDiffTab(runtime) && command != ClipboardCommand::Copy) {
        return failure("clipboard mutation is unavailable in diff mode");
    }
    auto id = runtime.activeDocumentId();
    auto* document = runtime.activeDocument();
    if (!id || document == nullptr) return failure("no active document");
    auto& history = runtime.historyFor(*id);
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
    if (result.documentChanged) {
        // Reveal the primary caret (a pure viewport op that preserves the whole
        // selection set) so a cut/paste with the caret off-screen scrolls into
        // view. Do NOT clamp here: clamp_selection_to_active_document collapses
        // the set to a single caret and would discard a multi-cursor cut/paste.
        runtime.revealPrimaryCaret();
        (void)runtime.updateTabsFor(*id);
        runtime.refreshSyntax();
    }
    return success();
}

// Move the primary selection onto the active find match and reveal it so the
// viewport scrolls to follow find navigation (find.next/previous/update_query).
void revealActiveFindMatch(EditorRuntime::Impl& runtime) {
    auto const& state = runtime.findReplace.viewState();
    if (!state.open || !state.activeMatch ||
        *state.activeMatch >= state.matches.size()) {
        return;
    }
    auto const& match = state.matches[*state.activeMatch];
    auto text = runtime.activeText();
    auto anchor = ssg::SelectionNavigator::resolvePosition(text, match.begin);
    auto active = ssg::SelectionNavigator::resolvePosition(text, match.end);
    if (!anchor || !active) return;
    runtime.selection.selections =
        SelectionSet{std::vector<Selection>{Selection{*anchor, *active}}};
    // Reveal against the real document pane from the last snapshot, reserving the
    // find prompt's rows plus a one-row margin so the match never sits flush
    // against the prompt.  Adding back the rows that snapshot reserved yields a
    // prompt-agnostic pane height; subtracting the find prompt's rows (and the
    // margin) guarantees the match lands above the prompt whether or not it was
    // open last snapshot.
    auto const reserved = promptRowCount(PromptKind::Find) + 1;
    auto const baseRows = runtime.lastPaneContentRows +
                           runtime.lastReservedPromptRows;
    auto const revealRows = baseRows > reserved
                                 ? baseRows - reserved
                                 : std::uint32_t{1};
    ViewportDimensions revealViewport{runtime.lastPaneContentColumns,
                                       revealRows};
    const auto diffFile = runtime.activeDiffFile();
    auto result = ssg::SelectionNavigator{}.apply(
        text, runtime.selection, SelectionCommand::ViewRevealCaret,
        revealViewport, {}, {}, 4, runtime.wordWrap,
        diffFile ? &*diffFile : nullptr);
    if (result.accepted() && result.delta.replacement) {
        runtime.selection = *result.delta.replacement;
    }
    runtime.requestedFirstVisualRow = runtime.selection.firstVisualRow;
    runtime.requestedFirstVisualColumn = runtime.selection.firstVisualColumn;
}

// A replace prompt is active when the controller is open in replace mode AND
// the active prompt is the replace prompt.  The replacement-editing and
// destructive replace commands guard on this so a stray prompt-context chord
// from an unrelated (palette/settings) prompt cannot mutate hidden state (an
// open-only guard leaks because find can stay open behind another prompt).
bool replacePromptActive(EditorRuntime::Impl& runtime) {
    auto const& state = runtime.findReplace.viewState();
    auto const& request = runtime.prompt.request();
    return state.open && state.replaceMode && request &&
           request->kind == PromptKind::Replace;
}

// A find or replace prompt is the active prompt over an open controller.  The
// toggle/next/previous chords guard on this so a stray prompt-context chord from
// an unrelated (palette/settings) prompt cannot mutate hidden find state (find
// can remain open behind another prompt).
bool findOrReplacePromptActive(EditorRuntime::Impl& runtime) {
    auto const& state = runtime.findReplace.viewState();
    auto const& request = runtime.prompt.request();
    return state.open && request &&
           (request->kind == PromptKind::Find ||
            request->kind == PromptKind::Replace);
}

// The find/replace option indicators shared by the find and replace prompts,
// seeded from the current options; project_find_replace_prompt refreshes their
// `checked` state from the authoritative options at snapshot time.
std::vector<PromptToggle> findOptionToggles(EditorRuntime::Impl& runtime) {
    auto const& options = runtime.findReplace.viewState().options;
    return {{"find.toggle_case", "Case", options.caseSensitive, 9},
            {"find.toggle_whole_word", "Word", options.wholeWord, 9},
            {"find.toggle_regex", "Regex", options.regex, 10}};
}

CommandHandlerResult bindFindReplace(EditorRuntime::Impl& runtime,
                                     Revision revision,
                                     FindReplaceCommand command,
                                     std::any const& payload) {
    if (activeLiveDiffTab(runtime) &&
        (command == FindReplaceCommand::ReplaceOpen ||
         command == FindReplaceCommand::ReplaceCurrent ||
         command == FindReplaceCommand::ReplaceAll ||
         command == FindReplaceCommand::ReplaceWorkspaceApply)) {
        return failure("replace commands are unavailable in diff mode");
    }
    auto* document = runtime.activeDocument();
    auto query = payloadAs<std::string>(payload) ? *payloadAs<std::string>(payload) : runtime.findReplace.viewState().query;
    std::optional<ByteRange> range;
    DocumentSnapshot snapshot{{}, Revision{0}, DocumentMode::Edit, false};
    if (document != nullptr) {
        snapshot = document->snapshot();
        auto selected = runtime.selection.selections.primary();
        if (!selected.isCaret()) range = ByteRange{selected.lower().byteOffset, selected.upper().byteOffset};
    } else if (command != FindReplaceCommand::ReplaceWorkspacePreview &&
               command != FindReplaceCommand::ReplaceWorkspaceApply &&
               command != FindReplaceCommand::FindClose &&
               command != FindReplaceCommand::FindNext &&
               command != FindReplaceCommand::FindPrevious) {
        return failure("no active document");
    }
    switch (command) {
        case FindReplaceCommand::FindOpen:
            runtime.findReplace.open(snapshot, FindRequest{query, runtime.findReplace.viewState().options, range});
            runtime.findDocumentId = runtime.activeDocumentId();
            revealActiveFindMatch(runtime);
            // Open the find prompt so focus moves to it and the reserved rows
            // display the controller query (projected at snapshot time).
            (void)runtime.prompt.open(PromptRequest{
                PromptKind::Find, "Find", {{"find.query", "Find query", query}},
                findOptionToggles(runtime),
                PromptMatchCount{"find.count", "Match count", ""}});
            return success();
        case FindReplaceCommand::ReplaceOpen:
            runtime.findReplace.openReplace(snapshot, FindRequest{query, runtime.findReplace.viewState().options, range});
            runtime.findDocumentId = runtime.activeDocumentId();
            revealActiveFindMatch(runtime);
            // Three-row replace prompt: query (row 0, display-only, seeded from
            // the current find query), replacement (row 1, editable), and the
            // option/match-count row.  The client edits only the replacement.
            (void)runtime.prompt.open(PromptRequest{
                PromptKind::Replace, "Replace",
                {{"find.query", "Find query", query},
                 {"replace.replacement", "Replace with",
                  runtime.findReplace.viewState().replacement}},
                findOptionToggles(runtime),
                PromptMatchCount{"find.count", "Match count", ""}});
            return success();
        case FindReplaceCommand::ReplaceUpdateReplacement: {
            if (!replacePromptActive(runtime)) return success();
            auto const* arguments = payloadAs<FindQueryArguments>(payload);
            if (arguments == nullptr) return failure("replace.update_replacement requires a replacement payload");
            runtime.findReplace.updateReplacement(arguments->query);
            runtime.findDocumentId = runtime.activeDocumentId();
            return success();
        }
        case FindReplaceCommand::FindClose:
            runtime.findReplace.close();
            runtime.findDocumentId.reset();
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
            if (!findOrReplacePromptActive(runtime)) return success();
            runtime.findReplace.next();
            revealActiveFindMatch(runtime);
            return success();
        case FindReplaceCommand::FindPrevious:
            if (!findOrReplacePromptActive(runtime)) return success();
            runtime.findReplace.previous();
            revealActiveFindMatch(runtime);
            return success();
        case FindReplaceCommand::FindUpdateQuery: {
            auto const* arguments = payloadAs<FindQueryArguments>(payload);
            if (arguments == nullptr) return failure("find.update_query requires a query payload");
            runtime.findReplace.updateQuery(snapshot, arguments->query, range);
            runtime.findDocumentId = runtime.activeDocumentId();
            revealActiveFindMatch(runtime);
            return success();
        }
        case FindReplaceCommand::FindToggleCase:
            if (!findOrReplacePromptActive(runtime)) return success();
            runtime.findReplace.toggleCase(snapshot);
            runtime.findDocumentId = runtime.activeDocumentId();
            return success();
        case FindReplaceCommand::FindToggleWholeWord:
            if (!findOrReplacePromptActive(runtime)) return success();
            runtime.findReplace.toggleWholeWord(snapshot);
            runtime.findDocumentId = runtime.activeDocumentId();
            return success();
        case FindReplaceCommand::FindToggleRegex:
            if (!findOrReplacePromptActive(runtime)) return success();
            runtime.findReplace.toggleRegex(snapshot);
            runtime.findDocumentId = runtime.activeDocumentId();
            return success();
        case FindReplaceCommand::FindToggleSelection:
            runtime.findReplace.toggleSelection(snapshot, range);
            runtime.findDocumentId = runtime.activeDocumentId();
            return success();
        case FindReplaceCommand::ReplaceCurrent:
        case FindReplaceCommand::ReplaceAll: {
            if (!replacePromptActive(runtime)) return success();
            auto id = runtime.activeDocumentId();
            if (!id) return failure("no active document");
            auto replacement = runtime.findReplace.viewState().replacement;
            auto before = runtime.selection.selections;
            auto after = runtime.selection.selections;
            auto result = command == FindReplaceCommand::ReplaceCurrent
                ? runtime.findReplace.replaceCurrent(*document, runtime.historyFor(*id), before, after, replacement, 0)
                : runtime.findReplace.replaceAll(*document, runtime.historyFor(*id), before, after, replacement, 0);
            if (!result.accepted()) return failure(result.message);
            runtime.refreshSyntax();
            auto tabsResult = runtime.updateTabsFor(*id);
            revealActiveFindMatch(runtime);
            // If no match remains to reveal (common after replace.all), still
            // reveal the primary caret so a replace with the caret off-screen
            // scrolls into view, per the edits-reveal policy (doc/spec-scroll.md
            // R5). When a match does remain, reveal_active_find_match already
            // revealed it above the prompt; don't override that.
            auto const& fr = runtime.findReplace.viewState();
            if (!fr.open || !fr.activeMatch ||
                *fr.activeMatch >= fr.matches.size()) {
                runtime.revealPrimaryCaret();
            }
            return tabsResult;
        }
        case FindReplaceCommand::ReplaceWorkspacePreview:
        {
            auto const* arguments = payloadAs<WorkspaceReplaceArguments>(payload);
            if (arguments == nullptr) {
                return failure("replace.workspace_preview requires a workspace replace payload");
            }
            auto result = WorkspaceReplacer{}.preview(runtime, revision,
                                                    arguments->request,
                                                    arguments->replacement);
            if (!result.accepted()) return failure(result.message);
            runtime.workspaceReplacePreview = std::move(result.preview);
            return success();
        }
        case FindReplaceCommand::ReplaceWorkspaceApply: {
            auto const* explicitPreview = payloadAs<WorkspaceReplacePreview>(payload);
            if (payload.has_value() && explicitPreview == nullptr) {
                return failure("replace.workspace_apply payload has the wrong type");
            }
            auto const* preview = runtime.workspaceReplacePreview
                ? &*runtime.workspaceReplacePreview
                : nullptr;
            if (preview == nullptr) {
                return failure("replace.workspace_apply requires a workspace replace preview payload");
            }
            if (explicitPreview != nullptr && *explicitPreview != *preview) {
                return failure("replace.workspace_apply payload does not match the current workspace preview");
            }
            auto result = WorkspaceReplacer{}.apply(runtime, *preview, runtime);
            if (!result.accepted()) return failure(result.message);
            runtime.workspaceReplacePreview.reset();
            runtime.refreshTree();
            return success();
        }
    }
    return failure("unknown find/replace command");
}

} // namespace

CommandHandlerResult executeFindReplaceCommand(EditorRuntime::Impl& runtime,
                                               Revision revision,
                                               FindReplaceCommand command,
                                               std::any const& payload) {
    return bindFindReplace(runtime, revision, command, payload);
}

void EditorRuntime::Impl::revealPrimaryCaret() {
    // Reveal against the real editor pane cached from the last snapshot: the
    // content rows/columns already exclude any reserved prompt rows, so no prompt
    // adjustment is needed (unlike reveal_active_find_match, which runs while the
    // find prompt is open). The offset is re-clamped in compute_viewport, so a
    // one-frame-stale cache can never place it out of range.
    ViewportDimensions revealViewport{
        std::max<std::uint32_t>(lastPaneContentColumns, 1),
        std::max<std::uint32_t>(lastPaneContentRows, 1)};
    const auto diffFile = activeDiffFile();
    auto result = ssg::SelectionNavigator{}.apply(
        activeText(), selection, SelectionCommand::ViewRevealCaret,
        revealViewport, {}, {}, 4, wordWrap,
        diffFile ? &*diffFile : nullptr);
    if (result.accepted() && result.delta.replacement) {
        selection = *result.delta.replacement;
    }
    requestedFirstVisualRow = selection.firstVisualRow;
    requestedFirstVisualColumn = selection.firstVisualColumn;
}

// The text-input commands, declared where they are implemented.
//
// The first component migrated off the static table
// (doc/spec-command-registry.md, D3).  Each command's facts and its handler are
// one expression, so the argument type is written once -- in `handler<...>` --
// and the codec, the unwrap and the reference's argument column are all derived
// from it.  There is no row elsewhere to keep in step.
void registerTextInputCommands(EditorSessionBuilder& builder,
                               EditorRuntime::Impl& runtime) {
    // Only insertion carries text.  The other five never read a payload -- the
    // old handler default-constructed one and ignored it -- yet the static table
    // declared all six as taking text.  Deducing the type from the handler makes
    // that fiction impossible to write: a command that does not consume an
    // argument cannot declare one.
    auto declareTextless = [&](std::string id, std::string label,
                               std::string summary, TextInputCommand command) {
        builder.add(CommandSpecBuilder{std::move(id)}
                        .owner("text-input-commands")
                        .label(std::move(label))
                        .summary(std::move(summary))
                        .mutates()
                        .lua()
                        .handler([&runtime, command](CommandContext&) {
                            return runtime.runTransaction([&] {
                                return bindText(runtime, command, {});
                            });
                        }));
    };

    builder.add(CommandSpecBuilder{"text.insert"}
                    .owner("text-input-commands")
                    .label("Insert")
                    .summary("Insert")
                    .mutates()
                    .lua()
                    .handler<TextInputArguments>(
                        [&runtime](CommandContext&,
                                   TextInputArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                return bindText(runtime,
                                                TextInputCommand::Insert,
                                                arguments);
                            });
                        }));
    declareTextless("text.newline", "Newline", "Newline",
                    TextInputCommand::Newline);
    declareTextless("text.delete_backward", "Delete Backward",
                    "Delete Backward", TextInputCommand::DeleteBackward);
    declareTextless("text.delete_forward", "Delete Forward", "Delete Forward",
                    TextInputCommand::DeleteForward);
    declareTextless("text.delete_word_backward", "Delete Word Backward",
                    "Delete Word Backward",
                    TextInputCommand::DeleteWordBackward);
    declareTextless("text.delete_word_forward", "Delete Word Forward",
                    "Delete Word Forward",
                    TextInputCommand::DeleteWordForward);
}

// Undo and redo.
void registerHistoryCommands(EditorSessionBuilder& builder,
                             EditorRuntime::Impl& runtime) {
    auto declare = [&](std::string id, std::string summary,
                       HistoryCommand command) {
        builder.add(CommandSpecBuilder{std::move(id)}
                        .owner("undo-redo-history")
                        .label(summary)
                        .summary(std::move(summary))
                        .mutates()
                        .lua()
                        .handler([&runtime, command](CommandContext&) {
                            return runtime.runTransaction([&] {
                                return bindHistory(runtime, command);
                            });
                        }));
    };
    declare("edit.undo", "Undo", HistoryCommand::Undo);
    declare("edit.redo", "Redo", HistoryCommand::Redo);
}

// The clipboard register: copy, cut and paste over the current selections.
void registerClipboardCommands(EditorSessionBuilder& builder,
                               EditorRuntime::Impl& runtime) {
    auto declare = [&](std::string id, std::string summary,
                       ClipboardCommand command) {
        builder.add(CommandSpecBuilder{std::move(id)}
                        .owner("clipboard-register")
                        .label(summary)
                        .summary(std::move(summary))
                        .mutates()
                        .lua()
                        .handler([&runtime, command](CommandContext&) {
                            return runtime.runTransaction([&] {
                                return bindClipboard(runtime, command);
                            });
                        }));
    };
    declare("clipboard.copy", "Copy", ClipboardCommand::Copy);
    declare("clipboard.cut", "Cut", ClipboardCommand::Cut);
    declare("clipboard.paste", "Paste", ClipboardCommand::Paste);
}

// Whole-line and whole-selection edits.  None takes an argument: each acts on
// wherever the selections already are.
void registerEditSuiteCommands(EditorSessionBuilder& builder,
                               EditorRuntime::Impl& runtime) {
    auto declare = [&](std::string id, std::string label, std::string summary,
                       EditCommand command) {
        auto built = CommandSpecBuilder{std::move(id)}
                         .owner("edit-command-suite")
                         .summary(std::move(summary))
                         .mutates()
                         .lua()
                         .handler([&runtime, command](CommandContext&) {
                             return runtime.runTransaction([&] {
                                 return bindEdit(runtime, command);
                             });
                         });
        if (!label.empty()) built.label(std::move(label));
        builder.add(std::move(built));
    };
    declare("edit.indent", "Indent", "Indent", EditCommand::Indent);
    declare("edit.outdent", "Outdent", "Outdent", EditCommand::Outdent);
    declare("edit.duplicate_line", "", "Duplicate Line",
            EditCommand::DuplicateLine);
    declare("edit.move_line_up", "", "Move Line Up", EditCommand::MoveLineUp);
    declare("edit.move_line_down", "", "Move Line Down",
            EditCommand::MoveLineDown);
    declare("edit.delete_line", "", "Delete Line", EditCommand::DeleteLine);
    declare("edit.join_lines", "", "Join Lines", EditCommand::JoinLines);
    declare("edit.uppercase", "", "Uppercase", EditCommand::Uppercase);
    declare("edit.lowercase", "", "Lowercase", EditCommand::Lowercase);
    declare("edit.swap_case", "", "Swap Case", EditCommand::SwapCase);
    declare("edit.sort_lines", "", "Sort Lines", EditCommand::SortLines);
    declare("edit.transpose", "", "Transpose", EditCommand::Transpose);
    declare("edit.toggle_comment", "Toggle Comment", "Toggle Comment",
            EditCommand::ToggleComment);
}

// Find and replace, in the open document and across the workspace.
void registerFindReplaceCommands(EditorSessionBuilder& builder,
                                 EditorRuntime::Impl& runtime) {
    auto spec = [](std::string id, std::string summary) {
        return CommandSpecBuilder{std::move(id)}
            .owner("find-replace")
            .summary(std::move(summary))
            .mutates()
            .lua();
    };
    auto bare = [&](std::string id, std::string label, std::string summary,
                    FindReplaceCommand command) {
        auto built = spec(std::move(id), std::move(summary))
                         .handler([&runtime, command](CommandContext& context) {
                             return runtime.runTransaction([&] {
                                 return executeFindReplaceCommand(
                                     runtime, context.revision(), command, {});
                             });
                         });
        if (!label.empty()) built.label(std::move(label));
        builder.add(std::move(built));
    };

    bare("find.open", "Find", "Find", FindReplaceCommand::FindOpen);
    bare("find.close", "", "Close", FindReplaceCommand::FindClose);
    bare("find.next", "", "Next", FindReplaceCommand::FindNext);
    bare("find.previous", "", "Previous", FindReplaceCommand::FindPrevious);
    bare("find.toggle_case", "", "Toggle Case",
         FindReplaceCommand::FindToggleCase);
    bare("find.toggle_whole_word", "", "Toggle Whole Word",
         FindReplaceCommand::FindToggleWholeWord);
    bare("find.toggle_regex", "", "Toggle Regex",
         FindReplaceCommand::FindToggleRegex);
    bare("find.toggle_selection", "", "Toggle Selection",
         FindReplaceCommand::FindToggleSelection);
    bare("replace.open", "Replace", "Replace", FindReplaceCommand::ReplaceOpen);
    bare("replace.current", "", "Current", FindReplaceCommand::ReplaceCurrent);
    bare("replace.all", "", "All", FindReplaceCommand::ReplaceAll);

    // These four carry a payload, and each has a defined meaning without one:
    // an absent query keeps the current one, and an absent preview applies the
    // one already held.  Demanding a payload would refuse calls that work.
    auto carrying = [&]<typename Arguments>(std::string id, std::string summary,
                                            FindReplaceCommand command,
                                            Arguments const*) {
        builder.add(spec(std::move(id), std::move(summary))
                        .optionalHandler<Arguments>(
                            [&runtime, command](
                                CommandContext& context,
                                std::optional<Arguments> const& arguments) {
                                return runtime.runTransaction([&] {
                                    return executeFindReplaceCommand(
                                        runtime, context.revision(), command,
                                        arguments ? std::any{*arguments}
                                                  : std::any{});
                                });
                            }));
    };
    carrying("find.update_query", "Update Query",
             FindReplaceCommand::FindUpdateQuery,
             static_cast<FindQueryArguments const*>(nullptr));
    carrying("replace.update_replacement", "Update Replacement",
             FindReplaceCommand::ReplaceUpdateReplacement,
             static_cast<FindQueryArguments const*>(nullptr));
    carrying("replace.workspace_preview", "Workspace Preview",
             FindReplaceCommand::ReplaceWorkspacePreview,
             static_cast<WorkspaceReplaceArguments const*>(nullptr));
    carrying("replace.workspace_apply", "Workspace Apply",
             FindReplaceCommand::ReplaceWorkspaceApply,
             static_cast<WorkspaceReplacePreview const*>(nullptr));
}

void bindRuntimeEditing(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime) {
    registerTextInputCommands(builder, runtime);
    registerEditSuiteCommands(builder, runtime);
    registerFindReplaceCommands(builder, runtime);
    registerHistoryCommands(builder, runtime);
    registerClipboardCommands(builder, runtime);
    auto selectionCommands = selectionNavigationCommandSet();
    for (auto const& descriptor : selectionCommands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext& context, std::any const& payload) {
            return runtime.runTransaction([&] {
                return bindSelection(runtime, context.principal().clientId(),
                                     descriptor.command, payload);
            });
        });
    }
}

} // namespace ssg
