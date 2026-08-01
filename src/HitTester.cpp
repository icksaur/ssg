#include <ssg/HitTester.h>

#include <algorithm>

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

RegionHit editorHit(SessionSnapshot const& snapshot, Rect const& content,
                     int column, int row) {
    auto const& viewport = snapshot.client().viewport;
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

RegionHit panelHit(SessionSnapshot const& snapshot, Rect const& panel,
                    std::optional<Rect> const& gutter, int column, int row) {
    if (gutter && contains(*gutter, column, row)) {
        return scrollbarHit(HitRegion::PanelScrollbar, *gutter, row);
    }
    if (row == panel.y) return {};
    auto const& tree = snapshot.sections().tree;
    if (tree.providers.empty()) return {};
    auto const& provider = tree.providers.front();
    auto const viewportRow = static_cast<std::size_t>(row - (panel.y + 1));
    if (viewportRow >= provider.visibleNodeIds.size()) return {};
    RegionHit hit;
    hit.region = HitRegion::Panel;
    hit.nodeId = provider.visibleNodeIds[viewportRow];
    return hit;
}

}  // namespace

RegionHit HitTester::at(int column, int row) const {
    auto const& snapshot = snapshot_;
    auto const& shell = snapshot.sections().shell;
    if (column < 0 || row < 0 || column >= shell.viewport.columns ||
        row >= shell.viewport.rows) {
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
    }

    // The side panel and its gutter occupy the leftmost columns, disjoint from
    // the editor/palette pane.
    if (shell.panel && contains(*shell.panel, column, row)) {
        return panelHit(snapshot, *shell.panel, shell.panelScrollbar, column,
                         row);
    }

    // The tab bar sits above the editor pane (disjoint from the panel and the
    // pane content), so a tab click resolves here even while the palette
    // overlays the pane below.
    for (auto const& tab : shell.tabHits) {
        if (contains(tab.rect, column, row)) {
            RegionHit hit;
            hit.region = HitRegion::Tab;
            hit.tabIndex = tab.index;
            return hit;
        }
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
    auto const& shell = snapshot_.sections().shell;
    auto const make = [](Rect const& gutter, ScrollbarMetrics const& m) {
        return GutterThumb{gutter.y, m.viewportRows, m.thumbStart, m.thumbSize};
    };
    switch (region) {
    case HitRegion::EditorScrollbar:
        if (shell.panes.empty()) return std::nullopt;
        return make(shell.panes.front().scrollbar,
                    snapshot_.client().viewport.scrollbar);
    case HitRegion::PanelScrollbar: {
        if (!shell.panelScrollbar) return std::nullopt;
        auto const& tree = snapshot_.sections().tree;
        if (tree.providers.empty()) return std::nullopt;
        return make(*shell.panelScrollbar, tree.providers.front().scrollbar);
    }
    case HitRegion::PaletteScrollbar:
        if (!shell.palette) return std::nullopt;
        return make(shell.palette->scrollbarRect, shell.palette->scrollbar);
    default:
        return std::nullopt;
    }
}

}  // namespace ssg
