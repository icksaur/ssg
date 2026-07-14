#include "pointer_routing.h"

namespace ssg::app {

PointerDispatch route_pointer(ssg::RegionHit const& hit, PointerButton button,
                              PointerKind kind, bool dragging,
                              std::optional<ssg::DocumentPosition> drag_anchor,
                              PointerTargets const& targets) {
    PointerDispatch dispatch;
    // Only the left button drives editing actions in M8; other buttons are a
    // no-op (right-click menus etc. are out of scope).
    if (button != PointerButton::left) return dispatch;

    switch (kind) {
        case PointerKind::press:
            // A left press on the editor places the caret and begins a potential
            // selection drag (the drag itself is routed on subsequent motion in
            // M8-S).  Requires the caller to have resolved the document position.
            if (hit.region == ssg::HitRegion::editor && targets.document_position) {
                dispatch.commands.push_back(
                    {"cursor.set_position",
                     ssg::SelectionCommandArguments{*targets.document_position,
                                                    std::nullopt}});
                dispatch.begins_drag = true;
            }
            return dispatch;
        case PointerKind::drag:
            // While dragging, a motion over an editor cell extends the selection
            // from the press anchor to the cell under the pointer. A drag over a
            // cell with no document target (short line, blank row, or beyond the
            // viewport edge) dispatches nothing, so the selection holds at the
            // last in-viewport position (edge auto-scroll is M8-S2).
            if (dragging && drag_anchor && hit.region == ssg::HitRegion::editor &&
                targets.document_position) {
                dispatch.commands.push_back(
                    {"select.set_range",
                     ssg::SelectionCommandArguments{
                         std::nullopt,
                         ssg::Selection{*drag_anchor, *targets.document_position}}});
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

}  // namespace ssg::app
