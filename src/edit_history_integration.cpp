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
    const SelectionSet& selections_before, const EditTransaction& transaction,
    const SelectionSet& selections_after, HistoryEditKind kind,
    std::uint64_t timestamp_ms) {
    auto history_result =
        history.applyEdit(document, transaction, selections_before,
                           selections_after, kind, timestamp_ms);
    if (!history_result.accepted()) {
        auto message = history_result.message;
        return {EditHistoryIntegrationError::HistoryRejected,
                std::nullopt,
                std::nullopt,
                std::move(history_result),
                std::nullopt,
                std::move(message)};
    }

    auto restored = history_result.selections;
    return {EditHistoryIntegrationError::None,
            std::nullopt,
            std::nullopt,
            std::move(history_result),
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
    std::uint64_t timestamp_ms) {
    auto result =
        applyTextInput(document.snapshot(), selections, std::move(settings),
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
                              historyEditKind(command), timestamp_ms);
}

EditHistoryIntegrationResult applyEditCommandWithHistory(
    Document& document, DocumentHistory& history,
    const SelectionSet& selections, EditCommandSettings settings,
    EditCommand command, std::uint64_t timestamp_ms) {
    auto result =
        applyEditCommand(document.snapshot(), selections, std::move(settings),
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
                              historyEditKind(command), timestamp_ms);
}

}  // namespace ssg
