#pragma once

#include <ssg/types.h>
#include <ssg/viewport.h>

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
    [[nodiscard]] bool is_caret() const noexcept;
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
    std::uint32_t first_visual_row;
    // Horizontal scroll offset in cells (word wrap OFF only; VP-H / M12). Reveal
    // keeps the primary caret's cell column within [first_visual_column,
    // first_visual_column + pane_width). Always 0 when word wrap is on.
    std::uint32_t first_visual_column = 0;
    std::optional<CellIndex> desired_cell;

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
    ViewRevealCaret,
    ViewCenterCaret,
};

struct SelectionCommandDescriptor {
    std::string_view id;
    SelectionCommand command;

    bool operator==(const SelectionCommandDescriptor&) const noexcept = default;
};

class SelectionNavigationCommandSet {
public:
    SelectionNavigationCommandSet(const SelectionNavigationCommandSet&) =
        default;
    SelectionNavigationCommandSet& operator=(
        const SelectionNavigationCommandSet&) = delete;

    [[nodiscard]] const std::array<SelectionCommandDescriptor, 36>&
    descriptors() const noexcept;

private:
    friend SelectionNavigationCommandSet selection_navigation_command_set();
    SelectionNavigationCommandSet();

    const std::array<SelectionCommandDescriptor, 36> descriptors_;
};

[[nodiscard]] SelectionNavigationCommandSet
selection_navigation_command_set();

struct SelectionCommandArguments {
    std::optional<DocumentPosition> position;
    std::optional<Selection> selection;
};

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

[[nodiscard]] std::optional<DocumentPosition> resolve_document_position(
    std::string_view text, ByteOffset byte_offset, int tab_width = 4);

[[nodiscard]] SelectionNavigationResult apply_selection_navigation(
    std::string_view text, const SelectionViewState& before,
    SelectionCommand command, ViewportDimensions viewport,
    SelectionCommandArguments arguments = {},
    std::span<const BracketPair> bracket_pairs = {}, int tab_width = 4,
    bool word_wrap = true);

} // namespace ssg
