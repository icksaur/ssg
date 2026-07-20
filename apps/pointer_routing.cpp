#include "pointer_routing.h"

#include <ssg/input.h>
#include <ssg/palette.h>
#include <ssg/tree.h>

namespace ssg::app {

PointerDispatch route_pointer(ssg::RegionHit const& hit, PointerButton button,
                              PointerKind kind, bool dragging,
                              std::optional<ssg::DocumentPosition> dragAnchor,
                              PointerTargets const& targets) {
    PointerDispatch dispatch;
    // Only the left button drives editing actions in M8; other buttons are a
    // no-op (right-click menus etc. are out of scope).
    if (button != PointerButton::left) return dispatch;

    switch (kind) {
        case PointerKind::press:
            // A left press or drag on the editor gutter scrolls the document to
            // the fraction hit_test computed from the pointer row (M8-B). This is
            // independent of the selection drag state: the server-owned scroll
            // offset moves live as the thumb is dragged. Panel/palette gutters
            // are not draggable yet, so they fall through to no command.
            if (hit.region == ssg::HitRegion::EditorScrollbar) {
                dispatch.commands.push_back(
                    {"view.scroll_to_fraction",
                     ssg::ScrollFractionArguments{hit.scroll_numerator,
                                                  hit.scroll_denominator}});
                return dispatch;
            }
            // A left press on a tab activates it (the caller resolved tab_index
            // -> TabId); on a palette row it executes that candidate (the caller
            // mapped the absolute item_index -> candidate id). Neither begins a
            // selection drag.
            if (hit.region == ssg::HitRegion::Tab && targets.tab_id) {
                dispatch.commands.push_back({"tab.activate", *targets.tab_id});
                return dispatch;
            }
            if (hit.region == ssg::HitRegion::Palette && targets.palette_command_id) {
                dispatch.commands.push_back(
                    {"palette.execute",
                     ssg::PaletteExecuteArguments{*targets.palette_command_id}});
                return dispatch;
            }
            // A left press on a tree row selects that node and then activates it
            // (opens a file / toggles a directory), matching the keyboard
            // select-then-Enter behavior. The node id travels on the hit.
            if (hit.region == ssg::HitRegion::Panel && hit.node_id) {
                dispatch.commands.push_back(
                    {"tree.select", ssg::TreeSelectArguments{*hit.node_id}});
                dispatch.commands.push_back({"tree.activate", std::any{}});
                return dispatch;
            }
            // A left press on the editor places the caret and begins a potential
            // selection drag (the drag itself is routed on subsequent motion in
            // M8-S).  Requires the caller to have resolved the document position.
            if (hit.region == ssg::HitRegion::Editor && targets.document_position) {
                dispatch.commands.push_back(
                    {"cursor.set_position",
                     ssg::SelectionCommandArguments{*targets.document_position,
                                                    std::nullopt}});
                dispatch.begins_drag = true;
            }
            return dispatch;
        case PointerKind::drag:
            // Dragging the editor gutter thumb scrolls live, each motion (M8-B),
            // independent of the selection drag state.
            if (hit.region == ssg::HitRegion::EditorScrollbar) {
                dispatch.commands.push_back(
                    {"view.scroll_to_fraction",
                     ssg::ScrollFractionArguments{hit.scroll_numerator,
                                                  hit.scroll_denominator}});
                return dispatch;
            }
            // While dragging, a motion over an editor cell extends the selection
            // from the press anchor to the cell under the pointer. A drag over a
            // cell with no document target (short line, blank row, or beyond the
            // viewport edge) dispatches nothing, so the selection holds at the
            // last in-viewport position (edge auto-scroll is M8-S2).
            if (dragging && dragAnchor && hit.region == ssg::HitRegion::Editor &&
                targets.document_position) {
                dispatch.commands.push_back(
                    {"select.set_range",
                     ssg::SelectionCommandArguments{
                         std::nullopt,
                         ssg::Selection{*dragAnchor, *targets.document_position}}});
            }
            return dispatch;
        case PointerKind::release:
            // Release ends the drag; the last set_position/set_range already
            // reflects the selection, so no command is dispatched.
            if (dragging) dispatch.ends_drag = true;
            return dispatch;
    }
    return dispatch;
}

WheelTarget route_wheel(ssg::HitRegion region) {
    switch (region) {
        case ssg::HitRegion::Panel:
        case ssg::HitRegion::PanelScrollbar:
            return WheelTarget::tree;
        case ssg::HitRegion::Palette:
        case ssg::HitRegion::PaletteScrollbar:
            // The palette is a client-owned overlay; the app scrolls its window
            // directly (no server command). Falling through to view.scroll_lines
            // would wrongly scroll the editor underneath the palette.
            return WheelTarget::palette;
        default:
            return WheelTarget::editor;
    }
}

std::optional<int> edge_scroll(bool dragging, int pointerRow,
                               ssg::Rect const& content) {
    if (!dragging || content.height <= 0) return std::nullopt;
    if (pointerRow < content.y) return -1;
    if (pointerRow >= content.bottom()) return 1;
    return std::nullopt;
}

}  // namespace ssg::app
