#include "ssg/Commands.h"

#include <algorithm>
#include <array>

namespace ssg {
namespace {

// THE command catalog.  One row per command, hand-authored and reviewed as data.
//
// Adding a command means adding a row here and binding its handler.  Nothing
// else declares a command id: the assembled registry, the protocol argument
// codecs, palette labels, Lua host grants and the generated command reference
// are all projections of this table (doc/spec-commands.md).
//
// Columns: id, owner, label ("" = humanise the id), summary, effect, argument
// shape, surfaces {luaApi, initScript}, required capabilities.
constexpr std::array<CommandSpec, 52> kCommands{{
    {"cursor.set_position", "selection-navigation", "", "Set Position",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"cursor.left", "selection-navigation", "", "Left",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"cursor.right", "selection-navigation", "", "Right",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"cursor.word_left", "selection-navigation", "", "Word Left",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"cursor.word_right", "selection-navigation", "", "Word Right",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"cursor.line_up", "selection-navigation", "", "Line Up",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"cursor.line_down", "selection-navigation", "", "Line Down",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"cursor.line_start", "selection-navigation", "", "Line Start",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"cursor.line_end", "selection-navigation", "", "Line End",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"cursor.page_up", "selection-navigation", "", "Page Up",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"cursor.page_down", "selection-navigation", "", "Page Down",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"cursor.document_start", "selection-navigation", "", "Document Start",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"cursor.document_end", "selection-navigation", "", "Document End",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.set_range", "selection-navigation", "", "Set Range",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.add_range", "selection-navigation", "", "Add Range",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.left", "selection-navigation", "", "Left",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.right", "selection-navigation", "", "Right",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.word_left", "selection-navigation", "", "Word Left",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.word_right", "selection-navigation", "", "Word Right",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.line_up", "selection-navigation", "", "Line Up",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.line_down", "selection-navigation", "", "Line Down",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.line_start", "selection-navigation", "", "Line Start",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.line_end", "selection-navigation", "", "Line End",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.page_up", "selection-navigation", "", "Page Up",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.page_down", "selection-navigation", "", "Page Down",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.document_start", "selection-navigation", "", "Document Start",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.document_end", "selection-navigation", "", "Document End",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.all", "selection-navigation", "", "All",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.add_next_occurrence", "selection-navigation", "", "Add Next Occurrence",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.add_cursor_up", "selection-navigation", "", "Add Cursor Up",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.add_cursor_down", "selection-navigation", "", "Add Cursor Down",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.split_into_lines", "selection-navigation", "", "Split Into Lines",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"select.to_matching_bracket", "selection-navigation", "", "To Matching Bracket",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"view.reveal_caret", "selection-navigation", "", "Reveal Caret",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"view.center_caret", "selection-navigation", "", "Center Caret",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"goto.matching_bracket", "selection-navigation", "", "Matching Bracket",
     CommandEffect::Mutation, ArgumentKind::SelectionCommand, {true, false},
     {}},
    {"pane.split_horizontal", "shell-layout", "", "Split Horizontal",
     CommandEffect::Mutation, ArgumentKind::None, {true, false},
     {}},
    {"pane.split_vertical", "shell-layout", "", "Split Vertical",
     CommandEffect::Mutation, ArgumentKind::None, {true, false},
     {}},
    {"pane.close", "shell-layout", "", "Close",
     CommandEffect::Mutation, ArgumentKind::None, {true, false},
     {}},
    {"pane.next", "shell-layout", "", "Next",
     CommandEffect::Mutation, ArgumentKind::None, {true, false},
     {}},
    {"pane.previous", "shell-layout", "", "Previous",
     CommandEffect::Mutation, ArgumentKind::None, {true, false},
     {}},
    {"pane.focus_left", "shell-layout", "", "Focus Left",
     CommandEffect::Mutation, ArgumentKind::None, {true, false},
     {}},
    {"pane.focus_right", "shell-layout", "", "Focus Right",
     CommandEffect::Mutation, ArgumentKind::None, {true, false},
     {}},
    {"pane.focus_up", "shell-layout", "", "Focus Up",
     CommandEffect::Mutation, ArgumentKind::None, {true, false},
     {}},
    {"pane.focus_down", "shell-layout", "", "Focus Down",
     CommandEffect::Mutation, ArgumentKind::None, {true, false},
     {}},
    {"panel.toggle", "shell-layout", "Toggle Sidebar", "Toggle Sidebar",
     CommandEffect::Mutation, ArgumentKind::None, {true, false},
     {}},
    {"panel.focus", "shell-layout", "Focus Sidebar", "Focus Sidebar",
     CommandEffect::Mutation, ArgumentKind::None, {true, false},
     {}},
    {"panel.show_files", "shell-layout", "Show Files Sidebar", "Show Files Sidebar",
     CommandEffect::Mutation, ArgumentKind::None, {true, false},
     {}},
    {"panel.show_git_status", "shell-layout", "Show Git Sidebar", "Show Git Sidebar",
     CommandEffect::Mutation, ArgumentKind::None, {true, false},
     {}},
    {"panel.next_provider", "shell-layout", "", "Next Provider",
     CommandEffect::Mutation, ArgumentKind::None, {true, false},
     {}},
    {"panel.previous_provider", "shell-layout", "", "Previous Provider",
     CommandEffect::Mutation, ArgumentKind::None, {true, false},
     {}},
    {"view.toggle_distraction_free", "shell-layout", "", "Toggle Distraction Free",
     CommandEffect::Mutation, ArgumentKind::None, {true, false},
     {}},
}};

}  // namespace

std::span<CommandSpec const> commandCatalog() { return kCommands; }

std::vector<CommandSpec const*> commandsOwnedBy(std::string_view owner) {
    std::vector<CommandSpec const*> owned;
    for (auto const& command : kCommands) {
        if (command.owner == owner) owned.push_back(&command);
    }
    return owned;
}

CommandSpec const* findCommand(std::string_view id) {
    auto const found = std::ranges::find(kCommands, id, &CommandSpec::id);
    return found == kCommands.end() ? nullptr : &*found;
}

CommandHandle commandHandleFromIndex(std::size_t index) noexcept {
    return CommandHandle{static_cast<std::uint16_t>(index)};
}

CommandHandle commandHandle(std::string_view id) noexcept {
    auto const found = std::ranges::find(kCommands, id, &CommandSpec::id);
    if (found == kCommands.end()) return {};
    return CommandHandle{static_cast<std::uint16_t>(found - kCommands.begin())};
}

CommandSpec const* CommandHandle::spec() const noexcept {
    return valid() ? &kCommands[index_] : nullptr;
}

std::string_view CommandHandle::id() const noexcept {
    return valid() ? kCommands[index_].id : std::string_view{};
}

}  // namespace ssg
