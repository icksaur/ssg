#include "ssg_terminal.h"

#include <ssg/Keymap.h>
#include <ssg/Theme.h>

#include <csignal>

#include <atomic>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

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


struct TerminalModes::Impl {
    explicit Impl(Writer configuredWriter) : writer{std::move(configuredWriter)} {}
    Writer writer;
    std::vector<TerminalMode> entered;
};

TerminalModes::TerminalModes(Writer writer)
    : impl_{std::make_unique<Impl>(std::move(writer))} {}

TerminalModes::~TerminalModes() { leaveThrough(0); }

TerminalModes::Guard TerminalModes::enter(TerminalMode mode) {
    impl_->entered.push_back(mode);
    impl_->writer(mode.enter);
    return Guard{*this, impl_->entered.size()};
}

// Leaves every mode entered at or above `depth`, deepest first.
void TerminalModes::leaveThrough(std::size_t depth) noexcept {
    while (impl_->entered.size() > depth) {
        auto const mode = impl_->entered.back();
        impl_->entered.pop_back();
        impl_->writer(mode.leave);
    }
}

std::size_t TerminalModes::depth() const noexcept {
    return impl_->entered.size();
}

TerminalModes::Guard::~Guard() {
    if (owner_ != nullptr) owner_->leaveThrough(depth_ - 1);
}

TerminalModes::Guard::Guard(Guard&& other) noexcept
    : owner_{other.owner_}, depth_{other.depth_} {
    other.owner_ = nullptr;
}

TerminalModes::Guard& TerminalModes::Guard::operator=(Guard&& other) noexcept {
    if (this != &other) {
        if (owner_ != nullptr) owner_->leaveThrough(depth_ - 1);
        owner_ = other.owner_;
        depth_ = other.depth_;
        other.owner_ = nullptr;
    }
    return *this;
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

// Base64 for OSC 52, which carries its payload that way.  Written out rather
// than pulled in: this is the only base64 in the app, and a dependency for
// twenty lines would cost more than it saves.
std::string base64(std::string_view bytes) {
    static constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t index = 0;
    while (index + 2 < bytes.size()) {
        std::uint32_t const triple =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 1])) << 8) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index + 2]));
        encoded += kAlphabet[(triple >> 18) & 0x3f];
        encoded += kAlphabet[(triple >> 12) & 0x3f];
        encoded += kAlphabet[(triple >> 6) & 0x3f];
        encoded += kAlphabet[triple & 0x3f];
        index += 3;
    }
    if (index < bytes.size()) {
        std::uint32_t triple =
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index])) << 16;
        bool const two = index + 1 < bytes.size();
        if (two) {
            triple |= static_cast<std::uint32_t>(
                          static_cast<unsigned char>(bytes[index + 1]))
                      << 8;
        }
        encoded += kAlphabet[(triple >> 18) & 0x3f];
        encoded += kAlphabet[(triple >> 12) & 0x3f];
        encoded += two ? kAlphabet[(triple >> 6) & 0x3f] : '=';
        encoded += '=';
    }
    return encoded;
}

std::string encode_clipboard_write(std::string_view text) {
    // OSC 52 selection 'c' is the system clipboard.  Terminated with ST rather
    // than BEL: both are accepted, and ST is the form the standard specifies.
    return "\x1b]52;c;" + base64(text) + "\x1b\\";
}

std::optional<std::string> SystemClipboardWriter::bytesFor(
    std::optional<ssg::ClipboardWrite> const& write, bool terminalCanWrite) {
    if (!write || !terminalCanWrite) return std::nullopt;
    if (served_ && *served_ == write->id) return std::nullopt;
    served_ = write->id;
    return encode_clipboard_write(write->text);
}

std::string encode_frame(ssg::CellGrid const& screen, ssg::ColorDepth depth,
                         bool showCursor) {
    std::string frame;
    {
        // Appends to the frame rather than writing to the terminal, so the whole
        // frame is still one write.
        TerminalModes modes{
            [&frame](std::string_view bytes) { frame.append(bytes); }};
        auto const hidden = modes.enter(kCursorHidden);
        frame += encode_ansi_frame(screen, depth);
        if (screen.caret && showCursor) {
            frame += "\x1b[" + std::to_string(screen.caret->row + 1) + ";" +
                     std::to_string(screen.caret->column + 1) + "H";
        }
    }
    if (!showCursor) {
        // Painting hides the cursor and the guard above shows it again, which is
        // right for a normal frame.  While a scrollbar thumb is being dragged the
        // caret is not what the user is looking at, and a visible cursor lands
        // wherever painting ended -- it flickers around the screen chasing each
        // frame.  Re-hide it, using the mode's own declared bytes rather than a
        // hand-written escape.  Deliberately NOT balanced within the frame: it is
        // meant to persist until the drag ends and the next frame shows it again,
        // and terminal teardown restores every mode in kAllModes unconditionally,
        // so no exit path can leave the cursor hidden
        // (doc/spec-terminal-escape-discipline.md).
        frame.append(kCursorHidden.enter);
    }
    return frame;
}

namespace {

std::string_view underlineSgr(ssg::CellUnderline underline) {
    // SGR 4:3 is a CURLY underline and 58;5;n colours it independently of the
    // text, so a squiggle sits under syntax-coloured code without recolouring
    // it.  Both are widely supported and, crucially, DEGRADE WELL: a terminal
    // that does not know 4:3 draws a plain underline, and one that does not know
    // 58 draws it in the text colour.  Nothing is queried because there is
    // nothing to ask -- and nothing worth refusing to draw.
    switch (underline) {
    case ssg::CellUnderline::Error: return "\x1b[4:3m\x1b[58;5;1m";
    case ssg::CellUnderline::Warning: return "\x1b[4:3m\x1b[58;5;3m";
    case ssg::CellUnderline::Info: return "\x1b[4:2m\x1b[58;5;4m";
    case ssg::CellUnderline::None: return "\x1b[4:0m\x1b[59m";
    }
    return "\x1b[4:0m\x1b[59m";
}

}  // namespace

std::string encode_ansi_frame(ssg::CellGrid const& screen, ssg::ColorDepth depth) {    constexpr std::size_t maxIndex = ssg::kThemePaletteSize - 1;
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
    // Where each clickable run starts and ends, so the OSC 8 open and close land
    // exactly around it.  A hyperlink left open would make the rest of the line
    // clickable, which is worse than not linking at all.
    std::unordered_map<std::int64_t, std::string const*> linkOpensAt;
    std::unordered_set<std::int64_t> linkClosesAt;
    auto const cellKey = [columns](int x, int y) {
        return static_cast<std::int64_t>(y) * columns + x;
    };
    for (auto const& link : screen.hyperlinks) {
        if (link.width <= 0) continue;
        linkOpensAt.emplace(cellKey(link.column, link.row), &link.uri);
        linkClosesAt.insert(cellKey(link.column + link.width, link.row));
    }
    for (int y = 0; y < rows; ++y) {
        out += "\x1b[" + std::to_string(y + 1) + ";1H\x1b[0m";
        int foreground = -1;
        int background = -1;
        int role = -1;
        auto tint = ssg::DiffTint::None;
        auto underline = ssg::CellUnderline::None;
        for (int x = 0; x < columns; ++x) {
            auto const& cell =
                screen.cells[static_cast<std::size_t>(y * columns + x)];
            if (cell.continuation) continue;
            // OSC 8: the terminal makes the bytes between open and close
            // clickable.  Emitted around the run rather than per cell, and
            // always closed, so a link cannot bleed into the text after it.
            auto const key = cellKey(x, y);
            if (linkClosesAt.contains(key)) out += "\x1b]8;;\x1b\\";
            if (auto const opens = linkOpensAt.find(key);
                opens != linkOpensAt.end()) {
                out += "\x1b]8;;" + *opens->second + "\x1b\\";
            }
            if (cell.foreground != foreground || cell.background != background ||
                cell.tint != tint || cell.underline != underline ||
                static_cast<int>(cell.role) != role) {
                out += paletteColor(cell.foreground, '3');
                if (cell.tint != ssg::DiffTint::None) {
                    out += color(tintColor(cell.tint), '4');
                } else if (cell.role == ssg::SemanticRole::Selection) {
                    out += color(screen.selectionFill, '4');
                } else {
                    out += paletteColor(cell.background, '4');
                }
                out += underlineSgr(cell.underline);
                foreground = cell.foreground;
                background = cell.background;
                tint = cell.tint;
                underline = cell.underline;
                role = static_cast<int>(cell.role);
            }
            out += cell.text.empty() ? std::string{" "} : cell.text;
        }
        // A run reaching the last column has no cell to close at, so close here
        // rather than leaving the link open into the next row.
        if (linkClosesAt.contains(cellKey(columns, y))) out += "\x1b]8;;\x1b\\";
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

// A CSI runs ESC [ , then parameter bytes (0x30-0x3F), then intermediate bytes
// (0x20-0x2F), then one final byte (0x40-0x7E).  Locating that final byte is the
// only way to know where an unrecognized sequence ends; guessing at its length
// is what lets a sequence's tail spill into the document as typed text
// (doc/spec-terminal-capabilities.md, INV-reply-never-input).
enum class CsiScan {
    complete,    // `end` is one past the final byte.
    incomplete,  // The final byte has not arrived; wait for more input.
    malformed,   // Not a legal CSI, or longer than any real one; discard `end`.
};

CsiScan scanCsi(std::string_view bytes, std::size_t& end) {
    std::size_t index = 2;
    auto within = [&] { return index < bytes.size() && index < kMaxSequenceBytes; };
    auto const at = [&](std::size_t i) { return static_cast<unsigned char>(bytes[i]); };
    while (within() && at(index) >= 0x30 && at(index) <= 0x3f) ++index;
    while (within() && at(index) >= 0x20 && at(index) <= 0x2f) ++index;
    if (index >= kMaxSequenceBytes) {
        // No real CSI is this long, so nothing is gained by holding the buffer
        // waiting for a terminator that is evidently not coming.
        end = kMaxSequenceBytes;
        return CsiScan::malformed;
    }
    if (index >= bytes.size()) {
        end = index;
        return CsiScan::incomplete;
    }
    end = index + 1;
    return at(index) >= 0x40 && at(index) <= 0x7e ? CsiScan::complete
                                                  : CsiScan::malformed;
}

// A terminal report rather than a keypress: private-prefixed CSIs (DA1, DECRQM,
// the keyboard protocol) and the window/cell reports ending in 't'.  DCS and OSC
// replies are deliberately not recognized -- SSG asks no DCS/OSC question, so it
// can never receive one, and treating ESC P / ESC ] as a report introducer would
// break the Alt+<key> chords the keymap relies on (they are the same bytes).
bool isReplyCsi(std::string_view bytes, std::size_t end) {
    if (end < 3) return false;
    auto const final = static_cast<unsigned char>(bytes[end - 1]);
    if (bytes[2] == '?') return final == 'c' || final == 'u' || final == 'y';
    return final == 't';
}

Decoded unhandledCsi(std::string_view bytes, std::size_t end, std::size_t& consumed) {
    consumed = end;
    if (!isReplyCsi(bytes, end)) return {DecodeStatus::none, {}, {}, 0};
    Decoded decoded;
    decoded.status = DecodeStatus::reply;
    decoded.reply = std::string{bytes.substr(0, end)};
    return decoded;
}

enum class ModifierMode { Legacy, Kitty };

// The `1 + bitmask` modifier encoding shared by legacy CSI sequences (arrows,
// Home/End, Delete/Page) and the Kitty `CSI ... u` form -- one place owns what a
// modifier bit means (bit0 Shift, bit1 Alt, bit2 Ctrl, and in Kitty additionally
// bit3 Super, bit4 Hyper, bit5 Meta, bit6 CapsLock, bit7 NumLock).  `modifier` is
// the raw parameter as sent (1-based; 1 or absent means no modifiers).
//
// Legacy callers IGNORE any bit outside Shift/Alt/Ctrl: an unknown modifier falls
// back to the plain key, preserving the pre-Kitty behaviour.  Kitty callers
// additionally map Meta to the bindable `meta` modifier.  Super and Hyper are
// ignored (Tier A binds neither, and collapsing three physical modifiers onto
// `meta` would make them indistinguishable).  CapsLock and NumLock are
// deliberately NOT applied to any KeyStroke bit -- a lock must never enter a
// stroke, or it would fail to match every binding (KeyStroke is compared by value
// for keymap resolution).  That is exactly what fixes the caps-lock ambiguity:
// caps no longer perturbs the decoded stroke.  Surfacing lock state for a future
// CAPS indicator is a Tier-B concern (report-all-keys) not built here.
void applyModifierBitmask(ssg::KeyStroke& stroke, std::int64_t modifier,
                          ModifierMode mode) {
    if (modifier <= 1) return;
    auto const bitmask = modifier - 1;
    if (mode == ModifierMode::Legacy && (bitmask & ~std::int64_t{0b111}) != 0) {
        return;  // Unknown modifier -> plain key, as before.
    }
    stroke.shift = (bitmask & 0b001) != 0;
    stroke.alt = (bitmask & 0b010) != 0;
    stroke.control = (bitmask & 0b100) != 0;
    if (mode == ModifierMode::Kitty) {
        stroke.meta = (bitmask & 0b100000) != 0;
    }
}

// The `KeyCode` a Kitty unicode-key-code stands for.  Printable ASCII folds
// through the same table the legacy path uses (letters -> KeyA..KeyZ; the shift
// flag comes from the modifier bitmask, never the codepoint, which is already the
// unshifted layout key -- this is what makes it caps-lock-immune).  The
// disambiguated named keys map by their control codepoint.  Kitty's functional
// PUA codepoints (arrows/Home/End/Page under the higher flag sets) are not
// emitted under the disambiguate flag this build enables, so they are not mapped
// here.
ssg::KeyCode kittyKeyCode(std::int64_t codepoint) {
    switch (codepoint) {
    case 27: return ssg::KeyCode::Escape;
    case 13: return ssg::KeyCode::Enter;
    case 9: return ssg::KeyCode::Tab;
    case 127: return ssg::KeyCode::Backspace;
    default: break;
    }
    if (codepoint >= 0x20 && codepoint <= 0x7e) {
        return asciiKeyCode(static_cast<unsigned char>(codepoint));
    }
    return ssg::KeyCode::None;
}

// Decode a Kitty keyboard-protocol key event: `CSI unicode-key[:alt[:base]]
// [;mods[:event]][;text] u`, with NO private prefix.  Returns nullopt when the
// sequence is not a Kitty key event (so the caller can still classify it as a
// terminal reply or drop it) -- the classification is purely structural: a
// non-private, `u`-terminated CSI whose first field is a numeric key code.  The
// capability reply `CSI ? ... u` (private prefix) and legacy input (never a bare
// `u`) are both excluded, so no mode flag is needed to tell key events apart.
//
// Only the unshifted key code and the modifier field are read; the shifted-key /
// base-layout sub-parameters, the event-type sub-parameter, and the associated-
// text field are skipped.  Tier A relies on the disambiguate flag alone, under
// which unmodified printables stay on the UTF-8 text path and only modified or
// disambiguated keys (which are bindings, not text) arrive here -- so no committed
// text is produced.
std::optional<Decoded> decodeKittyKey(std::string_view bytes, std::size_t end,
                                      std::size_t& consumed) {
    if (end < 3 || static_cast<unsigned char>(bytes[end - 1]) != 'u') {
        return std::nullopt;
    }
    if (bytes[2] == '?') return std::nullopt;  // capability reply, not a key.
    std::size_t pos = 2;
    std::int64_t const codepoint = parseDecimal(bytes, pos);
    if (pos == 2) return std::nullopt;  // no numeric key code -> not a key event.
    while (pos + 1 < end && bytes[pos] != ';') ++pos;  // skip field-1 sub-params.
    std::int64_t modifier = 1;
    if (pos + 1 < end && bytes[pos] == ';') {
        ++pos;
        modifier = parseDecimal(bytes, pos);
        if (modifier < 1) modifier = 1;  // empty field -> no modifiers.
    }
    consumed = end;
    ssg::KeyCode const code = kittyKeyCode(codepoint);
    if (code == ssg::KeyCode::None) {
        // A key SSG does not name: consume it so its bytes never reach the
        // document, but emit no stroke.
        return Decoded{DecodeStatus::none, {}, {}, 0};
    }
    ssg::KeyStroke stroke{code};
    applyModifierBitmask(stroke, modifier, ModifierMode::Kitty);
    return Decoded{DecodeStatus::key, stroke, {}, 0};
}

}  // namespace

std::string_view color_depth_name(ssg::ColorDepth depth) {
    switch (depth) {
    case ssg::ColorDepth::Truecolor: return "truecolor";
    case ssg::ColorDepth::Indexed256: return "indexed256";
    case ssg::ColorDepth::Ansi16: return "ansi16";
    }
    return "unknown";
}

std::string_view capability_name(Capability capability) {
    switch (capability) {
    case Capability::SynchronizedOutput: return "synchronized_output";
    case Capability::KeyboardProtocol: return "keyboard_protocol";
    case Capability::ClipboardWrite: return "clipboard_write";
    }
    return "unknown";
}

namespace {

// A report's parts: the private-prefix flag, its numeric parameters, its
// intermediate byte if any, and its final byte.  Parsing is separated from
// interpretation so each answer below reads as the protocol rule it encodes.
struct ReplyParts {
    bool privatePrefix = false;
    std::vector<std::int64_t> params;
    char intermediate = '\0';
    char final = '\0';
};

std::optional<ReplyParts> parseReply(std::string_view reply) {
    if (reply.size() < 3 || reply[0] != 0x1b || reply[1] != '[') return std::nullopt;
    ReplyParts parts;
    std::size_t index = 2;
    if (reply[index] == '?') {
        parts.privatePrefix = true;
        ++index;
    }
    // Parameters are decimal runs separated by ';'.  An empty run is a defaulted
    // parameter and is reported as such rather than skipped, so positions hold.
    std::int64_t current = 0;
    bool anyDigit = false;
    for (; index < reply.size(); ++index) {
        auto const byte = static_cast<unsigned char>(reply[index]);
        if (byte >= '0' && byte <= '9') {
            if (current < 1'000'000) current = current * 10 + (byte - '0');
            anyDigit = true;
            continue;
        }
        if (byte == ';') {
            parts.params.push_back(anyDigit ? current : 0);
            current = 0;
            anyDigit = false;
            continue;
        }
        break;
    }
    if (anyDigit) parts.params.push_back(current);
    if (index < reply.size()) {
        auto const byte = static_cast<unsigned char>(reply[index]);
        if (byte >= 0x20 && byte <= 0x2f) {
            parts.intermediate = reply[index];
            ++index;
        }
    }
    if (index >= reply.size()) return std::nullopt;
    parts.final = reply[index];
    return parts;
}

std::string overrideVariable(Capability capability) {
    std::string name = "SSG_TERM_";
    for (char const ch : capability_name(capability)) {
        name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    return name;
}

}  // namespace

TerminalCapabilities::TerminalCapabilities(EnvironmentLookup lookup, Clock clock)
    : lookup_{std::move(lookup)}, clock_{std::move(clock)} {
    if (!clock_) {
        clock_ = [] { return std::chrono::steady_clock::now(); };
    }
    answers_.fill(Answer::Unknown);
    auto const read = [&](char const* name) {
        return lookup_ ? lookup_(name) : nullptr;
    };
    colorDepth_ = detect_color_depth(read("SSG_COLOR_DEPTH"), read("COLORTERM"),
                                     read("TERM"), read("TERM_PROGRAM"));
}

std::string TerminalCapabilities::beginProbe() {
    // Start from Unknown rather than carrying answers forward.  A second probe
    // asks a terminal that may not be the one that answered the first -- a
    // resumed session, a reattached multiplexer -- and a stale Present would
    // survive the fence that is supposed to be able to retire it.  Resetting can
    // only ever turn features off until their answers land, which is the safe
    // direction.
    answers_.fill(Answer::Unknown);
    probing_ = true;
    log_ = {};
    deadline_ = clock_() + kProbeWindow;
    // DA1 is written last: every terminal answers it, so its reply is the fence
    // that tells us the speculative questions above have had their chance.
    return std::string{"\x1b[?2026$p"}  // Synchronized output (DECRQM).
           + "\x1b[?u"                  // Keyboard protocol flags.
           + "\x1b[c";                  // Primary device attributes: the fence.
}

void TerminalCapabilities::observeReply(std::string_view reply) {
    if (expired()) endProbe();
    if (!probing_) {
        log_.ignored.emplace_back(reply);
        return;
    }
    log_.believed.emplace_back(reply);
    auto const parts = parseReply(reply);
    if (!parts) return;
    auto const record = [&](Capability capability, bool present) {
        answers_[static_cast<std::size_t>(capability)] =
            present ? Answer::Present : Answer::Absent;
    };

    if (parts->final == 'c' && parts->privatePrefix) {
        // DA1: the leading parameter is the terminal class; the rest are
        // extensions, of which 52 advertises clipboard access.
        bool clipboard = false;
        for (std::size_t i = 1; i < parts->params.size(); ++i) {
            if (parts->params[i] == 52) clipboard = true;
        }
        record(Capability::ClipboardWrite, clipboard);
        endProbe();
        return;
    }
    if (parts->final == 'y' && parts->intermediate == '$' && parts->privatePrefix) {
        // DECRPM: mode, then state.  0 means the terminal does not recognize the
        // mode; 4 ("permanently reset") means it recognizes it but can never
        // enable it, which is indistinguishable from absent for our purposes.
        // 1 (set), 2 (reset) and 3 (permanently set) all mean usable.
        if (parts->params.size() >= 2 && parts->params[0] == 2026) {
            auto const state = parts->params[1];
            record(Capability::SynchronizedOutput,
                   state == 1 || state == 2 || state == 3);
        }
        return;
    }
    if (parts->final == 'u' && parts->privatePrefix) {
        // Only a terminal implementing the keyboard protocol answers at all.
        record(Capability::KeyboardProtocol, true);
    }
}

void TerminalCapabilities::endProbe() { probing_ = false; }

bool TerminalCapabilities::expired() const {
    return probing_ && clock_ && clock_() > deadline_;
}

bool TerminalCapabilities::probing() const { return probing_ && !expired(); }

std::optional<bool> TerminalCapabilities::override_for(Capability capability) const {
    if (!lookup_) return std::nullopt;
    char const* const value = lookup_(overrideVariable(capability));
    if (value == nullptr) return std::nullopt;
    auto const normalized = lowercase(value);
    if (matchesAny(normalized, {"1", "on", "yes", "true"})) return true;
    if (matchesAny(normalized, {"0", "off", "no", "false"})) return false;
    return std::nullopt;  // Unparseable: fall through to what the terminal said.
}

bool TerminalCapabilities::has(Capability capability) const {
    // An explicit override always wins: terminals lie, and a user hitting a
    // rendering bug needs a way to switch a feature off without rebuilding.
    if (auto const forced = override_for(capability)) return *forced;
    return answers_[static_cast<std::size_t>(capability)] == Answer::Present;
}

ssg::ColorDepth TerminalCapabilities::colorDepth() const { return colorDepth_; }

TerminalCapabilities::ProbeLog const& TerminalCapabilities::probeLog() const {
    return log_;
}

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
            // Meta-prefixed escape sequence: a terminal that transmits Alt as a
            // leading ESC sends Alt+<special key> as ESC followed by that key's
            // own CSI/SS3 sequence (e.g. Alt+Home = ESC ESC [ H, Alt+End =
            // ESC ESC [ F).  Decode the inner sequence and fold in Alt so it
            // matches the Alt+<named key> bindings, instead of surfacing a bare
            // Escape and an unmodified key.
            if (second == 0x1b && bytes.size() >= 3 &&
                (static_cast<unsigned char>(bytes[2]) == '[' ||
                 static_cast<unsigned char>(bytes[2]) == 'O')) {
                std::size_t innerConsumed = 0;
                auto inner =
                    decode_input(bytes.substr(1), inputExhausted, innerConsumed);
                if (inner.status == DecodeStatus::incomplete) {
                    return {DecodeStatus::incomplete, {}, {}, 0};
                }
                if (inner.status == DecodeStatus::key) {
                    inner.stroke.alt = true;
                    consumed = 1 + innerConsumed;
                    return inner;
                }
                // The inner bytes were not a key (a reply or noise): let the ESC
                // stand alone and re-decode the remainder on the next call.
                consumed = 1;
                return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::Escape}, {}, 0};
            }
            // Legacy meta-prefix: Alt+<key> transmits as ESC then the key's
            // byte, so ESC followed by a printable coalesces into one Alt stroke.
            // ESC [ and ESC O are excluded above as the CSI/SS3 introducers.  The
            // DCS/OSC/APC/PM/SOS string introducers (ESC P/]/X/^/_) are
            // byte-identical to Alt+<key> chords; that is safe only because SSG
            // solicits no DCS/OSC reply (noDcsOrOscQueryMaySolicitAnUnparsedReply),
            // so those bytes reach the decoder only from the keyboard.
            // Alt+<named key> whose byte is not a graphic printable: Backspace
            // (0x7f/0x08), Enter (0x0d/0x0a), Tab (0x09).  These carry no text
            // and must map to the named key, not fall into the printable branch
            // (where 0x7f would become a keycode-less text stroke and the
            // Alt+Backspace = delete-word binding would never resolve).
            auto const metaNamed = [&]() -> ssg::KeyCode {
                if (second == 0x7f || second == 0x08) return ssg::KeyCode::Backspace;
                if (second == '\r' || second == '\n') return ssg::KeyCode::Enter;
                if (second == '\t') return ssg::KeyCode::Tab;
                return ssg::KeyCode::None;
            }();
            if (metaNamed != ssg::KeyCode::None) {
                consumed = 2;
                ssg::KeyStroke stroke{metaNamed};
                stroke.alt = true;
                return {DecodeStatus::key, stroke, {}, 0};
            }
            if (second >= 0x20) {
                auto const length = utf8Length(second);
                if (length == 0) {
                    // A stray continuation byte after ESC: the ESC stands alone.
                    consumed = 1;
                    return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::Escape}, {}, 0};
                }
                if (bytes.size() < 1 + length) return {DecodeStatus::incomplete, {}, {}, 0};
                auto text = std::string{bytes.substr(1, length)};
                consumed = 1 + length;
                ssg::KeyStroke stroke;
                stroke.alt = true;
                if (length == 1) {
                    stroke.code = asciiKeyCode(second);
                    stroke.shift = second >= 'A' && second <= 'Z';
                }
                return {DecodeStatus::key, stroke, std::move(text), 0};
            }
            // ESC followed by another control byte (e.g. ESC ESC): a bare Escape;
            // the following byte decodes on the next call.
            consumed = 1;
            return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::Escape}, {}, 0};
        }
        if (bytes.size() < 3) return {DecodeStatus::incomplete, {}, {}, 0};
        auto const third = static_cast<unsigned char>(bytes[2]);
        // Every arm below that cannot interpret its sequence defers here rather
        // than guessing a length, so the sequence is consumed whole or not at
        // all (INV-reply-never-input).
        auto const unhandled = [&]() -> Decoded {
            if (second == 'O') {  // SS3 is always ESC O <final>.
                consumed = 3;
                return {DecodeStatus::none, {}, {}, 0};
            }
            std::size_t end = 0;
            switch (scanCsi(bytes, end)) {
            case CsiScan::incomplete:
                consumed = 0;
                return {DecodeStatus::incomplete, {}, {}, 0};
            case CsiScan::malformed:
                consumed = end;
                return {DecodeStatus::none, {}, {}, 0};
            case CsiScan::complete:
                break;
            }
            // The single Kitty seam: a complete, non-private, `u`-terminated CSI
            // is the self-identifying shape of a Kitty key event, so every arm
            // that could not interpret its sequence funnels through here.  When it
            // is not a Kitty key, fall through to the reply/none classifier.
            if (auto kitty = decodeKittyKey(bytes, end, consumed)) return *kitty;
            return unhandledCsi(bytes, end, consumed);
        };
        switch (third) {
        case '1': {
            // Modified key: ESC [ 1 ; m {A|B|C|D|H|F}, modifier m = 1 + bitmask
            // (bit0 Shift, bit1 Alt, bit2 Ctrl).  Any partial parameter is
            // incomplete until the final letter arrives.
            if (bytes.size() < 4) return {DecodeStatus::incomplete, {}, {}, 0};
            if (bytes[3] != ';') return unhandled();
            std::size_t pos = 4;
            auto const modifier = parseDecimal(bytes, pos);
            if (pos >= bytes.size()) {
                return {DecodeStatus::incomplete, {}, {}, 0};  // Await digits/final.
            }
            if (pos == 4) return unhandled();  // No modifier digits.
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
                return unhandled();  // Unknown final byte.
            }
            consumed = pos + 1;
            ssg::KeyStroke stroke{code};
            applyModifierBitmask(stroke, modifier, ModifierMode::Legacy);
            return {DecodeStatus::key, stroke, {}, 0};
        }
        case 'A': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::ArrowUp}, {}, 0};
        case 'B': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::ArrowDown}, {}, 0};
        case 'C': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::ArrowRight}, {}, 0};
        case 'D': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::ArrowLeft}, {}, 0};
        case 'H': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::Home}, {}, 0};
        case 'F': consumed = 3; return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::End}, {}, 0};
        case '2': {
            // Bracketed paste: ESC [ 200 ~ <payload> ESC [ 201 ~.  The payload is
            // CONTENT -- it must never be interpreted as keys, or a pasted
            // newline fires whatever Enter is bound to and a pasted escape
            // sequence is obeyed.  Consumed whole so nothing inside it reaches
            // the keymap.
            constexpr std::string_view kPasteStart = "\x1b[200~";
            constexpr std::string_view kPasteEnd = "\x1b[201~";
            if (bytes.size() < kPasteStart.size()) {
                if (kPasteStart.starts_with(bytes)) {
                    return {DecodeStatus::incomplete, {}, {}, 0};
                }
                return unhandled();  // Some other '2'-prefixed CSI.
            }
            if (bytes.substr(0, kPasteStart.size()) != kPasteStart) {
                return unhandled();
            }
            auto const end = bytes.find(kPasteEnd, kPasteStart.size());
            if (end == std::string_view::npos) {
                // The terminal writes the whole paste before anything else, but
                // a large one can still arrive across reads.  Bounded like every
                // other scan so a terminator that never comes cannot hold the
                // buffer (INV-decode-terminates).
                if (bytes.size() < kMaxPasteBytes) {
                    return {DecodeStatus::incomplete, {}, {}, 0};
                }
                consumed = bytes.size();
                return {DecodeStatus::none, {}, {}, 0};
            }
            Decoded decoded;
            decoded.status = DecodeStatus::paste;
            decoded.text = std::string{
                bytes.substr(kPasteStart.size(), end - kPasteStart.size())};
            consumed = end + kPasteEnd.size();
            return decoded;
        }
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
            if (bytes[3] != ';') return unhandled();
            std::size_t pos = 4;
            auto const modifier = parseDecimal(bytes, pos);
            if (pos >= bytes.size()) {
                return {DecodeStatus::incomplete, {}, {}, 0};  // Await digits/final.
            }
            if (pos == 4 || bytes[pos] != '~') return unhandled();
            consumed = pos + 1;
            ssg::KeyStroke stroke{code};
            applyModifierBitmask(stroke, modifier, ModifierMode::Legacy);
            return {DecodeStatus::key, stroke, {}, 0};
        }
        case '<': {
            // SGR mouse: ESC [ < Cb ; Cx ; Cy (M|m).  Cb encodes the button in
            // its low 2 bits, motion in bit 5 (a drag when a button is held),
            // and the wheel in bit 6 (64 up, 65 down).  The final byte is 'M'
            // for press/drag and 'm' for release.  Coordinates are 1-based.
            // The sequence's extent is decided by the CSI grammar, not by
            // hunting for 'M'/'m', so a truncated prefix cannot hold arbitrary
            // typed text hostage waiting for a terminator (INV-decode-terminates).
            std::size_t sequenceEnd = 0;
            switch (scanCsi(bytes, sequenceEnd)) {
            case CsiScan::incomplete: return {DecodeStatus::incomplete, {}, {}, 0};
            case CsiScan::malformed: consumed = sequenceEnd;
                return {DecodeStatus::none, {}, {}, 0};
            case CsiScan::complete: break;
            }
            std::size_t const end = sequenceEnd - 1;
            char const finalByte = bytes[end];
            if (finalByte != 'M' && finalByte != 'm') return unhandled();
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
            return unhandled();
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
        // stroke so it can resolve a binding (e.g. a single letter in a prompt).
        return {DecodeStatus::key, stroke, std::move(text), 0};
    }
    if (first >= 0x01 && first <= 0x1a) {
        // C0 control byte -> Ctrl+<letter>.  The bytes that name a key
        // (0x08 Backspace, 0x09 Tab, 0x0a/0x0d Enter, 0x1b Escape) are handled
        // above and never reach here, so what remains maps cleanly onto A..Z.
        consumed = 1;
        ssg::KeyStroke stroke;
        stroke.control = true;
        stroke.code = static_cast<ssg::KeyCode>(
            static_cast<std::uint16_t>(ssg::KeyCode::KeyA) + (first - 1));
        return {DecodeStatus::key, stroke, {}, 0};
    }
    consumed = 1;  // Other control byte: ignore.
    return {DecodeStatus::none, {}, {}, 0};
}

}  // namespace ssg::app
