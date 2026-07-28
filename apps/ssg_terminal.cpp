#include "ssg_terminal.h"

#include <ssg/Keymap.h>
#include <ssg/Theme.h>

#include <csignal>

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>

namespace ssg::app {

namespace fs = std::filesystem;

SignalEvents classify_signal_tags(std::string_view drained) {
    SignalEvents events;
    for (unsigned char byte : drained) {
        int const signo = static_cast<int>(byte);
        if (signo == SIGWINCH) {
            events.resize = true;
        } else if (signo == SIGTERM || signo == SIGHUP) {
            events.terminate = signo;
        }
    }
    return events;
}

std::string terminal_setup_sequence() {
    // Alternate screen, blinking bar cursor, SGR mouse reporting. 1000h = button
    // press/release, 1002h = button-event motion (drags), 1006h = SGR extended
    // coordinates.
    return "\x1b[?1049h\x1b[5 q\x1b[?1000h\x1b[?1002h\x1b[?1006h";
}

std::string terminal_restore_sequence() {
    return "\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[0 q\x1b[?25h\x1b[?1049l";
}

std::string lowercase(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char ch : value) {
        normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
    return normalized;
}

bool matchesAny(std::string_view value,
                std::initializer_list<std::string_view> options) {
    return std::ranges::any_of(options, [&](std::string_view option) {
        return value == option;
    });
}

ssg::ColorDepth detect_color_depth(char const* color_depth_override,
                                   char const* colorterm, char const* term,
                                   char const* term_program) {
    if (color_depth_override != nullptr) {
        const auto overrideValue = lowercase(color_depth_override);
        if (matchesAny(overrideValue, {"truecolor", "24bit"})) {
            return ssg::ColorDepth::Truecolor;
        }
        if (matchesAny(overrideValue, {"256", "256color", "indexed256"})) {
            return ssg::ColorDepth::Indexed256;
        }
        if (matchesAny(overrideValue, {"16", "ansi16"})) {
            return ssg::ColorDepth::Ansi16;
        }
    }

    if (colorterm != nullptr) {
        const auto colortermValue = lowercase(colorterm);
        if (matchesAny(colortermValue, {"truecolor", "24bit"})) {
            return ssg::ColorDepth::Truecolor;
        }
    }

    const auto termValue =
        term == nullptr ? std::string{} : lowercase(std::string_view{term});
    if (termValue == "dumb" || termValue.empty()) {
        return ssg::ColorDepth::Ansi16;
    }

    const auto termProgramValue = term_program == nullptr
                                      ? std::string{}
                                      : lowercase(std::string_view{term_program});
    if (matchesAny(termProgramValue,
                   {"iterm.app", "wezterm", "vscode", "hyper", "ghostty"})) {
        return ssg::ColorDepth::Truecolor;
    }
    constexpr std::array<std::string_view, 6> termTruecolorHints{
        "kitty", "alacritty", "wezterm", "foot", "contour", "ghostty"};
    if (std::ranges::any_of(termTruecolorHints, [&](std::string_view hint) {
            return termValue.find(hint) != std::string::npos;
        })) {
        return ssg::ColorDepth::Truecolor;
    }

    return ssg::ColorDepth::Truecolor;
}

LaunchTarget resolve_launch(fs::path const& argument) {
    if (argument.empty()) {
        return {fs::current_path(), std::nullopt};
    }
    std::error_code code;
    if (fs::is_directory(argument, code)) {
        return {fs::absolute(argument), std::nullopt};
    }
    auto absolute = fs::absolute(argument);
    auto parent =
        absolute.has_parent_path() ? absolute.parent_path() : fs::current_path();
    return {parent, absolute.filename().string()};
}

std::string encode_ansi_frame(ssg::CellGrid const& screen, ssg::ColorDepth depth) {
    constexpr std::size_t maxIndex = ssg::kThemePaletteSize - 1;
    auto color = [&](ssg::SrgbColor c, char kind) -> std::string {
        auto const resolved = ssg::resolveColor(c, depth);
        switch (resolved.encoding) {
            case ssg::ResolvedColor::Encoding::Truecolor:
                return "\x1b[" + std::string{kind} + "8;2;" +
                       std::to_string(resolved.rgb.red) + ";" +
                       std::to_string(resolved.rgb.green) + ";" +
                       std::to_string(resolved.rgb.blue) + "m";
            case ssg::ResolvedColor::Encoding::Indexed256:
                return "\x1b[" + std::string{kind} + "8;5;" +
                       std::to_string(resolved.index) + "m";
            case ssg::ResolvedColor::Encoding::Ansi16: {
                int const base = kind == '3' ? 30 : 40;
                int const bright = kind == '3' ? 90 : 100;
                int const code = resolved.index < 8
                                     ? base + resolved.index
                                     : bright + (resolved.index - 8);
                return "\x1b[" + std::to_string(code) + "m";
            }
        }
        return {};
    };
    auto paletteColor = [&](std::uint8_t index, char kind) {
        return color(screen.palette[std::min<std::size_t>(index, maxIndex)], kind);
    };
    auto tintColor = [&](ssg::DiffTint tint) {
        switch (tint) {
            case ssg::DiffTint::AddedRow: return screen.diffTints.addedRow;
            case ssg::DiffTint::RemovedRow: return screen.diffTints.removedRow;
            case ssg::DiffTint::ModifiedRow: return screen.diffTints.modifiedRow;
            case ssg::DiffTint::AddedWord: return screen.diffTints.addedWord;
            case ssg::DiffTint::RemovedWord: return screen.diffTints.removedWord;
            case ssg::DiffTint::ModifiedWord: return screen.diffTints.modifiedWord;
            case ssg::DiffTint::None: break;
        }
        return screen.diffTints.addedRow;
    };

    std::string out = "\x1b[H";
    int const columns = screen.size.columns;
    int const rows = screen.size.rows;
    for (int y = 0; y < rows; ++y) {
        out += "\x1b[" + std::to_string(y + 1) + ";1H\x1b[0m";
        int foreground = -1;
        int background = -1;
        int role = -1;
        auto tint = ssg::DiffTint::None;
        for (int x = 0; x < columns; ++x) {
            auto const& cell =
                screen.cells[static_cast<std::size_t>(y * columns + x)];
            if (cell.continuation) continue;
            if (cell.foreground != foreground || cell.background != background ||
                cell.tint != tint || static_cast<int>(cell.role) != role) {
                out += paletteColor(cell.foreground, '3');
                if (cell.tint != ssg::DiffTint::None) {
                    out += color(tintColor(cell.tint), '4');
                } else if (cell.role == ssg::SemanticRole::Selection) {
                    out += color(screen.selectionFill, '4');
                } else {
                    out += paletteColor(cell.background, '4');
                }
                foreground = cell.foreground;
                background = cell.background;
                tint = cell.tint;
                role = static_cast<int>(cell.role);
            }
            out += cell.text.empty() ? std::string{" "} : cell.text;
        }
    }
    out += "\x1b[0m";
    return out;
}

namespace {

// Parse a leading run of decimal digits; returns the value and advances `pos`.
// The accumulator saturates at a safe bound so a maliciously long parameter
// cannot overflow the signed integer (undefined behavior); all digits are still
// consumed so `pos` (and the caller's `consumed`) stays correct.
std::int64_t parseDecimal(std::string_view text, std::size_t& pos) {
    constexpr std::int64_t saturation = 1'000'000'000;
    std::int64_t value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        if (value < saturation) value = value * 10 + (text[pos] - '0');
        ++pos;
    }
    return value;
}

// The key a single printable ASCII byte stands for, or None if the byte has no
// key we bind or route as a chord.  Letters fold to KeyA..KeyZ with a shift flag
// (the key itself carries no shift; the caller sets it).  Letters and digits are
// contiguous in KeyCode, so folding is arithmetic and allocates nothing.
constexpr ssg::KeyCode asciiKeyCode(unsigned char byte) {
    auto const offsetFrom = [](ssg::KeyCode base, int offset) {
        return static_cast<ssg::KeyCode>(
            static_cast<std::uint16_t>(base) + offset);
    };
    if (byte >= 'a' && byte <= 'z') return offsetFrom(ssg::KeyCode::KeyA, byte - 'a');
    if (byte >= 'A' && byte <= 'Z') return offsetFrom(ssg::KeyCode::KeyA, byte - 'A');
    if (byte >= '0' && byte <= '9') return offsetFrom(ssg::KeyCode::Digit0, byte - '0');
    switch (byte) {
    case '[': return ssg::KeyCode::BracketLeft;
    case ']': return ssg::KeyCode::BracketRight;
    case '\\': return ssg::KeyCode::Backslash;
    case ';': return ssg::KeyCode::Semicolon;
    case '\'': return ssg::KeyCode::Quote;
    case ',': return ssg::KeyCode::Comma;
    case '.': return ssg::KeyCode::Period;
    case '/': return ssg::KeyCode::Slash;
    case '-': return ssg::KeyCode::Minus;
    case '=': return ssg::KeyCode::Equal;
    case '`': return ssg::KeyCode::Backquote;
    case ' ': return ssg::KeyCode::Space;
    default: return ssg::KeyCode::None;
    }
}

// Length of the UTF-8 sequence introduced by `lead`, or 0 for a continuation or
// invalid lead byte.
std::size_t utf8Length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if (lead >= 0xF0) return 4;
    if (lead >= 0xE0) return 3;
    if (lead >= 0xC0) return 2;
    return 0;
}

}  // namespace

Decoded decode_input(std::string_view bytes, bool inputExhausted,
                     std::size_t& consumed) {
    consumed = 0;
    if (bytes.empty()) return {};

    auto const first = static_cast<unsigned char>(bytes[0]);

    if (first == '\r' || first == '\n') {
        consumed = 1;
        return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::Enter}, {}, 0};
    }
    if (first == 0x7f || first == 0x08) {
        consumed = 1;
        return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::Backspace}, {}, 0};
    }
    if (first == '\t') {
        consumed = 1;
        return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::Tab}, {}, 0};
    }
    if (first == 0x1b) {
        if (bytes.size() < 2) {
            // A lone ESC: a complete Escape stroke only once input is exhausted;
            // otherwise wait for the byte that disambiguates CSI vs. chord.
            if (inputExhausted) {
                consumed = 1;
                return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::Escape}, {}, 0};
            }
            return {DecodeStatus::incomplete, {}, {}, 0};
        }
        auto const second = static_cast<unsigned char>(bytes[1]);
        if (second != '[' && second != 'O') {
            // ESC followed by a non-CSI byte: ESC is a standalone Escape stroke;
            // the next byte is decoded on the following call.
            consumed = 1;
            return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::Escape}, {}, 0};
        }
        if (bytes.size() < 3) return {DecodeStatus::incomplete, {}, {}, 0};
        auto const third = static_cast<unsigned char>(bytes[2]);
        switch (third) {
        case '1': {
            // Modified key: ESC [ 1 ; m {A|B|C|D|H|F}, modifier m = 1 + bitmask
            // (bit0 Shift, bit1 Alt, bit2 Ctrl).  Any partial parameter is
            // incomplete until the final letter arrives.
            if (bytes.size() < 4) return {DecodeStatus::incomplete, {}, {}, 0};
            if (bytes[3] != ';') {
                consumed = 3;  // Some other '1'-prefixed CSI; skip conservatively.
                return {DecodeStatus::none, {}, {}, 0};
            }
            std::size_t pos = 4;
            auto const modifier = parseDecimal(bytes, pos);
            if (pos == 4 || pos >= bytes.size()) {
                return {DecodeStatus::incomplete, {}, {}, 0};  // Await digits/final.
            }
            auto const final = static_cast<unsigned char>(bytes[pos]);
            ssg::KeyCode code = ssg::KeyCode::None;
            switch (final) {
            case 'A': code = ssg::KeyCode::ArrowUp; break;
            case 'B': code = ssg::KeyCode::ArrowDown; break;
            case 'C': code = ssg::KeyCode::ArrowRight; break;
            case 'D': code = ssg::KeyCode::ArrowLeft; break;
            case 'H': code = ssg::KeyCode::Home; break;
            case 'F': code = ssg::KeyCode::End; break;
            default:
                consumed = pos + 1;  // Unknown final byte; skip.
                return {DecodeStatus::none, {}, {}, 0};
            }
            consumed = pos + 1;
            ssg::KeyStroke stroke{code};
            auto const bitmask = modifier - 1;
            // Only Shift/Alt/Ctrl are supported; any other bits (e.g. m=9) fall
            // back to the plain, unmodified arrow.
            if (bitmask > 0 && (bitmask & ~std::int64_t{0b111}) == 0) {
                stroke.shift = (bitmask & 0b001) != 0;
                stroke.alt = (bitmask & 0b010) != 0;
                stroke.control = (bitmask & 0b100) != 0;
            }
            return {DecodeStatus::key, stroke, {}, 0};
        }
        case 'A': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::ArrowUp}, {}, 0};
        case 'B': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::ArrowDown}, {}, 0};
        case 'C': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::ArrowRight}, {}, 0};
        case 'D': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::ArrowLeft}, {}, 0};
        case 'H': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::Home}, {}, 0};
        case 'F': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::End}, {}, 0};
        case '3':
        case '5':
        case '6': {
            // Delete/PageUp/PageDown: plain ESC [ 3|5|6 ~, or the modified form
            // ESC [ 3|5|6 ; m ~ (m = 1 + bitmask: bit0 Shift, bit1 Alt, bit2
            // Ctrl), mirroring the '1'-prefixed arrow/Home/End modifier
            // handling above. Without this case, ESC [ 3 ~ (Delete) fell
            // through to the `default` branch below, which consumes only the
            // "ESC [ 3" introducer and leaves the trailing '~' byte to be
            // decoded on the NEXT call as plain printable text -- inserting a
            // literal "~" instead of deleting forward.
            ssg::KeyCode const code = third == '3'   ? ssg::KeyCode::Delete
                                     : third == '5' ? ssg::KeyCode::PageUp
                                                    : ssg::KeyCode::PageDown;
            if (bytes.size() < 4) return {DecodeStatus::incomplete, {}, {}, 0};
            if (bytes[3] == '~') {
                consumed = 4;
                return {DecodeStatus::key, ssg::KeyStroke{code}, {}, 0};
            }
            if (bytes[3] != ';') {
                consumed = 4;  // Unknown '3'/'5'/'6'-prefixed CSI; skip conservatively.
                return {DecodeStatus::none, {}, {}, 0};
            }
            std::size_t pos = 4;
            auto const modifier = parseDecimal(bytes, pos);
            if (pos == 4 || pos >= bytes.size()) {
                return {DecodeStatus::incomplete, {}, {}, 0};  // Await digits/final.
            }
            if (bytes[pos] != '~') {
                consumed = pos + 1;  // Malformed; skip.
                return {DecodeStatus::none, {}, {}, 0};
            }
            consumed = pos + 1;
            ssg::KeyStroke stroke{code};
            auto const bitmask = modifier - 1;
            if (bitmask > 0 && (bitmask & ~std::int64_t{0b111}) == 0) {
                stroke.shift = (bitmask & 0b001) != 0;
                stroke.alt = (bitmask & 0b010) != 0;
                stroke.control = (bitmask & 0b100) != 0;
            }
            return {DecodeStatus::key, stroke, {}, 0};
        }
        case '<': {
            // SGR mouse: ESC [ < Cb ; Cx ; Cy (M|m).  Cb encodes the button in
            // its low 2 bits, motion in bit 5 (a drag when a button is held),
            // and the wheel in bit 6 (64 up, 65 down).  The final byte is 'M'
            // for press/drag and 'm' for release.  Coordinates are 1-based.
            std::size_t end = 3;
            while (end < bytes.size() && bytes[end] != 'M' && bytes[end] != 'm') ++end;
            if (end >= bytes.size()) return {DecodeStatus::incomplete, {}, {}, 0};
            char const finalByte = bytes[end];
            consumed = end + 1;  // A malformed-but-terminated sequence is consumed.
            std::size_t pos = 3;
            // Each of Cb/Cx/Cy must be a non-empty run of digits followed by its
            // delimiter; Cy must end exactly at the final byte (no trailing junk).
            // Otherwise the sequence is malformed and dropped rather than
            // dispatching a command from garbage bytes.
            auto parseField = [&](std::int64_t& out) {
                std::size_t const start = pos;
                out = parseDecimal(bytes, pos);
                return pos > start;
            };
            std::int64_t cb = 0;
            std::int64_t cx = 0;
            std::int64_t cy = 0;
            if (!parseField(cb)) return {DecodeStatus::none, {}, {}, 0};
            if (pos >= bytes.size() || bytes[pos] != ';') return {DecodeStatus::none, {}, {}, 0};
            ++pos;
            if (!parseField(cx)) return {DecodeStatus::none, {}, {}, 0};
            if (pos >= bytes.size() || bytes[pos] != ';') return {DecodeStatus::none, {}, {}, 0};
            ++pos;
            if (!parseField(cy)) return {DecodeStatus::none, {}, {}, 0};
            if (pos != end) return {DecodeStatus::none, {}, {}, 0};
            if (cb == 64 || cb == 65) {
                Decoded decoded;
                decoded.status = DecodeStatus::scroll;
                decoded.scroll = cb == 64 ? -3 : 3;
                // Carry the pointer position so the app can route the wheel to
                // the region under the cursor (the panel scrolls, not just the
                // editor).
                decoded.pointer.column = static_cast<int>(cx > 0 ? cx - 1 : 0);
                decoded.pointer.row = static_cast<int>(cy > 0 ? cy - 1 : 0);
                return decoded;
            }
            if ((cb & 64) != 0) return {DecodeStatus::none, {}, {}, 0};  // other wheel/ext
            PointerEvent event;
            event.column = static_cast<int>(cx > 0 ? cx - 1 : 0);
            event.row = static_cast<int>(cy > 0 ? cy - 1 : 0);
            auto const buttonBits = cb & 3;
            event.button = buttonBits == 0   ? PointerButton::left
                           : buttonBits == 1 ? PointerButton::middle
                           : buttonBits == 2 ? PointerButton::right
                                              : PointerButton::other;
            event.kind = finalByte == 'm' ? PointerKind::release
                         : (cb & 32) != 0  ? PointerKind::drag
                                           : PointerKind::press;
            Decoded decoded;
            decoded.status = DecodeStatus::pointer;
            decoded.pointer = event;
            return decoded;
        }
        case 'M': {
            // Legacy X10 mouse: ESC [ M b x y.  Wheel up 0x60, down 0x61.
            if (bytes.size() < 6) return {DecodeStatus::incomplete, {}, {}, 0};
            auto const button = static_cast<unsigned char>(bytes[3]);
            consumed = 6;
            if (button == 0x60) return {DecodeStatus::scroll, {}, {}, -3};
            if (button == 0x61) return {DecodeStatus::scroll, {}, {}, 3};
            return {DecodeStatus::none, {}, {}, 0};
        }
        default:
            consumed = 3;  // Unknown CSI: skip its introducer conservatively.
            return {DecodeStatus::none, {}, {}, 0};
        }
    }
    if (first >= 0x20) {
        auto const length = utf8Length(first);
        if (length == 0) {
            consumed = 1;  // Stray UTF-8 continuation byte; skip.
            return {DecodeStatus::none, {}, {}, 0};
        }
        if (bytes.size() < length) return {DecodeStatus::incomplete, {}, {}, 0};
        auto text = std::string{bytes.substr(0, length)};
        consumed = length;
        ssg::KeyStroke stroke;
        if (length == 1) {
            stroke.code = asciiKeyCode(first);
            stroke.shift = first >= 'A' && first <= 'Z';
        }
        // A printable commits text and, when it has a keycode, also carries a
        // stroke so it can participate in a chord (e.g. Escape then KeyS).
        return {DecodeStatus::key, stroke, std::move(text), 0};
    }
    consumed = 1;  // Other control byte: ignore.
    return {DecodeStatus::none, {}, {}, 0};
}

}  // namespace ssg::app
