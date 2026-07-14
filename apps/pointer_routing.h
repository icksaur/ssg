#pragma once

// Pure pointer-event routing (M8): given a classified hit (from the library
// `hit_test`), the button/kind, the current drag state, and the caller-resolved
// targets (document position / candidate / tab id), decide which command(s) the
// I/O loop should dispatch and how the drag state changes.  This is a pure
// function with no terminal or runtime dependency so it can be unit-tested
// directly; the loop does only decode -> refresh -> hit_test -> resolve -> route
// -> dispatch (see doc/spec-m8.md).

#include "ssg_terminal.h"

#include <ssg/hit_test.h>
#include <ssg/selection.h>
#include <ssg/tabs.h>

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

}  // namespace ssg::app
