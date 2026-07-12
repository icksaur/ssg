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
    cursor_set_position,
    cursor_left,
    cursor_right,
    cursor_word_left,
    cursor_word_right,
    cursor_line_up,
    cursor_line_down,
    cursor_line_start,
    cursor_line_end,
    cursor_page_up,
    cursor_page_down,
    cursor_document_start,
    cursor_document_end,
    select_set_range,
    select_add_range,
    select_left,
    select_right,
    select_word_left,
    select_word_right,
    select_line_up,
    select_line_down,
    select_line_start,
    select_line_end,
    select_page_up,
    select_page_down,
    select_document_start,
    select_document_end,
    select_all,
    select_add_next_occurrence,
    select_add_cursor_up,
    select_add_cursor_down,
    select_split_into_lines,
    select_to_matching_bracket,
    goto_matching_bracket,
    view_reveal_caret,
    view_center_caret,
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
    none,
    missing_argument,
    invalid_position,
    invalid_bracket_pairs,
    invalid_tab_width,
};

struct SelectionNavigationResult {
    SelectionNavigationError error;
    SelectionViewDelta delta;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == SelectionNavigationError::none;
    }
};

[[nodiscard]] std::optional<DocumentPosition> resolve_document_position(
    std::string_view text, ByteOffset byte_offset, int tab_width = 4);

[[nodiscard]] SelectionNavigationResult apply_selection_navigation(
    std::string_view text, const SelectionViewState& before,
    SelectionCommand command, ViewportDimensions viewport,
    SelectionCommandArguments arguments = {},
    std::span<const BracketPair> bracket_pairs = {}, int tab_width = 4);

} // namespace ssg
