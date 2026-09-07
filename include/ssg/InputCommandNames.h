#pragma once

#include <array>
#include <string_view>

namespace ssg::input_command_names {

inline constexpr std::string_view kPaletteClose = "palette.close";
inline constexpr std::string_view kViewScrollLines = "view.scroll_lines";
inline constexpr std::string_view kTreeScroll = "tree.scroll";
inline constexpr std::string_view kViewScrollToFraction =
    "view.scroll_to_fraction";
inline constexpr std::string_view kTreeScrollToFraction =
    "tree.scroll_to_fraction";
inline constexpr std::string_view kSelectWordAtPosition =
    "select.word_at_position";
inline constexpr std::string_view kSelectSetRanges = "select.set_ranges";
inline constexpr std::string_view kSelectAddRange = "select.add_range";
inline constexpr std::string_view kCursorSetPosition = "cursor.set_position";
inline constexpr std::string_view kSelectSetRange = "select.set_range";
inline constexpr std::string_view kFollowEditsPause = "follow_edits.pause";
inline constexpr std::string_view kTabActivate = "tab.activate";
inline constexpr std::string_view kTabClose = "tab.close";
inline constexpr std::string_view kTreeActivateNode = "tree.activate_node";
inline constexpr std::string_view kPickerSubmit = "picker.submit";
inline constexpr std::string_view kExternalInvokeAction =
    "external.invoke_action";
inline constexpr std::string_view kPromptSubmit = "prompt.submit";
inline constexpr std::string_view kPromptNext = "prompt.next";
inline constexpr std::string_view kPromptPrevious = "prompt.previous";
inline constexpr std::string_view kPromptCancel = "prompt.cancel";
inline constexpr std::string_view kPaletteNext = "palette.next";
inline constexpr std::string_view kPalettePrevious = "palette.previous";
inline constexpr std::string_view kClipboardPaste = "clipboard.paste";

inline constexpr std::array kAll{
    kPaletteClose,
    kViewScrollLines,
    kTreeScroll,
    kViewScrollToFraction,
    kTreeScrollToFraction,
    kSelectWordAtPosition,
    kSelectSetRanges,
    kSelectAddRange,
    kCursorSetPosition,
    kSelectSetRange,
    kFollowEditsPause,
    kTabActivate,
    kTabClose,
    kTreeActivateNode,
    kPickerSubmit,
    kExternalInvokeAction,
    kPromptSubmit,
    kPromptNext,
    kPromptPrevious,
    kPromptCancel,
    kPaletteNext,
    kPalettePrevious,
    kClipboardPaste,
};

}  // namespace ssg::input_command_names
