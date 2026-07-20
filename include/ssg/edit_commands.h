#pragma once

#include <ssg/config.h>
#include <ssg/document.h>
#include <ssg/selection.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ssg {

enum class EditCommand : std::uint8_t {
    Indent,
    Outdent,
    DuplicateLine,
    MoveLineUp,
    MoveLineDown,
    DeleteLine,
    JoinLines,
    Uppercase,
    Lowercase,
    SwapCase,
    SortLines,
    Transpose,
    ToggleComment,
};

struct EditCommandDescriptor {
    std::string_view id;
    EditCommand command;

    bool operator==(const EditCommandDescriptor&) const noexcept = default;
};

class EditCommandSuiteCommandSet {
public:
    EditCommandSuiteCommandSet(const EditCommandSuiteCommandSet&) = default;
    EditCommandSuiteCommandSet& operator=(
        const EditCommandSuiteCommandSet&) = delete;

    [[nodiscard]] const std::array<EditCommandDescriptor, 13>&
    descriptors() const noexcept;

private:
    friend EditCommandSuiteCommandSet editCommandSuiteCommandSet();
    EditCommandSuiteCommandSet();

    const std::array<EditCommandDescriptor, 13> descriptors_;
};

[[nodiscard]] EditCommandSuiteCommandSet
editCommandSuiteCommandSet();

struct EditCommandSettings {
    IndentStyle indentStyle;
    std::uint32_t indentWidth;
    std::uint32_t tabWidth;
    LineEnding lineEnding;
    std::string lineCommentToken;

    bool operator==(const EditCommandSettings&) const = default;
};

enum class EditCommandError : std::uint8_t {
    None,
    ReadOnly,
    Diff,
    InvalidSelection,
    InvalidSettings,
    UnknownCommand,
};

struct EditCommandResult {
    EditCommandError error;
    std::optional<EditTransaction> transaction;
    std::optional<SelectionSet> selections;
    std::string resultingText;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == EditCommandError::None;
    }
};

[[nodiscard]] EditCommandResult applyEditCommand(
    const DocumentSnapshot& document, const SelectionSet& selections,
    EditCommandSettings settings, EditCommand command);

}  // namespace ssg
