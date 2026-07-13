#pragma once

// Pure, testable helpers for the `ssg` terminal application.  Terminal I/O
// (raw mode, reads, writes, signals) lives in ssg_main.cpp; everything that can
// be computed without touching the terminal lives here so it can be unit
// tested.  All editor and layout behavior belongs to the ssg library; this
// module only resolves launch arguments and formats an already-rendered cell
// grid into terminal bytes.

#include "tui_fixture.h"

#include <filesystem>
#include <optional>
#include <string>

namespace ssg::app {

// The workspace directory to open and, optionally, a file within it to open in
// a tab.  A file argument opens its parent directory; a directory argument
// opens that directory; no argument opens the current working directory.
struct LaunchTarget {
    std::filesystem::path cwd;
    std::optional<std::string> file;
};

[[nodiscard]] LaunchTarget resolve_launch(std::filesystem::path const& argument);

// Encode a rendered cell grid as a full-screen ANSI frame: cursor-addressed
// rows with 24-bit foreground/background colors drawn from the snapshot's
// 16-color palette.  Continuation cells (the trailing half of a wide glyph)
// emit nothing because the wide glyph already advanced the cursor.
[[nodiscard]] std::string encode_ansi_frame(ssg::tui::ScreenSnapshot const& screen);

}  // namespace ssg::app
