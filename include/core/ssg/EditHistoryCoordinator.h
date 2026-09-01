#pragma once

#include <ssg/EditCommands.h>
#include <ssg/DocumentHistory.h>
#include <ssg/TextInputCommands.h>

#include <cstdint>
#include <optional>
#include <string>

namespace ssg {

enum class EditHistoryError : std::uint8_t {
    None,
    TextInputRejected,
    EditCommandRejected,
    InvalidCommandResult,
    HistoryRejected,
};

struct EditHistoryResult {
    EditHistoryError error;
    std::optional<TextInputError> textInputError;
    std::optional<EditCommandError> editCommandError;
    std::optional<HistoryResult> historyResult;
    std::optional<SelectionSet> selections;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == EditHistoryError::None;
    }

    [[nodiscard]] bool documentChanged() const noexcept {
        return historyResult.has_value() && historyResult->accepted();
    }
};

// Coordinates a text-input or edit-suite command with undo history over a
// Document: it interprets the command against the document snapshot, applies the
// resulting transaction through the DocumentHistory (so undo/redo and selection
// restoration stay consistent), and reports the composite result. It borrows the
// document and history for its lifetime; construct it around them and call.
class EditHistoryCoordinator {
public:
    EditHistoryCoordinator(Document& document,
                           DocumentHistory& history) noexcept
        : document_(document), history_(history) {}

    [[nodiscard]] EditHistoryResult applyTextInput(
        const SelectionSet& selections, TextInputSettings settings,
        TextInputCommand command, TextInputArguments arguments,
        std::uint64_t timestampMs);

    [[nodiscard]] EditHistoryResult applyEditCommand(
        const SelectionSet& selections, EditCommandSettings settings,
        EditCommand command, std::uint64_t timestampMs);

    [[nodiscard]] static HistoryEditKind editKind(
        TextInputCommand command) noexcept;
    [[nodiscard]] static HistoryEditKind editKind(EditCommand command) noexcept;

private:
    Document& document_;
    DocumentHistory& history_;
};

}  // namespace ssg
