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
#include <chrono>
#include <optional>
#include <string>
#include <span>
#include <string_view>
#include <vector>

namespace ssg::app {

// One command the loop should dispatch, with its typed argument (empty for a
// command that takes no payload).
struct PointerCommand {
    std::string command_id;
    std::any payload;
};

// Which scroll a gesture drives, by the surface it targets. Declared here
// because both the wheel and the gutter routing name surfaces with it.
enum class WheelTarget : std::uint8_t {
    none,     // no scroll (nothing under the pointer wants the wheel)
    editor,   // dispatch view.scroll_lines (the document)
    tree,     // dispatch tree.scroll (the side panel)
    palette,  // scroll the client-owned palette window (no command)
};

// A scrollable surface, for the routing that must treat all of them alike.
//
// A table rather than a `switch`, because a switch over `HitRegion` does NOT
// fail to compile when a region is added -- verified by perturbation, this
// build does not enable -Wswitch. Two of the three gutters were already
// forgotten once; the exhaustiveness test over this table is what stops it
// happening again.
struct ScrollableRegionDescriptor {
    ssg::HitRegion content;
    ssg::HitRegion scrollbar;
    WheelTarget target;
    // The command a gutter gesture dispatches, or empty when the surface's
    // offset is client-owned and must not round-trip (the picker: its ranked
    // list is produced client-side for latency, so scrolling it stays local).
    std::string_view scrollCommand;
};

// Every scrollable surface. Routing drives from this, so listing a surface here
// is what wires it, and the exhaustiveness test fails if one is not routed.
[[nodiscard]] std::span<const ScrollableRegionDescriptor>
scrollable_regions() noexcept;

// Whether `region` is a scrollbar gutter, from that same list -- so a caller
// deciding "did this press start a thumb drag" cannot fall out of step with the
// surfaces routing knows about.
[[nodiscard]] bool is_scrollbar_region(ssg::HitRegion region) noexcept;

// Grab-offset scrollbar dragging.  `rel` is the pointer row within the gutter,
// `thumbStart`/`thumbSize` the thumb geometry, `travel = viewportRows -
// thumbSize` the range of the thumb's top row.  A press ON the thumb grabs it in
// place; a press in the well centres the thumb on the cursor.  The fraction the
// gesture sends is the grabbed thumb-top position over the travel, so the point
// grabbed at press tracks the cursor for the whole drag.
struct GutterFraction {
    std::uint32_t numerator;
    std::uint32_t denominator;

    bool operator==(const GutterFraction&) const = default;
};
[[nodiscard]] int scrollbar_grab_offset(int rel, int thumbStart,
                                        int thumbSize) noexcept;
[[nodiscard]] GutterFraction gutter_fraction(int rel, int grabOffset,
                                             int travel) noexcept;

// Double-click detection.  The terminal mouse protocol carries no click count,
// so the client pairs presses by time, target surface, and grid cell.
// `register_click_is_double` returns true iff this press falls within `window` of
// the previous tracked press AND on the same `surface` and cell; it ALWAYS
// updates the tracker, and RESETS it after reporting a double-click so a third
// press starts fresh (no triple-click).  `surface` identifies the document/view
// (e.g. the active tab id) so a fast click on the same cell of a DIFFERENT
// document is not mistaken for a double-click.  It is pure: `now` is passed in,
// no clock is read here.
struct ClickTracker {
    std::optional<std::chrono::steady_clock::time_point> time;
    std::uint64_t surface = 0;
    int row = 0;
    int column = 0;
};
[[nodiscard]] bool register_click_is_double(
    ClickTracker& tracker, std::chrono::steady_clock::time_point now,
    std::uint64_t surface, int row, int column,
    std::chrono::milliseconds window) noexcept;

// A gutter gesture the CALLER must apply to a client-owned offset, because no
// server command may be dispatched for it (see ScrollableRegionDescriptor).
struct ClientScroll {
    WheelTarget target = WheelTarget::none;
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 0;
};

// The result of routing one pointer event: an ordered command sequence (0, 1,
// or — for a tree-row click — 2 commands, dispatched in order) plus how the
// client-local drag state changes.
struct PointerDispatch {
    std::vector<PointerCommand> commands;
    bool begins_drag = false;  // a press that starts an editor selection drag
    bool ends_drag = false;    // a release that ends a drag
    // Set when the gesture targets a surface whose offset the client owns, so
    // there is no command to dispatch. The loop applies it locally.
    std::optional<ClientScroll> client_scroll;
};

// The snapshot-derived data a pointer event needs, resolved by the caller (only
// the field matching the hit region is populated).
struct PointerTargets {
    std::optional<ssg::DocumentPosition> document_position;  // an editor hit
    std::optional<ssg::TabId> tab_id;             // a tab hit (tabs[index] id)
    // A picker-row candidate id, plus which picker published it.  The id alone
    // is not actionable: for the command palette it is a command id, for the
    // file picker a workspace-relative path, and submitting one as the other
    // either fails the server guard or is nonsense.
    std::optional<std::string> picker_candidate_id;
    ssg::SearchMode picker_mode = ssg::SearchMode::Command;
    std::optional<std::string> field_command_id;    // a header/footer field command
};

// Route one pointer event.  `dragging`/`drag_anchor` are the loop's current
// drag state.  `alt` is the EFFECTIVE Alt modifier for this event: the app
// supplies the press event's own Alt to establish an Alt-drag gesture, then the
// established drag state alone for subsequent drag/release (the per-motion bit
// is ignored once a drag is under way). During an Alt-drag the app also supplies
// `alt_drag_baseline`, the selection set captured at press, so the router can
// rebuild the whole set from an immutable baseline each motion. Pure: depends
// only on its arguments.
[[nodiscard]] PointerDispatch route_pointer(
    ssg::RegionHit const& hit, PointerButton button, PointerKind kind, bool alt,
    bool dragging, std::optional<ssg::DocumentPosition> drag_anchor,
    PointerTargets const& targets,
    std::vector<ssg::Selection> const& alt_drag_baseline = {});

// The dispatch for a recognized double-click on the editor: select the word at
// `position` and arm no drag.  A named helper (rather than an inline dispatch in
// the app loop) so the "double-click selects the word, no drag" decision is a
// pure, unit-tested unit like route_pointer.
[[nodiscard]] PointerDispatch double_click_dispatch(
    ssg::DocumentPosition position);

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
