#include <ssg/edit_history_integration.h>

#include <utility>

namespace ssg {
namespace {

EditHistoryIntegrationResult invalid_result(std::string message) {
    return {EditHistoryIntegrationError::invalid_command_result,
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
        return {EditHistoryIntegrationError::history_rejected,
                std::nullopt,
                std::nullopt,
                std::move(history_result),
                std::nullopt,
                std::move(message)};
    }

    auto restored = history_result.selections;
    return {EditHistoryIntegrationError::none,
            std::nullopt,
            std::nullopt,
            std::move(history_result),
            std::move(restored),
            {}};
}

}  // namespace

HistoryEditKind history_edit_kind(TextInputCommand command) noexcept {
    switch (command) {
        case TextInputCommand::insert:
            return HistoryEditKind::typing;
        case TextInputCommand::delete_backward:
        case TextInputCommand::delete_word_backward:
            return HistoryEditKind::delete_backward;
        case TextInputCommand::delete_forward:
        case TextInputCommand::delete_word_forward:
            return HistoryEditKind::delete_forward;
        case TextInputCommand::newline:
            return HistoryEditKind::other;
    }
    return HistoryEditKind::other;
}

HistoryEditKind history_edit_kind(EditCommand) noexcept {
    return HistoryEditKind::other;
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
        return {EditHistoryIntegrationError::text_input_rejected,
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
        return {EditHistoryIntegrationError::none,
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
        return {EditHistoryIntegrationError::edit_command_rejected,
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
        return {EditHistoryIntegrationError::none,
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
