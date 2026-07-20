#pragma once

// Pure pointer-event routing (M8): given a classified hit (from the library
// `hit_test`), the button/kind, the current drag state, and the caller-resolved
// targets (document position / candidate / tab id), decide which command(s) the
// I/O loop should dispatch and how the drag state changes.  This is a pure
// function with no terminal or runtime dependency so it can be unit-tested
// directly; the loop does only decode -> refresh -> hit_test -> resolve -> route
// -> dispatch (see doc/spec-m8.md).

#include "ssg_terminal.h"

#include <ssg/HitTester.h>
#include <ssg/Selection.h>
#include <ssg/TabManager.h>
#include <ssg/ShellState.h>

#include <any>
#include <optional>
#include <string>
#include <vector>

namespace ssg::app {

// One command the loop should dispatch, with its typed argument (empty for a
// command that takes no payload).
struct PointerCommand {
    std::string command_id;
    std::any payload;
};

// The result of routing one pointer event: an ordered command sequence (0, 1,
// or — for a tree-row click — 2 commands, dispatched in order) plus how the
// client-local drag state changes.
struct PointerDispatch {
    std::vector<PointerCommand> commands;
    bool begins_drag = false;  // a press that starts an editor selection drag
    bool ends_drag = false;    // a release that ends a drag
};

// The snapshot-derived data a pointer event needs, resolved by the caller (only
// the field matching the hit region is populated).
struct PointerTargets {
    std::optional<ssg::DocumentPosition> document_position;  // an editor hit
    std::optional<ssg::TabId> tab_id;             // a tab hit (tabs[index] id)
    std::optional<std::string> palette_command_id;  // a palette-row candidate id
};

// Route one pointer event.  `dragging`/`drag_anchor` are the loop's current
// drag state.  Pure: depends only on its arguments.
[[nodiscard]] PointerDispatch route_pointer(
    ssg::RegionHit const& hit, PointerButton button, PointerKind kind,
    bool dragging, std::optional<ssg::DocumentPosition> drag_anchor,
    PointerTargets const& targets);

// Which scroll a mouse-wheel event drives, by the region under the pointer.
enum class WheelTarget : std::uint8_t {
    none,     // no scroll (nothing under the pointer wants the wheel)
    editor,   // dispatch view.scroll_lines (the document)
    tree,     // dispatch tree.scroll (the side panel)
    palette,  // scroll the client-owned palette window (no command)
};

// Route a mouse-wheel event to the scroll it drives for the region under the
// pointer: the side panel (or its gutter) scrolls the tree, the palette (or its
// gutter) scrolls the client-owned palette window, and the editor and everywhere
// else scroll the document. Pure; the caller applies the wheel's line delta.
[[nodiscard]] WheelTarget route_wheel(ssg::HitRegion region);

// Decide whether an active drag whose pointer is held at `pointer_row` should
// auto-scroll the editor (M8-S2): -1 when the pointer is above the content's top
// row, +1 when at or below its bottom, nullopt when inside the content rows or
// not dragging. Vertical only (the column does not affect the direction). Pure;
// the loop wakes on a timer and re-extends the selection to the new edge cell.
[[nodiscard]] std::optional<int> edge_scroll(bool dragging, int pointer_row,
                                             ssg::Rect const& content);

}  // namespace ssg::app
