#pragma once

#include <array>
#include <string_view>

namespace ssg::input_command_names {

inline constexpr std::string_view kPaletteClose = "palette.close";
inline constexpr std::string_view kFollowEditsPause = "follow_edits.pause";
inline constexpr std::string_view kPickerSubmit = "picker.submit";
inline constexpr std::string_view kPromptSubmit = "prompt.submit";
inline constexpr std::string_view kPromptNext = "prompt.next";
inline constexpr std::string_view kPromptPrevious = "prompt.previous";
inline constexpr std::string_view kPromptCancel = "prompt.cancel";
inline constexpr std::string_view kPaletteNext = "palette.next";
inline constexpr std::string_view kPalettePrevious = "palette.previous";
inline constexpr std::string_view kClipboardPaste = "clipboard.paste";

inline constexpr std::array kAll{
    kPaletteClose,
    kFollowEditsPause,
    kPickerSubmit,
    kPromptSubmit,
    kPromptNext,
    kPromptPrevious,
    kPromptCancel,
    kPaletteNext,
    kPalettePrevious,
    kClipboardPaste,
};

}  // namespace ssg::input_command_names
