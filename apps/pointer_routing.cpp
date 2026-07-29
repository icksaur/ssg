#include "pointer_routing.h"

#include <ssg/Keymap.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/TreeModel.h>

namespace ssg::app {
namespace {

// The one list of scrollable surfaces. `route_pointer` and `route_wheel` both
// drive from it, so a surface cannot be wired for one gesture and forgotten for
// the other -- which is exactly how the panel and picker gutters ended up
// wheel-scrollable but not draggable.
constexpr ScrollableRegionDescriptor kScrollableRegions[] = {
    {ssg::HitRegion::Editor, ssg::HitRegion::EditorScrollbar,
     WheelTarget::editor, "view.scroll_to_fraction"},
    {ssg::HitRegion::Panel, ssg::HitRegion::PanelScrollbar, WheelTarget::tree,
     "tree.scroll_to_fraction"},
        // No command: the picker's ranked list is client-owned for latency, so its
    // scroll must never round-trip. route_pointer returns a ClientScroll for
    // this one instead (doc/spec-scroll.md S-I5).
    {ssg::HitRegion::Palette, ssg::HitRegion::PaletteScrollbar,
     WheelTarget::palette, {}},
};

// The descriptor whose GUTTER this region is, if any.
const ScrollableRegionDescriptor* gutterRegion(ssg::HitRegion region) {
    for (auto const& descriptor : kScrollableRegions) {
        if (descriptor.scrollbar == region) return &descriptor;
    }
    return nullptr;
}

// The gesture a gutter press or drag produces: a command for a server-owned
// offset, or a client-local scroll for a client-owned one. One place, so every
// gutter behaves alike.
PointerDispatch gutterScroll(ScrollableRegionDescriptor const& descriptor,
                             ssg::RegionHit const& hit) {
    PointerDispatch dispatch;
    if (descriptor.scrollCommand.empty()) {
        dispatch.client_scroll = ClientScroll{
            descriptor.target, hit.scrollNumerator, hit.scrollDenominator};
        return dispatch;
    }
    dispatch.commands.push_back(
        {std::string{descriptor.scrollCommand},
         ssg::ScrollFractionArguments{hit.scrollNumerator,
                                      hit.scrollDenominator}});
    return dispatch;
}

}  // namespace

std::span<const ScrollableRegionDescriptor> scrollable_regions() noexcept {
    return kScrollableRegions;
}

bool is_scrollbar_region(ssg::HitRegion region) noexcept {
    return gutterRegion(region) != nullptr;
}

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
            // A left press on ANY scrollable surface's gutter scrolls it to the
            // fraction hit_test computed from the pointer row. Driven from the
            // catalog rather than per region, so every gutter answers the same
            // gesture -- the panel's and picker's used to be silently ignored.
            // Independent of the selection drag state: the offset moves live as
            // the thumb is dragged.
            if (auto const* gutter = gutterRegion(hit.region)) {
                return gutterScroll(*gutter, hit);
            }
            // A left press on a tab activates it (the caller resolved tab_index
            // -> TabId); on a palette row it executes that candidate (the caller
            // mapped the absolute item_index -> candidate id). Neither begins a
            // selection drag.
            if (hit.region == ssg::HitRegion::Tab && targets.tab_id) {
                dispatch.commands.push_back({"tab.activate", *targets.tab_id});
                return dispatch;
            }
            if (hit.region == ssg::HitRegion::Palette && targets.picker_candidate_id) {
                // Same dispatch decision as a keyboard submit, and it must stay
                // that way: a click and an Enter on the same row mean the same
                // thing.
                switch (targets.picker_mode) {
                case ssg::SearchMode::Command:
                    dispatch.commands.push_back(
                        {"palette.execute",
                         ssg::PaletteExecuteArguments{*targets.picker_candidate_id}});
                    break;
                case ssg::SearchMode::File:
                    // Just the open: the server dismisses the picker when it
                    // succeeds, so click and Enter behave identically on both
                    // the success and the failure path.
                    dispatch.commands.push_back(
                        {"file.open", *targets.picker_candidate_id});
                    break;
                default:
                    break;
                }
                return dispatch;
            }
            if ((hit.region == ssg::HitRegion::HeaderField ||
                 hit.region == ssg::HitRegion::FooterField) &&
                targets.field_command_id) {
                dispatch.commands.push_back({*targets.field_command_id, std::any{}});
                return dispatch;
            }
            // A left press on a tree row selects that node and then activates it
            // (opens a file / toggles a directory), matching the keyboard
            // select-then-Enter behavior. The node id travels on the hit.
            if (hit.region == ssg::HitRegion::Panel && hit.nodeId) {
                dispatch.commands.push_back(
                    {"tree.select", ssg::TreeSelectArguments{*hit.nodeId}});
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
            // Dragging any gutter thumb scrolls that surface live, each motion,
            // independent of the selection drag state. Same catalog as press.
            if (auto const* gutter = gutterRegion(hit.region)) {
                return gutterScroll(*gutter, hit);
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
    // Same catalog as the gutter routing, so a surface cannot be wheel-scrollable
    // but not draggable (or the reverse) by omission.
    for (auto const& descriptor : kScrollableRegions) {
        if (descriptor.content == region || descriptor.scrollbar == region) {
            return descriptor.target;
        }
    }
    // Everything else scrolls the document: the editor is the default surface,
    // and a wheel over chrome should not be inert.
    return WheelTarget::editor;
}

std::optional<int> edge_scroll(bool dragging, int pointerRow,
                               ssg::Rect const& content) {
    if (!dragging || content.height <= 0) return std::nullopt;
    if (pointerRow < content.y) return -1;
    if (pointerRow >= content.bottom()) return 1;
    return std::nullopt;
}

}  // namespace ssg::app
