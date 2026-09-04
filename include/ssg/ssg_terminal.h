#pragma once

// Helpers for the `ssg` terminal application. TerminalSession owns raw mode and
// terminal modes; the application module owns reads and signals. All editor and
// layout behavior belongs to the ssg library.

#include <ssg/color.h>
#include <ssg/Renderer.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <termios.h>
#include <string>
#include <string_view>
#include <vector>

namespace ssg::app {

// Process lifecycle remains a terminal-application concern and is consumed
// before editor input routing, so committed text on the same stroke cannot hide it.
[[nodiscard]] inline bool application_quit_requested(
    KeyStroke const& stroke) noexcept {
    return stroke.code == KeyCode::KeyQ && stroke.alt && !stroke.control;
}

// M9-W signal-event wakeup.  Terminating and resize signals cannot do work in
// async-signal context, so their handlers only write one tag byte per signal to
// a self-pipe the event loop selects on.  The tag byte IS the signal number
// (SIGWINCH/SIGTERM/SIGHUP all fit in a byte), so no separate tag table is
// needed and `terminate` can carry the exact signal for a correct re-raise.
struct SignalEvents {
    bool resize = false;             // At least one SIGWINCH was drained.
    std::optional<int> terminate;    // The last terminating signal drained, if any.
};

// Interpret each byte of the drained self-pipe as a signal number: SIGWINCH sets
// `resize`; SIGTERM/SIGHUP set `terminate` (last wins).  Pure and total: unknown
// bytes are ignored, and any number of duplicate tags coalesce.
[[nodiscard]] SignalEvents classify_signal_tags(std::string_view drained);

// A terminal MODE: a state SSG enters that persists until it is explicitly
// left.  Distinct from PAINTING (cursor positioning, SGR colour), which takes
// effect where it lands, owes nothing, and is re-issued every frame -- painting
// is deliberately not modelled here, so the hot path carries no bookkeeping.
//
// Enter and leave bytes are declared TOGETHER, so a mode cannot be added in one
// place and forgotten in another.  That was the failure mode: two hand-written
// strings that had to mirror each other by inspection.
struct TerminalMode {
    std::string_view enter;
    std::string_view leave;
};

// The modes SSG uses.  Adding one here is the only way to add one at all.
inline constexpr TerminalMode kAlternateScreen{"\x1b[?1049h", "\x1b[?1049l"};
inline constexpr TerminalMode kCursorStyleBar{"\x1b[5 q", "\x1b[0 q"};
inline constexpr TerminalMode kMouseButtons{"\x1b[?1000h", "\x1b[?1000l"};
inline constexpr TerminalMode kMouseMotion{"\x1b[?1002h", "\x1b[?1002l"};
inline constexpr TerminalMode kMouseSgrCoordinates{"\x1b[?1006h", "\x1b[?1006l"};
inline constexpr TerminalMode kCursorHidden{"\x1b[?25l", "\x1b[?25h"};
// Bracketed paste: the terminal wraps pasted text in ESC[200~ ... ESC[201~ so it
// can be told apart from typing.  Without it a paste is indistinguishable from
// someone typing very fast, and any newline in it fires whatever Enter is bound
// to -- submitting a prompt, or splitting lines the paste did not ask to split.
inline constexpr TerminalMode kBracketedPaste{"\x1b[?2004h", "\x1b[?2004l"};

// The Kitty keyboard protocol (progressive-enhancement flag 1, "disambiguate
// escape codes"): enable pushes flag 1, leave pops it.  Deliberately NOT a member
// of kAllModes below: that constant undo superset is written unconditionally on
// teardown and is safe only because leaving an unentered mode is harmless -- but
// `CSI < u` is a STACK POP, not an idempotent reset, so writing it when SSG never
// pushed would pop an outer program's keyboard state.  It is therefore entered
// through a Guard whose destructor pops it exactly once, on the same normal-
// context teardown paths as every other mode (return, exception, and the
// SIGTERM/SIGHUP restore), and is enabled only after the terminal answers the
// keyboard-protocol capability query.
inline constexpr TerminalMode kKeyboardProtocol{"\x1b[>1u", "\x1b[<u"};

// Every mode above, in the order they are entered.  The one iterable list: the
// crash undo and its test are both DERIVED from it, so a new mode cannot be
// covered by one and missed by the other.
inline constexpr TerminalMode kAllModes[]{
    kAlternateScreen, kCursorStyleBar,      kMouseButtons,
    kMouseMotion,     kMouseSgrCoordinates, kCursorHidden,
    kBracketedPaste,
};

// An ordered stack of entered modes, and the bytes that undo them.
//
// Entering returns a guard; the mode is left when the guard is destroyed.  A
// mode entered without a guard is not expressible, which is the point: the
// enter and the leave are one statement, so no branch can pay half the debt.
// Modes leave in reverse order of entry -- leaving the alternate screen before
// disabling mouse reporting would leave reporting on in the primary screen.
//
// CRASH SAFETY.  Destructors handle paths that unwind; a fatal signal is
// precisely the path that does not.  So the bytes undoing everything currently
// entered are maintained as data and published atomically, and a handler needs
// only write(2) -- no allocation, no traversal.  See undoBytes().
class TerminalModes {
public:
    // Writes bytes to the terminal.  Injected so the whole class is testable
    // with no terminal, and so a frame-scoped guard can append to the frame
    // buffer being built rather than issuing its own syscalls.
    using Writer = std::function<void(std::string_view)>;

    explicit TerminalModes(Writer writer);
    ~TerminalModes();

    TerminalModes(TerminalModes const&) = delete;
    TerminalModes& operator=(TerminalModes const&) = delete;

    // Leaves `mode` when destroyed.  Movable so it can be stored or returned;
    // not copyable, or two guards would pay one debt.
    class Guard {
    public:
        Guard() noexcept = default;
        ~Guard();
        Guard(Guard&& other) noexcept;
        Guard& operator=(Guard&& other) noexcept;
        Guard(Guard const&) = delete;
        Guard& operator=(Guard const&) = delete;

    private:
        friend class TerminalModes;
        Guard(TerminalModes& owner, std::size_t depth) noexcept
            : owner_{&owner}, depth_{depth} {}
        TerminalModes* owner_ = nullptr;
        std::size_t depth_ = 0;
    };

    [[nodiscard]] Guard enter(TerminalMode mode);

    // The number of modes currently entered.
    [[nodiscard]] std::size_t depth() const noexcept;

private:
    void leaveThrough(std::size_t depth) noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Owns raw terminal mode and entered terminal modes for one editor session.
class TerminalSession {
public:
    TerminalSession();
    ~TerminalSession();

    TerminalSession(TerminalSession const&) = delete;
    TerminalSession& operator=(TerminalSession const&) = delete;

    void restore() noexcept;
    [[nodiscard]] bool active() const noexcept;
    void enableKeyboardProtocol();

private:
    TerminalModes modes_;
    std::vector<TerminalModes::Guard> entered_;
    termios original_{};
    bool active_ = false;
    bool keyboardProtocolEntered_ = false;
};

void writeAll(std::string_view bytes);
[[nodiscard]] ssg::ViewportDimensions terminalSize();

namespace detail {

inline constexpr std::size_t kUndoLength = [] {
    std::size_t total = 0;
    for (auto const& mode : kAllModes) total += mode.leave.size();
    return total;
}();

// Built at COMPILE TIME.  A lazily initialised static string would allocate and
// take a thread-safe-static guard on first call, neither of which a fatal-signal
// handler may do -- and the first call might BE the handler.
inline constexpr std::array<char, kUndoLength> kUndoStorage = [] {
    std::array<char, kUndoLength> bytes{};
    std::size_t at = 0;
    for (std::size_t i = std::size(kAllModes); i-- > 0;) {
        for (char byte : kAllModes[i].leave) bytes[at++] = byte;
    }
    return bytes;
}();

}  // namespace detail

// The bytes that leave EVERY declared mode, in reverse declaration order, for a
// fatal-signal handler to write.
//
// A constant rather than a running record of what is entered, and the reason is
// worth stating because the obvious design is the other one.  Tracking the live
// set precisely means publishing it for a reader that may be on another thread
// -- a signal is not always delivered to the thread that is mid-update -- and
// no lock-free publication of a variable-length buffer survives a writer that
// laps a reader.  A double-buffered version of this failed exactly that test.
//
// It is safe to be imprecise in this direction and only this direction:
// leaving a mode that is not set does nothing (leaving an alternate screen you
// are not on, disabling mouse reporting that is off), while failing to leave
// one that IS set is the bug.  So the constant is a superset by construction,
// needs no synchronisation, allocates nothing, and is trivially
// async-signal-safe to write.
//
// Order still matters and is encoded here: mouse reporting is disabled before
// the alternate screen is left, or reporting stays on in the primary screen.
[[nodiscard]] constexpr std::string_view all_modes_undo_sequence() noexcept {
    return {detail::kUndoStorage.data(), detail::kUndoStorage.size()};
}

// The workspace directory to open and, optionally, a file within it to open in
// a tab.  A file argument opens its parent directory; a directory argument
// opens that directory; no argument opens the current working directory.
struct LaunchTarget {
    std::filesystem::path cwd;
    std::optional<std::string> file;
};

[[nodiscard]] LaunchTarget resolve_launch(std::filesystem::path const& argument);

// K3a decoder: the outcome of decoding one input event as a
// KeyStroke (so it can drive keymap resolution) and/or committed text.
enum class DecodeStatus : std::uint8_t {
    none,        // A recognized but unhandled byte was skipped (consumed > 0).
    incomplete,  // Only a partial escape sequence is buffered; read more bytes.
    key,         // A KeyStroke (with optional committed text for printables).
    scroll,      // A mouse-wheel event (signed line count in `scroll`).
    pointer,     // A mouse button press/release/drag (see `pointer`).
    paste,       // A bracketed paste's payload (`text`); never a keypress.
    reply,       // A terminal report answering a capability query (`reply`).
};

// No real escape sequence approaches this length.  A scan that reaches it is
// looking for a terminator that is not coming, and holding the buffer any longer
// would stall every keystroke queued behind it
inline constexpr std::size_t kMaxSequenceBytes = 256;

// A bracketed paste's payload is unbounded in principle, so it gets its own much
// larger cap.  Past this the closing marker is not coming and the run is
// discarded rather than holding the input buffer for the rest of the session.
inline constexpr std::size_t kMaxPasteBytes = 4u * 1024u * 1024u;

// A decoded mouse button event (M8): which button, whether it is a press,
// release, or drag (motion with a button held), and the 0-based grid cell it
// occurred over.  Wheel events remain `DecodeStatus::scroll`, not pointer events.
enum class PointerButton : std::uint8_t { left, middle, right, other };
enum class PointerKind : std::uint8_t { press, release, drag };

struct PointerEvent {
    int column = 0;  // 0-based grid column (SGR reports 1-based).
    int row = 0;     // 0-based grid row.
    PointerButton button = PointerButton::left;
    PointerKind kind = PointerKind::press;
    bool alt = false;

    friend bool operator==(const PointerEvent&, const PointerEvent&) = default;
};

struct Decoded {
    DecodeStatus status = DecodeStatus::none;
    ssg::KeyStroke stroke;    // status == key.
    std::string text;         // status == key: the committed UTF-8 if the stroke
                              // also commits text (a printable); empty otherwise.
    std::int64_t scroll = 0;  // status == scroll: signed line count.
    PointerEvent pointer;     // status == pointer; also carries the wheel's
                              // 0-based grid position when status == scroll.
    std::string reply;        // status == reply: the report's bytes, verbatim.
                              // The decoder classifies; it does not interpret.
};

// Decode the first event from `bytes` into a KeyStroke / committed text / scroll.
// `input_exhausted` resolves the terminal Escape ambiguity: a lone ESC byte is
// both a complete Escape stroke and the start of a CSI/SS3 sequence.  When the
// byte after ESC is buffered it disambiguates ('['/'O' -> CSI/SS3, else ESC is a
// standalone Escape stroke and the next byte is decoded separately).  When no
// byte follows ESC, `input_exhausted == true` (a bounded read returned nothing)
// emits the Escape stroke; otherwise the result is `incomplete` (await bytes).
[[nodiscard]] Decoded decode_input(std::string_view bytes, bool input_exhausted,
                                   std::size_t& consumed);

// Encode a rendered cell grid as a full-screen ANSI frame: cursor-addressed
// rows whose colors are drawn from the snapshot's 16-color palette and adapted
// to `depth` via ssg::resolve_color (truecolor 38;2, indexed256 38;5, or ANSI16
// 30-37/90-97).  Continuation cells (the trailing half of a wide glyph) emit
// nothing because the wide glyph already advanced the cursor.
// One complete frame's bytes: the grid's cells, the caret placed if there is
// one, and the cursor hidden for the duration of the redraw.
//
// Assembled here rather than in the loop so the balance is unit-testable: the
// cursor hide is a frame-scoped mode entered through a guard, so every frame
// leaves the cursor visible whether or not it had a caret.  A caret-less frame
// -- the "too small" placeholder -- previously hid the cursor and never showed
// it.
//
// `showCursor` false ends the frame with the cursor hidden, for a scrollbar drag
// where a visible cursor only flickers around chasing each frame's caret.  That
// is the one deliberately unbalanced case; see the note at the implementation.
// The bytes that put `text` on the user's system clipboard via OSC 52.
//
// Matters most over SSH: the editor runs on the remote host but the clipboard a
// user pastes into is local, and this is the only mechanism that crosses that
// gap.  Write only -- reading the clipboard is refused outright by Windows
// Terminal on security grounds, so nothing may be designed around it
// (doc/terminal-rendering-capabilities.html).
[[nodiscard]] std::string encode_clipboard_write(std::string_view text);

// Turns the clipboard write a snapshot publishes into the bytes to send, or
// nothing.
//
// Owns the two rules that make the fire-and-forget write correct, so neither
// can be forgotten at a call site: a given write id is served EXACTLY ONCE (a
// snapshot keeps republishing the last copy, because nothing reports back and
// the register cannot know it has been served), and nothing is sent at all
// unless the terminal advertised OSC 52 -- where it would be an unrecognised
// escape sequence rather than a copy.
class SystemClipboardWriter {
public:
    [[nodiscard]] std::optional<std::string> bytesFor(
        std::optional<ssg::ClipboardWrite> const& write, bool terminalCanWrite);

private:
    std::optional<std::uint64_t> served_;
};

[[nodiscard]] std::string encode_frame(ssg::CellGrid const& screen,
                                       ssg::ColorDepth depth,
                                       bool showCursor = true);

[[nodiscard]] std::string encode_ansi_frame(
    ssg::CellGrid const& screen, ssg::ColorDepth depth = ssg::ColorDepth::Truecolor);

// Detect the terminal's color capability from environment-derived hints.
// `color_depth_override` models SSG_COLOR_DEPTH and can force truecolor,
// indexed256, or ansi16. Nullable inputs (a missing variable) are treated as
// absent. Pure, so it is unit-testable without touching the real environment:
// it reads no environment itself, which is what keeps TerminalCapabilities the
// single place that does (INV-capability-single-source).
[[nodiscard]] ssg::ColorDepth detect_color_depth(char const* color_depth_override,
                                                 char const* colorterm,
                                                 char const* term,
                                                 char const* term_program);

// What the terminal can do beyond drawing colored text.  Each is answered by a
// query at startup; each defaults to absent, so a terminal that stays silent
// simply gets the conservative rendering.
enum class Capability : std::uint8_t {
    SynchronizedOutput,  // DEC private mode 2026: tear-free full-frame redraw.
    KeyboardProtocol,    // The kitty keyboard protocol: disambiguated keys.
    ClipboardWrite,      // OSC 52: copy to the user's system clipboard.
};

// Every capability, so a diagnostic or a test can enumerate them without a
// second list drifting out of step with the enum.
inline constexpr std::array<Capability, 3> kAllCapabilities{
    Capability::SynchronizedOutput,
    Capability::KeyboardProtocol,
    Capability::ClipboardWrite,
};

// The stable name used in diagnostics and to build the override variable
// (`synchronized_output` -> SSG_TERM_SYNCHRONIZED_OUTPUT).
[[nodiscard]] std::string_view capability_name(Capability capability);

// The resolved color depth as a user-facing word, for the same diagnostic.
[[nodiscard]] std::string_view color_depth_name(ssg::ColorDepth depth);

// What the attached terminal can do, resolved in one place.
//
// Answers are discovered by writing queries at startup and reading the replies
// the decoder hands back as `DecodeStatus::reply`.  Nothing waits for them: the
// first frame renders against the defaults, and a capability flips from absent
// to present when its answer lands (INV-startup-unblocked).  "Unknown" and
// "absent" deliberately resolve identically, so a late answer can only ever turn
// something on.
//
// Interpretation is scoped to the probe window.  Replies are always *consumed*
// by the decoder, but they are only *believed* between `beginProbe()` and the
// DA1 answer that fences it, so a byte sequence that merely looks like a report
// -- pasted, or from a protocol adopted later -- cannot silently reconfigure the
// editor mid-session.
class TerminalCapabilities {
public:
    // Reads a variable, or returns nullptr when it is unset.  Taking this rather
    // than calling getenv keeps the class pure and unit-testable against a fake
    // environment, and keeps every environment read in one object.
    using EnvironmentLookup = std::function<char const*(std::string_view)>;

    // Injected so the probe window can be tested without waiting in real time.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    // How long a silent terminal keeps the window open.  Every answer that is
    // coming arrives within one round trip; past this the terminal is not
    // answering at all.
    static constexpr std::chrono::milliseconds kProbeWindow{250};

    explicit TerminalCapabilities(EnvironmentLookup lookup, Clock clock = {});

    // The bytes to write to the terminal, and the opening of the window in which
    // their answers count.  The two are one call because neither is correct
    // alone: a window that no query will close never closes, and queries whose
    // answers land outside a window are silently discarded.  Speculative queries
    // come first and DA1 last, so the DA1 answer fences them -- anything that was
    // going to reply has replied by the time it arrives.
    [[nodiscard]] std::string beginProbe();

    // Offer a reply the decoder classified.  Ignored unless the window is open.
    // The window's expiry is enforced HERE, at the moment a reply is considered,
    // rather than by whoever owns the event loop.  A caller polling the clock
    // separately would always be a step behind: the loop wakes *because* bytes
    // arrived, so its check runs before the very reply it should have excluded.
    void observeReply(std::string_view reply);

    // Close the window without a DA1 answer: the terminal is silent or is not a
    // terminal.  Every unanswered capability keeps its conservative default.
    void endProbe();

    [[nodiscard]] bool probing() const;

    [[nodiscard]] bool has(Capability capability) const;
    [[nodiscard]] ssg::ColorDepth colorDepth() const;

    // What the terminal actually said, for the `--capabilities` diagnostic.
    // Without this a reported "no" is unexplainable: it could mean the terminal
    // stayed silent, or answered in a shape the parser rejects, or answered
    // after the fence closed the window.  Those need different fixes, so the
    // report has to be able to tell them apart.  Replies are recorded whether or
    // not they are believed -- recording is not believing.
    struct ProbeLog {
        std::vector<std::string> believed;  // Arrived inside the window.
        std::vector<std::string> ignored;   // Arrived after it closed.
    };
    [[nodiscard]] ProbeLog const& probeLog() const;

private:
    enum class Answer : std::uint8_t { Unknown, Absent, Present };

    [[nodiscard]] std::optional<bool> override_for(Capability capability) const;
    [[nodiscard]] bool expired() const;

    EnvironmentLookup lookup_;
    Clock clock_;
    ProbeLog log_;
    std::array<Answer, kAllCapabilities.size()> answers_{};
    bool probing_ = false;
    std::chrono::steady_clock::time_point deadline_{};
    ssg::ColorDepth colorDepth_ = ssg::ColorDepth::Ansi16;
};

// The exact control bytes that put the terminal into / take it out of the
// editor's display mode.  Pure so the RAII guard, a signal-driven restore, and a
// test all share one definition (M9-X): setup enters the alternate screen with a
// blinking bar cursor and SGR mouse reporting; restore reverses each in the

}  // namespace ssg::app
