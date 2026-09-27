#include "editor_test_support.h"

#include <algorithm>
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
    auto result = editor.applyTextInput(TextInputCommand::Insert,
                                        TextInputArguments{std::move(text)});
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

struct EditorAccess {
    static bool hasDocumentAssociation(
        const Editor& editor, FileDocumentId document) {
        const auto mappedDocument =
            [&](const auto& entry) { return entry.second == document; };
        return editor.workspace.tryDocument(document) != nullptr ||
               editor.documentRuntimeStates.contains(document.value()) ||
               editor.documentLanguageOverrides.contains(document.value()) ||
               editor.findDocumentId == document ||
               std::any_of(editor.liveDiffDocuments.begin(),
                           editor.liveDiffDocuments.end(), mappedDocument) ||
               std::any_of(editor.readOnlyTabDocuments.begin(),
                           editor.readOnlyTabDocuments.end(), mappedDocument);
    }

    static void seedDocumentAssociations(
        Editor& editor, FileDocumentId document) {
        editor.documentLanguageOverrides.insert_or_assign(
            document.value(), LanguageId::plainText());
        editor.findDocumentId = document;
    }

    static std::optional<FileDocumentId> findAssociatedDocument(
        const Editor& editor) {
        return editor.findDocumentId;
    }

    static FileDocumentId contentTabDocument(
        const Editor& editor, TabKind kind, std::string_view identity) {
        const auto& documents = kind == TabKind::LiveDiff
                                    ? editor.liveDiffDocuments
                                    : editor.readOnlyTabDocuments;
        return documents.at(std::string{identity});
    }

    static std::size_t liveDiffDocumentCount(const Editor& editor) {
        return editor.liveDiffDocuments.size();
    }
};

bool hasDocumentAssociation(const Editor& editor, FileDocumentId document) {
    return EditorAccess::hasDocumentAssociation(editor, document);
}

std::optional<FileDocumentId> findAssociatedDocument(const Editor& editor) {
    return EditorAccess::findAssociatedDocument(editor);
}

void seedDocumentAssociations(Editor& editor, FileDocumentId document) {
    EditorAccess::seedDocumentAssociations(editor, document);
}

FileDocumentId contentTabDocument(
    const Editor& editor, TabKind kind, std::string_view identity) {
    return EditorAccess::contentTabDocument(editor, kind, identity);
}

std::size_t liveDiffDocumentCount(const Editor& editor) {
    return EditorAccess::liveDiffDocumentCount(editor);
}

}
