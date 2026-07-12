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
    indent,
    outdent,
    duplicate_line,
    move_line_up,
    move_line_down,
    delete_line,
    join_lines,
    uppercase,
    lowercase,
    swap_case,
    sort_lines,
    transpose,
    toggle_comment,
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
    friend EditCommandSuiteCommandSet edit_command_suite_command_set();
    EditCommandSuiteCommandSet();

    const std::array<EditCommandDescriptor, 13> descriptors_;
};

[[nodiscard]] EditCommandSuiteCommandSet
edit_command_suite_command_set();

struct EditCommandSettings {
    IndentStyle indent_style;
    std::uint32_t indent_width;
    std::uint32_t tab_width;
    LineEnding line_ending;
    std::string line_comment_token;

    bool operator==(const EditCommandSettings&) const = default;
};

enum class EditCommandError : std::uint8_t {
    none,
    read_only,
    diff,
    invalid_selection,
    invalid_settings,
    unknown_command,
};

struct EditCommandResult {
    EditCommandError error;
    std::optional<EditTransaction> transaction;
    std::optional<SelectionSet> selections;
    std::string resulting_text;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == EditCommandError::none;
    }
};

[[nodiscard]] EditCommandResult apply_edit_command(
    const DocumentSnapshot& document, const SelectionSet& selections,
    EditCommandSettings settings, EditCommand command);

}  // namespace ssg
