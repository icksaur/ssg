#include "ssg_terminal.h"

#include <ssg/input.h>
#include <ssg/theme.h>

#include <algorithm>
#include <system_error>

namespace ssg::app {

namespace fs = std::filesystem;

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

std::string encode_ansi_frame(ssg::CellGrid const& screen) {
    constexpr std::size_t max_index = ssg::theme_palette_size - 1;
    auto color = [&](std::uint8_t index, char kind) {
        auto const& c = screen.palette[std::min<std::size_t>(index, max_index)];
        return "\x1b[" + std::string{kind} + "8;2;" +
               std::to_string(c.red) + ";" + std::to_string(c.green) + ";" +
               std::to_string(c.blue) + "m";
    };

    std::string out = "\x1b[H";
    int const columns = screen.size.columns;
    int const rows = screen.size.rows;
    for (int y = 0; y < rows; ++y) {
        out += "\x1b[" + std::to_string(y + 1) + ";1H\x1b[0m";
        int foreground = -1;
        int background = -1;
        for (int x = 0; x < columns; ++x) {
            auto const& cell =
                screen.cells[static_cast<std::size_t>(y * columns + x)];
            if (cell.continuation) continue;
            if (cell.foreground != foreground || cell.background != background) {
                out += color(cell.foreground, '3');
                out += color(cell.background, '4');
                foreground = cell.foreground;
                background = cell.background;
            }
            out += cell.text.empty() ? std::string{" "} : cell.text;
        }
    }
    out += "\x1b[0m";
    return out;
}

namespace {

// Parse a leading run of decimal digits; returns the value and advances `pos`.
std::int64_t parse_decimal(std::string_view text, std::size_t& pos) {
    std::int64_t value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        value = value * 10 + (text[pos] - '0');
        ++pos;
    }
    return value;
}

// The named KeyStroke code for a single printable ASCII byte, or "" if the byte
// has no keycode we bind or route as a chord.  Letters fold to KeyA..KeyZ with a
// shift flag; the code carries no shift itself (the caller sets it).
std::string ascii_key_code(unsigned char byte) {
    if (byte >= 'a' && byte <= 'z') return std::string{"Key"} + static_cast<char>(byte - 'a' + 'A');
    if (byte >= 'A' && byte <= 'Z') return std::string{"Key"} + static_cast<char>(byte);
    if (byte >= '0' && byte <= '9') return std::string{"Digit"} + static_cast<char>(byte);
    switch (byte) {
    case '[': return "BracketLeft";
    case ']': return "BracketRight";
    case '\\': return "Backslash";
    case ';': return "Semicolon";
    case '\'': return "Quote";
    case ',': return "Comma";
    case '.': return "Period";
    case '/': return "Slash";
    case '-': return "Minus";
    case '=': return "Equal";
    case '`': return "Backquote";
    case ' ': return "Space";
    default: return {};
    }
}

// Length of the UTF-8 sequence introduced by `lead`, or 0 for a continuation or
// invalid lead byte.
std::size_t utf8_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if (lead >= 0xF0) return 4;
    if (lead >= 0xE0) return 3;
    if (lead >= 0xC0) return 2;
    return 0;
}

}  // namespace

Decoded decode_input(std::string_view bytes, bool input_exhausted,
                     std::size_t& consumed) {
    consumed = 0;
    if (bytes.empty()) return {};

    auto const first = static_cast<unsigned char>(bytes[0]);

    if (first == '\r' || first == '\n') {
        consumed = 1;
        return {DecodeStatus::key, ssg::KeyStroke{"Enter"}, {}, 0};
    }
    if (first == 0x7f || first == 0x08) {
        consumed = 1;
        return {DecodeStatus::key, ssg::KeyStroke{"Backspace"}, {}, 0};
    }
    if (first == '\t') {
        consumed = 1;
        return {DecodeStatus::key, ssg::KeyStroke{"Tab"}, {}, 0};
    }
    if (first == 0x1b) {
        if (bytes.size() < 2) {
            // A lone ESC: a complete Escape stroke only once input is exhausted;
            // otherwise wait for the byte that disambiguates CSI vs. chord.
            if (input_exhausted) {
                consumed = 1;
                return {DecodeStatus::key, ssg::KeyStroke{"Escape"}, {}, 0};
            }
            return {DecodeStatus::incomplete, {}, {}, 0};
        }
        auto const second = static_cast<unsigned char>(bytes[1]);
        if (second != '[' && second != 'O') {
            // ESC followed by a non-CSI byte: ESC is a standalone Escape stroke;
            // the next byte is decoded on the following call.
            consumed = 1;
            return {DecodeStatus::key, ssg::KeyStroke{"Escape"}, {}, 0};
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
            auto const modifier = parse_decimal(bytes, pos);
            if (pos == 4 || pos >= bytes.size()) {
                return {DecodeStatus::incomplete, {}, {}, 0};  // Await digits/final.
            }
            auto const final = static_cast<unsigned char>(bytes[pos]);
            std::string code;
            switch (final) {
            case 'A': code = "ArrowUp"; break;
            case 'B': code = "ArrowDown"; break;
            case 'C': code = "ArrowRight"; break;
            case 'D': code = "ArrowLeft"; break;
            case 'H': code = "Home"; break;
            case 'F': code = "End"; break;
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
        case 'A': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{"ArrowUp"}, {}, 0};
        case 'B': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{"ArrowDown"}, {}, 0};
        case 'C': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{"ArrowRight"}, {}, 0};
        case 'D': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{"ArrowLeft"}, {}, 0};
        case 'H': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{"Home"}, {}, 0};
        case 'F': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{"End"}, {}, 0};
        case '5':
        case '6': {
            if (bytes.size() < 4) return {DecodeStatus::incomplete, {}, {}, 0};
            consumed = 4;
            if (bytes[3] == '~') {
                return {DecodeStatus::key,
                        ssg::KeyStroke{third == '5' ? "PageUp" : "PageDown"}, {}, 0};
            }
            return {DecodeStatus::none, {}, {}, 0};
        }
        case '<': {
            // SGR mouse: ESC [ < Cb ; Cx ; Cy (M|m).  Wheel up 64, down 65.
            std::size_t end = 3;
            while (end < bytes.size() && bytes[end] != 'M' && bytes[end] != 'm') ++end;
            if (end >= bytes.size()) return {DecodeStatus::incomplete, {}, {}, 0};
            std::size_t pos = 3;
            auto const button = parse_decimal(bytes, pos);
            consumed = end + 1;
            if (button == 64) return {DecodeStatus::scroll, {}, {}, -3};
            if (button == 65) return {DecodeStatus::scroll, {}, {}, 3};
            return {DecodeStatus::none, {}, {}, 0};
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
        auto const length = utf8_length(first);
        if (length == 0) {
            consumed = 1;  // Stray UTF-8 continuation byte; skip.
            return {DecodeStatus::none, {}, {}, 0};
        }
        if (bytes.size() < length) return {DecodeStatus::incomplete, {}, {}, 0};
        auto text = std::string{bytes.substr(0, length)};
        consumed = length;
        ssg::KeyStroke stroke;
        if (length == 1) {
            stroke.code = ascii_key_code(first);
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
