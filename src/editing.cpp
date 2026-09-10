#include <ssg/Editor.h>


#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace ssg {
namespace {

enum class ClipboardCommand { Copy, Cut, Paste };
enum class HistoryCommand { Undo, Redo };

TextInputSettings textInputSettings(Editor const&) {
    return {IndentStyle::Spaces, 4, true, LineEnding::Lf};
}

std::vector<SyntaxEdit> syntaxEdits(std::string_view previousText,
                                    EditTransaction const& transaction) {
    std::vector<TextEdit const*> ordered;
    ordered.reserve(transaction.edits.size());
    for (auto const& edit : transaction.edits) ordered.push_back(&edit);
    std::sort(ordered.begin(), ordered.end(),
              [](TextEdit const* left, TextEdit const* right) {
                  return left->offset < right->offset;
              });
    std::vector<SyntaxEdit> edits;
    edits.reserve(ordered.size());
    std::size_t cursor = 0;
    SyntaxPoint point{LineIndex{0}, 0};
    const auto advance = [](SyntaxPoint current, std::string_view text) {
        for (const char value : text) {
            if (value == '\n') {
                current.row = LineIndex{current.row.value() + 1};
                current.columnByte = 0;
            } else {
                ++current.columnByte;
            }
        }
        return current;
    };
    const auto advanceTo = [&](std::size_t offset) {
        point = advance(point, previousText.substr(cursor, offset - cursor));
        cursor = offset;
        return point;
    };
    for (auto const* edit : ordered) {
        const auto start = static_cast<std::size_t>(edit->offset.value());
        const auto oldEnd = start + static_cast<std::size_t>(edit->erasedBytes);
        const auto startPoint = advanceTo(start);
        const auto oldEndPoint = advanceTo(oldEnd);
        edits.push_back({edit->offset, ByteOffset{oldEnd},
                         ByteOffset{start + edit->insertedText.size()},
                         startPoint, oldEndPoint,
                         advance(startPoint, edit->insertedText)});
    }
    return edits;
}

EditCommandSettings editSettings(Editor const&) {
    return {IndentStyle::Spaces, 4, 4, LineEnding::Lf, "//"};
}

OperationResult applyTransaction(Editor& runtime,
                                 EditTransaction const& transaction,
                                 SelectionSet const& selectionsAfter,
                                 HistoryEditKind kind) {
    auto id = runtime.activeDocumentId();
    auto* document = runtime.activeDocument();
    if (!id || document == nullptr) return failure("no active document");
    const auto previousText = document->snapshot().text;
    auto before = runtime.selection.selections;
    auto result = runtime.historyFor(*id).applyEdit(*document, transaction, before,
                                                       selectionsAfter, kind, 0);
    if (!result.accepted()) return failure(result.message);
    const auto syntax = syntaxEdits(previousText, transaction);
    runtime.selection.selections = result.selections.value_or(selectionsAfter);
    runtime.clampSelectionsToActiveDocument();
    (void)runtime.updateTabsFor(*id);
    runtime.refreshSyntax(syntax);
    return success();
}

OperationResult bindText(Editor& runtime, TextInputCommand command,
                         TextInputArguments arguments) {
    if (runtime.activeTabIsLiveDiff()) {
        return failure("text input is unavailable in diff mode");
    }
    auto const* document = runtime.activeDocument();
    if (document == nullptr) return failure("no active document");
    if (command != TextInputCommand::Insert) arguments = {};
    std::string inserted = arguments.text;
    auto result =
        applyTextInput(document->snapshot(), runtime.selection.selections,
                       textInputSettings(runtime), command,
                       std::move(arguments));
    if (!result.accepted() || !result.transaction || !result.selections) {
        return failure(result.message);
    }
    auto outcome =
        applyTransaction(runtime, *result.transaction,
                         *result.selections, historyEditKind(command));
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

OperationResult bindSelection(Editor& runtime, SelectionCommand command,
                              SelectionCommandArguments arguments = {}) {
    auto navigation = runtime.selection;
    const auto diffFile = runtime.activeDiffFile();
    auto result = ssg::navigateSelection(runtime.activeText(), navigation,
                                         command, {1, 1}, arguments, {}, 4,
                                         runtime.wordWrap,
                                         diffFile ? &*diffFile : nullptr);
    if (!result.accepted()) return failure(result.message);
    if (result.delta.replacement) {
        runtime.selection.selections = result.delta.replacement->selections;
        navigation = *result.delta.replacement;
    }
    if (auto active = runtime.activeDocumentId()) {
        runtime.historyFor(*active).breakCoalescing();
    }
    runtime.recordNavigation(NavigationClass::User);
    return success();
}

OperationResult bindEdit(Editor& runtime, EditCommand command) {
    if (runtime.activeTabIsLiveDiff()) {
        return failure("edit command cannot mutate a diff document");
    }
    auto const* document = runtime.activeDocument();
    if (document == nullptr) return failure("no active document");
    auto result = applyEditCommand(document->snapshot(), runtime.selection.selections,
                                     editSettings(runtime), command);
    if (!result.accepted() || !result.transaction || !result.selections) {
        return failure(result.message);
    }
    return applyTransaction(runtime, *result.transaction,
                            *result.selections, HistoryEditKind::Other);
}

OperationResult bindHistory(Editor& runtime, HistoryCommand command) {
    if (runtime.activeTabIsLiveDiff()) {
        return failure("history command cannot mutate a diff document");
    }
    auto id = runtime.activeDocumentId();
    auto* document = runtime.activeDocument();
    if (!id || document == nullptr) return failure("no active document");
    auto& history = runtime.historyFor(*id);
    auto result = command == HistoryCommand::Undo ? history.undo(*document) : history.redo(*document);
    if (!result.accepted()) return failure(result.message);
    if (result.selections) runtime.selection.selections = *result.selections;
    runtime.clampSelectionsToActiveDocument();
    (void)runtime.updateTabsFor(*id);
    runtime.refreshSyntax();
    return success();
}

OperationResult bindClipboard(Editor& runtime, ClipboardCommand command) {
    if (runtime.activeTabIsLiveDiff() && command != ClipboardCommand::Copy) {
        return failure("clipboard mutation is unavailable in diff mode");
    }
    auto id = runtime.activeDocumentId();
    auto* document = runtime.activeDocument();
    if (!id || document == nullptr) return failure("no active document");
    auto& history = runtime.historyFor(*id);
    ClipboardResult result{ClipboardError::None, document->revision(),
                           std::nullopt, std::nullopt, false, {}};
    switch (command) {
        case ClipboardCommand::Copy:
            result = runtime.clipboard.copy(document->snapshot(), runtime.selection.selections);
            break;
        case ClipboardCommand::Cut:
            result = runtime.clipboard.cut(*document, history, runtime.selection.selections, 0);
            break;
        case ClipboardCommand::Paste:
            result = runtime.clipboard.paste(*document, history,
                                             runtime.selection.selections, 0);
            break;
    }
    if (!result.accepted()) return failure(result.message);
    if (result.selections) runtime.selection.selections = *result.selections;
    if (result.documentChanged) {
        // Reveal the primary caret (a pure viewport op that preserves the whole
        // selection set) so a cut/paste with the caret off-screen scrolls into
        // view. Do NOT clamp here: clamp_selection_to_active_document collapses
        // the set to a single caret and would discard a multi-cursor cut/paste.
        (void)runtime.updateTabsFor(*id);
        runtime.refreshSyntax();
    }
    return success();
}

// Move the primary selection onto the active find match and reveal it so the
// viewport scrolls to follow find navigation (find.next/previous).
void revealActiveFindMatch(Editor& runtime) {
    auto const& state = runtime.findReplace.viewState();
    if (!state.open || !state.activeMatch ||
        *state.activeMatch >= state.matches.size()) {
        return;
    }
    auto const& match = state.matches[*state.activeMatch];
    auto const& text = runtime.activeText();
    auto anchor = ssg::resolveSelectionPosition(text, match.begin);
    auto active = ssg::resolveSelectionPosition(text, match.end);
    if (!anchor || !active) return;
    runtime.selection.selections =
        SelectionSet{std::vector<Selection>{Selection{*anchor, *active}}};
}

// A replace prompt is active when the controller is open in replace mode AND
// the active prompt is the replace prompt.  The replacement-editing and
// destructive replace commands guard on this so a stray prompt-context chord
// from an unrelated (palette/settings) prompt cannot mutate hidden state (an
// open-only guard leaks because find can stay open behind another prompt).
bool replacePromptActive(Editor& runtime) {
    auto const& state = runtime.findReplace.viewState();
    auto const& request = runtime.screen.prompt().request();
    return state.open && state.replaceMode && request &&
           request->kind == PromptKind::Replace;
}

// A find or replace prompt is the active prompt over an open controller.  The
// toggle/next/previous chords guard on this so a stray prompt-context chord from
// an unrelated (palette/settings) prompt cannot mutate hidden find state (find
// can remain open behind another prompt).
bool findOrReplacePromptActive(Editor& runtime) {
    auto const& state = runtime.findReplace.viewState();
    auto const& request = runtime.screen.prompt().request();
    return state.open && request &&
           (request->kind == PromptKind::Find ||
            request->kind == PromptKind::Replace);
}

// The find/replace option indicators shared by the find and replace prompts,
// seeded from the current options; project_find_replace_prompt refreshes their
// `checked` state from the authoritative options at snapshot time.
std::vector<PromptToggle> findOptionToggles(Editor& runtime) {
    auto const& options = runtime.findReplace.viewState().options;
    return {{"find.toggle_case", "case", options.caseSensitive, 9},
            {"find.toggle_whole_word", "word", options.wholeWord, 9},
            {"find.toggle_regex", "regex", options.regex, 10}};
}

OperationResult bindFindReplace(Editor& runtime, FindReplaceCommand command) {
    if (runtime.activeTabIsLiveDiff() &&
        (command == FindReplaceCommand::ReplaceOpen ||
         command == FindReplaceCommand::ReplaceCurrent ||
         command == FindReplaceCommand::ReplaceAll)) {
        return failure("replace commands are unavailable in diff mode");
    }
    auto* document = runtime.activeDocument();
    auto query = runtime.findReplace.viewState().query;
    std::optional<ByteRange> range;
    DocumentSnapshot snapshot{{}, std::uint64_t{0}, DocumentMode::Edit, false};
    if (document != nullptr) {
        snapshot = document->snapshot();
        auto selected = runtime.selection.selections.primary();
        if (!selected.isCaret()) range = ByteRange{selected.lower().byteOffset, selected.upper().byteOffset};
    } else if (command != FindReplaceCommand::FindClose &&
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
            if (auto opened = openGenericPrompt(runtime.screen.prompt(), PromptRequest{
                    PromptKind::Find, "find", {{"find.query", "find query", query}},
                    findOptionToggles(runtime),
                    PromptMatchCount{"find.count", "match count", ""}});
                !opened.accepted()) {
                return failure(opened.error->message);
            }
            return success();
        case FindReplaceCommand::FindWordUnderCursor: {
            if (document == nullptr) return success();
            auto const needle = runtime.selection.selections.primary()
                                    .wordOrCoveredText(snapshot.text);
            if (needle.empty()) return success();
            // Always a literal, whole-document search: regex and selection-only
            // are forced off so a needle with metacharacters (or a multi-line
            // selection needle) can never be reinterpreted as a pattern, and the
            // seeded prompt shows the clean word rather than an escaped form.
            auto options = runtime.findReplace.viewState().options;
            options.regex = false;
            options.selectionOnly = false;
            runtime.findReplace.open(
                snapshot, FindRequest{needle, options, std::nullopt});
            runtime.findDocumentId = runtime.activeDocumentId();
            revealActiveFindMatch(runtime);
            if (auto opened = openGenericPrompt(runtime.screen.prompt(), PromptRequest{
                    PromptKind::Find, "find", {{"find.query", "find query", needle}},
                    findOptionToggles(runtime),
                    PromptMatchCount{"find.count", "match count", ""}});
                !opened.accepted()) {
                return failure(opened.error->message);
            }
            return success();
        }
        case FindReplaceCommand::ReplaceOpen:
            runtime.findReplace.openReplace(snapshot, FindRequest{query, runtime.findReplace.viewState().options, range});
            runtime.findDocumentId = runtime.activeDocumentId();
            revealActiveFindMatch(runtime);
            // Three-row replace prompt: query (row 0, display-only, seeded from
            // the current find query), replacement (row 1, editable), and the
            // option/match-count row.  The client edits only the replacement.
            if (auto opened = openGenericPrompt(runtime.screen.prompt(), PromptRequest{
                    PromptKind::Replace, "replace",
                    {{"find.query", "find query", query},
                     {"replace.replacement", "replace with",
                      runtime.findReplace.viewState().replacement}},
                    findOptionToggles(runtime),
                    PromptMatchCount{"find.count", "match count", ""}});
                !opened.accepted()) {
                return failure(opened.error->message);
            }
            return success();
        case FindReplaceCommand::FindClose:
            runtime.findReplace.close();
            runtime.findDocumentId.reset();
            // Dismiss the find or replace prompt (both belong to this
            // controller); a global find.close must not cancel an unrelated
            // palette/settings prompt.
            if (auto const& request = runtime.screen.prompt().request();
                request && (request->kind == PromptKind::Find ||
                            request->kind == PromptKind::Replace)) {
                (void)runtime.screen.prompt().cancel();
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
            runtime.clampSelectionsToActiveDocument();
            runtime.refreshSyntax();
            auto tabsResult = runtime.updateTabsFor(*id);
            revealActiveFindMatch(runtime);
            // If no match remains to reveal (common after replace.all), still
            // reveal the primary caret so a replace with the caret off-screen
            // scrolls into view, per the edits-reveal policy. When a match does
            // remain, reveal_active_find_match already
            // revealed it above the prompt; don't override that.
            auto const& fr = runtime.findReplace.viewState();
            if (!fr.open || !fr.activeMatch ||
                *fr.activeMatch >= fr.matches.size()) {
            }
            return tabsResult;
        }
    }
    return failure("unknown find/replace command");
}

} // namespace

FindReplaceOperationResult applyFindQuery(Editor& runtime, std::string query) {
    auto* document = runtime.activeDocument();
    if (document == nullptr) {
        return {FindReplaceError::DocumentRejected, 0, "no active document"};
    }
    DocumentSnapshot snapshot{{}, std::uint64_t{0}, DocumentMode::Edit, false};
    std::optional<ByteRange> range;
    snapshot = document->snapshot();
    auto selected = runtime.selection.selections.primary();
    if (!selected.isCaret()) {
        range = ByteRange{selected.lower().byteOffset,
                          selected.upper().byteOffset};
    }
    runtime.findReplace.updateQuery(snapshot, query, range);
    runtime.findDocumentId = runtime.activeDocumentId();
    revealActiveFindMatch(runtime);
    return {};
}

FindReplaceOperationResult applyReplacement(Editor& runtime,
                                            std::string replacement) {
    if (!replacePromptActive(runtime)) return {};
    runtime.findReplace.updateReplacement(std::move(replacement));
    runtime.findDocumentId = runtime.activeDocumentId();
    return {};
}

OperationResult applyEditorSelections(Editor& runtime, ApplySelections mutation) {
    auto const active = runtime.activeDocumentId();
    if (!active || *active != mutation.document ||
        runtime.activeDocument() == nullptr) {
        return failure("selection target changed before execution");
    }
    runtime.selection.selections = std::move(mutation.selections);
    runtime.historyFor(mutation.document).breakCoalescing();
    if (mutation.focusEditor) {
        runtime.screen.focusEditor();
    }
    runtime.recordNavigation(NavigationClass::User);
    return success();
}

OperationResult applyEditorTextInput(Editor& runtime, TextInputCommand command,
                                     TextInputArguments arguments) {
    return bindText(runtime, command, std::move(arguments));
}

OperationResult executeFindReplaceCommand(Editor& runtime,
                                          FindReplaceCommand command) {
    return bindFindReplace(runtime, command);
}

void registerTextInputCommands(Commands& commands, Editor& runtime) {
    auto declare = [&](std::string id, std::string label, TextInputCommand command) {
        commands.add(std::move(id), std::move(label), [&runtime, command] {
            return applyEditorTextInput(runtime, command, {});
        });
    };
    commands.add("text.tab", "Insert Tab", [&runtime] {
        return applyEditorTextInput(runtime, TextInputCommand::Insert, {"\t"});
    });
    declare("text.newline", "Newline", TextInputCommand::Newline);
    declare("text.delete_backward", "Delete Backward", TextInputCommand::DeleteBackward);
    declare("text.delete_forward", "Delete Forward", TextInputCommand::DeleteForward);
    declare("text.delete_word_backward", "Delete Word Backward",
            TextInputCommand::DeleteWordBackward);
    declare("text.delete_word_forward", "Delete Word Forward",
            TextInputCommand::DeleteWordForward);
}

void registerHistoryCommands(Commands& commands, Editor& runtime) {
    auto declare = [&](std::string id, std::string label, HistoryCommand command) {
        commands.add(std::move(id), std::move(label), [&runtime, command] {
            return bindHistory(runtime, command);
        });
    };
    declare("edit.undo", "Undo", HistoryCommand::Undo);
    declare("edit.redo", "Redo", HistoryCommand::Redo);
}

void registerClipboardCommands(Commands& commands, Editor& runtime) {
    auto declare = [&](std::string id, std::string label, ClipboardCommand command) {
        commands.add(std::move(id), std::move(label), [&runtime, command] {
            return bindClipboard(runtime, command);
        });
    };
    declare("clipboard.copy", "Copy", ClipboardCommand::Copy);
    declare("clipboard.cut", "Cut", ClipboardCommand::Cut);
    declare("clipboard.paste", "Paste", ClipboardCommand::Paste);
}

void registerEditSuiteCommands(Commands& commands, Editor& runtime) {
    auto declare = [&](std::string id, std::string label, EditCommand command) {
        commands.add(std::move(id), std::move(label), [&runtime, command] {
            return bindEdit(runtime, command);
        });
    };
    declare("edit.indent", "Indent", EditCommand::Indent);
    declare("edit.outdent", "Outdent", EditCommand::Outdent);
    declare("edit.duplicate_line", "Edit Duplicate Line", EditCommand::DuplicateLine);
    declare("edit.move_line_up", "Edit Move Line Up", EditCommand::MoveLineUp);
    declare("edit.move_line_down", "Edit Move Line Down", EditCommand::MoveLineDown);
    declare("edit.delete_line", "Edit Delete Line", EditCommand::DeleteLine);
    declare("edit.join_lines", "Edit Join Lines", EditCommand::JoinLines);
    declare("edit.uppercase", "Edit Uppercase", EditCommand::Uppercase);
    declare("edit.lowercase", "Edit Lowercase", EditCommand::Lowercase);
    declare("edit.swap_case", "Edit Swap Case", EditCommand::SwapCase);
    declare("edit.sort_lines", "Edit Sort Lines", EditCommand::SortLines);
    declare("edit.transpose", "Edit Transpose", EditCommand::Transpose);
    declare("edit.toggle_comment", "Toggle Comment", EditCommand::ToggleComment);
}

void registerFindReplaceCommands(Commands& commands, Editor& runtime) {
    auto declare = [&](std::string id, std::string label, FindReplaceCommand command) {
        commands.add(std::move(id), std::move(label), [&runtime, command] {
            return executeFindReplaceCommand(runtime, command);
        });
    };
    declare("find.open", "Find", FindReplaceCommand::FindOpen);
    declare("find.word_under_cursor", "Find Word Under Cursor",
            FindReplaceCommand::FindWordUnderCursor);
    declare("find.close", "Find Close", FindReplaceCommand::FindClose);
    declare("find.next", "Find Next", FindReplaceCommand::FindNext);
    declare("find.previous", "Find Previous", FindReplaceCommand::FindPrevious);
    declare("find.toggle_case", "Find Toggle Case", FindReplaceCommand::FindToggleCase);
    declare("find.toggle_whole_word", "Find Toggle Whole Word",
            FindReplaceCommand::FindToggleWholeWord);
    declare("find.toggle_regex", "Find Toggle Regex", FindReplaceCommand::FindToggleRegex);
    declare("find.toggle_selection", "Find Toggle Selection",
            FindReplaceCommand::FindToggleSelection);
    declare("replace.open", "Replace", FindReplaceCommand::ReplaceOpen);
    declare("replace.current", "Replace Current", FindReplaceCommand::ReplaceCurrent);
    declare("replace.all", "Replace All", FindReplaceCommand::ReplaceAll);
}

std::string selectionLabel(std::string_view id) {
    std::string result;
    bool wordStart = true;
    for (const char raw : id) {
        if (raw == '.' || raw == '_') {
            result += ' ';
            wordStart = true;
            continue;
        }
        const auto ch = static_cast<unsigned char>(raw);
        result += wordStart ? static_cast<char>(std::toupper(ch)) : raw;
        wordStart = false;
    }
    return result;
}

void registerSelectionCommands(Commands& commands, Editor& runtime) {
    for (const auto& descriptor : kSelectionCommands) {
        const auto command = descriptor.command;
        if (command == SelectionCommand::CursorSetPosition ||
            command == SelectionCommand::SelectSetRange ||
            command == SelectionCommand::SelectSetRanges ||
            command == SelectionCommand::SelectAddRange ||
            command == SelectionCommand::SelectWordAtPosition) {
            continue;
        }
        const auto visualAction = [command]() -> std::optional<MoveVisualSelection> {
            switch (command) {
            case SelectionCommand::CursorLineUp:
                return MoveVisualSelection{VisualSelectionDirection::LineUp, false};
            case SelectionCommand::CursorLineDown:
                return MoveVisualSelection{VisualSelectionDirection::LineDown, false};
            case SelectionCommand::CursorPageUp:
                return MoveVisualSelection{VisualSelectionDirection::PageUp, false};
            case SelectionCommand::CursorPageDown:
                return MoveVisualSelection{VisualSelectionDirection::PageDown, false};
            case SelectionCommand::SelectLineUp:
                return MoveVisualSelection{VisualSelectionDirection::LineUp, true};
            case SelectionCommand::SelectLineDown:
                return MoveVisualSelection{VisualSelectionDirection::LineDown, true};
            case SelectionCommand::SelectPageUp:
                return MoveVisualSelection{VisualSelectionDirection::PageUp, true};
            case SelectionCommand::SelectPageDown:
                return MoveVisualSelection{VisualSelectionDirection::PageDown, true};
            default:
                return std::nullopt;
            }
        }();
        const auto id = std::string{descriptor.id};
        const auto label = selectionLabel(id);
        if (visualAction) {
            commands.add(id, label, [action = *visualAction] {
                return OperationResult{true, {}, ViewAction{action}};
            });
            continue;
        }
        if (command == SelectionCommand::ViewRevealCaret ||
            command == SelectionCommand::ViewCenterCaret) {
            commands.add(id, label, [command] {
                return OperationResult{
                    true, {},
                    command == SelectionCommand::ViewRevealCaret
                        ? ViewAction{RevealSelection{}}
                        : ViewAction{CenterSelection{}}};
            });
            continue;
        }
        commands.add(id, label, [&runtime, command] {
            return bindSelection(runtime, command);
        });
    }
}

void bindRuntimeEditing(Commands& commands, Editor& runtime) {
    registerTextInputCommands(commands, runtime);
    registerSelectionCommands(commands, runtime);
    registerEditSuiteCommands(commands, runtime);
    registerFindReplaceCommands(commands, runtime);
    registerHistoryCommands(commands, runtime);
    registerClipboardCommands(commands, runtime);
}

} // namespace ssg
