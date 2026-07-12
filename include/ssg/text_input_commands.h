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

enum class TextInputCommand : std::uint8_t {
    insert,
    newline,
    delete_backward,
    delete_forward,
    delete_word_backward,
    delete_word_forward,
};

struct TextInputCommandDescriptor {
    std::string_view id;
    TextInputCommand command;

    bool operator==(const TextInputCommandDescriptor&) const noexcept = default;
};

class TextInputCommandSet {
public:
    TextInputCommandSet(const TextInputCommandSet&) = default;
    TextInputCommandSet& operator=(const TextInputCommandSet&) = delete;

    [[nodiscard]] const std::array<TextInputCommandDescriptor, 6>&
    descriptors() const noexcept;

private:
    friend TextInputCommandSet text_input_command_set();
    TextInputCommandSet();

    const std::array<TextInputCommandDescriptor, 6> descriptors_;
};

[[nodiscard]] TextInputCommandSet text_input_command_set();

struct TextInputSettings {
    IndentStyle indent_style;
    std::uint32_t indent_width;
    bool auto_indent;
    LineEnding line_ending;

    bool operator==(const TextInputSettings&) const noexcept = default;
};

struct TextInputArguments {
    std::string text;

    bool operator==(const TextInputArguments&) const = default;
};

enum class TextInputError : std::uint8_t {
    none,
    read_only,
    diff,
    invalid_selection,
    invalid_utf8,
    invalid_settings,
    unknown_command,
};

struct TextInputResult {
    TextInputError error;
    std::optional<EditTransaction> transaction;
    std::optional<SelectionSet> selections;
    std::string resulting_text;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == TextInputError::none;
    }
};

[[nodiscard]] TextInputResult apply_text_input(
    const DocumentSnapshot& document, const SelectionSet& selections,
    TextInputSettings settings, TextInputCommand command,
    TextInputArguments arguments = {});

}  // namespace ssg
