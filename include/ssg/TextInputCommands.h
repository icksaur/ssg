#pragma once

#include <ssg/Settings.h>
#include <ssg/Document.h>
#include <ssg/Selection.h>

#include <cstdint>
#include <optional>
#include <string>

namespace ssg {

[[nodiscard]] inline bool isWordByte(unsigned char byte) noexcept {
    return byte >= 0x80 || (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
           byte == '_';
}

enum class TextInputCommand : std::uint8_t {
    Insert,
    Newline,
    DeleteBackward,
    DeleteForward,
    DeleteWordBackward,
    DeleteWordForward,
};

struct TextInputSettings {
    IndentStyle indentStyle;
    std::uint32_t indentWidth;
    bool autoIndent;
    LineEnding lineEnding;

    bool operator==(const TextInputSettings&) const noexcept = default;
};

struct TextInputArguments {
    std::string text;

    bool operator==(const TextInputArguments&) const = default;
};

enum class TextInputError : std::uint8_t {
    None,
    ReadOnly,
    Diff,
    InvalidSelection,
    InvalidUtf8,
    InvalidSettings,
    UnknownCommand,
};

struct TextInputResult {
    TextInputError error;
    std::optional<EditTransaction> transaction;
    std::optional<SelectionSet> selections;
    std::string resultingText;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == TextInputError::None;
    }
};

// Interprets a text-input command against a document snapshot + selections and
// returns the resulting edits/selections (a pure transform over the snapshot
// value; it owns no document and mutates nothing). The runtime owns the Document
// and applies the result.
class TextInputInterpreter {
public:
    [[nodiscard]] TextInputResult apply(
        const DocumentSnapshot& document, const SelectionSet& selections,
        TextInputSettings settings, TextInputCommand command,
        TextInputArguments arguments = {}) const;
};

}  // namespace ssg
