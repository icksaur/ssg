#include <ssg/edit_history_coordinator.h>

#include <utility>

namespace ssg {
namespace {

EditHistoryResult invalidResult(std::string message) {
    return {EditHistoryError::InvalidCommandResult,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::move(message)};
}

EditHistoryResult applyDerivedEdit(
    Document& document, DocumentHistory& history,
    const SelectionSet& selectionsBefore, const EditTransaction& transaction,
    const SelectionSet& selectionsAfter, HistoryEditKind kind,
    std::uint64_t timestampMs) {
    auto historyResult =
        history.applyEdit(document, transaction, selectionsBefore,
                           selectionsAfter, kind, timestampMs);
    if (!historyResult.accepted()) {
        auto message = historyResult.message;
        return {EditHistoryError::HistoryRejected,
                std::nullopt,
                std::nullopt,
                std::move(historyResult),
                std::nullopt,
                std::move(message)};
    }

    auto restored = historyResult.selections;
    return {EditHistoryError::None,
            std::nullopt,
            std::nullopt,
            std::move(historyResult),
            std::move(restored),
            {}};
}

}  // namespace

HistoryEditKind EditHistoryCoordinator::editKind(
    TextInputCommand command) noexcept {
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

HistoryEditKind EditHistoryCoordinator::editKind(EditCommand) noexcept {
    return HistoryEditKind::Other;
}

EditHistoryResult EditHistoryCoordinator::applyTextInput(
    const SelectionSet& selections, TextInputSettings settings,
    TextInputCommand command, TextInputArguments arguments,
    std::uint64_t timestampMs) {
    auto result =
        TextInputInterpreter{}.apply(document_.snapshot(), selections,
                         std::move(settings), command, std::move(arguments));
    if (!result.accepted()) {
        auto message = result.message;
        return {EditHistoryError::TextInputRejected,
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
        return {EditHistoryError::None,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::move(result.selections),
                {}};
    }
    return applyDerivedEdit(document_, history_, selections,
                              *result.transaction, *result.selections,
                              editKind(command), timestampMs);
}

EditHistoryResult EditHistoryCoordinator::applyEditCommand(
    const SelectionSet& selections, EditCommandSettings settings,
    EditCommand command, std::uint64_t timestampMs) {
    auto result =
        EditInterpreter{}.apply(document_.snapshot(), selections,
                           std::move(settings), command);
    if (!result.accepted()) {
        auto message = result.message;
        return {EditHistoryError::EditCommandRejected,
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
        return {EditHistoryError::None,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::move(result.selections),
                {}};
    }
    return applyDerivedEdit(document_, history_, selections,
                              *result.transaction, *result.selections,
                              editKind(command), timestampMs);
}

}  // namespace ssg
