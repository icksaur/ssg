#include <ssg/edit_commands.h>

#include <ssg/layout.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ssg {
namespace {

struct Line {
    std::size_t start;
    std::size_t content_end;
    std::size_t end;
};

struct LineRun {
    std::size_t first;
    std::size_t last;
};

EditCommandResult failure(EditCommandError error, std::string message) {
    return {error, std::nullopt, std::nullopt, {}, std::move(message)};
}

bool valid_utf8(std::string_view text) {
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        std::uint32_t value = 0;
        if (first <= 0x7F) {
            length = 1;
            value = first;
        } else if (first >= 0xC2 && first <= 0xDF) {
            length = 2;
            value = first & 0x1F;
        } else if (first >= 0xE0 && first <= 0xEF) {
            length = 3;
            value = first & 0x0F;
        } else if (first >= 0xF0 && first <= 0xF4) {
            length = 4;
            value = first & 0x07;
        } else {
            return false;
        }
        if (index + length > text.size()) {
            return false;
        }
        for (std::size_t byte = 1; byte < length; ++byte) {
            const auto continuation =
                static_cast<unsigned char>(text[index + byte]);
            if ((continuation & 0xC0) != 0x80) {
                return false;
            }
            value = (value << 6) | (continuation & 0x3F);
        }
        if ((length == 3 && value < 0x800) ||
            (length == 4 && value < 0x10000) ||
            (value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF) {
            return false;
        }
        index += length;
    }
    return true;
}

std::vector<Line> lines_of(std::string_view text) {
    std::vector<Line> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t content_end = start;
        while (content_end < text.size() && text[content_end] != '\r' &&
               text[content_end] != '\n') {
            ++content_end;
        }
        std::size_t end = content_end;
        if (end < text.size()) {
            if (text[end] == '\r' && end + 1 < text.size() &&
                text[end + 1] == '\n') {
                end += 2;
            } else {
                ++end;
            }
        }
        lines.push_back({start, content_end, end});
        start = end;
    }
    if (lines.empty() || lines.back().end == text.size() &&
                             lines.back().content_end != lines.back().end) {
        lines.push_back({text.size(), text.size(), text.size()});
    }
    return lines;
}

std::size_t line_for_offset(const std::vector<Line>& lines,
                            std::size_t offset) {
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (offset < lines[index].end ||
            (offset == lines[index].content_end &&
             lines[index].content_end < lines[index].end)) {
            return index;
        }
    }
    return lines.size() - 1;
}

std::vector<std::size_t> touched_lines(
    const SelectionSet& selections, const std::vector<Line>& lines) {
    std::vector<std::size_t> touched;
    for (const auto& selection : selections.items()) {
        const auto lower =
            static_cast<std::size_t>(selection.lower().byte_offset.value());
        const auto upper =
            static_cast<std::size_t>(selection.upper().byte_offset.value());
        const auto first = line_for_offset(lines, lower);
        std::size_t last = first;
        if (upper != lower) {
            last = line_for_offset(lines, upper);
            if (last > first && upper == lines[last].start) {
                --last;
            }
        }
        for (std::size_t line = first; line <= last; ++line) {
            touched.push_back(line);
        }
    }
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    return touched;
}

std::vector<LineRun> runs_of(const std::vector<std::size_t>& touched) {
    std::vector<LineRun> runs;
    for (const auto line : touched) {
        if (runs.empty() || line != runs.back().last + 1) {
            runs.push_back({line, line});
        } else {
            runs.back().last = line;
        }
    }
    return runs;
}

std::string preferred_ending(LineEnding ending) {
    switch (ending) {
    case LineEnding::crlf:
        return "\r\n";
    case LineEnding::cr:
        return "\r";
    case LineEnding::lf:
    case LineEnding::mixed:
        return "\n";
    }
    return "\n";
}

std::string content(std::string_view text, const Line& line) {
    return std::string{text.substr(line.start, line.content_end - line.start)};
}

std::string terminator(std::string_view text, const Line& line) {
    return std::string{
        text.substr(line.content_end, line.end - line.content_end)};
}

std::string reordered_lines(std::string_view text,
                            const std::vector<Line>& lines,
                            std::size_t first,
                            const std::vector<std::string>& contents) {
    std::string replacement;
    for (std::size_t index = 0; index < contents.size(); ++index) {
        replacement += contents[index];
        replacement += terminator(text, lines[first + index]);
    }
    return replacement;
}

bool edit_less(const TextEdit& left, const TextEdit& right) {
    return left.offset.value() < right.offset.value();
}

void add_edit(std::vector<TextEdit>& edits, std::size_t offset,
              std::size_t erased, std::string inserted,
              std::string_view original) {
    if (erased == inserted.size() &&
        original.substr(offset, erased) == inserted) {
        return;
    }
    if (erased == 0 && inserted.empty()) {
        return;
    }
    edits.push_back(
        {ByteOffset{static_cast<std::uint64_t>(offset)},
         static_cast<std::uint64_t>(erased), std::move(inserted)});
}

std::string apply_edits(std::string text, std::vector<TextEdit> edits) {
    std::sort(edits.begin(), edits.end(), edit_less);
    for (auto iterator = edits.rbegin(); iterator != edits.rend(); ++iterator) {
        text.replace(
            static_cast<std::size_t>(iterator->offset.value()),
            static_cast<std::size_t>(iterator->erased_bytes),
            iterator->inserted_text);
    }
    return text;
}

std::uint64_t remap_offset(std::uint64_t offset,
                           const std::vector<TextEdit>& edits) {
    std::int64_t delta = 0;
    for (const auto& edit : edits) {
        const auto start = edit.offset.value();
        const auto end = start + edit.erased_bytes;
        if (offset < start) {
            break;
        }
        if (edit.erased_bytes == 0 && offset == start) {
            delta += static_cast<std::int64_t>(edit.inserted_text.size());
            continue;
        }
        if (offset <= end) {
            const auto relative = offset - start;
            return static_cast<std::uint64_t>(
                static_cast<std::int64_t>(start) + delta) +
                   std::min<std::uint64_t>(relative,
                                           edit.inserted_text.size());
        }
        delta += static_cast<std::int64_t>(edit.inserted_text.size()) -
                 static_cast<std::int64_t>(edit.erased_bytes);
    }
    return static_cast<std::uint64_t>(
        static_cast<std::int64_t>(offset) + delta);
}

std::optional<SelectionSet> remap_selections(
    const SelectionSet& before, const std::vector<TextEdit>& edits,
    std::string_view resulting_text, int tab_width) {
    std::vector<Selection> values;
    values.reserve(before.items().size());
    const auto resolve_mapped = [&](std::uint64_t offset) {
        auto resolved = resolve_document_position(
            resulting_text, ByteOffset{offset}, tab_width);
        while (!resolved && offset < resulting_text.size()) {
            ++offset;
            resolved = resolve_document_position(
                resulting_text, ByteOffset{offset}, tab_width);
        }
        return resolved;
    };
    for (const auto& selection : before.items()) {
        const auto anchor = resolve_mapped(
            remap_offset(selection.anchor.byte_offset.value(), edits));
        const auto active = resolve_mapped(
            remap_offset(selection.active.byte_offset.value(), edits));
        if (!anchor || !active) {
            return std::nullopt;
        }
        values.push_back({*anchor, *active});
    }
    return SelectionSet{std::move(values)};
}

bool validate_selections(std::string_view text,
                         const SelectionSet& selections, int tab_width) {
    for (const auto& selection : selections.items()) {
        for (const auto* endpoint : {&selection.anchor, &selection.active}) {
            const auto resolved = resolve_document_position(
                text, endpoint->byte_offset, tab_width);
            if (!resolved || *resolved != *endpoint) {
                return false;
            }
        }
    }
    return true;
}

std::vector<TextEdit> line_edits(
    const DocumentSnapshot& document, const SelectionSet& selections,
    const EditCommandSettings& settings, EditCommand command) {
    const auto& text = document.text;
    const auto lines = lines_of(text);
    const auto touched = touched_lines(selections, lines);
    const auto runs = runs_of(touched);
    std::vector<TextEdit> edits;

    if (command == EditCommand::indent) {
        const std::string unit =
            settings.indent_style == IndentStyle::tabs
                ? "\t"
                : std::string(settings.indent_width, ' ');
        for (const auto index : touched) {
            add_edit(edits, lines[index].start, 0, unit, text);
        }
    } else if (command == EditCommand::outdent) {
        for (const auto index : touched) {
            const auto& line = lines[index];
            std::size_t erased = 0;
            if (line.start < line.content_end && text[line.start] == '\t') {
                erased = 1;
            } else {
                while (erased < settings.indent_width &&
                       line.start + erased < line.content_end &&
                       text[line.start + erased] == ' ') {
                    ++erased;
                }
            }
            add_edit(edits, line.start, erased, {}, text);
        }
    } else if (command == EditCommand::duplicate_line) {
        for (const auto& run : runs) {
            const auto start = lines[run.first].start;
            const auto end = lines[run.last].end;
            std::string copy = text.substr(start, end - start);
            if (lines[run.last].content_end == lines[run.last].end) {
                copy = preferred_ending(settings.line_ending) + copy;
            }
            add_edit(edits, end, 0, std::move(copy), text);
        }
    } else if (command == EditCommand::move_line_up) {
        for (const auto& run : runs) {
            if (run.first == 0) {
                continue;
            }
            const auto first = run.first - 1;
            std::vector<std::string> contents;
            for (std::size_t index = run.first; index <= run.last; ++index) {
                contents.push_back(content(text, lines[index]));
            }
            contents.push_back(content(text, lines[first]));
            const auto replacement =
                reordered_lines(text, lines, first, contents);
            add_edit(edits, lines[first].start,
                     lines[run.last].end - lines[first].start,
                     replacement, text);
        }
    } else if (command == EditCommand::move_line_down) {
        for (const auto& run : runs) {
            if (run.last + 1 >= lines.size()) {
                continue;
            }
            const auto last = run.last + 1;
            std::vector<std::string> contents{
                content(text, lines[last])};
            for (std::size_t index = run.first; index <= run.last; ++index) {
                contents.push_back(content(text, lines[index]));
            }
            const auto replacement =
                reordered_lines(text, lines, run.first, contents);
            add_edit(edits, lines[run.first].start,
                     lines[last].end - lines[run.first].start,
                     replacement, text);
        }
    } else if (command == EditCommand::delete_line) {
        for (const auto& run : runs) {
            std::size_t start = lines[run.first].start;
            std::size_t end = lines[run.last].end;
            if (run.last + 1 == lines.size() &&
                lines[run.last].content_end == lines[run.last].end &&
                run.first > 0) {
                start = lines[run.first - 1].content_end;
            }
            add_edit(edits, start, end - start, {}, text);
        }
    } else if (command == EditCommand::join_lines) {
        for (const auto index : touched) {
            if (index + 1 >= lines.size()) {
                continue;
            }
            add_edit(edits, lines[index].content_end,
                     lines[index].end - lines[index].content_end, " ", text);
        }
    } else if (command == EditCommand::sort_lines) {
        for (const auto& run : runs) {
            if (run.first == run.last) {
                continue;
            }
            std::vector<std::string> contents;
            for (std::size_t index = run.first; index <= run.last; ++index) {
                contents.push_back(content(text, lines[index]));
            }
            std::stable_sort(contents.begin(), contents.end(),
                             [](const std::string& left,
                                const std::string& right) {
                                 return std::lexicographical_compare(
                                     left.begin(), left.end(), right.begin(),
                                     right.end(),
                                     [](unsigned char a, unsigned char b) {
                                         return a < b;
                                     });
                             });
            const auto replacement =
                reordered_lines(text, lines, run.first, contents);
            add_edit(edits, lines[run.first].start,
                     lines[run.last].end - lines[run.first].start,
                     replacement, text);
        }
    } else if (command == EditCommand::toggle_comment) {
        bool all_commented = true;
        bool any_nonblank = false;
        for (const auto index : touched) {
            std::size_t offset = lines[index].start;
            while (offset < lines[index].content_end &&
                   (text[offset] == ' ' || text[offset] == '\t')) {
                ++offset;
            }
            if (offset == lines[index].content_end) {
                continue;
            }
            any_nonblank = true;
            if (text.substr(offset, settings.line_comment_token.size()) !=
                settings.line_comment_token) {
                all_commented = false;
            }
        }
        if (any_nonblank) {
            for (const auto index : touched) {
                std::size_t offset = lines[index].start;
                while (offset < lines[index].content_end &&
                       (text[offset] == ' ' || text[offset] == '\t')) {
                    ++offset;
                }
                if (offset == lines[index].content_end) {
                    continue;
                }
                add_edit(edits, offset,
                         all_commented
                             ? settings.line_comment_token.size()
                             : 0,
                         all_commented ? std::string{}
                                       : settings.line_comment_token,
                         text);
            }
        }
    }
    return edits;
}

std::vector<TextEdit> case_edits(const DocumentSnapshot& document,
                                 const SelectionSet& selections,
                                 EditCommand command) {
    std::vector<TextEdit> edits;
    for (const auto& selection : selections.items()) {
        if (selection.is_caret()) {
            continue;
        }
        const auto start =
            static_cast<std::size_t>(selection.lower().byte_offset.value());
        const auto end =
            static_cast<std::size_t>(selection.upper().byte_offset.value());
        std::string replacement = document.text.substr(start, end - start);
        for (char& character : replacement) {
            const auto value = static_cast<unsigned char>(character);
            if (command == EditCommand::uppercase) {
                if (value >= 'a' && value <= 'z') {
                    character = static_cast<char>(value - 'a' + 'A');
                }
            } else if (command == EditCommand::lowercase) {
                if (value >= 'A' && value <= 'Z') {
                    character = static_cast<char>(value - 'A' + 'a');
                }
            } else if (value >= 'A' && value <= 'Z') {
                character = static_cast<char>(value - 'A' + 'a');
            } else if (value >= 'a' && value <= 'z') {
                character = static_cast<char>(value - 'a' + 'A');
            }
        }
        add_edit(edits, start, end - start, std::move(replacement),
                 document.text);
    }
    return edits;
}

std::vector<TextEdit> transpose_edits(const DocumentSnapshot& document,
                                      const SelectionSet& selections) {
    const auto lines = lines_of(document.text);
    std::vector<TextEdit> edits;
    for (const auto& selection : selections.items()) {
        if (!selection.is_caret()) {
            continue;
        }
        const auto caret =
            static_cast<std::size_t>(selection.active.byte_offset.value());
        auto line_index = line_for_offset(lines, caret);
        const bool at_document_end = caret == document.text.size();
        if (at_document_end && line_index > 0 &&
            lines[line_index].start == lines[line_index].end) {
            --line_index;
        }
        const auto& line = lines[line_index];
        const auto run = compute_cell_run(
            std::string_view{document.text}.substr(
                line.start, line.content_end - line.start));
        if (run.spans.size() < 2) {
            continue;
        }
        std::size_t left_index = run.spans.size();
        if (at_document_end) {
            left_index = run.spans.size() - 2;
        } else {
            for (std::size_t index = 0; index + 1 < run.spans.size();
                 ++index) {
                if (line.start + run.spans[index].byte_offset +
                        run.spans[index].byte_len ==
                    caret) {
                    left_index = index;
                    break;
                }
            }
        }
        if (left_index + 1 >= run.spans.size()) {
            continue;
        }
        const auto& left = run.spans[left_index];
        const auto& right = run.spans[left_index + 1];
        const auto start = line.start + left.byte_offset;
        const auto end = line.start + right.byte_offset + right.byte_len;
        bool overlaps = false;
        for (const auto& edit : edits) {
            const auto other_start =
                static_cast<std::size_t>(edit.offset.value());
            const auto other_end =
                other_start + static_cast<std::size_t>(edit.erased_bytes);
            overlaps = overlaps ||
                       (start < other_end && other_start < end);
        }
        if (overlaps) {
            continue;
        }
        std::string replacement = document.text.substr(
            line.start + right.byte_offset, right.byte_len);
        replacement += document.text.substr(start, left.byte_len);
        add_edit(edits, start, end - start, std::move(replacement),
                 document.text);
    }
    return edits;
}

}  // namespace

EditCommandSuiteCommandSet::EditCommandSuiteCommandSet()
    : descriptors_{{
          {"edit.indent", EditCommand::indent},
          {"edit.outdent", EditCommand::outdent},
          {"edit.duplicate_line", EditCommand::duplicate_line},
          {"edit.move_line_up", EditCommand::move_line_up},
          {"edit.move_line_down", EditCommand::move_line_down},
          {"edit.delete_line", EditCommand::delete_line},
          {"edit.join_lines", EditCommand::join_lines},
          {"edit.uppercase", EditCommand::uppercase},
          {"edit.lowercase", EditCommand::lowercase},
          {"edit.swap_case", EditCommand::swap_case},
          {"edit.sort_lines", EditCommand::sort_lines},
          {"edit.transpose", EditCommand::transpose},
          {"edit.toggle_comment", EditCommand::toggle_comment},
      }} {}

const std::array<EditCommandDescriptor, 13>&
EditCommandSuiteCommandSet::descriptors() const noexcept {
    return descriptors_;
}

EditCommandSuiteCommandSet edit_command_suite_command_set() {
    return EditCommandSuiteCommandSet{};
}

EditCommandResult apply_edit_command(
    const DocumentSnapshot& document, const SelectionSet& selections,
    EditCommandSettings settings, EditCommand command) {
    if (document.mode == DocumentMode::read_only) {
        return failure(EditCommandError::read_only,
                       "edit command requires an editable document");
    }
    if (document.mode == DocumentMode::diff) {
        return failure(EditCommandError::diff,
                       "edit command cannot mutate a diff document");
    }
    const bool valid_indent_style =
        settings.indent_style == IndentStyle::spaces ||
        settings.indent_style == IndentStyle::tabs;
    const bool valid_line_ending =
        settings.line_ending == LineEnding::lf ||
        settings.line_ending == LineEnding::crlf ||
        settings.line_ending == LineEnding::cr ||
        settings.line_ending == LineEnding::mixed;
    if (!valid_indent_style || !valid_line_ending ||
        settings.indent_width < 1 || settings.indent_width > 16 ||
        settings.tab_width < 1 || settings.tab_width > 16 ||
        settings.line_comment_token.empty() ||
        !valid_utf8(settings.line_comment_token) ||
        settings.line_comment_token.find('\0') != std::string::npos ||
        settings.line_comment_token.find_first_of("\r\n") !=
            std::string::npos) {
        return failure(
            EditCommandError::invalid_settings,
            "indent width must be 1..16 and comment token must be non-empty UTF-8 without line endings");
    }
    if (!validate_selections(document.text, selections,
                             static_cast<int>(settings.tab_width))) {
        return failure(EditCommandError::invalid_selection,
                       "selection position is stale or inconsistent");
    }

    std::vector<TextEdit> edits;
    switch (command) {
    case EditCommand::indent:
    case EditCommand::outdent:
    case EditCommand::duplicate_line:
    case EditCommand::move_line_up:
    case EditCommand::move_line_down:
    case EditCommand::delete_line:
    case EditCommand::join_lines:
    case EditCommand::sort_lines:
    case EditCommand::toggle_comment:
        edits = line_edits(document, selections, settings, command);
        break;
    case EditCommand::uppercase:
    case EditCommand::lowercase:
    case EditCommand::swap_case:
        edits = case_edits(document, selections, command);
        break;
    case EditCommand::transpose:
        edits = transpose_edits(document, selections);
        break;
    default:
        return failure(EditCommandError::unknown_command,
                       "unknown edit command");
    }

    std::sort(edits.begin(), edits.end(), edit_less);
    const auto resulting_text = apply_edits(document.text, edits);
    if (resulting_text == document.text) {
        return {EditCommandError::none, std::nullopt, selections,
                document.text, {}};
    }
    const auto remapped = remap_selections(
        selections, edits, resulting_text,
        static_cast<int>(settings.tab_width));
    if (!remapped) {
        return failure(EditCommandError::invalid_selection,
                       "edit result could not resolve selection positions");
    }
    return {
        EditCommandError::none,
        EditTransaction{document.revision, edits},
        std::move(remapped),
        resulting_text,
        {},
    };
}

}  // namespace ssg
