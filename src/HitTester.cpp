#include <ssg/HitTester.h>

#include <stdexcept>

namespace ssg {
namespace {

bool contains(Rect const& rect, int column, int row) {
    return column >= rect.x && column < rect.right() && row >= rect.y &&
           row < rect.bottom();
}

// Map a cell inside a scrollbar gutter to a RegionHit for that gutter. The
// vertical position becomes numerator/denominator for view.scroll_to_fraction
// (top of the gutter -> 0, bottom -> 1), so a click maps directly to a scroll.
// Classifies a gutter press to its region.  The scroll position a press or drag
// sends is computed by the app from the region's published thumb geometry
// (grab-offset dragging), never from the absolute pointer row, so this no longer
// derives a fraction of its own.
RegionHit scrollbarHit(HitRegion region, Rect const& /*gutter*/, int /*row*/) {
    RegionHit hit;
    hit.region = region;
    return hit;
}

RegionHit editorHit(GridFrame const& snapshot, Rect const& content,
                     int column, int row) {
    auto const& viewport = snapshot.presentation().viewport;
    auto const viewportRow = static_cast<std::uint32_t>(row - content.y);
    auto const viewportColumn = static_cast<std::uint32_t>(column - content.x);
    for (auto const& target : viewport.hitTargets) {
        if (target.viewportRow == viewportRow &&
            target.viewportColumn == viewportColumn) {
            RegionHit hit;
            hit.region = HitRegion::Editor;
            hit.byteOffset = target.byteOffset;
            hit.byteLen = target.byteLen;
            return hit;
        }
    }
    // A cell past a row's content (or on a blank row, which has no hit targets)
    // clamps to that visual row's end — the caret lands at the row's end-of-line
    // (M8 click-past-EOL). A row BELOW the last visible row (an empty area under a
    // short document) clamps to the LAST visible row's end (Decision B), so
    // clicking/dragging below the text reaches the last line. An empty viewport
    // (no visible rows) has nowhere to place the caret -> none.
    if (viewport.visibleRows.empty()) return {};
    const auto targetRow =
        viewportRow < viewport.visibleRows.size()
            ? viewportRow
            : static_cast<std::uint32_t>(viewport.visibleRows.size() - 1);
    RegionHit hit;
    hit.region = HitRegion::Editor;
    hit.byteOffset = viewport.editableOffset(targetRow);
    hit.byteLen = 0;
    return hit;
}

RegionHit paletteHit(SolvedPaletteSurface const& palette, int column, int row) {
    if (contains(palette.scrollbar, column, row)) {
        return scrollbarHit(HitRegion::PaletteScrollbar, palette.scrollbar,
                             row);
    }
    for (const auto& paletteRow : palette.visibleRows) {
        if (!contains(paletteRow.rect, column, row)) continue;
        RegionHit hit;
        hit.region = HitRegion::Palette;
        hit.itemIndex = paletteRow.absoluteIndex;
        return hit;
    }
    return {};
}

RegionHit panelHit(SolvedPanelSurface const& panel, int column, int row) {
    if (panel.scrollbarGutter &&
        contains(*panel.scrollbarGutter, column, row)) {
        return scrollbarHit(HitRegion::PanelScrollbar,
                           *panel.scrollbarGutter, row);
    }
    for (const auto& item : panel.rows) {
        if (!contains(item.rect, column, row)) continue;
        RegionHit hit;
        hit.region = HitRegion::Panel;
        hit.nodeId = item.nodeId;
        return hit;
    }
    return {};
}

}  // namespace

RegionHit HitTester::at(int column, int row) const {
    auto const& snapshot = snapshot_;
    auto const& shell = snapshot.presentation().shell;
    if (column < 0 || row < 0 || column >= shell.viewport.columns ||
        row >= shell.viewport.rows) {
        return {};
    }

    if (snapshot.sections().promptView) {
        const auto* prompt =
            snapshot.layout().find(UiNodeId{std::string{kFooterPromptNodeId}});
        if (!prompt) {
            throw std::logic_error(
                "HitTester: prompt has no solved UI node");
        }
        if (contains(prompt->rect, column, row)) {
            for (auto const& control :
                 snapshot.sections().promptView->controls) {
                const auto* solved = snapshot.layout().find(
                    footerPromptControlNodeId(control.id));
                if (!solved) {
                    throw std::logic_error(
                        "HitTester: prompt control has no solved UI node");
                }
                if (!contains(solved->rect, column, row)) continue;
                if (control.kind == PromptControlKind::Count ||
                    control.command.empty()) {
                    return {};
                }
                RegionHit hit;
                hit.region = HitRegion::PromptControl;
                hit.fieldId = control.id;
                hit.commandId = control.command;
                return hit;
            }
            return {};
        }
    }

    if (snapshot.sections().noticeView) {
        const auto* node =
            snapshot.layout().find(UiNodeId{std::string{kNoticeNodeId}});
        if (!node) {
            throw std::logic_error(
                "HitTester: notice has no solved UI node");
        }
        if (contains(node->rect, column, row)) {
            const auto solved =
                solveNoticeSurface(*snapshot.sections().noticeView, node->rect);
            for (const auto& action : solved.actions) {
                if (!contains(action.rect, column, row)) continue;
                RegionHit hit;
                hit.region = HitRegion::NoticeAction;
                hit.fieldId = action.id;
                return hit;
            }
            return {};
        }
    }

    if (!snapshot.sections().externalModification.files.empty()) {
        const auto* node = snapshot.layout().find(
            UiNodeId{std::string{kExternalModNodeId}});
        if (!node) {
            throw std::logic_error(
                "HitTester: external modification has no solved UI node");
        }
        if (contains(node->rect, column, row)) {
            const auto solved = solveExternalModificationSurface(
                snapshot.sections().externalModification, node->rect);
            for (const auto& externalRow : solved.rows) {
                for (const auto& action : externalRow.actions) {
                    if (!contains(action.rect, column, row)) continue;
                    RegionHit hit;
                    hit.region = HitRegion::ExternalAction;
                    hit.externalFileId = action.fileId.value();
                    hit.commandId = action.command;
                    return hit;
                }
            }
            return {};
        }
    }

    if (const auto* node = snapshot.layout().find(
            UiNodeId{std::string{kTabBarNodeId}});
        node && contains(node->rect, column, row)) {
        const auto solved = solveTabBar(
            snapshot.sections().tabs, snapshot.presentation().style.tab,
            node->rect);
        for (const auto& tab : solved.tabs) {
            if (!contains(tab.rect, column, row)) continue;
            RegionHit hit;
            hit.region = HitRegion::Tab;
            hit.tabIndex = static_cast<std::uint32_t>(tab.index);
            return hit;
        }
        return {};
    }

    // The picker replaces the editor branch, so its solved viewport takes
    // precedence over every legacy hit.
    if (const auto* node = snapshot.layout().find(
            UiNodeId{std::string{kFindResultsViewportNodeId}})) {
        const auto solved = solvePaletteSurface(
            snapshot.palette(), node->rect,
            snapshot.presentation().style.dimensions.scrollbarGutterWidth);
        auto hit = paletteHit(solved, column, row);
        if (hit.hit()) return hit;
        if (contains(solved.rect, column, row)) return {};
    }

    for (auto const& node : shell.accessibilityNodes) {
        if (!contains(node.rect, column, row)) continue;
        if (node.kind == ShellNodeKind::HeaderField) {
            RegionHit hit;
            hit.region = HitRegion::HeaderField;
            hit.fieldId = node.id;
            hit.commandId = node.commandId;
            return hit;
        }
        if (node.kind == ShellNodeKind::FooterField) {
            RegionHit hit;
            hit.region = HitRegion::FooterField;
            hit.fieldId = node.id;
            hit.commandId = node.commandId;
            return hit;
        }
        if (node.kind == ShellNodeKind::FooterAction) {
            RegionHit hit;
            hit.region = HitRegion::StatusAction;
            hit.fieldId = node.id;
            hit.statusInvocation = node.statusInvocation;
            return hit;
        }
        if (node.kind == ShellNodeKind::FooterHint) {
            // The persistent help hint dispatches its command id directly. It is
            // NOT a status-queue action, so it deliberately does not go through
            // status.invoke_action's generation freshness gate.
            RegionHit hit;
            hit.region = HitRegion::FooterField;
            hit.fieldId = node.id;
            hit.commandId = node.commandId;
            return hit;
        }
    }

    for (const auto id : {kHeaderNodeId, kFooterNodeId}) {
        const auto* node =
            snapshot.layout().find(UiNodeId{std::string{id}});
        if (node && contains(node->rect, column, row)) return {};
    }

    if (snapshot.panel() &&
        contains(snapshot.panel()->rect, column, row)) {
        return panelHit(*snapshot.panel(), column, row);
    }

    if (snapshot.document()) {
        const auto& document = *snapshot.document();
        if (contains(document.scrollbarGutter, column, row)) {
            return scrollbarHit(HitRegion::EditorScrollbar,
                                document.scrollbarGutter, row);
        }
        if (contains(document.content, column, row)) {
            return editorHit(snapshot, document.content, column, row);
        }
    }

    return {};
}

std::optional<HitTester::GutterThumb> HitTester::gutterThumb(
    HitRegion region) const {
    auto const make = [](Rect const& gutter, ScrollbarMetrics const& m) {
        return GutterThumb{gutter.y, m.viewportRows, m.thumbStart, m.thumbSize};
    };
    switch (region) {
    case HitRegion::EditorScrollbar:
        if (!snapshot_.document()) return std::nullopt;
        return make(snapshot_.document()->scrollbarGutter,
                    snapshot_.presentation().viewport.scrollbar);
    case HitRegion::PanelScrollbar: {
        if (!snapshot_.panel() || !snapshot_.panel()->scrollbarGutter) {
            return std::nullopt;
        }
        return make(*snapshot_.panel()->scrollbarGutter,
                    snapshot_.panel()->scrollbar);
    }
    case HitRegion::PaletteScrollbar:
        if (const auto* node = snapshot_.layout().find(
                UiNodeId{std::string{kFindResultsViewportNodeId}})) {
            const auto solved = solvePaletteSurface(
                snapshot_.palette(), node->rect,
                snapshot_.presentation().style.dimensions.scrollbarGutterWidth);
            return make(solved.scrollbar, snapshot_.palette().scrollbar);
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

}  // namespace ssg
