#pragma once

#include <ssg/config.h>
#include <ssg/Document.h>
#include <ssg/Selection.h>

#include <cstdint>
#include <optional>
#include <string>

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

// Interprets an edit-suite command against a document snapshot + selections and
// returns the resulting edits/selections (a pure transform over the snapshot
// value; owns no document and mutates nothing). The runtime owns the Document
// and applies the result.
class EditInterpreter {
public:
    [[nodiscard]] EditCommandResult apply(
        const DocumentSnapshot& document, const SelectionSet& selections,
        EditCommandSettings settings, EditCommand command) const;
};

}  // namespace ssg
