#pragma once

// Human-facing command metadata (doc/spec-keymap.md K7).  The library owns the
// mapping from a command id to a display label so the palette can present
// "Save File" instead of "file.save".  This is the future home of richer
// metadata (category, description); today it carries labels only.

#include <string>
#include <string_view>

namespace ssg {

// The human-readable label for a command id.  Curated ids get an authored label;
// any other id is humanized from its segments (e.g. "cursor.line_down" ->
// "Cursor Line Down") so a palette candidate never shows a raw dotted id.
[[nodiscard]] std::string commandLabel(std::string_view commandId);

}  // namespace ssg
