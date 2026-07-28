#include <ssg/command_metadata.h>

#include <ssg/Commands.h>

#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace ssg {
namespace {

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
    // Authored labels live in the command catalog, beside the command they name
    // (src/Commands.cpp).  They used to sit in a separate kLabels table here,
    // which was one more place a command's data could be added or forgotten.
    if (auto const* command = findCommand(commandId);
        command != nullptr && !command->label.empty()) {
        return std::string{command->label};
    }
    return humanize(commandId);
}

}  // namespace ssg
