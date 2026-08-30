#include "editor_session_internal.h"

#include <ssg/HistoryEditClassification.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace ssg {
namespace {

TextInputSettings textInputSettings(EditorSession::Impl const&) {
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

EditCommandSettings editSettings(EditorSession::Impl const&) {
    return {IndentStyle::Spaces, 4, 4, LineEnding::Lf, "//"};
}

CommandHandlerResult applyTransaction(EditorSession::Impl& runtime,
                                       ViewId viewId,
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
    runtime.revealPrimaryCaret(viewId);
    (void)runtime.updateTabsFor(*id);
    runtime.refreshSyntax(syntax);
    return success();
}

CommandHandlerResult bindText(EditorSession::Impl& runtime,
                               ViewId viewId,
                               TextInputCommand command,
                               TextInputArguments arguments) {
    if (runtime.activeTabIsLiveDiff()) {
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
    auto outcome =
        applyTransaction(runtime, viewId, *result.transaction,
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

CommandHandlerResult bindSelection(EditorSession::Impl& runtime,
                                    ViewId viewId,
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
    // on a wide line is not clamped at column 80).
    auto& presentation = runtime.presentation(viewId);
    ViewportDimensions const viewport{
        std::max<std::uint32_t>(presentation.paneContentColumns, 1),
        std::max<std::uint32_t>(presentation.paneContentRows, 1)};
    auto navigation = runtime.selection;
    navigation.firstVisualRow = presentation.requestedFirstVisualRow;
    navigation.firstVisualColumn = presentation.requestedFirstVisualColumn;
    navigation.desiredCell = presentation.desiredCell;
    const auto diffFile = runtime.activeDiffFile();
    auto result = ssg::SelectionNavigator{}.apply(runtime.activeText(), navigation,
                                             command, viewport,
                                             arguments, {}, 4,
                                             runtime.wordWrap,
                                             diffFile ? &*diffFile : nullptr);
    if (!result.accepted()) return failure(result.message);
    if (result.delta.replacement) {
        runtime.selection.selections = result.delta.replacement->selections;
        navigation = *result.delta.replacement;
    }
    presentation.requestedFirstVisualRow = navigation.firstVisualRow;
    presentation.requestedFirstVisualColumn =
        navigation.firstVisualColumn;
    presentation.desiredCell = navigation.desiredCell;
    if (auto active = runtime.activeDocumentId()) {
        runtime.historyFor(*active).breakCoalescing();
    }
    // Focus follows the pointer (M8-F): a click-to-caret / drag-select / Alt+click
    // add-caret / Alt+drag add-range acts on the editor, so it moves the
    // authoritative keyboard focus there. Gated on the pointer-driven selection
    // commands; keyboard caret motion is a different SelectionCommand and never
    // reaches here.
    if (command == SelectionCommand::CursorSetPosition ||
        command == SelectionCommand::SelectSetRange ||
        command == SelectionCommand::SelectSetRanges ||
        command == SelectionCommand::SelectAddRange ||
        command == SelectionCommand::SelectWordAtPosition) {
        runtime.interaction.focusEditor();
    }
    runtime.recordNavigation(client, viewId, NavigationClass::User);
    return success();
}

CommandHandlerResult bindEdit(EditorSession::Impl& runtime, ViewId viewId,
                              EditCommand command) {
    if (runtime.activeTabIsLiveDiff()) {
        return failure("edit command cannot mutate a diff document");
    }
    auto const* document = runtime.activeDocument();
    if (document == nullptr) return failure("no active document");
    auto result = EditInterpreter{}.apply(document->snapshot(), runtime.selection.selections,
                                     editSettings(runtime), command);
    if (!result.accepted() || !result.transaction || !result.selections) {
        return failure(result.message);
    }
    return applyTransaction(runtime, viewId, *result.transaction,
                            *result.selections, HistoryEditKind::Other);
}

CommandHandlerResult bindHistory(EditorSession::Impl& runtime, ViewId viewId,
                                 HistoryCommand command) {
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
    runtime.revealPrimaryCaret(viewId);
    (void)runtime.updateTabsFor(*id);
    runtime.refreshSyntax();
    return success();
}

CommandHandlerResult bindClipboard(EditorSession::Impl& runtime, ViewId viewId,
                                   ClipboardCommand command) {
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
        runtime.revealPrimaryCaret(viewId);
        (void)runtime.updateTabsFor(*id);
        runtime.refreshSyntax();
    }
    return success();
}

// Move the primary selection onto the active find match and reveal it so the
// viewport scrolls to follow find navigation (find.next/previous/update_query).
void revealActiveFindMatch(EditorSession::Impl& runtime, ViewId viewId) {
    auto const& state = runtime.findReplace.viewState();
    if (!state.open || !state.activeMatch ||
        *state.activeMatch >= state.matches.size()) {
        return;
    }
    auto const& match = state.matches[*state.activeMatch];
    auto const& text = runtime.activeText();
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
    auto& presentation = runtime.presentation(viewId);
    auto const baseRows = presentation.paneContentRows +
                          presentation.reservedPromptRows;
    auto const revealRows = baseRows > reserved
                                 ? baseRows - reserved
                                 : std::uint32_t{1};
    ViewportDimensions revealViewport{presentation.paneContentColumns,
                                       revealRows};
    auto navigation = runtime.selection;
    navigation.firstVisualRow = presentation.requestedFirstVisualRow;
    navigation.firstVisualColumn = presentation.requestedFirstVisualColumn;
    navigation.desiredCell = presentation.desiredCell;
    const auto diffFile = runtime.activeDiffFile();
    auto result = ssg::SelectionNavigator{}.apply(
        text, navigation, SelectionCommand::ViewRevealCaret,
        revealViewport, {}, {}, 4, runtime.wordWrap,
        diffFile ? &*diffFile : nullptr);
    if (result.accepted() && result.delta.replacement) {
        navigation = *result.delta.replacement;
    }
    presentation.requestedFirstVisualRow = navigation.firstVisualRow;
    presentation.requestedFirstVisualColumn =
        navigation.firstVisualColumn;
    presentation.desiredCell = navigation.desiredCell;
}

// A replace prompt is active when the controller is open in replace mode AND
// the active prompt is the replace prompt.  The replacement-editing and
// destructive replace commands guard on this so a stray prompt-context chord
// from an unrelated (palette/settings) prompt cannot mutate hidden state (an
// open-only guard leaks because find can stay open behind another prompt).
bool replacePromptActive(EditorSession::Impl& runtime) {
    auto const& state = runtime.findReplace.viewState();
    auto const& request = runtime.interaction.prompt().request();
    return state.open && state.replaceMode && request &&
           request->kind == PromptKind::Replace;
}

// A find or replace prompt is the active prompt over an open controller.  The
// toggle/next/previous chords guard on this so a stray prompt-context chord from
// an unrelated (palette/settings) prompt cannot mutate hidden find state (find
// can remain open behind another prompt).
bool findOrReplacePromptActive(EditorSession::Impl& runtime) {
    auto const& state = runtime.findReplace.viewState();
    auto const& request = runtime.interaction.prompt().request();
    return state.open && request &&
           (request->kind == PromptKind::Find ||
            request->kind == PromptKind::Replace);
}

// The find/replace option indicators shared by the find and replace prompts,
// seeded from the current options; project_find_replace_prompt refreshes their
// `checked` state from the authoritative options at snapshot time.
std::vector<PromptToggle> findOptionToggles(EditorSession::Impl& runtime) {
    auto const& options = runtime.findReplace.viewState().options;
    return {{"find.toggle_case", "case", options.caseSensitive, 9},
            {"find.toggle_whole_word", "word", options.wholeWord, 9},
            {"find.toggle_regex", "regex", options.regex, 10}};
}

CommandHandlerResult bindFindReplace(EditorSession::Impl& runtime,
                                     ViewId viewId,
                                     Revision revision,
                                     FindReplaceCommand command,
                                     std::any const& payload) {
    if (runtime.activeTabIsLiveDiff() &&
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
            revealActiveFindMatch(runtime, viewId);
            // Open the find prompt so focus moves to it and the reserved rows
            // display the controller query (projected at snapshot time).
            if (auto opened = runtime.interaction.openPrompt(PromptRequest{
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
            revealActiveFindMatch(runtime, viewId);
            if (auto opened = runtime.interaction.openPrompt(PromptRequest{
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
            revealActiveFindMatch(runtime, viewId);
            // Three-row replace prompt: query (row 0, display-only, seeded from
            // the current find query), replacement (row 1, editable), and the
            // option/match-count row.  The client edits only the replacement.
            if (auto opened = runtime.interaction.openPrompt(PromptRequest{
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
            if (auto const& request = runtime.interaction.prompt().request();
                request && (request->kind == PromptKind::Find ||
                            request->kind == PromptKind::Replace)) {
                (void)runtime.interaction.cancelPrompt();
            }
            return success();
        case FindReplaceCommand::FindNext:
            if (!findOrReplacePromptActive(runtime)) return success();
            runtime.findReplace.next();
            revealActiveFindMatch(runtime, viewId);
            return success();
        case FindReplaceCommand::FindPrevious:
            if (!findOrReplacePromptActive(runtime)) return success();
            runtime.findReplace.previous();
            revealActiveFindMatch(runtime, viewId);
            return success();
        case FindReplaceCommand::FindUpdateQuery: {
            auto const* arguments = payloadAs<FindQueryArguments>(payload);
            if (arguments == nullptr) return failure("find.update_query requires a query payload");
            runtime.findReplace.updateQuery(snapshot, arguments->query, range);
            runtime.findDocumentId = runtime.activeDocumentId();
            revealActiveFindMatch(runtime, viewId);
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
            runtime.clampSelectionsToActiveDocument();
            runtime.refreshSyntax();
            auto tabsResult = runtime.updateTabsFor(*id);
            revealActiveFindMatch(runtime, viewId);
            // If no match remains to reveal (common after replace.all), still
            // reveal the primary caret so a replace with the caret off-screen
            // scrolls into view, per the edits-reveal policy. When a match does
            // remain, reveal_active_find_match already
            // revealed it above the prompt; don't override that.
            auto const& fr = runtime.findReplace.viewState();
            if (!fr.open || !fr.activeMatch ||
                *fr.activeMatch >= fr.matches.size()) {
                runtime.revealPrimaryCaret(viewId);
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
            (void)runtime.refreshTree();
            return success();
        }
    }
    return failure("unknown find/replace command");
}

} // namespace

CommandHandlerResult executeFindReplaceCommand(EditorSession::Impl& runtime,
                                                ViewId viewId,
                                               Revision revision,
                                               FindReplaceCommand command,
                                               std::any const& payload) {
    return bindFindReplace(runtime, viewId, revision, command, payload);
}

void EditorSession::Impl::revealPrimaryCaret(ViewId viewId) {
    // Reveal against the real editor pane cached from the last snapshot: the
    // content rows/columns already exclude any reserved prompt rows, so no prompt
    // adjustment is needed (unlike reveal_active_find_match, which runs while the
    // find prompt is open). The offset is re-clamped in compute_viewport, so a
    // one-frame-stale cache can never place it out of range.
    revealPrimaryCaret(presentation(viewId));
}

void EditorSession::Impl::revealPrimaryCaret(
    ViewPresentationState& view) const {
    ViewportDimensions revealViewport{
        std::max<std::uint32_t>(view.paneContentColumns, 1),
        std::max<std::uint32_t>(view.paneContentRows, 1)};
    auto navigation = selection;
    navigation.firstVisualRow = view.requestedFirstVisualRow;
    navigation.firstVisualColumn = view.requestedFirstVisualColumn;
    navigation.desiredCell = view.desiredCell;
    const auto diffFile = activeDiffFile();
    auto result = ssg::SelectionNavigator{}.apply(
        activeText(), navigation, SelectionCommand::ViewRevealCaret,
        revealViewport, {}, {}, 4, wordWrap,
        diffFile ? &*diffFile : nullptr);
    if (result.accepted() && result.delta.replacement) {
        navigation = *result.delta.replacement;
    }
    view.requestedFirstVisualRow = navigation.firstVisualRow;
    view.requestedFirstVisualColumn = navigation.firstVisualColumn;
    view.desiredCell = navigation.desiredCell;
}

// The text-input commands, declared where they are implemented.
//
// The first component migrated off the static table
//.  Each command's facts and its handler are
// one expression, so the argument type is written once -- in `handler<...>` --
// and the codec, the unwrap and the reference's argument column are all derived
// from it.  There is no row elsewhere to keep in step.
void registerTextInputCommands(CommandCatalog& builder,
                               EditorSession::Impl& runtime) {
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
                        .handler([&runtime, command](CommandContext& context) {
                            return runtime.runTransaction([&] {
                                return bindText(runtime, context.viewId(),
                                                command, {});
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
                        [&runtime](CommandContext& context,
                                   TextInputArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                return bindText(runtime, context.viewId(),
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
void registerHistoryCommands(CommandCatalog& builder,
                             EditorSession::Impl& runtime) {
    auto declare = [&](std::string id, std::string summary,
                       HistoryCommand command) {
        builder.add(CommandSpecBuilder{std::move(id)}
                        .owner("undo-redo-history")
                        .label(summary)
                        .summary(std::move(summary))
                        .mutates()
                        .lua()
                        .handler([&runtime, command](CommandContext& context) {
                            return runtime.runTransaction([&] {
                                return bindHistory(runtime, context.viewId(),
                                                   command);
                            });
                        }));
    };
    declare("edit.undo", "Undo", HistoryCommand::Undo);
    declare("edit.redo", "Redo", HistoryCommand::Redo);
}

// The clipboard register: copy, cut and paste over the current selections.
void registerClipboardCommands(CommandCatalog& builder,
                               EditorSession::Impl& runtime) {
    auto declare = [&](std::string id, std::string summary,
                       ClipboardCommand command) {
        builder.add(CommandSpecBuilder{std::move(id)}
                        .owner("clipboard-register")
                        .label(summary)
                        .summary(std::move(summary))
                        .mutates()
                        .lua()
                        .handler([&runtime, command](CommandContext& context) {
                            return runtime.runTransaction([&] {
                                return bindClipboard(runtime, context.viewId(),
                                                     command);
                            });
                        }));
    };
    declare("clipboard.copy", "Copy", ClipboardCommand::Copy);
    declare("clipboard.cut", "Cut", ClipboardCommand::Cut);
    declare("clipboard.paste", "Paste", ClipboardCommand::Paste);
}

// Whole-line and whole-selection edits.  None takes an argument: each acts on
// wherever the selections already are.
void registerEditSuiteCommands(CommandCatalog& builder,
                               EditorSession::Impl& runtime) {
    auto declare = [&](std::string id, std::string label, std::string summary,
                       EditCommand command) {
        auto built = CommandSpecBuilder{std::move(id)}
                         .owner("edit-command-suite")
                         .summary(std::move(summary))
                         .mutates()
                         .lua()
                         .handler([&runtime, command](CommandContext& context) {
                             return runtime.runTransaction([&] {
                                 return bindEdit(runtime, context.viewId(),
                                                 command);
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
void registerFindReplaceCommands(CommandCatalog& builder,
                                 EditorSession::Impl& runtime) {
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
                                     runtime, context.viewId(),
                                     context.revision(), command, {});
                             });
                         });
        if (!label.empty()) built.label(std::move(label));
        builder.add(std::move(built));
    };

    bare("find.open", "Find", "Find", FindReplaceCommand::FindOpen);
    bare("find.word_under_cursor", "Find Word Under Cursor",
         "Find Word Under Cursor", FindReplaceCommand::FindWordUnderCursor);
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
                                        runtime, context.viewId(),
                                        context.revision(), command,
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

// Moving the caret and changing the selection.
//
// All thirty-six take the same optional argument -- where to move to, absent
// meaning "the usual step from here" -- and differ only in which motion they
// perform.  They are registered by walking the descriptor table that already
// pairs each id with its motion, rather than restating that list: the summary
// is the id's last segment in words, which is the rule every one of them
// follows.
void registerSelectionCommands(CommandCatalog& builder,
                               EditorSession::Impl& runtime) {
    auto summaryOf = [](std::string_view id) {
        auto const segment = id.substr(id.find('.') + 1);
        std::string words;
        bool wordStart = true;
        for (char raw : segment) {
            if (raw == '_') {
                words += ' ';
                wordStart = true;
                continue;
            }
            auto const ch = static_cast<unsigned char>(raw);
            words += wordStart ? static_cast<char>(std::toupper(ch)) : raw;
            wordStart = false;
        }
        return words;
    };

    // Held in a local: descriptors() returns a reference into the command set,
    // and a range-for over a temporary's member would leave it dangling before
    // the first iteration (C++20 does not extend the temporary's lifetime).
    auto const motions = selectionNavigationCommandSet();
    for (auto const& descriptor : motions.descriptors()) {
        auto const command = descriptor.command;
        if (command == SelectionCommand::ViewRevealCaret ||
            command == SelectionCommand::ViewCenterCaret) {
            builder.add(
                CommandSpecBuilder{std::string{descriptor.id}}
                    .owner("selection-navigation")
                    .summary(summaryOf(descriptor.id))
                    .viewAction()
                    .lua()
                    .optionalHandler<SelectionCommandArguments>(
                        [command](
                            CommandContext&,
                            std::optional<SelectionCommandArguments> const&) {
                            return CommandHandlerResult::requireView(
                                command == SelectionCommand::ViewRevealCaret
                                    ? ViewAction{RevealSelection{}}
                                    : ViewAction{CenterSelection{}});
                        }));
            continue;
        }
        builder.add(
            CommandSpecBuilder{std::string{descriptor.id}}
                .owner("selection-navigation")
                .summary(summaryOf(descriptor.id))
                .mutates()
                .lua()
                .optionalHandler<SelectionCommandArguments>(
                    [&runtime, command](
                        CommandContext& context,
                        std::optional<SelectionCommandArguments> const&
                            arguments) {
                        return runtime.runTransaction([&] {
                            return bindSelection(
                                runtime, context.viewId(),
                                context.principal().clientId(),
                                command,
                                arguments ? std::any{*arguments} : std::any{});
                        });
                    }));
    }
}

void bindRuntimeEditing(CommandCatalog& builder, EditorSession::Impl& runtime) {
    registerTextInputCommands(builder, runtime);
    registerSelectionCommands(builder, runtime);
    registerEditSuiteCommands(builder, runtime);
    registerFindReplaceCommands(builder, runtime);
    registerHistoryCommands(builder, runtime);
    registerClipboardCommands(builder, runtime);
}

} // namespace ssg
