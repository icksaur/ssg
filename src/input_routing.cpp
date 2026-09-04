#include <ssg/editor_session_internal.h>
#include <ssg/prompt_resolution.h>

#include <array>

namespace ssg {
namespace {

ClientInputResult inputKeyLocked(EditorSession::Impl& impl_,
                                 ClientKeyInput const& input) {
    auto dispatchInput = [&](CommandName command,
                             std::any payload = {}) -> ClientInputResult {
        auto result = impl_.dispatchLocked({std::move(command), std::move(payload)});
        const auto activation = result.accepted()
                                    ? impl_.interaction.openPickerActivation()
                                    : std::nullopt;
        const auto outcome =
            !result.accepted()
                ? ClientInputOutcome::Rejected
                : result.viewAction
                      ? ClientInputOutcome::ViewOwned
                      : ClientInputOutcome::Dispatched;
        return {outcome, std::nullopt, std::move(result), activation};
    };
    auto clientOwned = [](ClientOwnedInputKind kind,
                          std::string text = {}) -> ClientInputResult {
        return {ClientInputOutcome::ClientOwned,
                ClientOwnedInput{kind, std::move(text)}, std::nullopt};
    };

    PromptRoutingState routing;
    routing.focus = impl_.interaction.effectiveFocus();
    auto const promptStatus = impl_.promptStatusView();
    if (promptStatus.activeKind == PromptKind::Palette) {
        routing.prompt = ActivePrompt::Palette;
    } else if (auto const view = detail::resolveRuntimePromptControls(
                   impl_.interaction.prompt(), impl_.findReplace.viewState())) {
        const auto kind = impl_.interaction.prompt().request()->kind;
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
        auto const route = PromptTextRouter{}.edit(routing, edit);
        if (route.kind == PromptTextRoute::Kind::Dispatch) {
            return dispatchInput(route.command, route.payload);
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

    auto const catalogRevision = impl_.catalog->revision();
    if (!impl_.inputKeymap ||
        impl_.inputKeymapGeneration != impl_.keymapGeneration ||
        impl_.inputCatalogRevision != catalogRevision) {
        impl_.inputKeymap =
            std::make_unique<CompiledKeymap>(impl_.keymap, *impl_.catalog);
        impl_.inputKeymapGeneration = impl_.keymapGeneration;
        impl_.inputCatalogRevision = catalogRevision;
    }

    if (input.stroke.code != KeyCode::None) {
        auto const resolved = impl_.inputKeymap->resolve(
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
                    return dispatchInput("palette.close");
                }
            }
            if (routing.focus == FocusTarget::Prompt &&
                command == "clipboard.paste") {
                auto const text = impl_.clipboard.viewState().plainText;
                if (text.empty()) {
                    return {ClientInputOutcome::Unhandled, std::nullopt,
                            std::nullopt};
                }
                return routeTextEdit(
                    {PromptTextEdit::Kind::Append, std::move(text)});
            }
            return dispatchInput(command);
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

} // namespace

ClientInputResult inputLocked(EditorSession::Impl& impl_,
                              ClientInput const& input) {
    return std::visit(
        [&](auto const& semantic) -> ClientInputResult {
            using Input = std::decay_t<decltype(semantic)>;
            if constexpr (std::same_as<Input, ClientKeyInput>) {
                return inputKeyLocked(impl_, semantic);
            } else {
                const auto unhandled = [] {
                    return ClientInputResult{ClientInputOutcome::Unhandled,
                                             std::nullopt, std::nullopt};
                };
                const auto rejectTarget = [&](std::string message) {
                    return ClientInputResult{
                        ClientInputOutcome::Rejected, std::nullopt,
                        CommandResult{CommandError::HandlerFailed,
                                      std::move(message), {}}};
                };
                if constexpr (!std::same_as<Input, DocumentPointerInput> &&
                              !std::same_as<Input, ScrollLinesInput> &&
                              !std::same_as<Input, ScrollFractionInput> &&
                              !std::same_as<Input, ViewTransitionInput>) {
                    if (semantic.phase != InputPointerPhase::Press) {
                        return unhandled();
                    }
                }
                if constexpr (!std::same_as<Input, ScrollLinesInput> &&
                              !std::same_as<Input, ScrollFractionInput> &&
                              !std::same_as<Input, ViewTransitionInput>) {
                    if (semantic.button != InputPointerButton::Primary &&
                        !std::same_as<Input, TabPointerInput>) {
                        return unhandled();
                    }
                }
                auto dispatch = [&](CommandName command,
                                    std::any payload) -> ClientInputResult {
                    auto result = impl_.dispatchLocked(
                        {std::move(command), std::move(payload)});
                    const auto activation =
                        result.accepted()
                            ? impl_.interaction.openPickerActivation()
                            : std::nullopt;
                    const auto outcome =
                        !result.accepted()
                            ? ClientInputOutcome::Rejected
                            : result.viewAction
                                  ? ClientInputOutcome::ViewOwned
                                  : ClientInputOutcome::Dispatched;
                    return {outcome, std::nullopt, std::move(result),
                            activation};
                };
                if constexpr (std::same_as<Input, DocumentPointerInput>) {
                    if (semantic.phase == InputPointerPhase::Press ||
                        semantic.phase == InputPointerPhase::Cancel) {
                        impl_.documentPointerGesture.reset();
                    }
                }
                if constexpr (std::same_as<Input, ScrollLinesInput>) {
                    if (semantic.action.rows == 0) {
                        return rejectTarget(
                            "line-scroll input must move at least one row");
                    }
                    switch (semantic.action.target) {
                        case ScrollTarget::Document:
                            return dispatch(
                                "view.scroll_lines",
                                ScrollLinesArguments{semantic.action.rows});
                        case ScrollTarget::Tree:
                            return dispatch(
                                "tree.scroll",
                                ScrollLinesArguments{semantic.action.rows});
                    }
                    return rejectTarget("line-scroll target is invalid");
                } else if constexpr (std::same_as<Input,
                                                  ScrollFractionInput>) {
                    if (semantic.action.denominator == 0 ||
                        semantic.action.numerator >
                            semantic.action.denominator) {
                        return rejectTarget(
                            "fraction-scroll input is invalid");
                    }
                    const auto fraction = ScrollFractionArguments{
                        semantic.action.numerator,
                        semantic.action.denominator};
                    switch (semantic.action.target) {
                        case ScrollTarget::Document:
                            return dispatch("view.scroll_to_fraction",
                                            fraction);
                        case ScrollTarget::Tree:
                            return dispatch("tree.scroll_to_fraction",
                                            fraction);
                    }
                    return rejectTarget("fraction-scroll target is invalid");
                } else if constexpr (std::same_as<Input,
                                                  ViewTransitionInput>) {
                    return std::visit(
                        [&](const auto& transition) -> ClientInputResult {
                            using Transition =
                                std::decay_t<decltype(transition)>;
                            if constexpr (std::same_as<Transition,
                                                       PauseFollowTransition>) {
                                if (impl_.follow.viewState().mode ==
                                    FollowMode::Paused) {
                                    return unhandled();
                                }
                                return dispatch("follow_edits.pause",
                                                std::any{});
                            } else if constexpr (std::same_as<
                                                     Transition,
                                                     PaneFocusTransition>) {
                                if (!impl_.focusPane(transition.pane)) {
                                    return rejectTarget(
                                        "pane focus target is not in this "
                                        "attachment");
                                }
                                const bool focusChanged =
                                    impl_.interaction.effectiveFocus() !=
                                    FocusTarget::Editor;
                                if (focusChanged) {
                                    impl_.interaction.focusEditor();
                                }
                                impl_.recordNavigation(NavigationClass::User);
                                return {ClientInputOutcome::Dispatched,
                                        std::nullopt,
                                        CommandResult{CommandError::None, {}}};
                            } else if constexpr (std::same_as<
                                                     Transition,
                                                     SelectionTransition>) {
                                const auto* tab = impl_.activeTabState();
                                const auto* document = impl_.activeDocument();
                                if (tab == nullptr ||
                                    tab->id != transition.activeTab) {
                                    return rejectTarget(
                                        "resolved selection active tab is "
                                        "stale");
                                }
                                auto documentRevision =
                                    document ? document->revision() : 0;
                                if (const auto* activeTab =
                                        impl_.activeTabState();
                                    activeTab &&
                                    activeTab->kind == TabKind::LiveDiff) {
                                    documentRevision =
                                        impl_.diff.viewState().revision;
                                }
                                if (document == nullptr ||
                                    documentRevision !=
                                        transition.documentRevision) {
                                    return rejectTarget(
                                        "resolved selection document is "
                                        "stale");
                                }
                                if (transition.selections.empty()) {
                                    return rejectTarget(
                                        "resolved selection must not be "
                                        "empty");
                                }
                                std::vector<Selection> selections;
                                selections.reserve(
                                    transition.selections.size());
                                const auto& text = impl_.activeText();
                                for (const auto& range :
                                     transition.selections) {
                                    auto anchor =
                                        SelectionNavigator::resolvePosition(
                                            text, range.anchor);
                                    auto active =
                                        SelectionNavigator::resolvePosition(
                                            text, range.active);
                                    if (!anchor || !active) {
                                        return rejectTarget(
                                            "resolved selection range is "
                                            "invalid");
                                    }
                                    selections.push_back({*anchor, *active});
                                }
                                impl_.selection.selections =
                                    SelectionSet{std::move(selections)};
                                if (const auto documentId =
                                        impl_.activeDocumentId()) {
                                    impl_.historyFor(*documentId)
                                        .breakCoalescing();
                                }
                                impl_.recordNavigation(NavigationClass::User);
                                return {
                                    ClientInputOutcome::Dispatched,
                                    std::nullopt,
                                    CommandResult{CommandError::None, {}}};
                            } else if constexpr (std::same_as<
                                                     Transition,
                                                     PointerSelectionTransition>) {
                                if (!impl_.documentPointerGesture) {
                                    return rejectTarget(
                                        "pointer selection has no active "
                                        "gesture");
                                }
                                return inputLocked(
                                    impl_,
                                    ClientInput{DocumentPointerInput{
                                        transition.position, false, false,
                                        InputPointerButton::Primary,
                                        InputPointerPhase::Move,
                                        DocumentPointerEdge::None}});
                            } else {
                                static_assert(
                                    std::same_as<
                                        Transition,
                                        PointerSelectionTransition>,
                                    "unhandled view transition");
                            }
                        },
                        semantic.transition);
                } else if constexpr (std::same_as<Input,
                                                  DocumentPointerInput>) {
                    const auto handled = [&] {
                        return ClientInputResult{
                            ClientInputOutcome::Dispatched, std::nullopt,
                            CommandResult{CommandError::None, {}, {}}};
                    };
                    if (semantic.phase == InputPointerPhase::Cancel) {
                        impl_.documentPointerGesture.reset();
                        return handled();
                    }
                    const auto resolvePosition = [&]()
                        -> std::optional<DocumentPosition> {
                        if (!semantic.position) return std::nullopt;
                        return SelectionNavigator::resolvePosition(
                            impl_.activeText(), *semantic.position);
                    };
                    if (semantic.phase == InputPointerPhase::Press) {
                        auto position = resolvePosition();
                        auto documentId = impl_.activeDocumentId();
                        if (!position || !documentId) {
                            return rejectTarget(
                                "document pointer target is not actionable");
                        }
                        if (semantic.selectWord) {
                            return dispatch(
                                "select.word_at_position",
                                SelectionCommandArguments{*position,
                                                          std::nullopt});
                        }
                        auto const& items = impl_.selection.selections.items();
                        std::vector<Selection> baseline{
                            items.begin(), items.end()};
                        if (semantic.additive && baseline.size() > 1) {
                            auto const hit = std::find_if(
                                baseline.begin(), baseline.end(),
                                [&](Selection const& selection) {
                                    auto const offset =
                                        position->byteOffset.value();
                                    auto const lower =
                                        selection.lower().byteOffset.value();
                                    auto const upper =
                                        selection.upper().byteOffset.value();
                                    return lower == upper ? offset == lower
                                                          : lower <= offset &&
                                                                offset < upper;
                                });
                            if (hit != baseline.end()) {
                                baseline.erase(hit);
                                return dispatch(
                                    "select.set_ranges",
                                    SelectionCommandArguments{
                                        std::nullopt, std::nullopt,
                                        std::move(baseline)});
                            }
                        }
                        impl_.documentPointerGesture =
                            EditorSession::Impl::DocumentPointerGesture{
                                *documentId,
                                impl_.activeDocument()->revision(),
                                *position, *position, semantic.additive,
                                baseline};
                        auto result =
                            semantic.additive
                                ? dispatch(
                                      "select.add_range",
                                      SelectionCommandArguments{
                                          std::nullopt,
                                          Selection{*position, *position}})
                                : dispatch(
                                      "cursor.set_position",
                                      SelectionCommandArguments{*position,
                                                                std::nullopt});
                        if (!result.command || !result.command->accepted()) {
                            impl_.documentPointerGesture.reset();
                        }
                        return result;
                    }
                    if (!impl_.documentPointerGesture) {
                        return unhandled();
                    }
                    auto& gesture = *impl_.documentPointerGesture;
                    if (impl_.activeDocumentId() !=
                        std::optional<FileDocumentId>{gesture.documentId}) {
                        impl_.documentPointerGesture.reset();
                        return rejectTarget(
                            "document pointer gesture target changed");
                    }
                    if (impl_.activeDocument()->revision() !=
                        gesture.documentRevision) {
                        impl_.documentPointerGesture.reset();
                        return rejectTarget(
                            "document changed during pointer gesture");
                    }
                    auto position = resolvePosition();
                    if (semantic.edge != DocumentPointerEdge::None) {
                        auto result = CommandResult{
                            CommandError::None, {}};
                        result.viewAction =
                            ContinuePointerEdge{semantic.edge};
                        return {ClientInputOutcome::ViewOwned, std::nullopt,
                                std::move(result)};
                    }
                    if (!position &&
                        semantic.phase == InputPointerPhase::Move) {
                        return rejectTarget(
                            "document pointer target is not actionable");
                    }
                    ClientInputResult result = handled();
                    if (position) {
                        if (gesture.additive) {
                            auto ranges = gesture.baseline;
                            ranges.push_back(Selection{
                                gesture.anchor, *position});
                            result = dispatch(
                                "select.set_ranges",
                                SelectionCommandArguments{
                                    std::nullopt, std::nullopt,
                                    std::move(ranges)});
                        } else {
                            result = dispatch(
                                "select.set_range",
                                SelectionCommandArguments{
                                    std::nullopt,
                                    Selection{gesture.anchor,
                                              *position}});
                        }
                        if (result.command && result.command->accepted() &&
                            position) {
                            gesture.active = *position;
                        }
                    }
                    if (semantic.phase == InputPointerPhase::Release) {
                        impl_.documentPointerGesture.reset();
                    }
                    return result;
                } else if constexpr (std::same_as<Input, TabPointerInput>) {
                    if (semantic.button == InputPointerButton::Primary) {
                        return dispatch("tab.activate", semantic.tabId);
                    }
                    if (semantic.button == InputPointerButton::Auxiliary) {
                        return dispatch("tab.close", semantic.tabId);
                    }
                    return unhandled();
                } else if constexpr (std::same_as<Input, TreePointerInput>) {
                    if (semantic.button != InputPointerButton::Primary) {
                        return unhandled();
                    }
                    return dispatch("tree.activate_node",
                                    TreeSelectArguments{semantic.nodeId});
                } else if constexpr (std::same_as<Input,
                                                  PickerPointerInput>) {
                    if (semantic.button != InputPointerButton::Primary) {
                        return unhandled();
                    }
                    return dispatch(
                        "picker.submit",
                        PickerSubmitArguments{semantic.activation,
                                              semantic.candidateId});
                } else if constexpr (std::same_as<
                                         Input, ExternalActionPointerInput>) {
                    if (semantic.button != InputPointerButton::Primary) {
                        return unhandled();
                    }
                    return dispatch("external.invoke_action",
                                    semantic.invocation);
                } else if constexpr (std::same_as<
                                         Input, NoticeActionPointerInput>) {
                    if (semantic.button != InputPointerButton::Primary) {
                        return unhandled();
                    }
                    const auto notice = impl_.noticeView();
                    if (!notice) {
                        return rejectTarget("notice action target is not present");
                    }
                    for (auto const& action : notice->actions) {
                        if (action.id == semantic.actionId) {
                            return dispatch(action.commandId, std::any{});
                        }
                    }
                    return rejectTarget("notice action target is not actionable");
                }
            }
        },
        input);
}

} // namespace ssg
