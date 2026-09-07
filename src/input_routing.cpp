#include <ssg/Editor.h>

#include <array>

namespace ssg {
namespace {

ClientInputResult unhandled() {
    return {ClientInputOutcome::Unhandled, std::nullopt, std::nullopt};
}

ClientInputResult rejected(std::string message) {
    return {
        ClientInputOutcome::Rejected, std::nullopt,
        CommandResult{CommandError::HandlerFailed, std::move(message), {}}};
}

ClientInputResult handled() {
    return {ClientInputOutcome::Dispatched, std::nullopt,
            CommandResult{CommandError::None, {}, {}}};
}

ClientInputResult dispatchInput(Editor& editor, CommandName command,
                               std::any payload = {}) {
    auto result =
        editor.dispatchLocked({std::move(command), std::move(payload)});
    const auto activation =
        result.accepted() ? editor.screen.openPickerActivation() : std::nullopt;
    const auto outcome =
        !result.accepted() ? ClientInputOutcome::Rejected
        : result.viewAction ? ClientInputOutcome::ViewOwned
                           : ClientInputOutcome::Dispatched;
    return {outcome, std::nullopt, std::move(result), activation};
}

ClientInputResult clientOwned(ClientOwnedInputKind kind, std::string text = {}) {
    return {ClientInputOutcome::ClientOwned,
            ClientOwnedInput{kind, std::move(text)}, std::nullopt};
}

bool isPrimaryPress(InputPointerPhase phase, InputPointerButton button) {
    return phase == InputPointerPhase::Press &&
           button == InputPointerButton::Primary;
}

ClientInputResult routeInput(Editor& editor, ClientKeyInput const& input) {
    PromptRoutingState routing;
    routing.focus = editor.screen.effectiveFocus();
    auto const promptStatus = editor.promptStatusView();
    if (promptStatus.activeKind == PromptKind::Palette) {
        routing.prompt = ActivePrompt::Palette;
    } else if (auto const view = editor.resolvedPromptControls()) {
        const auto kind = editor.screen.prompt().request()->kind;
        switch (kind) {
        case PromptKind::Find:
            routing.prompt = ActivePrompt::Find;
            break;
        case PromptKind::Replace:
            routing.prompt = ActivePrompt::Replace;
            break;
        case PromptKind::Path:
        case PromptKind::Settings:
        case PromptKind::CommandArgument:
            routing.prompt = ActivePrompt::TextPrompt;
            break;
        case PromptKind::Palette:
            break;
        }
        routing.activeInput = view->activeInput;
        std::size_t inputIndex = 0;
        for (auto const& control : view->controls) {
            if (control.kind != PromptControlKind::Input) continue;
            if (inputIndex++ == view->activeInput) {
                routing.currentValue = control.value;
                break;
            }
        }
    }

    auto routeTextEdit = [&](PromptTextEdit edit) -> ClientInputResult {
        auto const route = routePromptTextEdit(routing, edit);
        if (route.kind == PromptTextRoute::Kind::Dispatch) {
            return dispatchInput(editor, route.command, route.payload);
        }
        if (routing.prompt == ActivePrompt::Palette) {
            switch (edit.kind) {
            case PromptTextEdit::Kind::Append:
                return clientOwned(ClientOwnedInputKind::AppendText,
                                   route.appendText);
            case PromptTextEdit::Kind::DeleteGraphemeBack:
                return clientOwned(
                    ClientOwnedInputKind::DeleteGraphemeBackward);
            case PromptTextEdit::Kind::DeleteWordBack:
                return clientOwned(ClientOwnedInputKind::DeleteWordBackward);
            }
        }
        return {ClientInputOutcome::Unhandled, std::nullopt, std::nullopt};
    };

    if (input.stroke.code != KeyCode::None) {
        auto const resolved = editor.resolveInputKeymap().resolve(
            std::array{CompiledKeymap::compile(input.stroke)}, routing.focus);
        if (resolved.kind == KeymapMatchKind::Resolved) {
            auto const& command = resolved.command;
            if (routing.prompt == ActivePrompt::Palette) {
                if (command == "prompt.submit") {
                    return clientOwned(ClientOwnedInputKind::Submit);
                }
                if (command == "prompt.next" || command == "palette.next") {
                    return clientOwned(ClientOwnedInputKind::SelectNext);
                }
                if (command == "prompt.previous" ||
                    command == "palette.previous") {
                    return clientOwned(ClientOwnedInputKind::SelectPrevious);
                }
                if (command == "prompt.cancel") {
                    return dispatchInput(editor, "palette.close");
                }
            }
            if (routing.focus == FocusTarget::Prompt &&
                command == "clipboard.paste") {
                auto const text = editor.clipboard.viewState().plainText;
                if (text.empty()) {
                    return {ClientInputOutcome::Unhandled, std::nullopt,
                            std::nullopt};
                }
                return routeTextEdit(
                    {PromptTextEdit::Kind::Append, std::move(text)});
            }
            return dispatchInput(editor, command);
        }
        if (input.stroke.code == KeyCode::Backspace) {
            return routeTextEdit(
                {input.stroke.alt ? PromptTextEdit::Kind::DeleteWordBack
                                  : PromptTextEdit::Kind::DeleteGraphemeBack,
                 {}});
        }
    }
    if (!input.committedText.empty()) {
        return routeTextEdit(
            {PromptTextEdit::Kind::Append, input.committedText});
    }
    return {ClientInputOutcome::Unhandled, std::nullopt, std::nullopt};
}

ClientInputResult routeInput(Editor& editor, ScrollLinesInput const& input) {
    if (input.action.rows == 0) {
        return rejected("line-scroll input must move at least one row");
    }
    switch (input.action.target) {
    case ScrollTarget::Document:
        return dispatchInput(
            editor, "view.scroll_lines",
            ScrollLinesArguments{input.action.rows});
    case ScrollTarget::Tree:
        return dispatchInput(editor, "tree.scroll",
                             ScrollLinesArguments{input.action.rows});
    }
    return rejected("line-scroll target is invalid");
}

ClientInputResult routeInput(Editor& editor, ScrollFractionInput const& input) {
    if (input.action.denominator == 0 ||
        input.action.numerator > input.action.denominator) {
        return rejected("fraction-scroll input is invalid");
    }
    const auto fraction = ScrollFractionArguments{
        input.action.numerator, input.action.denominator};
    switch (input.action.target) {
    case ScrollTarget::Document:
        return dispatchInput(editor, "view.scroll_to_fraction", fraction);
    case ScrollTarget::Tree:
        return dispatchInput(editor, "tree.scroll_to_fraction", fraction);
    }
    return rejected("fraction-scroll target is invalid");
}

ClientInputResult routeInput(Editor& editor,
                             DocumentPointerInput const& input) {
    if (input.button != InputPointerButton::Primary) {
        return unhandled();
    }
    if (input.phase == InputPointerPhase::Press ||
        input.phase == InputPointerPhase::Cancel) {
        editor.documentPointerGesture.reset();
    }
    if (input.phase == InputPointerPhase::Cancel) {
        return handled();
    }

    const auto resolvePosition = [&]() -> std::optional<DocumentPosition> {
        if (!input.position) return std::nullopt;
        return resolveSelectionPosition(editor.activeText(), *input.position);
    };
    if (input.phase == InputPointerPhase::Press) {
        auto position = resolvePosition();
        auto documentId = editor.activeDocumentId();
        if (!position || !documentId) {
            return rejected("document pointer target is not actionable");
        }
        if (input.selectWord) {
            return dispatchInput(
                editor, "select.word_at_position",
                SelectionCommandArguments{*position, std::nullopt});
        }
        auto const& items = editor.selection.selections.items();
        std::vector<Selection> baseline{items.begin(), items.end()};
        if (input.additive && baseline.size() > 1) {
            auto const hit = std::find_if(
                baseline.begin(), baseline.end(),
                [&](Selection const& selection) {
                    auto const offset = position->byteOffset.value();
                    auto const lower = selection.lower().byteOffset.value();
                    auto const upper = selection.upper().byteOffset.value();
                    return lower == upper ? offset == lower
                                          : lower <= offset && offset < upper;
                });
            if (hit != baseline.end()) {
                baseline.erase(hit);
                return dispatchInput(
                    editor, "select.set_ranges",
                    SelectionCommandArguments{
                        std::nullopt, std::nullopt, std::move(baseline)});
            }
        }
        editor.documentPointerGesture = Editor::DocumentPointerGesture{
            *documentId,
            editor.activeDocument()->revision(),
            *position,
            *position,
            input.additive,
            baseline};
        auto result =
            input.additive
                ? dispatchInput(
                      editor, "select.add_range",
                      SelectionCommandArguments{
                          std::nullopt, Selection{*position, *position}})
                : dispatchInput(
                      editor, "cursor.set_position",
                      SelectionCommandArguments{*position, std::nullopt});
        if (!result.command || !result.command->accepted()) {
            editor.documentPointerGesture.reset();
        }
        return result;
    }
    if (!editor.documentPointerGesture) {
        return unhandled();
    }

    auto& gesture = *editor.documentPointerGesture;
    if (editor.activeDocumentId() !=
        std::optional<FileDocumentId>{gesture.documentId}) {
        editor.documentPointerGesture.reset();
        return rejected("document pointer gesture target changed");
    }
    if (editor.activeDocument()->revision() != gesture.documentRevision) {
        editor.documentPointerGesture.reset();
        return rejected("document changed during pointer gesture");
    }
    auto position = resolvePosition();
    if (input.edge != DocumentPointerEdge::None) {
        auto result = CommandResult{CommandError::None, {}};
        result.viewAction = ContinuePointerEdge{input.edge};
        return {ClientInputOutcome::ViewOwned, std::nullopt,
                std::move(result)};
    }
    if (!position && input.phase == InputPointerPhase::Move) {
        return rejected("document pointer target is not actionable");
    }
    ClientInputResult result = handled();
    if (position) {
        if (gesture.additive) {
            auto ranges = gesture.baseline;
            ranges.push_back(Selection{gesture.anchor, *position});
            result = dispatchInput(
                editor, "select.set_ranges",
                SelectionCommandArguments{
                    std::nullopt, std::nullopt, std::move(ranges)});
        } else {
            result = dispatchInput(
                editor, "select.set_range",
                SelectionCommandArguments{
                    std::nullopt, Selection{gesture.anchor, *position}});
        }
        if (result.command && result.command->accepted()) {
            gesture.active = *position;
        }
    }
    if (input.phase == InputPointerPhase::Release) {
        editor.documentPointerGesture.reset();
    }
    return result;
}

ClientInputResult routeTransition(Editor& editor,
                                  PauseFollowTransition const&) {
    if (editor.follow.viewState().mode == FollowMode::Paused) {
        return unhandled();
    }
    return dispatchInput(editor, "follow_edits.pause");
}

ClientInputResult routeTransition(Editor& editor,
                                  PaneFocusTransition const& transition) {
    if (!editor.focusPane(transition.pane)) {
        return rejected("pane focus target is not in this attachment");
    }
    if (editor.screen.effectiveFocus() != FocusTarget::Editor) {
        editor.screen.focusEditor();
    }
    editor.recordNavigation(NavigationClass::User);
    return handled();
}

ClientInputResult routeTransition(Editor& editor,
                                  SelectionTransition const& transition) {
    const auto* tab = editor.activeTabState();
    const auto* document = editor.activeDocument();
    if (tab == nullptr || tab->id != transition.activeTab) {
        return rejected("resolved selection active tab is stale");
    }
    auto documentRevision = document ? document->revision() : 0;
    if (const auto* activeTab = editor.activeTabState();
        activeTab && activeTab->kind == TabKind::LiveDiff) {
        documentRevision = editor.diff.viewState().revision;
    }
    if (document == nullptr ||
        documentRevision != transition.documentRevision) {
        return rejected("resolved selection document is stale");
    }
    if (transition.selections.empty()) {
        return rejected("resolved selection must not be empty");
    }

    std::vector<Selection> selections;
    selections.reserve(transition.selections.size());
    const auto& text = editor.activeText();
    for (const auto& range : transition.selections) {
        auto anchor = resolveSelectionPosition(text, range.anchor);
        auto active = resolveSelectionPosition(text, range.active);
        if (!anchor || !active) {
            return rejected("resolved selection range is invalid");
        }
        selections.push_back({*anchor, *active});
    }
    editor.selection.selections = SelectionSet{std::move(selections)};
    if (const auto documentId = editor.activeDocumentId()) {
        editor.historyFor(*documentId).breakCoalescing();
    }
    editor.recordNavigation(NavigationClass::User);
    return handled();
}

ClientInputResult routeTransition(
    Editor& editor, PointerSelectionTransition const& transition) {
    if (!editor.documentPointerGesture) {
        return rejected("pointer selection has no active gesture");
    }
    return routeInput(
        editor,
        DocumentPointerInput{
            transition.position, false, false, InputPointerButton::Primary,
            InputPointerPhase::Move, DocumentPointerEdge::None});
}

ClientInputResult routeInput(Editor& editor,
                             ViewTransitionInput const& input) {
    return std::visit(
        [&](auto const& transition) {
            return routeTransition(editor, transition);
        },
        input.transition);
}

ClientInputResult routeInput(Editor& editor, TabPointerInput const& input) {
    if (input.phase != InputPointerPhase::Press) {
        return unhandled();
    }
    switch (input.button) {
    case InputPointerButton::Primary:
        return dispatchInput(editor, "tab.activate", input.tabId);
    case InputPointerButton::Auxiliary:
        return dispatchInput(editor, "tab.close", input.tabId);
    case InputPointerButton::Secondary:
        return unhandled();
    }
    return unhandled();
}

ClientInputResult routeInput(Editor& editor, TreePointerInput const& input) {
    if (!isPrimaryPress(input.phase, input.button)) {
        return unhandled();
    }
    return dispatchInput(editor, "tree.activate_node",
                         TreeSelectArguments{input.nodeId});
}

ClientInputResult routeInput(Editor& editor, PickerPointerInput const& input) {
    if (!isPrimaryPress(input.phase, input.button)) {
        return unhandled();
    }
    return dispatchInput(
        editor, "picker.submit",
        PickerSubmitArguments{input.activation, input.candidateId});
}

ClientInputResult routeInput(Editor& editor,
                             ExternalActionPointerInput const& input) {
    if (!isPrimaryPress(input.phase, input.button)) {
        return unhandled();
    }
    return dispatchInput(editor, "external.invoke_action", input.invocation);
}

ClientInputResult routeInput(Editor& editor,
                             NoticeActionPointerInput const& input) {
    if (!isPrimaryPress(input.phase, input.button)) {
        return unhandled();
    }
    const auto notice = editor.noticeView();
    if (!notice) {
        return rejected("notice action target is not present");
    }
    for (const auto& action : notice->actions) {
        if (action.id == input.actionId) {
            return dispatchInput(editor, action.commandId);
        }
    }
    return rejected("notice action target is not actionable");
}

} // namespace

ClientInputResult inputLocked(Editor& editor, ClientInput const& input) {
    return std::visit(
        [&](auto const& semantic) {
            return routeInput(editor, semantic);
        },
        input);
}

} // namespace ssg
