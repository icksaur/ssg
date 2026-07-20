#pragma once

#include <ssg/edit_commands.h>
#include <ssg/history.h>
#include <ssg/text_input_commands.h>

#include <cstdint>
#include <optional>
#include <string>

namespace ssg {

enum class EditHistoryIntegrationError : std::uint8_t {
    None,
    TextInputRejected,
    EditCommandRejected,
    InvalidCommandResult,
    HistoryRejected,
};

struct EditHistoryIntegrationResult {
    EditHistoryIntegrationError error;
    std::optional<TextInputError> textInputError;
    std::optional<EditCommandError> editCommandError;
    std::optional<HistoryResult> historyResult;
    std::optional<SelectionSet> selections;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == EditHistoryIntegrationError::None;
    }

    [[nodiscard]] bool documentChanged() const noexcept {
        return historyResult.has_value() && historyResult->accepted();
    }
};

[[nodiscard]] HistoryEditKind historyEditKind(
    TextInputCommand command) noexcept;
[[nodiscard]] HistoryEditKind historyEditKind(EditCommand command) noexcept;

[[nodiscard]] EditHistoryIntegrationResult applyTextInputWithHistory(
    Document& document, DocumentHistory& history,
    const SelectionSet& selections, TextInputSettings settings,
    TextInputCommand command, TextInputArguments arguments,
    std::uint64_t timestampMs);

[[nodiscard]] EditHistoryIntegrationResult applyEditCommandWithHistory(
    Document& document, DocumentHistory& history,
    const SelectionSet& selections, EditCommandSettings settings,
    EditCommand command, std::uint64_t timestampMs);

}  // namespace ssg
