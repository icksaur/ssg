#include <ssg/Keymap.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <type_traits>

namespace ssg {
namespace {

bool validCode(std::string_view code) {
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

bool validUtf8WithoutNul(std::string_view text) {
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

bool validStroke(const KeyStroke& stroke) {
    return stroke.code != KeyCode::None;
}

bool startsWithSequence(const KeySequence& sequence,
                          const KeySequence& prefix) {
    return prefix.size() <= sequence.size() &&
           std::equal(prefix.begin(), prefix.end(), sequence.begin());
}

bool knownContext(std::string_view context) {
    const auto contexts = keymapContexts();
    return std::ranges::find(contexts, context) != contexts.end();
}

bool selectionArgumentsEqual(const SelectionCommandArguments& left,
                               const SelectionCommandArguments& right) {
    return left.position == right.position &&
           left.selection == right.selection;
}

} // namespace

std::optional<KeyStroke> parseKeyStroke(std::string_view encoded) {
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
            result.code = keyCodeFromName(token);
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
    if (!validStroke(result)) {
        return std::nullopt;
    }
    return result;
}

std::string formatKeyStroke(const KeyStroke& stroke) {
    if (!validStroke(stroke)) {
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
    append(keyCodeName(stroke.code));
    return result;
}

std::optional<KeySequence> parseKeySequence(
    std::initializer_list<std::string_view> encoded) {
    if (encoded.size() == 0) {
        return std::nullopt;
    }
    KeySequence result;
    result.reserve(encoded.size());
    for (const auto item : encoded) {
        const auto stroke = parseKeyStroke(item);
        if (!stroke) {
            return std::nullopt;
        }
        result.push_back(*stroke);
    }
    return result;
}

std::optional<KeySequence> parseKeySequenceString(
    std::string_view encoded) {
    // A single stroke only: the multi-stroke chord model is gone, so an input
    // naming more than one stroke (space-separated) is rejected rather than
    // silently binding the first.
    KeySequence result;
    std::size_t begin = 0;
    while (begin < encoded.size()) {
        while (begin < encoded.size() &&
               std::isspace(static_cast<unsigned char>(encoded[begin]))) {
            ++begin;
        }
        if (begin >= encoded.size()) break;
        std::size_t end = begin;
        while (end < encoded.size() &&
               !std::isspace(static_cast<unsigned char>(encoded[end]))) {
            ++end;
        }
        const auto stroke = parseKeyStroke(encoded.substr(begin, end - begin));
        if (!stroke) {
            return std::nullopt;
        }
        result.push_back(*stroke);
        begin = end;
    }
    if (result.size() != 1) {
        return std::nullopt;
    }
    return result;
}

namespace {


}  // namespace

std::string formatKeySequence(const KeySequence& sequence) {
    std::string result;
    for (const auto& stroke : sequence) {
        if (!result.empty()) result += ' ';
        if (stroke.control) result += "Ctrl+";
        if (stroke.alt) result += "Alt+";
        if (stroke.shift) result += "Shift+";
        if (stroke.meta) result += "Meta+";
        auto const display = keyCodeDisplay(stroke.code);
        // A letter key without Shift transmits as a LOWERCASE character in a
        // terminal (Alt+H is really ESC h); rendering it uppercase implies a
        // Shift that is not bound and does not work. So a single A-Z display
        // with no Shift modifier is lowercased. Shift+, digits, and named keys
        // keep their table display.
        if (!stroke.shift && display.size() == 1 && display[0] >= 'A' &&
            display[0] <= 'Z') {
            result += static_cast<char>(display[0] - 'A' + 'a');
        } else {
            result += display;
        }
    }
    return result;
}

std::vector<KeymapError> KeymapMatcher::validate() const {
    std::vector<KeymapError> errors;
    if (keymap_.name.empty()) {
        errors.push_back(
            {KeymapErrorCode::EmptyName, 0, "keymap name is empty"});
    }
    for (std::size_t index = 0; index < keymap_.bindings.size(); ++index) {
        const auto& binding = keymap_.bindings[index];
        if (binding.sequence.empty()) {
            errors.push_back({KeymapErrorCode::EmptySequence, index,
                              "binding sequence is empty"});
        } else if (binding.sequence.size() != 1) {
            errors.push_back({KeymapErrorCode::MultiStrokeBinding, index,
                              "binding sequence must be a single stroke"});
        }
        if (std::ranges::any_of(binding.sequence,
                                [](const auto& stroke) {
                                    return !validStroke(stroke);
                                })) {
            errors.push_back({KeymapErrorCode::InvalidStroke, index,
                              "binding contains an invalid key stroke"});
        }
        // Every Enter/Return keypress is normalized to a bare Enter stroke at the
        // input decoder, so a binding on a modified
        // Enter could never fire.  Reject it rather than accept a dead binding.
        if (std::ranges::any_of(binding.sequence, [](const KeyStroke& stroke) {
                return stroke.code == KeyCode::Enter &&
                       (stroke.shift || stroke.control || stroke.alt ||
                        stroke.meta);
            })) {
            errors.push_back(
                {KeymapErrorCode::ModifiedEnterBinding, index,
                 "Enter cannot be combined with a modifier; every Enter "
                 "inserts a newline"});
        }
        if (binding.commandId.empty()) {
            errors.push_back({KeymapErrorCode::EmptyCommand, index,
                              "binding command is empty"});
        }
        if (binding.context.empty()) {
            errors.push_back({KeymapErrorCode::EmptyContext, index,
                              "binding context is empty"});
        } else if (!knownContext(binding.context)) {
            errors.push_back({KeymapErrorCode::UnknownContext, index,
                              "binding context is not '*' or a focus target"});
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            const auto& earlier = keymap_.bindings[previous];
            if (earlier.sequence != binding.sequence) {
                continue;
            }
            if (earlier.context == binding.context) {
                errors.push_back({KeymapErrorCode::DuplicateBinding, index,
                                  "binding duplicates an earlier binding"});
                break;
            }
        }
        // A focus binding shadowed by a same-sequence global binding is
        // unreachable: "*"-precedence (K4) means the global command always wins.
        // Checked against all indices so the error does not depend on which of
        // the two is declared first (reported once, on the focus binding).
        if (binding.context != "*" && !binding.context.empty()) {
            const bool globallyShadowed = std::ranges::any_of(
                keymap_.bindings, [&](const KeyBinding& other) {
                    return other.context == "*" &&
                           other.sequence == binding.sequence;
                });
            if (globallyShadowed) {
                errors.push_back({KeymapErrorCode::UnreachableBinding, index,
                                  "a global binding shadows this binding"});
            }
        }
    }
    return errors;
}

KeymapDelta KeymapMatcher::deriveDelta(const KeymapViewState& previous,
                                       const KeymapViewState& current) {
    if (previous == current) {
        return {false, std::nullopt};
    }
    return {true, current};
}

namespace {

bool eligibleIn(const KeyBinding& binding, std::string_view context) {
    return binding.context == "*" || binding.context == context;
}

}  // namespace

KeymapResolution KeymapMatcher::resolveSequence(
    const KeySequence& pending, std::string_view context) const {
    if (pending.empty()) {
        return {KeymapMatchKind::None, {}};
    }
    const KeyBinding* match = nullptr;
    for (const auto& binding : keymap_.bindings) {
        if (!eligibleIn(binding, context)) {
            continue;
        }
        if (binding.sequence == pending) {
            // First eligible match wins; a "*" binding upgrades a focus match
            // (K4 precedence) but two "*" bindings keep the first, matching
            // has_global_binding's first-authoritative rule so the two agree on
            // any (even invalid) keymap.
            if (match == nullptr) {
                match = &binding;
            } else if (binding.context == "*" && match->context != "*") {
                match = &binding;
            }
        }
    }
    if (match != nullptr) {
        return {KeymapMatchKind::Resolved, match->commandId};
    }
    return {KeymapMatchKind::None, {}};
}

TextRouting textRoutingForContext(std::string_view context) noexcept {
    if (context == focusTargetName(FocusTarget::Editor)) {
        return TextRouting::Insert;
    }
    if (context == focusTargetName(FocusTarget::Prompt)) {
        return TextRouting::PromptQuery;
    }
    return TextRouting::Ignore;
}

bool KeymapMatcher::hasGlobalBinding(std::string_view commandId) const {
    for (std::size_t index = 0; index < keymap_.bindings.size(); ++index) {
        const auto& binding = keymap_.bindings[index];
        if (binding.context != "*" || binding.commandId != commandId) {
            continue;
        }
        const bool shadowed = std::any_of(
            keymap_.bindings.begin(), keymap_.bindings.begin() + index,
            [&](const KeyBinding& earlier) {
                return earlier.context == "*" &&
                       earlier.sequence == binding.sequence &&
                       earlier.commandId != binding.commandId;
            });
        if (!shadowed) {
            return true;
        }
    }
    return false;
}

std::optional<KeySequence> KeymapMatcher::preferredBinding(
    std::string_view commandId) const {
    const KeySequence* best = nullptr;
    std::string bestDisplay;
    for (const auto& binding : keymap_.bindings) {
        if (binding.commandId != commandId) continue;
        if (best == nullptr || binding.sequence.size() < best->size()) {
            best = &binding.sequence;
            bestDisplay = formatKeySequence(binding.sequence);
            continue;
        }
        if (binding.sequence.size() == best->size()) {
            auto display = formatKeySequence(binding.sequence);
            if (display < bestDisplay) {
                best = &binding.sequence;
                bestDisplay = std::move(display);
            }
        }
    }
    if (best == nullptr) return std::nullopt;
    return *best;
}

KeymapMutationResult applyKeymapBind(
    KeymapViewState const& current,
    KeymapBindArguments const& arguments) noexcept {
    if (arguments.command.empty()) {
        return {KeymapMutationError{"keymap.bind requires a command id"}, {}};
    }
    const auto sequence = parseKeySequenceString(arguments.sequence);
    if (!sequence) {
        return {KeymapMutationError{
                    "keymap.bind requires a valid space-separated key "
                    "sequence"},
                {}};
    }
    const std::string context =
        arguments.context.empty() ? "*" : arguments.context;

    KeymapViewState proposed{current.name, current.bindings};
    std::erase_if(proposed.bindings, [&](const KeyBinding& binding) {
        return binding.context == context && binding.sequence == *sequence;
    });
    proposed.bindings.push_back({*sequence, arguments.command, context});

    if (auto errors = KeymapMatcher{proposed}.validate(); !errors.empty()) {
        return {KeymapMutationError{errors.front().message}, {}};
    }
    if (!KeymapMatcher{proposed}.hasGlobalBinding("settings.open")) {
        return {KeymapMutationError{
                    "keymap.bind must not remove the settings.open "
                    "global escape hatch"},
                {}};
    }
    return {std::nullopt, std::move(proposed)};
}

KeymapMutationResult applyKeymapUnbind(
    KeymapViewState const& current,
    KeymapUnbindArguments const& arguments) noexcept {
    const auto sequence = parseKeySequenceString(arguments.sequence);
    if (!sequence) {
        return {KeymapMutationError{
                    "keymap.unbind requires a valid space-separated key "
                    "sequence"},
                {}};
    }
    const std::string context =
        arguments.context.empty() ? "*" : arguments.context;
    if (!knownContext(context)) {
        return {KeymapMutationError{
                    "keymap.unbind context is not '*' or a focus target"},
                {}};
    }

    KeymapViewState proposed{current.name, current.bindings};
    std::erase_if(proposed.bindings, [&](const KeyBinding& binding) {
        return binding.context == context && binding.sequence == *sequence;
    });
    if (!KeymapMatcher{proposed}.hasGlobalBinding("settings.open")) {
        return {KeymapMutationError{
                    "keymap.unbind must not remove the settings.open "
                    "global escape hatch"},
                {}};
    }
    return {std::nullopt, std::move(proposed)};
}

std::optional<CommittedText> CommittedText::fromUtf8(std::string text) {
    if (text.empty() || !validUtf8WithoutNul(text)) {
        return std::nullopt;
    }
    return CommittedText{std::move(text)};
}

ScrollFractionArguments::ScrollFractionArguments(
    std::uint32_t numeratorValue, std::uint32_t denominatorValue)
    : numerator{numeratorValue}, denominator{denominatorValue} {
    if (denominator == 0 || numerator > denominator) {
        throw std::invalid_argument(
            "scroll fraction requires 0 <= numerator <= denominator");
    }
}

bool SemanticCommand::operator==(const SemanticCommand& other) const {
    if (commandId != other.commandId ||
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
                return selectionArgumentsEqual(left, right);
            } else {
                return left == right;
            }
        },
        arguments, other.arguments);
}

SemanticCommand semanticInputForText(const CommittedText& committed) {
    return {"text.insert", TextInputArguments{committed.utf8()}};
}

} // namespace ssg
