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
// The palette's display text for a command: its authored label, or the
// humanised id when it has none.
//
// Takes the registered command rather than an id.  An id-only overload existed
// and had to look the id up in the one catalog it knew about, so a command
// registered anywhere else silently fell back to humanising -- losing the
// author's label with no error.  Passing the command removes the lookup and the
// failure mode with it.
struct CommandEntry;
[[nodiscard]] std::string commandLabel(CommandEntry const& command);

}  // namespace ssg
