#include <ssg/text_input_commands.h>

#include <ssg/layout.h>

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
    word,
    space,
    punctuation,
};

TextInputResult failure(TextInputError error, std::string message) {
    return TextInputResult{error, std::nullopt, std::nullopt, {},
                           std::move(message)};
}

bool valid_utf8(std::string_view text) {
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

std::vector<std::size_t> grapheme_boundaries(std::string_view text,
                                             int tab_width) {
    std::vector<std::size_t> boundaries{0};
    std::size_t line_start = 0;
    while (line_start < text.size()) {
        auto line_end = line_start;
        while (line_end < text.size() && text[line_end] != '\r' &&
               text[line_end] != '\n') {
            ++line_end;
        }
        const auto run =
            compute_cell_run(text.substr(line_start, line_end - line_start),
                             tab_width);
        for (const auto& span : run.spans) {
            boundaries.push_back(line_start + span.byte_offset + span.byte_len);
        }
        if (line_end == text.size()) {
            break;
        }
        if (text[line_end] == '\r' && line_end + 1 < text.size() &&
            text[line_end + 1] == '\n') {
            line_start = line_end + 2;
        } else {
            line_start = line_end + 1;
        }
        boundaries.push_back(line_start);
    }
    return boundaries;
}

std::size_t previous_boundary(const std::vector<std::size_t>& boundaries,
                              std::size_t offset) {
    const auto found = std::lower_bound(boundaries.begin(), boundaries.end(),
                                        offset);
    return found == boundaries.begin() ? 0 : *std::prev(found);
}

std::size_t next_boundary(const std::vector<std::size_t>& boundaries,
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
        return SegmentCategory::word;
    }
    if (first == ' ' || first == '\t' || first == '\r' || first == '\n') {
        return SegmentCategory::space;
    }
    return SegmentCategory::punctuation;
}

std::size_t word_left(std::string_view text,
                      const std::vector<std::size_t>& boundaries,
                      std::size_t offset) {
    if (offset == 0) {
        return 0;
    }
    auto position = previous_boundary(boundaries, offset);
    const auto target = category(text, position);
    if (target == SegmentCategory::punctuation) {
        return position;
    }
    while (position > 0) {
        const auto before = previous_boundary(boundaries, position);
        if (category(text, before) != target) {
            break;
        }
        position = before;
    }
    return position;
}

std::size_t word_right(std::string_view text,
                       const std::vector<std::size_t>& boundaries,
                       std::size_t offset) {
    if (offset >= text.size()) {
        return offset;
    }
    const auto target = category(text, offset);
    auto position = next_boundary(boundaries, offset);
    while (position < text.size() && category(text, position) == target) {
        position = next_boundary(boundaries, position);
    }
    return position;
}

std::pair<std::size_t, std::size_t> line_bounds(std::string_view text,
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

std::string terminator_for(std::string_view text, std::size_t offset,
                           LineEnding configured) {
    switch (configured) {
    case LineEnding::lf: return "\n";
    case LineEnding::crlf: return "\r\n";
    case LineEnding::cr: return "\r";
    case LineEnding::mixed: {
        const auto end = line_bounds(text, offset).second;
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

std::string indentation_for(std::string_view text, std::size_t offset,
                            TextInputSettings settings) {
    if (!settings.auto_indent) {
        return {};
    }
    const auto start = line_bounds(text, offset).first;
    std::uint64_t columns = 0;
    auto cursor = start;
    while (cursor < text.size()) {
        if (text[cursor] == ' ') {
            ++columns;
            ++cursor;
        } else if (text[cursor] == '\t') {
            columns += settings.indent_width -
                       (columns % settings.indent_width);
            ++cursor;
        } else {
            break;
        }
    }
    if (settings.indent_style == IndentStyle::spaces) {
        return std::string(static_cast<std::size_t>(columns), ' ');
    }
    const auto tabs = columns / settings.indent_width;
    const auto spaces = columns % settings.indent_width;
    return std::string(static_cast<std::size_t>(tabs), '\t') +
           std::string(static_cast<std::size_t>(spaces), ' ');
}

bool valid_position(std::string_view text, const DocumentPosition& position,
                    int tab_width) {
    const auto resolved =
        resolve_document_position(text, position.byte_offset, tab_width);
    return resolved.has_value() && *resolved == position;
}

void normalize_edits(std::vector<PendingEdit>& edits) {
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
          {"text.insert", TextInputCommand::insert},
          {"text.newline", TextInputCommand::newline},
          {"text.delete_backward", TextInputCommand::delete_backward},
          {"text.delete_forward", TextInputCommand::delete_forward},
          {"text.delete_word_backward",
           TextInputCommand::delete_word_backward},
          {"text.delete_word_forward", TextInputCommand::delete_word_forward},
      }} {}

const std::array<TextInputCommandDescriptor, 6>&
TextInputCommandSet::descriptors() const noexcept {
    return descriptors_;
}

TextInputCommandSet text_input_command_set() {
    return TextInputCommandSet{};
}

TextInputResult apply_text_input(const DocumentSnapshot& document,
                                 const SelectionSet& selections,
                                 TextInputSettings settings,
                                 TextInputCommand command,
                                 TextInputArguments arguments) {
    if (document.mode == DocumentMode::read_only) {
        return failure(TextInputError::read_only,
                       "text input requires an editable document");
    }
    if (document.mode == DocumentMode::diff) {
        return failure(TextInputError::diff,
                       "text input is unavailable in diff mode");
    }
    if (settings.indent_width < 1 || settings.indent_width > 16 ||
        (settings.indent_style != IndentStyle::spaces &&
         settings.indent_style != IndentStyle::tabs) ||
        static_cast<std::uint8_t>(settings.line_ending) >
            static_cast<std::uint8_t>(LineEnding::mixed)) {
        return failure(TextInputError::invalid_settings,
                       "text input settings are outside their valid range");
    }
    const auto tab_width = static_cast<int>(settings.indent_width);
    for (const auto& selection : selections.items()) {
        if (!valid_position(document.text, selection.anchor, tab_width) ||
            !valid_position(document.text, selection.active, tab_width)) {
            return failure(TextInputError::invalid_selection,
                           "selection position is inconsistent with document");
        }
    }
    if (command == TextInputCommand::insert && !valid_utf8(arguments.text)) {
        return failure(TextInputError::invalid_utf8,
                       "inserted text must be well-formed UTF-8 without NUL");
    }
    if (static_cast<std::uint8_t>(command) >
        static_cast<std::uint8_t>(TextInputCommand::delete_word_forward)) {
        return failure(TextInputError::unknown_command,
                       "text input command is not recognized");
    }

    const auto boundaries = grapheme_boundaries(document.text, tab_width);
    for (const auto& selection : selections.items()) {
        const auto anchor =
            static_cast<std::size_t>(selection.anchor.byte_offset.value());
        const auto active =
            static_cast<std::size_t>(selection.active.byte_offset.value());
        if (!std::binary_search(boundaries.begin(), boundaries.end(), anchor) ||
            !std::binary_search(boundaries.begin(), boundaries.end(), active)) {
            return failure(TextInputError::invalid_selection,
                           "selection must be on a grapheme boundary");
        }
    }
    std::vector<PendingEdit> edits;
    edits.reserve(selections.items().size());
    std::vector<std::size_t> action_targets;
    action_targets.reserve(selections.items().size());
    for (std::size_t action = 0; action < selections.items().size(); ++action) {
        const auto& selection = selections.items()[action];
        auto start =
            static_cast<std::size_t>(selection.lower().byte_offset.value());
        auto end =
            static_cast<std::size_t>(selection.upper().byte_offset.value());
        std::string inserted;

        if (command == TextInputCommand::insert) {
            inserted = arguments.text;
        } else if (command == TextInputCommand::newline) {
            const auto active =
                static_cast<std::size_t>(selection.active.byte_offset.value());
            inserted = terminator_for(document.text, active,
                                      settings.line_ending) +
                       indentation_for(document.text, active, settings);
        } else if (selection.is_caret()) {
            switch (command) {
            case TextInputCommand::delete_backward:
                start = previous_boundary(boundaries, start);
                break;
            case TextInputCommand::delete_forward:
                end = next_boundary(boundaries, end);
                break;
            case TextInputCommand::delete_word_backward:
                start = word_left(document.text, boundaries, start);
                break;
            case TextInputCommand::delete_word_forward:
                end = word_right(document.text, boundaries, end);
                break;
            case TextInputCommand::insert:
            case TextInputCommand::newline: break;
            }
        }
        if (start != end || !inserted.empty()) {
            edits.push_back(
                PendingEdit{start, end, std::move(inserted), action});
        }
        action_targets.push_back(start);
    }

    const bool deletion =
        command == TextInputCommand::delete_backward ||
        command == TextInputCommand::delete_forward ||
        command == TextInputCommand::delete_word_backward ||
        command == TextInputCommand::delete_word_forward;
    normalize_edits(edits);

    if (edits.empty()) {
        return TextInputResult{TextInputError::none, std::nullopt, selections,
                               document.text, {}};
    }

    std::string resulting_text = document.text;
    for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit) {
        resulting_text.replace(edit->start, edit->end - edit->start,
                               edit->inserted);
    }

    std::vector<Selection> resulting_selections;
    resulting_selections.reserve(action_targets.size());
    std::vector<std::optional<std::size_t>> own_carets(
        action_targets.size());
    std::int64_t prior_delta = 0;
    for (const auto& edit : edits) {
        const auto caret = static_cast<std::int64_t>(edit.start) + prior_delta +
                           static_cast<std::int64_t>(edit.inserted.size());
        if (!deletion) {
            own_carets[edit.action] = static_cast<std::size_t>(caret);
        }
        prior_delta +=
            static_cast<std::int64_t>(edit.inserted.size()) -
            static_cast<std::int64_t>(edit.end - edit.start);
    }
    const auto map_target = [&](std::size_t target) {
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
    for (std::size_t action = 0; action < action_targets.size(); ++action) {
        const auto caret =
            own_carets[action].value_or(map_target(action_targets[action]));
        const auto resolved = resolve_document_position(
            resulting_text, ByteOffset{caret}, tab_width);
        if (!resolved.has_value()) {
            return failure(TextInputError::invalid_selection,
                           "resulting caret is not a document boundary");
        }
        resulting_selections.push_back(Selection{*resolved, *resolved});
    }

    std::vector<TextEdit> transaction_edits;
    transaction_edits.reserve(edits.size());
    for (auto& edit : edits) {
        transaction_edits.push_back(TextEdit{
            ByteOffset{edit.start},
            static_cast<std::uint64_t>(edit.end - edit.start),
            std::move(edit.inserted)});
    }
    return TextInputResult{
        TextInputError::none,
        EditTransaction{document.revision, std::move(transaction_edits)},
        SelectionSet{std::move(resulting_selections)},
        std::move(resulting_text),
        {}};
}

}  // namespace ssg
