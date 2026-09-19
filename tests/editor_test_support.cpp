#include "editor_test_support.h"

#include <stdexcept>
#include <utility>
#include <vector>

namespace ssg::test {

CommandResult requireCommand(ClientInputResult result) {
    if (!result.command) {
        throw std::runtime_error{"client input produced no command result"};
    }
    return std::move(*result.command);
}

PromptEditState promptText(std::string text) {
    return PromptEditState{std::move(text)};
}

CommandResult dispatchInput(Editor& editor, ClientInput input) {
    return requireCommand(editor.input(std::move(input)));
}

CommandResult openFile(Editor& editor, std::string_view path) {
    auto result =
        applyFilePathCompletion(editor, PromptCompletion::FileOpen, path);
    return {result.accepted ? CommandError::None : CommandError::HandlerFailed,
            std::move(result.message), std::move(result.viewAction)};
}

CommandResult typeText(Editor& editor, std::string text) {
    std::lock_guard operationLock{editor.operationMutex};
    auto const active = editor.activeDocumentId();
    auto const revisionBefore =
        active && editor.activeDocument()
            ? std::optional<std::uint64_t>{editor.activeDocument()->revision()}
            : std::nullopt;
    editor.screen.refreshExternalModificationPresence(
        editor.externalModificationPresent());
    auto result = applyEditorTextInput(
        editor, TextInputCommand::Insert,
        TextInputArguments{std::move(text)});
    editor.reconcileFindDocument();
    editor.screen.refreshExternalModificationPresence(
        editor.externalModificationPresent());
    if (result.accepted && active && revisionBefore &&
        editor.activeDocumentId() == active &&
        editor.activeDocument() != nullptr &&
        editor.activeDocument()->revision() != *revisionBefore) {
        (void)editor.follow.notifyLocalEdit();
    }
    return {result.accepted ? CommandError::None : CommandError::HandlerFailed,
            std::move(result.message), std::move(result.viewAction)};
}

CommandResult setSelections(
    Editor& editor,
    std::initializer_list<std::pair<std::uint64_t, std::uint64_t>> ranges) {
    auto const* tab = editor.activeTabState();
    auto const* document = editor.activeDocument();
    if (tab == nullptr || document == nullptr) {
        throw std::runtime_error{
            "selection transition requires an active document"};
    }
    auto revision = document->revision();
    if (tab->kind == TabKind::LiveDiff) {
        revision = editor.diff.viewState().revision;
    }
    std::vector<SelectionRangeTransition> selections;
    selections.reserve(ranges.size());
    for (auto const& [anchor, active] : ranges) {
        selections.push_back({ByteOffset{anchor}, ByteOffset{active}});
    }
    return dispatchInput(
        editor, ViewTransitionInput{
                    SelectionTransition{
                        tab->id, revision, std::move(selections)}});
}

CommandResult clickDocument(Editor& editor, std::uint64_t byteOffset,
                            bool additive, bool selectWord) {
    auto result = dispatchInput(
        editor,
        DocumentPointerInput{
            ByteOffset{byteOffset}, additive, selectWord});
    if (editor.documentPointerGesture.has_value()) {
        auto release = dispatchInput(
            editor,
            DocumentPointerInput{
                ByteOffset{byteOffset}, false, false,
                InputPointerButton::Primary, InputPointerPhase::Release});
        if (!release.accepted()) return release;
    }
    return result;
}

CommandResult dragDocument(Editor& editor, std::uint64_t anchor,
                           std::uint64_t active, bool additive) {
    auto press = dispatchInput(
        editor,
        DocumentPointerInput{
            ByteOffset{anchor}, additive});
    if (!press.accepted()) return press;
    auto move = dispatchInput(
        editor,
        DocumentPointerInput{
            ByteOffset{active}, false, false,
            InputPointerButton::Primary, InputPointerPhase::Move});
    if (!move.accepted()) return move;
    auto release = dispatchInput(
        editor,
        DocumentPointerInput{
            ByteOffset{active}, false, false,
            InputPointerButton::Primary, InputPointerPhase::Release});
    if (!release.accepted()) return release;
    return move;
}

}
