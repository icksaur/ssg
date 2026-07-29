#pragma once

// Pure, testable helpers for the `ssg` terminal application.  Terminal I/O
// (raw mode, reads, writes, signals) lives in ssg_main.cpp; everything that can
// be computed without touching the terminal lives here so it can be unit
// tested.  All editor and layout behavior belongs to the ssg library; this
// module only resolves launch arguments and formats an already-rendered cell
// grid into terminal bytes.

#include <ssg/color.h>
#include <ssg/Renderer.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ssg::app {

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
[[nodiscard]] std::string_view all_modes_undo_sequence() noexcept;

// The workspace directory to open and, optionally, a file within it to open in
// a tab.  A file argument opens its parent directory; a directory argument
// opens that directory; no argument opens the current working directory.
struct LaunchTarget {
    std::filesystem::path cwd;
    std::optional<std::string> file;
};

[[nodiscard]] LaunchTarget resolve_launch(std::filesystem::path const& argument);

// K3a decoder (doc/spec-keymap.md): the outcome of decoding one input event as a
// KeyStroke (so it can drive keymap resolution) and/or committed text.
enum class DecodeStatus : std::uint8_t {
    none,        // A recognized but unhandled byte was skipped (consumed > 0).
    incomplete,  // Only a partial escape sequence is buffered; read more bytes.
    key,         // A KeyStroke (with optional committed text for printables).
    scroll,      // A mouse-wheel event (signed line count in `scroll`).
    pointer,     // A mouse button press/release/drag (see `pointer`).
};

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
// it (doc/spec-terminal-escape-discipline.md).
[[nodiscard]] std::string encode_frame(ssg::CellGrid const& screen,
                                       ssg::ColorDepth depth);

[[nodiscard]] std::string encode_ansi_frame(
    ssg::CellGrid const& screen, ssg::ColorDepth depth = ssg::ColorDepth::Truecolor);

// Detect the terminal's color capability from environment-derived hints.
// `color_depth_override` models SSG_COLOR_DEPTH and can force truecolor,
// indexed256, or ansi16. Nullable inputs (a missing variable) are treated as
// absent. Pure, so it is unit-testable without touching the real environment.
[[nodiscard]] ssg::ColorDepth detect_color_depth(char const* color_depth_override,
                                                 char const* colorterm,
                                                 char const* term,
                                                 char const* term_program);

// The exact control bytes that put the terminal into / take it out of the
// editor's display mode.  Pure so the RAII guard, a signal-driven restore, and a
// test all share one definition (M9-X): setup enters the alternate screen with a
// blinking bar cursor and SGR mouse reporting; restore reverses each in the

}  // namespace ssg::app
