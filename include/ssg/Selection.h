#pragma once

#include <ssg/types.h>
#include <ssg/Scroll.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

struct Selection {
    DocumentPosition anchor;
    DocumentPosition active;

    [[nodiscard]] const DocumentPosition& lower() const noexcept;
    [[nodiscard]] const DocumentPosition& upper() const noexcept;
    [[nodiscard]] bool isCaret() const noexcept;

    // The text this selection acts on for a word-oriented feature: the covered
    // substring when it is a range, or the whole word the caret sits in (or
    // touches on its trailing side) when it is a caret. Empty when a caret is
    // surrounded by non-word bytes or a range is out of range. `text` is the
    // whole document; word bytes are WordClassification's isWordByte.
    [[nodiscard]] std::string wordOrCoveredText(std::string_view text) const;

    bool operator==(const Selection&) const noexcept = default;
};

class SelectionSet {
public:
    explicit SelectionSet(std::vector<Selection> selections);

    [[nodiscard]] const std::vector<Selection>& items() const noexcept;
    [[nodiscard]] const Selection& primary() const noexcept;
    bool operator==(const SelectionSet&) const noexcept = default;

private:
    std::vector<Selection> selections_;
};

struct SelectionViewState {
    SelectionSet selections;
    std::uint32_t firstVisualRow;
    // Horizontal scroll offset in cells (word wrap OFF only; VP-H / M12). Reveal
    // keeps the primary caret's cell column within [first_visual_column,
    // first_visual_column + pane_width). Always 0 when word wrap is on.
    std::uint32_t firstVisualColumn = 0;
    std::optional<CellIndex> desiredCell;

    bool operator==(const SelectionViewState&) const noexcept = default;
};

struct SelectionViewDelta {
    bool changed;
    std::optional<SelectionViewState> replacement;

    bool operator==(const SelectionViewDelta&) const noexcept = default;
};

struct BracketPair {
    std::string opening;
    std::string closing;

    bool operator==(const BracketPair&) const noexcept = default;
};

enum class SelectionCommand : std::uint8_t {
    CursorSetPosition,
    CursorLeft,
    CursorRight,
    CursorWordLeft,
    CursorWordRight,
    CursorLineUp,
    CursorLineDown,
    CursorLineStart,
    CursorLineEnd,
    CursorPageUp,
    CursorPageDown,
    CursorDocumentStart,
    CursorDocumentEnd,
    SelectSetRange,
    SelectSetRanges,
    SelectAddRange,
    SelectLeft,
    SelectRight,
    SelectWordLeft,
    SelectWordRight,
    SelectLineUp,
    SelectLineDown,
    SelectLineStart,
    SelectLineEnd,
    SelectPageUp,
    SelectPageDown,
    SelectDocumentStart,
    SelectDocumentEnd,
    SelectAll,
    SelectAddNextOccurrence,
    SelectAddCursorUp,
    SelectAddCursorDown,
    SelectSplitIntoLines,
    SelectToMatchingBracket,
    GotoMatchingBracket,
    SelectWordAtPosition,
    ViewRevealCaret,
    ViewCenterCaret,
};

struct SelectionCommandArguments {
    std::optional<DocumentPosition> position;
    std::optional<Selection> selection;
    std::vector<Selection> selections;
};

// CONTRACT: This is the single selection-command inventory consumed by
// registration and command lookup.
struct SelectionCommandDescriptor {
    std::string_view id;
    SelectionCommand command;

};

extern const std::array<SelectionCommandDescriptor, 38> kSelectionCommands;


enum class SelectionNavigationError : std::uint8_t {
    None,
    MissingArgument,
    InvalidPosition,
    InvalidBracketPairs,
    InvalidTabWidth,
};

struct SelectionNavigationResult {
    SelectionNavigationError error;
    SelectionViewDelta delta;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == SelectionNavigationError::None;
    }
};

class SelectionNavigator {
public:
    [[nodiscard]] static std::optional<DocumentPosition> resolvePosition(
        std::string_view text, ByteOffset byteOffset, int tabWidth = 4);

    [[nodiscard]] SelectionNavigationResult apply(
        std::string_view text, const SelectionViewState& before,
        SelectionCommand command, ViewportDimensions viewport,
        SelectionCommandArguments arguments = {},
        std::span<const BracketPair> bracketPairs = {}, int tabWidth = 4,
        bool wordWrap = true,
        const DiffFileView* diff = nullptr) const;
};

} // namespace ssg
