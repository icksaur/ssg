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

RegionHit paletteHit(PaletteProjection const& palette, int column, int row) {
    if (contains(palette.scrollbarRect, column, row)) {
        return scrollbarHit(HitRegion::PaletteScrollbar, palette.scrollbarRect,
                             row);
    }
    if (!contains(palette.rect, column, row)) return {};
    auto const windowIndex = static_cast<std::size_t>(row - palette.rect.y);
    if (windowIndex >= palette.rows.size()) return {};
    RegionHit hit;
    hit.region = HitRegion::Palette;
    hit.itemIndex =
        palette.firstVisible + static_cast<std::uint32_t>(windowIndex);
    return hit;
}

RegionHit panelHit(GridFrame const& snapshot, Rect const& panel,
                    std::optional<Rect> const& gutter, int column, int row) {
    if (gutter && contains(*gutter, column, row)) {
        return scrollbarHit(HitRegion::PanelScrollbar, *gutter, row);
    }
    if (row == panel.y) return {};
    auto const& tree = snapshot.sections().tree;
    if (tree.providers.empty()) return {};
    auto const& windows = snapshot.presentation().treeWindows;
    if (windows.empty()) return {};
    auto const& window = windows.front();
    auto const viewportRow = static_cast<std::size_t>(row - (panel.y + 1));
    if (viewportRow >= window.visibleNodeIds.size()) return {};
    RegionHit hit;
    hit.region = HitRegion::Panel;
    hit.nodeId = window.visibleNodeIds[viewportRow];
    return hit;
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

    // The side panel and its gutter occupy the leftmost columns, disjoint from
    // the editor/palette pane.
    if (shell.panel && contains(*shell.panel, column, row)) {
        return panelHit(snapshot, *shell.panel, shell.panelScrollbar, column,
                         row);
    }

    // The palette overlays the editor pane while it is open, so it takes
    // precedence over the editor content in the same rectangle.
    if (shell.palette) {
        auto hit = paletteHit(*shell.palette, column, row);
        if (hit.hit()) return hit;
        // A pane cell not on a palette row or its gutter is inert while the
        // palette is open (the document is not interactive underneath).
        if (contains(shell.palette->rect, column, row) ||
            contains(shell.palette->scrollbarRect, column, row)) {
            return {};
        }
    }

    if (!shell.panes.empty()) {
        auto const& pane = shell.panes.front();
        if (contains(pane.scrollbar, column, row)) {
            return scrollbarHit(HitRegion::EditorScrollbar, pane.scrollbar,
                                 row);
        }
        if (contains(pane.content, column, row)) {
            return editorHit(snapshot, pane.content, column, row);
        }
    }

    return {};
}

std::optional<HitTester::GutterThumb> HitTester::gutterThumb(
    HitRegion region) const {
    auto const& shell = snapshot_.presentation().shell;
    auto const make = [](Rect const& gutter, ScrollbarMetrics const& m) {
        return GutterThumb{gutter.y, m.viewportRows, m.thumbStart, m.thumbSize};
    };
    switch (region) {
    case HitRegion::EditorScrollbar:
        if (shell.panes.empty()) return std::nullopt;
        return make(shell.panes.front().scrollbar,
                    snapshot_.presentation().viewport.scrollbar);
    case HitRegion::PanelScrollbar: {
        if (!shell.panelScrollbar) return std::nullopt;
        auto const& windows = snapshot_.presentation().treeWindows;
        if (windows.empty()) return std::nullopt;
        return make(*shell.panelScrollbar, windows.front().scrollbar);
    }
    case HitRegion::PaletteScrollbar:
        if (!shell.palette) return std::nullopt;
        return make(shell.palette->scrollbarRect, shell.palette->scrollbar);
    default:
        return std::nullopt;
    }
}

}  // namespace ssg
