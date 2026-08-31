#include "pointer_routing.h"

#include <algorithm>

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
     WheelTarget::editor},
    {ssg::HitRegion::Panel, ssg::HitRegion::PanelScrollbar, WheelTarget::tree},
        // No command: the picker's ranked list is client-owned for latency, so its
    // scroll must never round-trip. route_pointer returns a ClientScroll for
    // this one instead.
    {ssg::HitRegion::Palette, ssg::HitRegion::PaletteScrollbar,
     WheelTarget::palette},
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
                             ssg::RegionHit const& hit,
                             ssg::Revision observedRevision) {
    PointerDispatch dispatch;
    if (descriptor.target == WheelTarget::palette) {
        dispatch.client_scroll = ClientScroll{
            descriptor.target, hit.scrollNumerator, hit.scrollDenominator};
        return dispatch;
    }
    dispatch.semantic_input = ssg::ScrollFractionInput{
        {observedRevision},
        descriptor.target == WheelTarget::editor
            ? ssg::SemanticScrollTarget::Document
            : ssg::SemanticScrollTarget::Tree,
        hit.scrollNumerator, hit.scrollDenominator};
    return dispatch;
}

}  // namespace

std::span<const ScrollableRegionDescriptor> scrollable_regions() noexcept {
    return kScrollableRegions;
}

bool is_scrollbar_region(ssg::HitRegion region) noexcept {
    return gutterRegion(region) != nullptr;
}

int scrollbar_grab_offset(int rel, int thumbStart, int thumbSize) noexcept {
    if (rel >= thumbStart && rel < thumbStart + thumbSize) {
        return rel - thumbStart;  // grabbed on the thumb: hold it in place
    }
    return thumbSize / 2;  // well press: centre the thumb on the cursor
}

GutterFraction gutter_fraction(int rel, int grabOffset, int travel) noexcept {
    if (travel <= 0) return {0, 1};  // thumb fills the gutter; nothing scrolls
    int const desiredTop = std::clamp(rel - grabOffset, 0, travel);
    return {static_cast<std::uint32_t>(desiredTop),
            static_cast<std::uint32_t>(travel)};
}

bool register_click_is_double(ClickTracker& tracker,
                              std::chrono::steady_clock::time_point now,
                              std::uint64_t surface, int row, int column,
                              std::chrono::milliseconds window) noexcept {
    bool const isDouble = tracker.time.has_value() &&
                          now - *tracker.time <= window &&
                          tracker.surface == surface && tracker.row == row &&
                          tracker.column == column;
    if (isDouble) {
        tracker = {};  // reset so a third press is a fresh single (no triple)
        return true;
    }
    tracker.time = now;
    tracker.surface = surface;
    tracker.row = row;
    tracker.column = column;
    return false;
}

PointerDispatch double_click_dispatch(ssg::DocumentPosition position,
                                      ssg::Revision observedRevision) {
    PointerDispatch dispatch;
    dispatch.semantic_input = ssg::DocumentPointerInput{
        {observedRevision}, position.byteOffset, false, true};
    // A double-click selects a word; it must not also start a drag-select.
    dispatch.begins_drag = false;
    return dispatch;
}

PointerDispatch route_pointer(ssg::RegionHit const& hit, PointerButton button,
                              PointerKind kind, bool alt, bool dragging,
                              std::optional<ssg::DocumentPosition> dragAnchor,
                              PointerTargets const& targets) {
    PointerDispatch dispatch;
    // Middle-click a tab closes it (a common convention).  Handled before the
    // left-only guard below; no other middle-button gesture is recognised.
    if (button == PointerButton::middle) {
        if (kind == PointerKind::press && hit.region == ssg::HitRegion::Tab &&
            targets.tab_id) {
            dispatch.semantic_input = ssg::TabPointerInput{
                {targets.observed_revision}, *targets.tab_id,
                ssg::InputPointerButton::Auxiliary};
        }
        return dispatch;
    }
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
                return gutterScroll(*gutter, hit, targets.observed_revision);
            }
            // A left press on a tab activates it (the caller resolved tab_index
            // -> TabId); on a palette row it executes that candidate (the caller
            // mapped the absolute item_index -> candidate id). Neither begins a
            // selection drag.
            if (hit.region == ssg::HitRegion::Tab && targets.tab_id) {
                dispatch.semantic_input = ssg::TabPointerInput{
                    {targets.observed_revision}, *targets.tab_id};
                return dispatch;
            }
            if (hit.region == ssg::HitRegion::Palette &&
                targets.picker_candidate_id && targets.picker_activation) {
                dispatch.semantic_input = ssg::PickerPointerInput{
                    *targets.picker_activation, *targets.picker_candidate_id};
                return dispatch;
            }
            // An external-modification action selects its file then runs the
            // action on the library-owned selection -- select-then-act, the same
            // one behavior path the keyboard drives (Decision 3/6). The file id is
            // runtime-minted and travels on the hit; the action command is
            // payload-less and honored only if the selected file offers it (the
            // command's own guard).
            if (hit.region == ssg::HitRegion::ExternalAction &&
                targets.external_invocation) {
                dispatch.semantic_input = ssg::ExternalActionPointerInput{
                    {targets.observed_revision}, *targets.external_invocation};
                return dispatch;
            }
            if ((hit.region == ssg::HitRegion::HeaderField ||
                 hit.region == ssg::HitRegion::FooterField) &&
                targets.ui_generation && targets.ui_node_id) {
                dispatch.command = ssg::ClientCommand{
                   "ui.activate", targets.observed_revision,
                   ssg::UiNodeActivationArguments{
                       *targets.ui_generation, *targets.ui_node_id}};
                return dispatch;
            }
            if (hit.region == ssg::HitRegion::NoticeAction &&
                targets.notice_action_id) {
                dispatch.semantic_input = ssg::NoticeActionPointerInput{
                   {targets.observed_revision}, *targets.notice_action_id};
                return dispatch;
            }
            // A left press on a tree row selects that node and then activates it
            // (opens a file / toggles a directory), matching the keyboard
            // select-then-Enter behavior. The node id travels on the hit.
            if (hit.region == ssg::HitRegion::Panel && hit.nodeId) {
                dispatch.semantic_input = ssg::TreePointerInput{
                    {targets.observed_revision}, *hit.nodeId};
                return dispatch;
            }
            // A left press on the editor places the caret and begins a potential
            // selection drag (the drag itself is routed on subsequent motion in
            // M8-S).  Requires the caller to have resolved the document position.
            // With Alt, it instead ADDS a collapsed caret so multi-cursor
            // gestures compose with the existing set.
            if (hit.region == ssg::HitRegion::Editor && targets.document_position) {
                dispatch.semantic_input = ssg::DocumentPointerInput{
                    {targets.observed_revision},
                    targets.document_position->byteOffset, alt, false};
                dispatch.begins_drag = true;
            }
            return dispatch;
        case PointerKind::drag:
            // Dragging any gutter thumb scrolls that surface live, each motion,
            // independent of the selection drag state. Same catalog as press.
            if (auto const* gutter = gutterRegion(hit.region)) {
                return gutterScroll(*gutter, hit, targets.observed_revision);
            }
            // While dragging, a motion over an editor cell extends the selection
            // from the press anchor to the cell under the pointer. A drag over a
            // cell with no document target (short line, blank row, or beyond the
            // viewport edge) dispatches nothing, so the selection holds at the
            // last in-viewport position (edge auto-scroll is M8-S2).
            if (dragging && dragAnchor && hit.region == ssg::HitRegion::Editor &&
                targets.document_position) {
                dispatch.semantic_input = ssg::DocumentPointerInput{
                    {targets.observed_revision},
                    targets.document_position->byteOffset, false, false,
                    ssg::InputPointerButton::Primary,
                    ssg::InputPointerPhase::Move};
            }
            return dispatch;
        case PointerKind::release:
            // Release ends the drag; the last set_position/set_range already
            // reflects the selection, so no command is dispatched.
            if (dragging) {
                dispatch.semantic_input = ssg::DocumentPointerInput{
                    {targets.observed_revision}, std::nullopt, false, false,
                    ssg::InputPointerButton::Primary,
                    ssg::InputPointerPhase::Release};
                dispatch.ends_drag = true;
            }
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
