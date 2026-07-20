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
    std::size_t contentEnd;
    std::size_t end;
};

struct LineRun {
    std::size_t first;
    std::size_t last;
};

EditCommandResult failure(EditCommandError error, std::string message) {
    return {error, std::nullopt, std::nullopt, {}, std::move(message)};
}

bool validUtf8(std::string_view text) {
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

std::vector<Line> linesOf(std::string_view text) {
    std::vector<Line> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t contentEnd = start;
        while (contentEnd < text.size() && text[contentEnd] != '\r' &&
               text[contentEnd] != '\n') {
            ++contentEnd;
        }
        std::size_t end = contentEnd;
        if (end < text.size()) {
            if (text[end] == '\r' && end + 1 < text.size() &&
                text[end + 1] == '\n') {
                end += 2;
            } else {
                ++end;
            }
        }
        lines.push_back({start, contentEnd, end});
        start = end;
    }
    if (lines.empty() || lines.back().end == text.size() &&
                             lines.back().contentEnd != lines.back().end) {
        lines.push_back({text.size(), text.size(), text.size()});
    }
    return lines;
}

std::size_t lineForOffset(const std::vector<Line>& lines,
                            std::size_t offset) {
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (offset < lines[index].end ||
            (offset == lines[index].contentEnd &&
             lines[index].contentEnd < lines[index].end)) {
            return index;
        }
    }
    return lines.size() - 1;
}

std::vector<std::size_t> touchedLines(
    const SelectionSet& selections, const std::vector<Line>& lines) {
    std::vector<std::size_t> touched;
    for (const auto& selection : selections.items()) {
        const auto lower =
            static_cast<std::size_t>(selection.lower().byteOffset.value());
        const auto upper =
            static_cast<std::size_t>(selection.upper().byteOffset.value());
        const auto first = lineForOffset(lines, lower);
        std::size_t last = first;
        if (upper != lower) {
            last = lineForOffset(lines, upper);
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

std::vector<LineRun> runsOf(const std::vector<std::size_t>& touched) {
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

std::string preferredEnding(LineEnding ending) {
    switch (ending) {
    case LineEnding::Crlf:
        return "\r\n";
    case LineEnding::Cr:
        return "\r";
    case LineEnding::Lf:
    case LineEnding::Mixed:
        return "\n";
    }
    return "\n";
}

std::string content(std::string_view text, const Line& line) {
    return std::string{text.substr(line.start, line.contentEnd - line.start)};
}

std::string terminator(std::string_view text, const Line& line) {
    return std::string{
        text.substr(line.contentEnd, line.end - line.contentEnd)};
}

std::string reorderedLines(std::string_view text,
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

bool editLess(const TextEdit& left, const TextEdit& right) {
    return left.offset.value() < right.offset.value();
}

void addEdit(std::vector<TextEdit>& edits, std::size_t offset,
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

std::string applyEdits(std::string text, std::vector<TextEdit> edits) {
    std::sort(edits.begin(), edits.end(), editLess);
    for (auto iterator = edits.rbegin(); iterator != edits.rend(); ++iterator) {
        text.replace(
            static_cast<std::size_t>(iterator->offset.value()),
            static_cast<std::size_t>(iterator->erasedBytes),
            iterator->insertedText);
    }
    return text;
}

std::uint64_t remapOffset(std::uint64_t offset,
                           const std::vector<TextEdit>& edits) {
    std::int64_t delta = 0;
    for (const auto& edit : edits) {
        const auto start = edit.offset.value();
        const auto end = start + edit.erasedBytes;
        if (offset < start) {
            break;
        }
        if (edit.erasedBytes == 0 && offset == start) {
            delta += static_cast<std::int64_t>(edit.insertedText.size());
            continue;
        }
        if (offset <= end) {
            const auto relative = offset - start;
            return static_cast<std::uint64_t>(
                static_cast<std::int64_t>(start) + delta) +
                   std::min<std::uint64_t>(relative,
                                           edit.insertedText.size());
        }
        delta += static_cast<std::int64_t>(edit.insertedText.size()) -
                 static_cast<std::int64_t>(edit.erasedBytes);
    }
    return static_cast<std::uint64_t>(
        static_cast<std::int64_t>(offset) + delta);
}

std::optional<SelectionSet> remapSelections(
    const SelectionSet& before, const std::vector<TextEdit>& edits,
    std::string_view resultingText, int tabWidth) {
    std::vector<Selection> values;
    values.reserve(before.items().size());
    const auto resolveMapped = [&](std::uint64_t offset) {
        auto resolved = resolveDocumentPosition(
            resultingText, ByteOffset{offset}, tabWidth);
        while (!resolved && offset < resultingText.size()) {
            ++offset;
            resolved = resolveDocumentPosition(
                resultingText, ByteOffset{offset}, tabWidth);
        }
        return resolved;
    };
    for (const auto& selection : before.items()) {
        const auto anchor = resolveMapped(
            remapOffset(selection.anchor.byteOffset.value(), edits));
        const auto active = resolveMapped(
            remapOffset(selection.active.byteOffset.value(), edits));
        if (!anchor || !active) {
            return std::nullopt;
        }
        values.push_back({*anchor, *active});
    }
    return SelectionSet{std::move(values)};
}

bool validateSelections(std::string_view text,
                         const SelectionSet& selections, int tabWidth) {
    for (const auto& selection : selections.items()) {
        for (const auto* endpoint : {&selection.anchor, &selection.active}) {
            const auto resolved = resolveDocumentPosition(
                text, endpoint->byteOffset, tabWidth);
            if (!resolved || *resolved != *endpoint) {
                return false;
            }
        }
    }
    return true;
}

std::vector<TextEdit> lineEdits(
    const DocumentSnapshot& document, const SelectionSet& selections,
    const EditCommandSettings& settings, EditCommand command) {
    const auto& text = document.text;
    const auto lines = linesOf(text);
    const auto touched = touchedLines(selections, lines);
    const auto runs = runsOf(touched);
    std::vector<TextEdit> edits;

    if (command == EditCommand::Indent) {
        const std::string unit =
            settings.indentStyle == IndentStyle::Tabs
                ? "\t"
                : std::string(settings.indentWidth, ' ');
        for (const auto index : touched) {
            addEdit(edits, lines[index].start, 0, unit, text);
        }
    } else if (command == EditCommand::Outdent) {
        for (const auto index : touched) {
            const auto& line = lines[index];
            std::size_t erased = 0;
            if (line.start < line.contentEnd && text[line.start] == '\t') {
                erased = 1;
            } else {
                while (erased < settings.indentWidth &&
                       line.start + erased < line.contentEnd &&
                       text[line.start + erased] == ' ') {
                    ++erased;
                }
            }
            addEdit(edits, line.start, erased, {}, text);
        }
    } else if (command == EditCommand::DuplicateLine) {
        for (const auto& run : runs) {
            const auto start = lines[run.first].start;
            const auto end = lines[run.last].end;
            std::string copy = text.substr(start, end - start);
            if (lines[run.last].contentEnd == lines[run.last].end) {
                copy = preferredEnding(settings.lineEnding) + copy;
            }
            addEdit(edits, end, 0, std::move(copy), text);
        }
    } else if (command == EditCommand::MoveLineUp) {
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
                reorderedLines(text, lines, first, contents);
            addEdit(edits, lines[first].start,
                     lines[run.last].end - lines[first].start,
                     replacement, text);
        }
    } else if (command == EditCommand::MoveLineDown) {
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
                reorderedLines(text, lines, run.first, contents);
            addEdit(edits, lines[run.first].start,
                     lines[last].end - lines[run.first].start,
                     replacement, text);
        }
    } else if (command == EditCommand::DeleteLine) {
        for (const auto& run : runs) {
            std::size_t start = lines[run.first].start;
            std::size_t end = lines[run.last].end;
            if (run.last + 1 == lines.size() &&
                lines[run.last].contentEnd == lines[run.last].end &&
                run.first > 0) {
                start = lines[run.first - 1].contentEnd;
            }
            addEdit(edits, start, end - start, {}, text);
        }
    } else if (command == EditCommand::JoinLines) {
        for (const auto index : touched) {
            if (index + 1 >= lines.size()) {
                continue;
            }
            addEdit(edits, lines[index].contentEnd,
                     lines[index].end - lines[index].contentEnd, " ", text);
        }
    } else if (command == EditCommand::SortLines) {
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
                reorderedLines(text, lines, run.first, contents);
            addEdit(edits, lines[run.first].start,
                     lines[run.last].end - lines[run.first].start,
                     replacement, text);
        }
    } else if (command == EditCommand::ToggleComment) {
        bool allCommented = true;
        bool anyNonblank = false;
        for (const auto index : touched) {
            std::size_t offset = lines[index].start;
            while (offset < lines[index].contentEnd &&
                   (text[offset] == ' ' || text[offset] == '\t')) {
                ++offset;
            }
            if (offset == lines[index].contentEnd) {
                continue;
            }
            anyNonblank = true;
            if (text.substr(offset, settings.lineCommentToken.size()) !=
                settings.lineCommentToken) {
                allCommented = false;
            }
        }
        if (anyNonblank) {
            for (const auto index : touched) {
                std::size_t offset = lines[index].start;
                while (offset < lines[index].contentEnd &&
                       (text[offset] == ' ' || text[offset] == '\t')) {
                    ++offset;
                }
                if (offset == lines[index].contentEnd) {
                    continue;
                }
                addEdit(edits, offset,
                         allCommented
                             ? settings.lineCommentToken.size()
                             : 0,
                         allCommented ? std::string{}
                                       : settings.lineCommentToken,
                         text);
            }
        }
    }
    return edits;
}

std::vector<TextEdit> caseEdits(const DocumentSnapshot& document,
                                 const SelectionSet& selections,
                                 EditCommand command) {
    std::vector<TextEdit> edits;
    for (const auto& selection : selections.items()) {
        if (selection.isCaret()) {
            continue;
        }
        const auto start =
            static_cast<std::size_t>(selection.lower().byteOffset.value());
        const auto end =
            static_cast<std::size_t>(selection.upper().byteOffset.value());
        std::string replacement = document.text.substr(start, end - start);
        for (char& character : replacement) {
            const auto value = static_cast<unsigned char>(character);
            if (command == EditCommand::Uppercase) {
                if (value >= 'a' && value <= 'z') {
                    character = static_cast<char>(value - 'a' + 'A');
                }
            } else if (command == EditCommand::Lowercase) {
                if (value >= 'A' && value <= 'Z') {
                    character = static_cast<char>(value - 'A' + 'a');
                }
            } else if (value >= 'A' && value <= 'Z') {
                character = static_cast<char>(value - 'A' + 'a');
            } else if (value >= 'a' && value <= 'z') {
                character = static_cast<char>(value - 'a' + 'A');
            }
        }
        addEdit(edits, start, end - start, std::move(replacement),
                 document.text);
    }
    return edits;
}

std::vector<TextEdit> transposeEdits(const DocumentSnapshot& document,
                                      const SelectionSet& selections) {
    const auto lines = linesOf(document.text);
    std::vector<TextEdit> edits;
    for (const auto& selection : selections.items()) {
        if (!selection.isCaret()) {
            continue;
        }
        const auto caret =
            static_cast<std::size_t>(selection.active.byteOffset.value());
        auto lineIndex = lineForOffset(lines, caret);
        const bool atDocumentEnd = caret == document.text.size();
        if (atDocumentEnd && lineIndex > 0 &&
            lines[lineIndex].start == lines[lineIndex].end) {
            --lineIndex;
        }
        const auto& line = lines[lineIndex];
        const auto run = computeCellRun(
            std::string_view{document.text}.substr(
                line.start, line.contentEnd - line.start));
        if (run.spans.size() < 2) {
            continue;
        }
        std::size_t leftIndex = run.spans.size();
        if (atDocumentEnd) {
            leftIndex = run.spans.size() - 2;
        } else {
            for (std::size_t index = 0; index + 1 < run.spans.size();
                 ++index) {
                if (line.start + run.spans[index].byteOffset +
                        run.spans[index].byteLen ==
                    caret) {
                    leftIndex = index;
                    break;
                }
            }
        }
        if (leftIndex + 1 >= run.spans.size()) {
            continue;
        }
        const auto& left = run.spans[leftIndex];
        const auto& right = run.spans[leftIndex + 1];
        const auto start = line.start + left.byteOffset;
        const auto end = line.start + right.byteOffset + right.byteLen;
        bool overlaps = false;
        for (const auto& edit : edits) {
            const auto otherStart =
                static_cast<std::size_t>(edit.offset.value());
            const auto otherEnd =
                otherStart + static_cast<std::size_t>(edit.erasedBytes);
            overlaps = overlaps ||
                       (start < otherEnd && otherStart < end);
        }
        if (overlaps) {
            continue;
        }
        std::string replacement = document.text.substr(
            line.start + right.byteOffset, right.byteLen);
        replacement += document.text.substr(start, left.byteLen);
        addEdit(edits, start, end - start, std::move(replacement),
                 document.text);
    }
    return edits;
}

}  // namespace

EditCommandSuiteCommandSet::EditCommandSuiteCommandSet()
    : descriptors_{{
          {"edit.indent", EditCommand::Indent},
          {"edit.outdent", EditCommand::Outdent},
          {"edit.duplicate_line", EditCommand::DuplicateLine},
          {"edit.move_line_up", EditCommand::MoveLineUp},
          {"edit.move_line_down", EditCommand::MoveLineDown},
          {"edit.delete_line", EditCommand::DeleteLine},
          {"edit.join_lines", EditCommand::JoinLines},
          {"edit.uppercase", EditCommand::Uppercase},
          {"edit.lowercase", EditCommand::Lowercase},
          {"edit.swap_case", EditCommand::SwapCase},
          {"edit.sort_lines", EditCommand::SortLines},
          {"edit.transpose", EditCommand::Transpose},
          {"edit.toggle_comment", EditCommand::ToggleComment},
      }} {}

const std::array<EditCommandDescriptor, 13>&
EditCommandSuiteCommandSet::descriptors() const noexcept {
    return descriptors_;
}

EditCommandSuiteCommandSet editCommandSuiteCommandSet() {
    return EditCommandSuiteCommandSet{};
}

EditCommandResult applyEditCommand(
    const DocumentSnapshot& document, const SelectionSet& selections,
    EditCommandSettings settings, EditCommand command) {
    if (document.mode == DocumentMode::ReadOnly) {
        return failure(EditCommandError::ReadOnly,
                       "edit command requires an editable document");
    }
    if (document.mode == DocumentMode::Diff) {
        return failure(EditCommandError::Diff,
                       "edit command cannot mutate a diff document");
    }
    const bool validIndentStyle =
        settings.indentStyle == IndentStyle::Spaces ||
        settings.indentStyle == IndentStyle::Tabs;
    const bool validLineEnding =
        settings.lineEnding == LineEnding::Lf ||
        settings.lineEnding == LineEnding::Crlf ||
        settings.lineEnding == LineEnding::Cr ||
        settings.lineEnding == LineEnding::Mixed;
    if (!validIndentStyle || !validLineEnding ||
        settings.indentWidth < 1 || settings.indentWidth > 16 ||
        settings.tabWidth < 1 || settings.tabWidth > 16 ||
        settings.lineCommentToken.empty() ||
        !validUtf8(settings.lineCommentToken) ||
        settings.lineCommentToken.find('\0') != std::string::npos ||
        settings.lineCommentToken.find_first_of("\r\n") !=
            std::string::npos) {
        return failure(
            EditCommandError::InvalidSettings,
            "indent width must be 1..16 and comment token must be non-empty UTF-8 without line endings");
    }
    if (!validateSelections(document.text, selections,
                             static_cast<int>(settings.tabWidth))) {
        return failure(EditCommandError::InvalidSelection,
                       "selection position is stale or inconsistent");
    }

    std::vector<TextEdit> edits;
    switch (command) {
    case EditCommand::Indent:
    case EditCommand::Outdent:
    case EditCommand::DuplicateLine:
    case EditCommand::MoveLineUp:
    case EditCommand::MoveLineDown:
    case EditCommand::DeleteLine:
    case EditCommand::JoinLines:
    case EditCommand::SortLines:
    case EditCommand::ToggleComment:
        edits = lineEdits(document, selections, settings, command);
        break;
    case EditCommand::Uppercase:
    case EditCommand::Lowercase:
    case EditCommand::SwapCase:
        edits = caseEdits(document, selections, command);
        break;
    case EditCommand::Transpose:
        edits = transposeEdits(document, selections);
        break;
    default:
        return failure(EditCommandError::UnknownCommand,
                       "unknown edit command");
    }

    std::sort(edits.begin(), edits.end(), editLess);
    const auto resultingText = applyEdits(document.text, edits);
    if (resultingText == document.text) {
        return {EditCommandError::None, std::nullopt, selections,
                document.text, {}};
    }
    const auto remapped = remapSelections(
        selections, edits, resultingText,
        static_cast<int>(settings.tabWidth));
    if (!remapped) {
        return failure(EditCommandError::InvalidSelection,
                       "edit result could not resolve selection positions");
    }
    return {
        EditCommandError::None,
        EditTransaction{document.revision, edits},
        std::move(remapped),
        resultingText,
        {},
    };
}

}  // namespace ssg
