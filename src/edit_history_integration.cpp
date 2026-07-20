#include <ssg/edit_history_integration.h>

#include <utility>

namespace ssg {
namespace {

EditHistoryIntegrationResult invalid_result(std::string message) {
    return {EditHistoryIntegrationError::InvalidCommandResult,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::move(message)};
}

EditHistoryIntegrationResult apply_derived_edit(
    Document& document, DocumentHistory& history,
    const SelectionSet& selections_before, const EditTransaction& transaction,
    const SelectionSet& selections_after, HistoryEditKind kind,
    std::uint64_t timestamp_ms) {
    auto history_result =
        history.apply_edit(document, transaction, selections_before,
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

HistoryEditKind history_edit_kind(TextInputCommand command) noexcept {
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

HistoryEditKind history_edit_kind(EditCommand) noexcept {
    return HistoryEditKind::Other;
}

EditHistoryIntegrationResult apply_text_input_with_history(
    Document& document, DocumentHistory& history,
    const SelectionSet& selections, TextInputSettings settings,
    TextInputCommand command, TextInputArguments arguments,
    std::uint64_t timestamp_ms) {
    auto result =
        apply_text_input(document.snapshot(), selections, std::move(settings),
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
        return invalid_result(
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
    return apply_derived_edit(document, history, selections,
                              *result.transaction, *result.selections,
                              history_edit_kind(command), timestamp_ms);
}

EditHistoryIntegrationResult apply_edit_command_with_history(
    Document& document, DocumentHistory& history,
    const SelectionSet& selections, EditCommandSettings settings,
    EditCommand command, std::uint64_t timestamp_ms) {
    auto result =
        apply_edit_command(document.snapshot(), selections, std::move(settings),
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
        return invalid_result(
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
    return apply_derived_edit(document, history, selections,
                              *result.transaction, *result.selections,
                              history_edit_kind(command), timestamp_ms);
}

}  // namespace ssg
