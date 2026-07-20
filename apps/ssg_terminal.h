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
[[nodiscard]] std::string encode_ansi_frame(
    ssg::CellGrid const& screen, ssg::ColorDepth depth = ssg::ColorDepth::Truecolor);

// Detect the terminal's color capability from the environment (M9-C2): COLORTERM
// of "truecolor"/"24bit" -> truecolor; else a TERM containing "256color" ->
// indexed256; else ansi16.  Nullable inputs (a missing variable) are treated as
// absent.  Pure, so it is unit-testable without touching the real environment.
[[nodiscard]] ssg::ColorDepth detect_color_depth(char const* colorterm,
                                                 char const* term);

// The exact control bytes that put the terminal into / take it out of the
// editor's display mode.  Pure so the RAII guard, a signal-driven restore, and a
// test all share one definition (M9-X): setup enters the alternate screen with a
// blinking bar cursor and SGR mouse reporting; restore reverses each in the
// opposite order and shows the cursor.
[[nodiscard]] std::string terminal_setup_sequence();
[[nodiscard]] std::string terminal_restore_sequence();

}  // namespace ssg::app
