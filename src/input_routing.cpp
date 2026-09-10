#include <ssg/InputRouting.h>
#include <ssg/InputCommandNames.h>

#include <algorithm>
#include <array>
#include <utility>

namespace ssg {
namespace {

using namespace input_command_names;

// An engaged, empty gesture assigns "no gesture" on acceptance.
std::optional<DocumentPointerGesture> clearGesture() {
    return DocumentPointerGesture{};
}

RoutedInput unhandled() {
    return {RouteUnhandled{}, std::nullopt, false};
}

RoutedInput rejected(std::string message,
                     bool clearGestureOnRejection = false) {
    return {RouteRejected{std::move(message)}, std::nullopt,
            clearGestureOnRejection};
}

RoutedInput accepted(std::optional<DocumentPointerGesture> gesture = std::nullopt) {
    return {RouteAccepted{}, std::move(gesture), false};
}

RoutedInput accepted(EditorMutation mutation) {
    return {RouteAccepted{std::move(mutation)}, std::nullopt, false};
}

RoutedInput accepted(EditorMutation mutation,
                     std::optional<DocumentPointerGesture> gesture,
                     bool clearGestureOnRejection) {
    return {RouteAccepted{std::move(mutation)}, std::move(gesture),
            clearGestureOnRejection};
}

RoutedInput dispatch(CommandName command, std::any payload = {},
                     std::optional<DocumentPointerGesture> gesture = std::nullopt,
                     bool clearGestureOnRejection = false) {
    return {RouteDispatch{ClientCommand{std::move(command), std::move(payload)}},
            std::move(gesture), clearGestureOnRejection};
}
RoutedInput clientOwned(ClientOwnedInputKind kind, std::string text = {}) {
    return {RouteClientOwned{ClientOwnedInput{kind, std::move(text)}},
            std::nullopt, false};
}

RoutedInput viewAction(ViewAction action) {
    return {RouteViewAction{std::move(action)}, std::nullopt, false};
}

std::optional<SelectionSet> navigateSelections(
    InputRoutingSnapshot const& snapshot, SelectionCommand command,
    SelectionCommandArguments arguments, std::string& message) {
    auto const before = SelectionViewState{
        snapshot.selections.get(), 0, 0, std::nullopt};
    auto result =
        navigateSelection(snapshot.activeText, before, command, {1, 1},
                          std::move(arguments));
    if (!result.accepted()) {
        message = std::move(result.message);
        return std::nullopt;
    }
    return result.delta.replacement
               ? std::optional<SelectionSet>{
                     result.delta.replacement->selections}
               : std::optional<SelectionSet>{before.selections};
}

RoutedInput selectionMutation(
    InputRoutingSnapshot const& snapshot, SelectionCommand command,
    SelectionCommandArguments arguments,
    std::optional<DocumentPointerGesture> gesture = std::nullopt,
    bool clearGestureOnRejection = false, bool focusEditor = false) {
    if (!snapshot.activeDocument) {
        return rejected("document pointer target is not actionable",
                        clearGestureOnRejection);
    }
    std::string message;
    auto selections =
        navigateSelections(snapshot, command, std::move(arguments), message);
    if (!selections) {
        return rejected(std::move(message), clearGestureOnRejection);
    }
    return accepted(ApplySelections{*snapshot.activeDocument,
                                    std::move(*selections), focusEditor},
                    std::move(gesture), clearGestureOnRejection);
}

bool isPrimaryPress(InputPointerPhase phase, InputPointerButton button) {
    return phase == InputPointerPhase::Press &&
           button == InputPointerButton::Primary;
}

RoutedInput routeInput(InputRoutingSnapshot const& snapshot,
                       ClientKeyInput const& input) {
    auto const& routing = snapshot.prompt;
    const bool searchPanel =
        routing.prompt == ActivePrompt::None &&
        routing.focus == FocusTarget::Panel &&
        snapshot.activeTreeProvider == TreeProviderKind::Search;
    if (searchPanel && !input.committedText.empty()) {
        return accepted(SearchQueryChange{
            SearchQueryChange::Kind::Append, input.committedText});
    }
    auto routeTextEdit = [&](PromptTextEdit edit) -> RoutedInput {
        auto const route = routePromptTextEdit(routing, edit);
        if (route.kind == PromptTextRoute::Kind::Dispatch) {
            return dispatch(route.command, route.payload);
        }
        if (route.kind == PromptTextRoute::Kind::UpdatePromptValue) {
            return accepted(EditorMutation{route.promptValue});
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
        return unhandled();
    };

    if (input.stroke.code != KeyCode::None) {
        auto const resolved = snapshot.keymap.get().resolve(
            std::array{CompiledKeymap::compile(input.stroke)}, routing.focus);
        if (resolved.kind == KeymapMatchKind::Resolved) {
            auto const& command = resolved.command;
            if (command == kClipboardPaste) {
                if (routing.focus == FocusTarget::Panel && !searchPanel) {
                    return unhandled();
                }
                const auto kind =
                    routing.prompt != ActivePrompt::None || searchPanel
                        ? ClientOwnedInputKind::SystemClipboardPasteIntoText
                        : ClientOwnedInputKind::SystemClipboardPasteIntoEditor;
                return clientOwned(kind, std::string{snapshot.clipboardText});
            }
            if (searchPanel) {
                if (snapshot.searchEditing &&
                    command == "tree.select_next") {
                    return accepted(SearchQueryChange{
                        SearchQueryChange::Kind::MoveFirst, {}});
                }
                if (snapshot.searchEditing &&
                    command == "tree.select_previous") {
                    return accepted(SearchQueryChange{
                        SearchQueryChange::Kind::MoveLast, {}});
                }
                if (snapshot.searchEditing && command == "tree.activate") {
                    return accepted(SearchQueryChange{
                        SearchQueryChange::Kind::Submit, {}});
                }
            }
            switch (routing.prompt) {
            case ActivePrompt::Palette:
                if (command == kPromptSubmit) {
                    return clientOwned(ClientOwnedInputKind::Submit);
                }
                if (command == kPromptNext || command == kPaletteNext) {
                    return clientOwned(ClientOwnedInputKind::SelectNext);
                }
                if (command == kPromptPrevious ||
                    command == kPalettePrevious) {
                    return clientOwned(ClientOwnedInputKind::SelectPrevious);
                }
                if (command == kPromptCancel) {
                    return dispatch(kPaletteClose);
                }
                return dispatch(command);
            case ActivePrompt::Find:
            case ActivePrompt::Replace:
            case ActivePrompt::TextPrompt:
                return dispatch(command);
            case ActivePrompt::None:
                return dispatch(command);
            }
        }
        if (input.stroke.code == KeyCode::Backspace) {
            if (searchPanel) {
                return accepted(SearchQueryChange{
                    SearchQueryChange::Kind::DeleteGraphemeBack, {}});
            }
            return routeTextEdit(
                {input.stroke.mod ? PromptTextEdit::Kind::DeleteWordBack
                                  : PromptTextEdit::Kind::DeleteGraphemeBack,
                 {}});
        }
    }
    if (!input.committedText.empty()) {
        if (routing.prompt != ActivePrompt::None) {
            return routeTextEdit(
                {PromptTextEdit::Kind::Append, input.committedText});
        }
        if (routing.focus == FocusTarget::Editor) {
            return accepted(ApplyTextInput{
                TextInputCommand::Insert,
                TextInputArguments{input.committedText}});
        }
    }
    return unhandled();
}

RoutedInput routeInput(InputRoutingSnapshot const&,
                       ScrollLinesInput const& input) {
    if (input.action.rows == 0) {
        return rejected("line-scroll input must move at least one row");
    }
    switch (input.action.target) {
    case ScrollTarget::Document:
    case ScrollTarget::Tree:
        return viewAction(input.action);
    }
    return rejected("line-scroll target is invalid");
}

RoutedInput routeInput(InputRoutingSnapshot const&,
                       ScrollFractionInput const& input) {
    if (input.action.denominator == 0 ||
        input.action.numerator > input.action.denominator) {
        return rejected("fraction-scroll input is invalid");
    }
    switch (input.action.target) {
    case ScrollTarget::Document:
    case ScrollTarget::Tree:
        return viewAction(input.action);
    }
    return rejected("fraction-scroll target is invalid");
}

RoutedInput routeInput(InputRoutingSnapshot const& snapshot,
                       DocumentPointerInput const& input) {
    if (input.button != InputPointerButton::Primary) {
        return unhandled();
    }
    if (input.phase == InputPointerPhase::Cancel) {
        return accepted(clearGesture());
    }

    const auto resolvePosition = [&]() -> std::optional<DocumentPosition> {
        if (!input.position) return std::nullopt;
        return resolveSelectionPosition(snapshot.activeText, *input.position);
    };
    if (input.phase == InputPointerPhase::Press) {
        auto position = resolvePosition();
        if (!position || !snapshot.activeDocument) {
            return rejected("document pointer target is not actionable", true);
        }
        if (input.selectWord) {
            return selectionMutation(
                snapshot, SelectionCommand::SelectWordAtPosition,
                SelectionCommandArguments{*position, std::nullopt},
                clearGesture(), true, true);
        }
        auto const& items = snapshot.selections.get().items();
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
                return selectionMutation(
                    snapshot, SelectionCommand::SelectSetRanges,
                    SelectionCommandArguments{
                        std::nullopt, std::nullopt, std::move(baseline)},
                    clearGesture(), true, true);
            }
        }
        DocumentPointerGesture gesture;
        gesture.begin(*snapshot.activeDocument, snapshot.documentRevision,
                      *position, input.additive, std::move(baseline));
        return selectionMutation(
            snapshot,
            input.additive ? SelectionCommand::SelectAddRange
                           : SelectionCommand::CursorSetPosition,
            input.additive
                ? SelectionCommandArguments{
                      std::nullopt, Selection{*position, *position}}
                : SelectionCommandArguments{*position, std::nullopt},
            std::move(gesture), true, true);
    }
    if (!snapshot.gesture.has_value()) {
        return unhandled();
    }

    auto gesture = snapshot.gesture;
    auto const target = gesture.validateTarget(
        snapshot.activeDocument, snapshot.documentRevision);
    if (target == DocumentPointerTargetState::DocumentChanged) {
        return rejected("document pointer gesture target changed", true);
    }
    if (target == DocumentPointerTargetState::RevisionChanged) {
        return rejected("document changed during pointer gesture", true);
    }
    auto position = resolvePosition();
    if (input.edge != DocumentPointerEdge::None) {
        return viewAction(ContinuePointerEdge{input.edge});
    }
    if (!position && input.phase == InputPointerPhase::Move) {
        return rejected("document pointer target is not actionable");
    }
    if (!position) {
        return accepted(input.phase == InputPointerPhase::Release
                            ? clearGesture()
                            : std::nullopt);
    }

    auto const arguments = gesture.selectionThrough(*position);
    auto const command = gesture.additive() ? SelectionCommand::SelectSetRanges
                                            : SelectionCommand::SelectSetRange;
    gesture.moveTo(*position);
    return selectionMutation(
        snapshot, command, arguments,
        input.phase == InputPointerPhase::Release
            ? clearGesture()
            : std::optional<DocumentPointerGesture>{std::move(gesture)},
        true, true);
}

RoutedInput routeTransition(InputRoutingSnapshot const& snapshot,
                            PauseFollowTransition const&) {
    if (snapshot.followMode == FollowMode::Paused) {
        return unhandled();
    }
    return dispatch(kFollowEditsPause);
}

RoutedInput routeTransition(InputRoutingSnapshot const& snapshot,
                            PaneFocusTransition const& transition) {
    if (std::find(snapshot.panes.begin(), snapshot.panes.end(),
                  transition.pane) == snapshot.panes.end()) {
        return rejected("editor pane focus target is not in this attachment");
    }
    return accepted(FocusPane{transition.pane});
}

RoutedInput routeTransition(InputRoutingSnapshot const& snapshot,
                            SelectionTransition const& transition) {
    if (!snapshot.activeTab || *snapshot.activeTab != transition.activeTab) {
        return rejected("resolved selection active tab is stale");
    }
    auto documentRevision = snapshot.documentRevision;
    if (snapshot.activeTabKind == TabKind::LiveDiff) {
        documentRevision = snapshot.diffRevision;
    }
    if (!snapshot.activeDocument ||
        documentRevision != transition.documentRevision) {
        return rejected("resolved selection document is stale");
    }
    if (transition.selections.empty()) {
        return rejected("resolved selection must not be empty");
    }

    std::vector<Selection> selections;
    selections.reserve(transition.selections.size());
    for (auto const& range : transition.selections) {
        auto anchor = resolveSelectionPosition(snapshot.activeText, range.anchor);
        auto active = resolveSelectionPosition(snapshot.activeText, range.active);
        if (!anchor || !active) {
            return rejected("resolved selection range is invalid");
        }
        selections.push_back({*anchor, *active});
    }
    return accepted(ApplySelections{
        *snapshot.activeDocument, SelectionSet{std::move(selections)}});
}

RoutedInput routeTransition(InputRoutingSnapshot const& snapshot,
                            PointerSelectionTransition const& transition) {
    if (!snapshot.gesture.has_value()) {
        return rejected("pointer selection has no active gesture");
    }
    return routeInput(
        snapshot,
        DocumentPointerInput{
            transition.position, false, false, InputPointerButton::Primary,
            InputPointerPhase::Move, DocumentPointerEdge::None});
}

RoutedInput routeInput(InputRoutingSnapshot const& snapshot,
                       ViewTransitionInput const& input) {
    return std::visit(
        [&](auto const& transition) {
            return routeTransition(snapshot, transition);
        },
        input.transition);
}

RoutedInput routeInput(InputRoutingSnapshot const&,
                       TabPointerInput const& input) {
    if (input.phase != InputPointerPhase::Press) {
        return unhandled();
    }
    switch (input.button) {
    case InputPointerButton::Primary:
        return accepted(ActivateTab{input.tabId});
    case InputPointerButton::Auxiliary:
        return accepted(CloseTab{input.tabId});
    case InputPointerButton::Secondary:
        return unhandled();
    }
    return unhandled();
}

RoutedInput routeInput(InputRoutingSnapshot const&,
                       TreePointerInput const& input) {
    if (!isPrimaryPress(input.phase, input.button)) {
        return unhandled();
    }
    return {ActivateTreeNode{input.nodeId}};
}

RoutedInput routeInput(InputRoutingSnapshot const&,
                       SearchQueryPointerInput const& input) {
    if (!isPrimaryPress(input.phase, input.button)) {
        return unhandled();
    }
    return accepted(
        SearchQueryChange{SearchQueryChange::Kind::Focus, {}});
}

RoutedInput routeInput(InputRoutingSnapshot const&,
                       PickerPointerInput const& input) {
    if (!isPrimaryPress(input.phase, input.button)) {
        return unhandled();
    }
    return dispatch(
        kPickerSubmit,
        PickerSubmitArguments{input.activation, input.candidateId});
}

RoutedInput routeInput(InputRoutingSnapshot const&,
                       ExternalActionPointerInput const& input) {
    if (!isPrimaryPress(input.phase, input.button)) {
        return unhandled();
    }
    return {InvokeExternalAction{input.invocation}, std::nullopt, false};
}

RoutedInput routeInput(InputRoutingSnapshot const&,
                       UiNodePointerInput const& input) {
    return {ActivateUiNode{input.nodeId}, std::nullopt, false};
}

RoutedInput routeInput(InputRoutingSnapshot const& snapshot,
                       NoticeActionPointerInput const& input) {
    if (!isPrimaryPress(input.phase, input.button)) {
        return unhandled();
    }
    if (!snapshot.noticeActions) {
        return rejected("notice action target is not present");
    }
    for (auto const& action : *snapshot.noticeActions) {
        if (action.id == input.actionId) {
            return dispatch(action.command);
        }
    }
    return rejected("notice action target is not actionable");
}

RoutedInput routeInput(InputRoutingSnapshot const&,
                       UpdatePromptValueInput const& input) {
    return accepted(EditorMutation{PromptValueArguments{input.index, input.value}});
}

}  // namespace

RoutedInput routeInput(InputRoutingSnapshot const& snapshot,
                       ClientInput const& input) {
    return std::visit(
        [&](auto const& semantic) {
            return routeInput(snapshot, semantic);
        },
        input);
}

}  // namespace ssg
