#include <ssg/command_metadata.h>

#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace ssg {
namespace {

// Authored labels for the commands most often surfaced in the palette.  Any id
// absent here is humanized from its segments.
constexpr std::array<std::pair<std::string_view, std::string_view>, 34> kLabels{{
    {"file.save", "Save File"},
    {"file.save_all", "Save All Files"},
    {"file.save_as", "Save File As"},
    {"file.new", "New File"},
    {"file.open", "Open File"},
    {"file.reload", "Reload File"},
    {"file.rename", "Rename File"},
    {"file.delete", "Delete File"},
    {"edit.undo", "Undo"},
    {"edit.redo", "Redo"},
    {"edit.indent", "Indent"},
    {"edit.outdent", "Outdent"},
    {"edit.toggle_comment", "Toggle Comment"},
    {"clipboard.copy", "Copy"},
    {"clipboard.cut", "Cut"},
    {"clipboard.paste", "Paste"},
    {"palette.open", "Command Palette"},
    {"panel.toggle", "Toggle Sidebar"},
    {"panel.focus", "Focus Sidebar"},
    {"tab.next", "Next Tab"},
    {"tab.previous", "Previous Tab"},
    {"tab.close", "Close Tab"},
    {"tab.close_others", "Close Other Tabs"},
    {"tab.close_all", "Close All Tabs"},
    {"settings.open", "Open Settings"},
    {"find.open", "Find"},
    {"replace.open", "Replace"},
    {"goto.file", "Go to File"},
    {"goto.line", "Go to Line"},
    {"goto.symbol", "Go to Symbol"},
    {"goto.definition", "Go to Definition"},
    {"view.toggle_word_wrap", "Toggle Word Wrap"},
    {"tree.activate", "Open Selected"},
    {"prompt.submit", "Submit Prompt"},
}};

// Title-case a lowercase segment in place-friendly form: "line_down" -> "Line
// Down".  Underscores become spaces; each word's first letter is uppercased.
std::string humanize_segment(std::string_view segment) {
    std::string result;
    bool word_start = true;
    for (char raw : segment) {
        if (raw == '_') {
            result += ' ';
            word_start = true;
            continue;
        }
        auto ch = static_cast<unsigned char>(raw);
        if (word_start) {
            result += static_cast<char>(std::toupper(ch));
            word_start = false;
        } else {
            result += static_cast<char>(ch);
        }
    }
    return result;
}

std::string humanize(std::string_view command_id) {
    std::string result;
    std::size_t begin = 0;
    while (begin <= command_id.size()) {
        auto dot = command_id.find('.', begin);
        auto end = dot == std::string_view::npos ? command_id.size() : dot;
        if (!result.empty()) result += ' ';
        result += humanize_segment(command_id.substr(begin, end - begin));
        if (dot == std::string_view::npos) break;
        begin = dot + 1;
    }
    return result;
}

}  // namespace

std::string command_label(std::string_view command_id) {
    for (const auto& [id, label] : kLabels) {
        if (id == command_id) return std::string{label};
    }
    return humanize(command_id);
}

}  // namespace ssg
