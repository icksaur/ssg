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

bool is_strict_prefix(const KeySequence& shorter, const KeySequence& longer) {
    return shorter.size() < longer.size() &&
           std::equal(shorter.begin(), shorter.end(), longer.begin());
}

bool known_context(std::string_view context) {
    const auto contexts = keymap_contexts();
    return std::ranges::find(contexts, context) != contexts.end();
}

// Two bindings can both apply during resolution when their contexts overlap:
// either shares "*", or they name the same context.  A "*" binding is eligible
// in every context, so it overlaps every binding.
bool contexts_overlap(std::string_view left, std::string_view right) {
    return left == "*" || right == "*" || left == right;
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
        } else if (!known_context(binding.context)) {
            errors.push_back({KeymapErrorCode::unknown_context, index,
                              "binding context is not '*' or a focus target"});
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
        // A binding whose sequence strictly prefixes (or is strictly prefixed
        // by) another eligible binding's sequence makes resolution ambiguous:
        // one input would be both a resolved chord and a pending prefix.  The
        // check is symmetric and order-independent (it reports the longer,
        // higher-indexed binding once).
        for (std::size_t other = 0; other < index; ++other) {
            const auto& earlier = keymap.bindings[other];
            if (!contexts_overlap(earlier.context, binding.context)) {
                continue;
            }
            if (is_strict_prefix(earlier.sequence, binding.sequence) ||
                is_strict_prefix(binding.sequence, earlier.sequence)) {
                errors.push_back(
                    {KeymapErrorCode::ambiguous_prefix, index,
                     "binding sequence is a prefix of another eligible binding"});
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

namespace {

bool eligible_in(const KeyBinding& binding, std::string_view context) {
    return binding.context == "*" || binding.context == context;
}

}  // namespace

KeymapResolution resolve_key_sequence(const KeymapViewState& keymap,
                                      const KeySequence& pending,
                                      std::string_view context) {
    if (pending.empty()) {
        return {KeymapMatchKind::none, {}};
    }
    const KeyBinding* match = nullptr;
    bool has_pending = false;
    for (const auto& binding : keymap.bindings) {
        if (!eligible_in(binding, context)) {
            continue;
        }
        if (binding.sequence == pending) {
            // "*" wins over a focus binding for the same sequence (K4), so a
            // global chord is never shadowed.  Prefix-freeness guarantees no
            // eligible binding is also pending here.
            if (match == nullptr || binding.context == "*") {
                match = &binding;
            }
        } else if (is_strict_prefix(pending, binding.sequence)) {
            has_pending = true;
        }
    }
    if (match != nullptr) {
        return {KeymapMatchKind::resolved, match->command_id};
    }
    return {has_pending ? KeymapMatchKind::pending : KeymapMatchKind::none, {}};
}

TextRouting text_routing(std::string_view context) noexcept {
    if (context == focus_target_name(FocusTarget::editor)) {
        return TextRouting::insert;
    }
    if (context == focus_target_name(FocusTarget::prompt)) {
        return TextRouting::prompt_query;
    }
    return TextRouting::ignore;
}

bool has_global_binding(const KeymapViewState& keymap,
                        std::string_view command_id,
                        std::span<const KeySequence> reserved_sequences) {
    for (std::size_t index = 0; index < keymap.bindings.size(); ++index) {
        const auto& binding = keymap.bindings[index];
        if (binding.context != "*" || binding.command_id != command_id) {
            continue;
        }
        if (std::ranges::any_of(reserved_sequences, [&](const auto& reserved) {
                return starts_with_sequence(binding.sequence, reserved);
            })) {
            continue;
        }
        const bool shadowed = std::any_of(
            keymap.bindings.begin(), keymap.bindings.begin() + index,
            [&](const KeyBinding& earlier) {
                return earlier.context == "*" &&
                       earlier.sequence == binding.sequence &&
                       earlier.command_id != binding.command_id;
            });
        if (!shadowed) {
            return true;
        }
    }
    return false;
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
