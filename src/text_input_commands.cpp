#include <ssg/text_input_commands.h>

#include <ssg/grapheme_layout.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ssg {
namespace {

struct PendingEdit {
    std::size_t start;
    std::size_t end;
    std::string inserted;
    std::size_t action;
};

enum class SegmentCategory : std::uint8_t {
    Word,
    Space,
    Punctuation,
};

TextInputResult failure(TextInputError error, std::string message) {
    return TextInputResult{error, std::nullopt, std::nullopt, {},
                           std::move(message)};
}

bool validUtf8(std::string_view text) {
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first == 0) {
            return false;
        }
        std::size_t length = 0;
        std::uint32_t value = 0;
        if (first <= 0x7f) {
            length = 1;
            value = first;
        } else if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
            value = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
            value = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
            value = first & 0x07;
        } else {
            return false;
        }
        if (index + length > text.size()) {
            return false;
        }
        for (std::size_t tail = 1; tail < length; ++tail) {
            const auto byte =
                static_cast<unsigned char>(text[index + tail]);
            if ((byte & 0xc0) != 0x80) {
                return false;
            }
            value = (value << 6) | (byte & 0x3f);
        }
        if ((length == 3 && value < 0x800) ||
            (length == 4 && value < 0x10000) ||
            (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) {
            return false;
        }
        index += length;
    }
    return true;
}

std::vector<std::size_t> graphemeBoundaries(std::string_view text,
                                             int tabWidth) {
    std::vector<std::size_t> boundaries{0};
    std::size_t lineStart = 0;
    while (lineStart < text.size()) {
        auto lineEnd = lineStart;
        while (lineEnd < text.size() && text[lineEnd] != '\r' &&
               text[lineEnd] != '\n') {
            ++lineEnd;
        }
        const auto run =
            GraphemeLayout{}.computeRun(text.substr(lineStart, lineEnd - lineStart),
                             tabWidth);
        for (const auto& span : run.spans) {
            boundaries.push_back(lineStart + span.byteOffset + span.byteLen);
        }
        if (lineEnd == text.size()) {
            break;
        }
        if (text[lineEnd] == '\r' && lineEnd + 1 < text.size() &&
            text[lineEnd + 1] == '\n') {
            lineStart = lineEnd + 2;
        } else {
            lineStart = lineEnd + 1;
        }
        boundaries.push_back(lineStart);
    }
    return boundaries;
}

std::size_t previousBoundary(const std::vector<std::size_t>& boundaries,
                              std::size_t offset) {
    const auto found = std::lower_bound(boundaries.begin(), boundaries.end(),
                                        offset);
    return found == boundaries.begin() ? 0 : *std::prev(found);
}

std::size_t nextBoundary(const std::vector<std::size_t>& boundaries,
                          std::size_t offset) {
    const auto found =
        std::upper_bound(boundaries.begin(), boundaries.end(), offset);
    return found == boundaries.end() ? offset : *found;
}

SegmentCategory category(std::string_view text, std::size_t start) {
    const auto first = static_cast<unsigned char>(text[start]);
    if (first >= 0x80 || (first >= 'a' && first <= 'z') ||
        (first >= 'A' && first <= 'Z') ||
        (first >= '0' && first <= '9') || first == '_') {
        return SegmentCategory::Word;
    }
    if (first == ' ' || first == '\t' || first == '\r' || first == '\n') {
        return SegmentCategory::Space;
    }
    return SegmentCategory::Punctuation;
}

std::size_t wordLeft(std::string_view text,
                      const std::vector<std::size_t>& boundaries,
                      std::size_t offset) {
    if (offset == 0) {
        return 0;
    }
    auto position = previousBoundary(boundaries, offset);
    const auto target = category(text, position);
    if (target == SegmentCategory::Punctuation) {
        return position;
    }
    while (position > 0) {
        const auto before = previousBoundary(boundaries, position);
        if (category(text, before) != target) {
            break;
        }
        position = before;
    }
    return position;
}

std::size_t wordRight(std::string_view text,
                       const std::vector<std::size_t>& boundaries,
                       std::size_t offset) {
    if (offset >= text.size()) {
        return offset;
    }
    const auto target = category(text, offset);
    auto position = nextBoundary(boundaries, offset);
    while (position < text.size() && category(text, position) == target) {
        position = nextBoundary(boundaries, position);
    }
    return position;
}

std::pair<std::size_t, std::size_t> lineBounds(std::string_view text,
                                                std::size_t offset) {
    auto start = offset;
    while (start > 0 && text[start - 1] != '\r' && text[start - 1] != '\n') {
        --start;
    }
    auto end = offset;
    while (end < text.size() && text[end] != '\r' && text[end] != '\n') {
        ++end;
    }
    return {start, end};
}

std::string terminatorFor(std::string_view text, std::size_t offset,
                           LineEnding configured) {
    switch (configured) {
    case LineEnding::Lf: return "\n";
    case LineEnding::Crlf: return "\r\n";
    case LineEnding::Cr: return "\r";
    case LineEnding::Mixed: {
        const auto end = lineBounds(text, offset).second;
        if (end == text.size()) {
            return "\n";
        }
        if (text[end] == '\r' && end + 1 < text.size() &&
            text[end + 1] == '\n') {
            return "\r\n";
        }
        return std::string(1, text[end]);
    }
    }
    return {};
}

std::string indentationFor(std::string_view text, std::size_t offset,
                            TextInputSettings settings) {
    if (!settings.autoIndent) {
        return {};
    }
    const auto start = lineBounds(text, offset).first;
    std::uint64_t columns = 0;
    auto cursor = start;
    while (cursor < text.size()) {
        if (text[cursor] == ' ') {
            ++columns;
            ++cursor;
        } else if (text[cursor] == '\t') {
            columns += settings.indentWidth -
                       (columns % settings.indentWidth);
            ++cursor;
        } else {
            break;
        }
    }
    if (settings.indentStyle == IndentStyle::Spaces) {
        return std::string(static_cast<std::size_t>(columns), ' ');
    }
    const auto tabs = columns / settings.indentWidth;
    const auto spaces = columns % settings.indentWidth;
    return std::string(static_cast<std::size_t>(tabs), '\t') +
           std::string(static_cast<std::size_t>(spaces), ' ');
}

bool validPosition(std::string_view text, const DocumentPosition& position,
                    int tabWidth) {
    const auto resolved =
        ssg::SelectionNavigator::resolvePosition(text, position.byteOffset, tabWidth);
    return resolved.has_value() && *resolved == position;
}

void normalizeEdits(std::vector<PendingEdit>& edits) {
    std::sort(edits.begin(), edits.end(),
              [](const PendingEdit& left, const PendingEdit& right) {
                  if (left.start != right.start) {
                      return left.start < right.start;
                  }
                  return left.end < right.end;
              });
    std::vector<PendingEdit> normalized;
    for (auto& edit : edits) {
        if (!normalized.empty() &&
            (edit.start < normalized.back().end ||
             edit.start == normalized.back().start)) {
            if (edit.inserted.empty() &&
                normalized.back().inserted.empty()) {
                normalized.back().end =
                    std::max(normalized.back().end, edit.end);
            } else if (edit.start == normalized.back().start &&
                       edit.end > normalized.back().end) {
                normalized.back() = std::move(edit);
            }
        } else {
            normalized.push_back(std::move(edit));
        }
    }
    edits = std::move(normalized);
}

}  // namespace

TextInputCommandSet::TextInputCommandSet()
    : descriptors_{{
          {"text.insert", TextInputCommand::Insert},
          {"text.newline", TextInputCommand::Newline},
          {"text.delete_backward", TextInputCommand::DeleteBackward},
          {"text.delete_forward", TextInputCommand::DeleteForward},
          {"text.delete_word_backward",
           TextInputCommand::DeleteWordBackward},
          {"text.delete_word_forward", TextInputCommand::DeleteWordForward},
      }} {}

const std::array<TextInputCommandDescriptor, 6>&
TextInputCommandSet::descriptors() const noexcept {
    return descriptors_;
}

TextInputCommandSet textInputCommandSet() {
    return TextInputCommandSet{};
}

TextInputResult TextInputInterpreter::apply(
    const DocumentSnapshot& document, const SelectionSet& selections,
    TextInputSettings settings, TextInputCommand command,
    TextInputArguments arguments) const {
    if (document.mode == DocumentMode::ReadOnly) {
        return failure(TextInputError::ReadOnly,
                       "text input requires an editable document");
    }
    if (document.mode == DocumentMode::Diff) {
        return failure(TextInputError::Diff,
                       "text input is unavailable in diff mode");
    }
    if (settings.indentWidth < 1 || settings.indentWidth > 16 ||
        (settings.indentStyle != IndentStyle::Spaces &&
         settings.indentStyle != IndentStyle::Tabs) ||
        static_cast<std::uint8_t>(settings.lineEnding) >
            static_cast<std::uint8_t>(LineEnding::Mixed)) {
        return failure(TextInputError::InvalidSettings,
                       "text input settings are outside their valid range");
    }
    const auto tabWidth = static_cast<int>(settings.indentWidth);
    for (const auto& selection : selections.items()) {
        if (!validPosition(document.text, selection.anchor, tabWidth) ||
            !validPosition(document.text, selection.active, tabWidth)) {
            return failure(TextInputError::InvalidSelection,
                           "selection position is inconsistent with document");
        }
    }
    if (command == TextInputCommand::Insert && !validUtf8(arguments.text)) {
        return failure(TextInputError::InvalidUtf8,
                       "inserted text must be well-formed UTF-8 without NUL");
    }
    if (static_cast<std::uint8_t>(command) >
        static_cast<std::uint8_t>(TextInputCommand::DeleteWordForward)) {
        return failure(TextInputError::UnknownCommand,
                       "text input command is not recognized");
    }

    const auto boundaries = graphemeBoundaries(document.text, tabWidth);
    for (const auto& selection : selections.items()) {
        const auto anchor =
            static_cast<std::size_t>(selection.anchor.byteOffset.value());
        const auto active =
            static_cast<std::size_t>(selection.active.byteOffset.value());
        if (!std::binary_search(boundaries.begin(), boundaries.end(), anchor) ||
            !std::binary_search(boundaries.begin(), boundaries.end(), active)) {
            return failure(TextInputError::InvalidSelection,
                           "selection must be on a grapheme boundary");
        }
    }
    std::vector<PendingEdit> edits;
    edits.reserve(selections.items().size());
    std::vector<std::size_t> actionTargets;
    actionTargets.reserve(selections.items().size());
    for (std::size_t action = 0; action < selections.items().size(); ++action) {
        const auto& selection = selections.items()[action];
        auto start =
            static_cast<std::size_t>(selection.lower().byteOffset.value());
        auto end =
            static_cast<std::size_t>(selection.upper().byteOffset.value());
        std::string inserted;

        if (command == TextInputCommand::Insert) {
            inserted = arguments.text;
        } else if (command == TextInputCommand::Newline) {
            const auto active =
                static_cast<std::size_t>(selection.active.byteOffset.value());
            inserted = terminatorFor(document.text, active,
                                      settings.lineEnding) +
                       indentationFor(document.text, active, settings);
        } else if (selection.isCaret()) {
            switch (command) {
            case TextInputCommand::DeleteBackward:
                start = previousBoundary(boundaries, start);
                break;
            case TextInputCommand::DeleteForward:
                end = nextBoundary(boundaries, end);
                break;
            case TextInputCommand::DeleteWordBackward:
                start = wordLeft(document.text, boundaries, start);
                break;
            case TextInputCommand::DeleteWordForward:
                end = wordRight(document.text, boundaries, end);
                break;
            case TextInputCommand::Insert:
            case TextInputCommand::Newline: break;
            }
        }
        if (start != end || !inserted.empty()) {
            edits.push_back(
                PendingEdit{start, end, std::move(inserted), action});
        }
        actionTargets.push_back(start);
    }

    const bool deletion =
        command == TextInputCommand::DeleteBackward ||
        command == TextInputCommand::DeleteForward ||
        command == TextInputCommand::DeleteWordBackward ||
        command == TextInputCommand::DeleteWordForward;
    normalizeEdits(edits);

    if (edits.empty()) {
        return TextInputResult{TextInputError::None, std::nullopt, selections,
                               document.text, {}};
    }

    std::string resultingText = document.text;
    for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit) {
        resultingText.replace(edit->start, edit->end - edit->start,
                               edit->inserted);
    }

    std::vector<Selection> resultingSelections;
    resultingSelections.reserve(actionTargets.size());
    std::vector<std::optional<std::size_t>> ownCarets(
        actionTargets.size());
    std::int64_t priorDelta = 0;
    for (const auto& edit : edits) {
        const auto caret = static_cast<std::int64_t>(edit.start) + priorDelta +
                           static_cast<std::int64_t>(edit.inserted.size());
        if (!deletion) {
            ownCarets[edit.action] = static_cast<std::size_t>(caret);
        }
        priorDelta +=
            static_cast<std::int64_t>(edit.inserted.size()) -
            static_cast<std::int64_t>(edit.end - edit.start);
    }
    const auto mapTarget = [&](std::size_t target) {
        std::int64_t delta = 0;
        for (const auto& edit : edits) {
            if (target < edit.start) {
                break;
            }
            if (target <= edit.end) {
                return static_cast<std::size_t>(
                    static_cast<std::int64_t>(edit.start) + delta +
                    static_cast<std::int64_t>(edit.inserted.size()));
            }
            delta += static_cast<std::int64_t>(edit.inserted.size()) -
                     static_cast<std::int64_t>(edit.end - edit.start);
        }
        return static_cast<std::size_t>(
            static_cast<std::int64_t>(target) + delta);
    };
    for (std::size_t action = 0; action < actionTargets.size(); ++action) {
        const auto caret =
            ownCarets[action].value_or(mapTarget(actionTargets[action]));
        const auto resolved = ssg::SelectionNavigator::resolvePosition(
            resultingText, ByteOffset{caret}, tabWidth);
        if (!resolved.has_value()) {
            return failure(TextInputError::InvalidSelection,
                           "resulting caret is not a document boundary");
        }
        resultingSelections.push_back(Selection{*resolved, *resolved});
    }

    std::vector<TextEdit> transactionEdits;
    transactionEdits.reserve(edits.size());
    for (auto& edit : edits) {
        transactionEdits.push_back(TextEdit{
            ByteOffset{edit.start},
            static_cast<std::uint64_t>(edit.end - edit.start),
            std::move(edit.inserted)});
    }
    return TextInputResult{
        TextInputError::None,
        EditTransaction{document.revision, std::move(transactionEdits)},
        SelectionSet{std::move(resultingSelections)},
        std::move(resultingText),
        {}};
}

}  // namespace ssg
