#include <ssg/input.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <type_traits>

namespace ssg {
namespace {

bool valid_code(std::string_view code) {
    if (code.size() == 4 && code.starts_with("Key") &&
        code[3] >= 'A' && code[3] <= 'Z') {
        return true;
    }
    if (code.size() == 6 && code.starts_with("Digit") &&
        std::isdigit(static_cast<unsigned char>(code[5]))) {
        return true;
    }
    if (code.size() >= 2 && code[0] == 'F') {
        unsigned value = 0;
        for (const char c : code.substr(1)) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                return false;
            }
            value = value * 10 + static_cast<unsigned>(c - '0');
        }
        return value >= 1 && value <= 24;
    }
    static constexpr auto named = std::to_array<std::string_view>({
        "ArrowDown", "ArrowLeft", "ArrowRight", "ArrowUp", "Backquote",
        "Backslash", "Backspace", "BracketLeft", "BracketRight", "Comma",
        "Delete", "End", "Enter", "Equal", "Escape", "Home", "Minus",
        "PageDown", "PageUp", "Period", "Quote", "Semicolon", "Slash",
        "Space", "Tab",
    });
    return std::ranges::find(named, code) != named.end();
}

bool valid_utf8_without_nul(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead == 0) {
            return false;
        }
        if (lead < 0x80) {
            ++i;
            continue;
        }

        std::size_t count = 0;
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        if ((lead & 0xE0) == 0xC0) {
            count = 2;
            value = lead & 0x1F;
            minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            count = 3;
            value = lead & 0x0F;
            minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            count = 4;
            value = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (i + count > text.size()) {
            return false;
        }
        for (std::size_t j = 1; j < count; ++j) {
            const auto continuation =
                static_cast<unsigned char>(text[i + j]);
            if ((continuation & 0xC0) != 0x80) {
                return false;
            }
            value = (value << 6) | (continuation & 0x3F);
        }
        if (value < minimum || value > 0x10FFFF ||
            (value >= 0xD800 && value <= 0xDFFF)) {
            return false;
        }
        i += count;
    }
    return true;
}

bool valid_stroke(const KeyStroke& stroke) {
    return valid_code(stroke.code);
}

bool starts_with_sequence(const KeySequence& sequence,
                          const KeySequence& prefix) {
    return prefix.size() <= sequence.size() &&
           std::equal(prefix.begin(), prefix.end(), sequence.begin());
}

bool selection_arguments_equal(const SelectionCommandArguments& left,
                               const SelectionCommandArguments& right) {
    return left.position == right.position &&
           left.selection == right.selection;
}

} // namespace

std::optional<KeyStroke> parse_key_stroke(std::string_view encoded) {
    if (encoded.empty()) {
        return std::nullopt;
    }
    KeyStroke result;
    std::size_t begin = 0;
    while (begin < encoded.size()) {
        const auto separator = encoded.find('+', begin);
        const auto token = encoded.substr(
            begin, separator == std::string_view::npos
                       ? encoded.size() - begin
                       : separator - begin);
        if (token.empty()) {
            return std::nullopt;
        }
        const bool final = separator == std::string_view::npos;
        if (final) {
            result.code = token;
        } else if (token == "Ctrl" && !result.control) {
            result.control = true;
        } else if (token == "Alt" && !result.alt) {
            result.alt = true;
        } else if (token == "Meta" && !result.meta) {
            result.meta = true;
        } else if (token == "Shift" && !result.shift) {
            result.shift = true;
        } else {
            return std::nullopt;
        }
        begin = separator == std::string_view::npos ? encoded.size()
                                                    : separator + 1;
    }
    if (!valid_stroke(result)) {
        return std::nullopt;
    }
    return result;
}

std::string format_key_stroke(const KeyStroke& stroke) {
    if (!valid_stroke(stroke)) {
        return {};
    }
    std::string result;
    const auto append = [&](std::string_view part) {
        if (!result.empty()) {
            result += '+';
        }
        result += part;
    };
    if (stroke.control) {
        append("Ctrl");
    }
    if (stroke.alt) {
        append("Alt");
    }
    if (stroke.meta) {
        append("Meta");
    }
    if (stroke.shift) {
        append("Shift");
    }
    append(stroke.code);
    return result;
}

std::optional<KeySequence> parse_key_sequence(
    std::initializer_list<std::string_view> encoded) {
    if (encoded.size() == 0) {
        return std::nullopt;
    }
    KeySequence result;
    result.reserve(encoded.size());
    for (const auto item : encoded) {
        const auto stroke = parse_key_stroke(item);
        if (!stroke) {
            return std::nullopt;
        }
        result.push_back(*stroke);
    }
    return result;
}

std::vector<KeymapError> validate_keymap(
    const KeymapViewState& keymap,
    std::span<const KeySequence> reserved_sequences) {
    std::vector<KeymapError> errors;
    if (keymap.name.empty()) {
        errors.push_back(
            {KeymapErrorCode::empty_name, 0, "keymap name is empty"});
    }
    for (std::size_t index = 0; index < keymap.bindings.size(); ++index) {
        const auto& binding = keymap.bindings[index];
        if (binding.sequence.empty()) {
            errors.push_back({KeymapErrorCode::empty_sequence, index,
                              "binding sequence is empty"});
        }
        if (std::ranges::any_of(binding.sequence,
                                [](const auto& stroke) {
                                    return !valid_stroke(stroke);
                                })) {
            errors.push_back({KeymapErrorCode::invalid_stroke, index,
                              "binding contains an invalid key stroke"});
        }
        if (binding.command_id.empty()) {
            errors.push_back({KeymapErrorCode::empty_command, index,
                              "binding command is empty"});
        }
        if (binding.context.empty()) {
            errors.push_back({KeymapErrorCode::empty_context, index,
                              "binding context is empty"});
        }
        if (std::ranges::any_of(
                reserved_sequences, [&](const auto& reserved) {
                    return starts_with_sequence(binding.sequence, reserved);
                })) {
            errors.push_back({KeymapErrorCode::reserved_binding, index,
                              "binding uses a browser-reserved sequence"});
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            const auto& earlier = keymap.bindings[previous];
            if (earlier.sequence != binding.sequence) {
                continue;
            }
            if (earlier.context == binding.context) {
                errors.push_back({KeymapErrorCode::duplicate_binding, index,
                                  "binding duplicates an earlier binding"});
                break;
            }
            if (earlier.context == "*") {
                errors.push_back({KeymapErrorCode::unreachable_binding, index,
                                  "a global binding shadows this binding"});
                break;
            }
        }
    }
    return errors;
}

KeymapDelta derive_keymap_delta(const KeymapViewState& previous,
                                const KeymapViewState& current) {
    if (previous == current) {
        return {false, std::nullopt};
    }
    return {true, current};
}

std::optional<CommittedText> CommittedText::from_utf8(std::string text) {
    if (text.empty() || !valid_utf8_without_nul(text)) {
        return std::nullopt;
    }
    return CommittedText{std::move(text)};
}

ScrollFractionArguments::ScrollFractionArguments(
    std::uint32_t numerator_value, std::uint32_t denominator_value)
    : numerator{numerator_value}, denominator{denominator_value} {
    if (denominator == 0 || numerator > denominator) {
        throw std::invalid_argument(
            "scroll fraction requires 0 <= numerator <= denominator");
    }
}

bool SemanticCommand::operator==(const SemanticCommand& other) const {
    if (command_id != other.command_id ||
        arguments.index() != other.arguments.index()) {
        return false;
    }
    return std::visit(
        [](const auto& left, const auto& right) {
            using Left = std::decay_t<decltype(left)>;
            using Right = std::decay_t<decltype(right)>;
            if constexpr (!std::is_same_v<Left, Right>) {
                return false;
            } else if constexpr (
                std::is_same_v<Left, SelectionCommandArguments>) {
                return selection_arguments_equal(left, right);
            } else {
                return left == right;
            }
        },
        arguments, other.arguments);
}

SemanticCommand semantic_input(const CommittedText& committed) {
    return {"text.insert", TextInputArguments{committed.utf8()}};
}

const SemanticCommand& activate_hit_target(
    const SemanticHitTarget& target) noexcept {
    return target.command;
}

} // namespace ssg
