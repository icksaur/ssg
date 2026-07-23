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
constexpr std::array<std::pair<std::string_view, std::string_view>, 36> kLabels{{
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
    {"panel.show_files", "Show Files Sidebar"},
    {"panel.show_git_status", "Show Git Sidebar"},
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
std::string humanizeSegment(std::string_view segment) {
    std::string result;
    bool wordStart = true;
    for (char raw : segment) {
        if (raw == '_') {
            result += ' ';
            wordStart = true;
            continue;
        }
        auto ch = static_cast<unsigned char>(raw);
        if (wordStart) {
            result += static_cast<char>(std::toupper(ch));
            wordStart = false;
        } else {
            result += static_cast<char>(ch);
        }
    }
    return result;
}

std::string humanize(std::string_view commandId) {
    std::string result;
    std::size_t begin = 0;
    while (begin <= commandId.size()) {
        auto dot = commandId.find('.', begin);
        auto end = dot == std::string_view::npos ? commandId.size() : dot;
        if (!result.empty()) result += ' ';
        result += humanizeSegment(commandId.substr(begin, end - begin));
        if (dot == std::string_view::npos) break;
        begin = dot + 1;
    }
    return result;
}

}  // namespace

std::string commandLabel(std::string_view commandId) {
    for (const auto& [id, label] : kLabels) {
        if (id == commandId) return std::string{label};
    }
    return humanize(commandId);
}

}  // namespace ssg
