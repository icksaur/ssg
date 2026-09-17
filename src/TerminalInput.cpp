#include <ssg/TerminalInput.h>

#include <algorithm>
#include <array>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace ssg {

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
// Ctrl and Alt both mean SSG's single `mod`, and Ctrl+Alt together means NOTHING:
// that combination belongs to the windowing system, so a stroke carrying both is
// decoded as the plain key rather than as a chord SSG would swallow.
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
    // CapsLock (bit6) and NumLock (bit7) are lock STATES, not chord modifiers,
    // and a terminal speaking the Kitty protocol reports them in the modifier
    // field of even the legacy letter/tilde functional-key forms (Home/End/etc.).
    // Strip them first so they neither enter the stroke nor trip the Legacy
    // "unknown modifier" guard below -- otherwise Mod+Home with NumLock on
    // (bitmask 130 = NumLock|Alt) would be discarded as unsupported and lose its
    // modifier, and the binding would never resolve.
    constexpr std::int64_t kLockBits = 0b11000000;  // CapsLock | NumLock
    auto const bitmask = (modifier - 1) & ~kLockBits;
    if (mode == ModifierMode::Legacy && (bitmask & ~std::int64_t{0b111}) != 0) {
        return;  // Unknown modifier -> plain key, as before.
    }
    bool const alt = (bitmask & 0b010) != 0;
    bool const control = (bitmask & 0b100) != 0;
    if (alt && control) {
        return;  // Ctrl+Alt is the windowing system's; decode as the plain key.
    }
    stroke.shift = (bitmask & 0b001) != 0;
    stroke.mod = alt || control;
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
    case 57414: return ssg::KeyCode::Enter;  // Kitty keypad Enter (KP_ENTER).
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

Decoded decodeInputRaw(std::string_view bytes, bool inputExhausted,
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
                    decodeInputRaw(bytes.substr(1), inputExhausted, innerConsumed);
                if (inner.status == DecodeStatus::incomplete) {
                    return {DecodeStatus::incomplete, {}, {}, 0};
                }
                if (inner.status == DecodeStatus::key) {
                    // The ESC prefix IS Alt.  If the inner sequence already
                    // carried a modifier, that modifier was Ctrl, so this is
                    // Ctrl+Alt -- the windowing system's, not ours.  Drop the
                    // modifier rather than reporting a Mod chord SSG would then
                    // swallow.
                    inner.stroke.mod = !inner.stroke.mod;
                    consumed = 1 + innerConsumed;
                    return inner;
                }
                // The inner bytes were not a key (a reply or noise): let the ESC
                // stand alone and re-decode the remainder on the next call.
                consumed = 1;
                return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::Escape}, {}, 0};
            }
            // Legacy meta-prefix: Mod+<key> transmits as ESC then the key's
            // byte, so ESC followed by a printable coalesces into one Mod stroke.
            // ESC [ and ESC O are excluded above as the CSI/SS3 introducers.  The
            // DCS/OSC/APC/PM/SOS string introducers (ESC P/]/X/^/_) are
            // byte-identical to Mod+<key> chords; that is safe only because SSG
            // solicits no DCS/OSC reply (noDcsOrOscQueryMaySolicitAnUnparsedReply),
            // so those bytes reach the decoder only from the keyboard.
            // Ctrl+Alt+<key> transmits as ESC then a C0 byte, which the
            // control-byte arm at the bottom of this branch already decodes as a
            // bare Escape -- so this path needs no separate Ctrl+Alt rejection.
            // Mod+<named key> whose byte is not a graphic printable: Backspace
            // (0x7f/0x08), Enter (0x0d/0x0a), Tab (0x09).  These carry no text
            // and must map to the named key, not fall into the printable branch
            // (where 0x7f would become a keycode-less text stroke and the
            // Mod+Backspace = delete-word binding would never resolve).
            auto const metaNamed = [&]() -> ssg::KeyCode {
                if (second == 0x7f || second == 0x08) return ssg::KeyCode::Backspace;
                if (second == '\r' || second == '\n') return ssg::KeyCode::Enter;
                if (second == '\t') return ssg::KeyCode::Tab;
                return ssg::KeyCode::None;
            }();
            if (metaNamed != ssg::KeyCode::None) {
                consumed = 2;
                ssg::KeyStroke stroke{metaNamed};
                stroke.mod = true;
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
                stroke.mod = true;
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
            // incomplete until the final letter arrives.  Some terminals also send
            // Home in the tilde form (ESC [ 1 ~, or modified ESC [ 1 ; m ~); those
            // are decoded here too so Home reaches the keymap regardless of which
            // encoding the terminal chose.
            if (bytes.size() < 4) return {DecodeStatus::incomplete, {}, {}, 0};
            if (bytes[3] == '~') {
                consumed = 4;
                return {DecodeStatus::key, ssg::KeyStroke{ssg::KeyCode::Home}, {}, 0};
            }
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
            case '~': code = ssg::KeyCode::Home; break;  // ESC [ 1 ; m ~
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
        case '4':
        case '5':
        case '6':
        case '7':
        case '8': {
            // Tilde-form navigation keys: plain ESC [ N ~, or the modified form
            // ESC [ N ; m ~ (m = 1 + bitmask: bit0 Shift, bit1 Alt, bit2 Ctrl),
            // mirroring the '1'-prefixed arrow/Home/End modifier handling above.
            // N: 3 Delete, 5 PageUp, 6 PageDown, plus the tilde encodings of
            // Home/End that some terminals send instead of the letter forms
            // (1/7 Home, 4/8 End -- 1 is handled in `case '1'`). Without this,
            // ESC [ 4 ~ (End) fell through to `default`, which consumes only the
            // "ESC [ 4" introducer and leaves the '~' to be decoded next as a
            // literal printable, and the modified forms never reached the keymap.
            ssg::KeyCode const code = third == '3'   ? ssg::KeyCode::Delete
                                     : third == '5' ? ssg::KeyCode::PageUp
                                     : third == '6' ? ssg::KeyCode::PageDown
                                     : third == '7' ? ssg::KeyCode::Home
                                                    : ssg::KeyCode::End;  // 4 or 8
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
            const auto wheelCode = cb & ~8;
            if (wheelCode == 64 || wheelCode == 65) {
                Decoded decoded;
                decoded.status = DecodeStatus::scroll;
                decoded.scroll = wheelCode == 64 ? -3 : 3;
                // Carry the pointer position so the app can route the wheel to
                // the region under the cursor (the panel scrolls, not just the
                // editor).
                decoded.pointer.column = static_cast<int>(cx > 0 ? cx - 1 : 0);
                decoded.pointer.row = static_cast<int>(cy > 0 ? cy - 1 : 0);
                decoded.pointer.alt = (cb & 8) != 0;
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
            event.alt = (cb & 8) != 0;
            Decoded decoded;
            decoded.status = DecodeStatus::pointer;
            decoded.pointer = event;
            return decoded;
        }
        case 'M': {
            // SS3 numpad Enter (ESC O M, application-keypad mode) shares the 'M'
            // final with the X10 mouse report (ESC [ M b x y); the SS3 introducer
            // disambiguates them.
            if (second == 'O') {
                consumed = 3;
                return {DecodeStatus::key,
                        ssg::KeyStroke{ssg::KeyCode::Enter}, {}, 0};
            }
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
        // C0 control byte -> Mod+<letter>.  The bytes that name a key
        // (0x08 Backspace, 0x09 Tab, 0x0a/0x0d Enter, 0x1b Escape) are handled
        // above and never reach here, so what remains maps cleanly onto A..Z.
        // Those four are why Mod+I, Mod+M, Mod+H and Mod+[ are unreachable from
        // the Ctrl key under the legacy encoding: the terminal sends the named
        // key's byte and no Ctrl-ness survives.  They remain reachable from Alt.
        consumed = 1;
        ssg::KeyStroke stroke;
        stroke.mod = true;
        stroke.code = static_cast<ssg::KeyCode>(
            static_cast<std::uint16_t>(ssg::KeyCode::KeyA) + (first - 1));
        return {DecodeStatus::key, stroke, {}, 0};
    }
    consumed = 1;  // Other control byte: ignore.
    return {DecodeStatus::none, {}, {}, 0};
}

// Every Enter/Return encoding -- legacy CR/LF, Kitty `CSI 13;mods u`, Kitty
// keypad `CSI 57414 u`, and SS3 `ESC O M` -- inserts a newline regardless of
// which modifiers are held.  Rather than teach each binding context about
// modified Enter, normalize at this single seam: a decoded Enter is always a
// bare stroke, so it matches the plain `Enter` binding in every context.  The
// keymap validator refuses modified-Enter bindings, so nothing downstream can
// depend on an Enter stroke carrying a modifier.
Decoded decodeInput(std::string_view bytes, bool inputExhausted,
                     std::size_t& consumed) {
    Decoded decoded = decodeInputRaw(bytes, inputExhausted, consumed);
    if (decoded.status == DecodeStatus::key &&
        decoded.stroke.code == ssg::KeyCode::Enter) {
        decoded.stroke.shift = false;
        decoded.stroke.mod = false;
        decoded.stroke.meta = false;
    }
    return decoded;
}

} // namespace ssg
