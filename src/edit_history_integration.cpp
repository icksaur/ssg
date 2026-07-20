#include <ssg/edit_history_integration.h>

#include <utility>

namespace ssg {
namespace {

EditHistoryIntegrationResult invalidResult(std::string message) {
    return {EditHistoryIntegrationError::InvalidCommandResult,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::move(message)};
}

EditHistoryIntegrationResult applyDerivedEdit(
    Document& document, DocumentHistory& history,
    const SelectionSet& selectionsBefore, const EditTransaction& transaction,
    const SelectionSet& selectionsAfter, HistoryEditKind kind,
    std::uint64_t timestampMs) {
    auto historyResult =
        history.applyEdit(document, transaction, selectionsBefore,
                           selectionsAfter, kind, timestampMs);
    if (!historyResult.accepted()) {
        auto message = historyResult.message;
        return {EditHistoryIntegrationError::HistoryRejected,
                std::nullopt,
                std::nullopt,
                std::move(historyResult),
                std::nullopt,
                std::move(message)};
    }

    auto restored = historyResult.selections;
    return {EditHistoryIntegrationError::None,
            std::nullopt,
            std::nullopt,
            std::move(historyResult),
            std::move(restored),
            {}};
}

}  // namespace

HistoryEditKind historyEditKind(TextInputCommand command) noexcept {
    switch (command) {
        case TextInputCommand::Insert:
            return HistoryEditKind::Typing;
        case TextInputCommand::DeleteBackward:
        case TextInputCommand::DeleteWordBackward:
            return HistoryEditKind::DeleteBackward;
        case TextInputCommand::DeleteForward:
        case TextInputCommand::DeleteWordForward:
            return HistoryEditKind::DeleteForward;
        case TextInputCommand::Newline:
            return HistoryEditKind::Other;
    }
    return HistoryEditKind::Other;
}

HistoryEditKind historyEditKind(EditCommand) noexcept {
    return HistoryEditKind::Other;
}

EditHistoryIntegrationResult applyTextInputWithHistory(
    Document& document, DocumentHistory& history,
    const SelectionSet& selections, TextInputSettings settings,
    TextInputCommand command, TextInputArguments arguments,
    std::uint64_t timestampMs) {
    auto result =
        TextInputInterpreter{}.apply(document.snapshot(), selections, std::move(settings),
                         command, std::move(arguments));
    if (!result.accepted()) {
        auto message = result.message;
        return {EditHistoryIntegrationError::TextInputRejected,
                result.error,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::move(message)};
    }
    if (!result.selections.has_value()) {
        return invalidResult(
            "accepted text-input command did not return selections");
    }
    if (!result.transaction.has_value()) {
        return {EditHistoryIntegrationError::None,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::move(result.selections),
                {}};
    }
    return applyDerivedEdit(document, history, selections,
                              *result.transaction, *result.selections,
                              historyEditKind(command), timestampMs);
}

EditHistoryIntegrationResult applyEditCommandWithHistory(
    Document& document, DocumentHistory& history,
    const SelectionSet& selections, EditCommandSettings settings,
    EditCommand command, std::uint64_t timestampMs) {
    auto result =
        EditInterpreter{}.apply(document.snapshot(), selections, std::move(settings),
                           command);
    if (!result.accepted()) {
        auto message = result.message;
        return {EditHistoryIntegrationError::EditCommandRejected,
                std::nullopt,
                result.error,
                std::nullopt,
                std::nullopt,
                std::move(message)};
    }
    if (!result.selections.has_value()) {
        return invalidResult(
            "accepted edit command did not return selections");
    }
    if (!result.transaction.has_value()) {
        return {EditHistoryIntegrationError::None,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::move(result.selections),
                {}};
    }
    return applyDerivedEdit(document, history, selections,
                              *result.transaction, *result.selections,
                              historyEditKind(command), timestampMs);
}

}  // namespace ssg
