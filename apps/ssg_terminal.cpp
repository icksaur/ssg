#include "ssg_terminal.h"

#include <ssg/Keymap.h>
#include <ssg/Theme.h>

#include <csignal>

#include <atomic>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

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
// break the Escape-then-letter chords the keymap relies on.
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
            // ESC followed by a non-CSI byte: ESC is a standalone Escape stroke;
            // the next byte is decoded on the following call.
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
            if (bytes[3] != ';') return unhandled();
            std::size_t pos = 4;
            auto const modifier = parseDecimal(bytes, pos);
            if (pos >= bytes.size()) {
                return {DecodeStatus::incomplete, {}, {}, 0};  // Await digits/final.
            }
            if (pos == 4 || bytes[pos] != '~') return unhandled();
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
        // stroke so it can participate in a chord (e.g. Escape then KeyS).
        return {DecodeStatus::key, stroke, std::move(text), 0};
    }
    consumed = 1;  // Other control byte: ignore.
    return {DecodeStatus::none, {}, {}, 0};
}

}  // namespace ssg::app
