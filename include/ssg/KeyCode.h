#pragma once

// Every key the terminal decoder can name, as an integer.
//
// A keystroke is identity, not text: the decoder recognises a closed set of
// keys, and a keymap binds names drawn from that same set.  Spelling that
// identity as a `std::string` meant the decoder ALLOCATED a name for every
// printable character typed, and every layer above compared names character by
// character -- to answer a question ("which key?") that has a fixed, known
// answer set.
//
// KeyCode is that answer set.  Names remain what configuration, `keymap.bind`
// and the wire protocol speak, but they are now a projection of the enum rather
// than the representation of it: `keyCodeFromName` is the one place a key name
// is matched, and it is called when a keymap is parsed, never per keystroke.
//
// The enum, its wire name and its display name are ONE table (`kKeyCodes`), so
// a key cannot be added without all three.  Static assertions below prove the
// table covers the enum exactly and in order.

#include <array>
#include <cstdint>
#include <string_view>

namespace ssg {

// Ordering matters: letters and digits are contiguous so the decoder can fold a
// printable byte to its key with arithmetic instead of a lookup.
enum class KeyCode : std::uint16_t {
    None = 0,
    Escape,
    Enter,
    Tab,
    Backspace,
    Delete,
    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
    Home,
    End,
    PageUp,
    PageDown,
    // The decoder does not yet emit function keys, so a binding on one cannot
    // fire today.  They are named here because they are real keys a terminal
    // sends and the key codec has always accepted them; closing that decoder
    // gap is a separate change.
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Space,
    BracketLeft,
    BracketRight,
    Backslash,
    Semicolon,
    Quote,
    Comma,
    Period,
    Slash,
    Minus,
    Equal,
    Backquote,
    Digit0,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Digit7,
    Digit8,
    Digit9,
    KeyA,
    KeyB,
    KeyC,
    KeyD,
    KeyE,
    KeyF,
    KeyG,
    KeyH,
    KeyI,
    KeyJ,
    KeyK,
    KeyL,
    KeyM,
    KeyN,
    KeyO,
    KeyP,
    KeyQ,
    KeyR,
    KeyS,
    KeyT,
    KeyU,
    KeyV,
    KeyW,
    KeyX,
    KeyY,
    KeyZ,
    Count,
};

struct KeyCodeEntry {
    KeyCode code;
    // The name a keymap binds and the protocol carries.
    std::string_view name;
    // The short form the keymap help renders.
    std::string_view display;
};

inline constexpr std::array<KeyCodeEntry, static_cast<std::size_t>(KeyCode::Count) - 1>
    kKeyCodes{{
        {KeyCode::Escape, "Escape", "Esc"},
        {KeyCode::Enter, "Enter", "Enter"},
        {KeyCode::Tab, "Tab", "Tab"},
        {KeyCode::Backspace, "Backspace", "Bksp"},
        {KeyCode::Delete, "Delete", "Delete"},
        {KeyCode::ArrowUp, "ArrowUp", "Up"},
        {KeyCode::ArrowDown, "ArrowDown", "Down"},
        {KeyCode::ArrowLeft, "ArrowLeft", "Left"},
        {KeyCode::ArrowRight, "ArrowRight", "Right"},
        {KeyCode::Home, "Home", "Home"},
        {KeyCode::End, "End", "End"},
        {KeyCode::PageUp, "PageUp", "PageUp"},
        {KeyCode::PageDown, "PageDown", "PageDown"},
        {KeyCode::F1, "F1", "F1"},
        {KeyCode::F2, "F2", "F2"},
        {KeyCode::F3, "F3", "F3"},
        {KeyCode::F4, "F4", "F4"},
        {KeyCode::F5, "F5", "F5"},
        {KeyCode::F6, "F6", "F6"},
        {KeyCode::F7, "F7", "F7"},
        {KeyCode::F8, "F8", "F8"},
        {KeyCode::F9, "F9", "F9"},
        {KeyCode::F10, "F10", "F10"},
        {KeyCode::F11, "F11", "F11"},
        {KeyCode::F12, "F12", "F12"},
        {KeyCode::Space, "Space", "Space"},
        {KeyCode::BracketLeft, "BracketLeft", "["},
        {KeyCode::BracketRight, "BracketRight", "]"},
        {KeyCode::Backslash, "Backslash", "\\"},
        {KeyCode::Semicolon, "Semicolon", ";"},
        {KeyCode::Quote, "Quote", "'"},
        {KeyCode::Comma, "Comma", ","},
        {KeyCode::Period, "Period", "."},
        {KeyCode::Slash, "Slash", "/"},
        {KeyCode::Minus, "Minus", "-"},
        {KeyCode::Equal, "Equal", "="},
        {KeyCode::Backquote, "Backquote", "`"},
        {KeyCode::Digit0, "Digit0", "0"},
        {KeyCode::Digit1, "Digit1", "1"},
        {KeyCode::Digit2, "Digit2", "2"},
        {KeyCode::Digit3, "Digit3", "3"},
        {KeyCode::Digit4, "Digit4", "4"},
        {KeyCode::Digit5, "Digit5", "5"},
        {KeyCode::Digit6, "Digit6", "6"},
        {KeyCode::Digit7, "Digit7", "7"},
        {KeyCode::Digit8, "Digit8", "8"},
        {KeyCode::Digit9, "Digit9", "9"},
        {KeyCode::KeyA, "KeyA", "A"},
        {KeyCode::KeyB, "KeyB", "B"},
        {KeyCode::KeyC, "KeyC", "C"},
        {KeyCode::KeyD, "KeyD", "D"},
        {KeyCode::KeyE, "KeyE", "E"},
        {KeyCode::KeyF, "KeyF", "F"},
        {KeyCode::KeyG, "KeyG", "G"},
        {KeyCode::KeyH, "KeyH", "H"},
        {KeyCode::KeyI, "KeyI", "I"},
        {KeyCode::KeyJ, "KeyJ", "J"},
        {KeyCode::KeyK, "KeyK", "K"},
        {KeyCode::KeyL, "KeyL", "L"},
        {KeyCode::KeyM, "KeyM", "M"},
        {KeyCode::KeyN, "KeyN", "N"},
        {KeyCode::KeyO, "KeyO", "O"},
        {KeyCode::KeyP, "KeyP", "P"},
        {KeyCode::KeyQ, "KeyQ", "Q"},
        {KeyCode::KeyR, "KeyR", "R"},
        {KeyCode::KeyS, "KeyS", "S"},
        {KeyCode::KeyT, "KeyT", "T"},
        {KeyCode::KeyU, "KeyU", "U"},
        {KeyCode::KeyV, "KeyV", "V"},
        {KeyCode::KeyW, "KeyW", "W"},
        {KeyCode::KeyX, "KeyX", "X"},
        {KeyCode::KeyY, "KeyY", "Y"},
        {KeyCode::KeyZ, "KeyZ", "Z"},
    }};

// The table is indexed by enumerator, so it must list every key exactly once and
// in declaration order.  Adding an enumerator without its row -- or listing rows
// out of order -- fails to compile rather than silently mapping a key to the
// wrong name.
consteval bool keyCodeTableIsExhaustive() {
    for (std::size_t index = 0; index < kKeyCodes.size(); ++index) {
        if (static_cast<std::size_t>(kKeyCodes[index].code) != index + 1) {
            return false;
        }
        if (kKeyCodes[index].name.empty() || kKeyCodes[index].display.empty()) {
            return false;
        }
    }
    return true;
}
static_assert(keyCodeTableIsExhaustive());

// The name a keymap binds; empty for `None`.
[[nodiscard]] constexpr std::string_view keyCodeName(KeyCode code) noexcept {
    if (code == KeyCode::None || code >= KeyCode::Count) return {};
    return kKeyCodes[static_cast<std::size_t>(code) - 1].name;
}

// The short form rendered in the keymap help; empty for `None`.
[[nodiscard]] constexpr std::string_view keyCodeDisplay(KeyCode code) noexcept {
    if (code == KeyCode::None || code >= KeyCode::Count) return {};
    return kKeyCodes[static_cast<std::size_t>(code) - 1].display;
}

// The one place a key name is matched.  Call it when a keymap is parsed, not
// per keystroke.  An unrecognised name yields `None`, which a keymap treats as
// an invalid stroke -- so a typo in a binding is now rejected rather than
// silently producing a binding no key can ever trigger.
[[nodiscard]] constexpr KeyCode keyCodeFromName(std::string_view name) noexcept {
    for (auto const& entry : kKeyCodes) {
        if (entry.name == name) return entry.code;
    }
    return KeyCode::None;
}

}  // namespace ssg
