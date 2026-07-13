#pragma once

// Pure, testable helpers for the `ssg` terminal application.  Terminal I/O
// (raw mode, reads, writes, signals) lives in ssg_main.cpp; everything that can
// be computed without touching the terminal lives here so it can be unit
// tested.  All editor and layout behavior belongs to the ssg library; this
// module only resolves launch arguments and formats an already-rendered cell
// grid into terminal bytes.

#include <ssg/render.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ssg::app {

// The workspace directory to open and, optionally, a file within it to open in
// a tab.  A file argument opens its parent directory; a directory argument
// opens that directory; no argument opens the current working directory.
struct LaunchTarget {
    std::filesystem::path cwd;
    std::optional<std::string> file;
};

[[nodiscard]] LaunchTarget resolve_launch(std::filesystem::path const& argument);

// A decoded terminal input event, reduced to the actions milestone 3 handles.
enum class InputAction { none, chord, scroll_lines, scroll_pages };

struct InputEvent {
    InputAction action = InputAction::none;
    std::int64_t amount = 0;  // Signed: negative scrolls up, positive down.
    char key = 0;             // For chord: the key pressed after the ESC leader.
};

// Decode the first complete event from `bytes`.
//   consumed > 0                    -> that many bytes form one event (which may
//                                      be InputAction::none for a recognized but
//                                      unhandled key/sequence to skip).
//   consumed == 0, action == none   -> `bytes` holds only an incomplete escape
//                                      sequence; read more before parsing again.
[[nodiscard]] InputEvent parse_input(std::string_view bytes,
                                     std::size_t& consumed);

// Encode a rendered cell grid as a full-screen ANSI frame: cursor-addressed
// rows with 24-bit foreground/background colors drawn from the snapshot's
// 16-color palette.  Continuation cells (the trailing half of a wide glyph)
// emit nothing because the wide glyph already advanced the cursor.
[[nodiscard]] std::string encode_ansi_frame(ssg::CellGrid const& screen);

}  // namespace ssg::app
