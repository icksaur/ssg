#pragma once

#include <ssg/edit_commands.h>
#include <ssg/history.h>
#include <ssg/text_input_commands.h>

#include <cstdint>
#include <optional>
#include <string>

namespace ssg {

enum class EditHistoryIntegrationError : std::uint8_t {
    none,
    text_input_rejected,
    edit_command_rejected,
    invalid_command_result,
    history_rejected,
};

struct EditHistoryIntegrationResult {
    EditHistoryIntegrationError error;
    std::optional<TextInputError> text_input_error;
    std::optional<EditCommandError> edit_command_error;
    std::optional<HistoryResult> history_result;
    std::optional<SelectionSet> selections;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == EditHistoryIntegrationError::none;
    }

    [[nodiscard]] bool document_changed() const noexcept {
        return history_result.has_value() && history_result->accepted();
    }
};

[[nodiscard]] HistoryEditKind history_edit_kind(
    TextInputCommand command) noexcept;
[[nodiscard]] HistoryEditKind history_edit_kind(EditCommand command) noexcept;

[[nodiscard]] EditHistoryIntegrationResult apply_text_input_with_history(
    Document& document, DocumentHistory& history,
    const SelectionSet& selections, TextInputSettings settings,
    TextInputCommand command, TextInputArguments arguments,
    std::uint64_t timestamp_ms);

[[nodiscard]] EditHistoryIntegrationResult apply_edit_command_with_history(
    Document& document, DocumentHistory& history,
    const SelectionSet& selections, EditCommandSettings settings,
    EditCommand command, std::uint64_t timestamp_ms);

}  // namespace ssg
