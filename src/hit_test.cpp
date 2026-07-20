#include <ssg/hit_test.h>

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
RegionHit scrollbar_hit(HitRegion region, Rect const& gutter, int row) {
    RegionHit hit;
    hit.region = region;
    auto const relative = static_cast<std::uint32_t>(
        std::clamp(row - gutter.y, 0, std::max(gutter.height - 1, 0)));
    hit.scroll_numerator = relative;
    hit.scroll_denominator =
        static_cast<std::uint32_t>(std::max(gutter.height - 1, 1));
    hit.scrollbar_fraction =
        static_cast<double>(hit.scroll_numerator) / hit.scroll_denominator;
    return hit;
}

RegionHit editor_hit(SessionSnapshot const& snapshot, Rect const& content,
                     int column, int row) {
    auto const& viewport = snapshot.client().viewport;
    auto const viewport_row = static_cast<std::uint32_t>(row - content.y);
    auto const viewport_column = static_cast<std::uint32_t>(column - content.x);
    for (auto const& target : viewport.hit_targets) {
        if (target.viewport_row == viewport_row &&
            target.viewport_column == viewport_column) {
            RegionHit hit;
            hit.region = HitRegion::editor;
            hit.byte_offset = target.byte_offset;
            hit.byte_len = target.byte_len;
            return hit;
        }
    }
    // A cell past a row's content (or on a blank row, which has no hit targets)
    // clamps to that visual row's end — the caret lands at the row's end-of-line
    // (M8 click-past-EOL). A row BELOW the last visible row (an empty area under a
    // short document) clamps to the LAST visible row's end (Decision B), so
    // clicking/dragging below the text reaches the last line. An empty viewport
    // (no visible rows) has nowhere to place the caret -> none.
    if (viewport.visible_rows.empty()) return {};
    auto const& target_row =
        viewport_row < viewport.visible_rows.size()
            ? viewport.visible_rows[viewport_row]
            : viewport.visible_rows.back();
    RegionHit hit;
    hit.region = HitRegion::editor;
    hit.byte_offset = target_row.end_byte_offset;
    hit.byte_len = 0;
    return hit;
}

RegionHit palette_hit(PaletteProjection const& palette, int column, int row) {
    if (contains(palette.scrollbar_rect, column, row)) {
        return scrollbar_hit(HitRegion::palette_scrollbar, palette.scrollbar_rect,
                             row);
    }
    if (!contains(palette.rect, column, row)) return {};
    auto const window_index = static_cast<std::size_t>(row - palette.rect.y);
    if (window_index >= palette.rows.size()) return {};
    RegionHit hit;
    hit.region = HitRegion::palette;
    hit.item_index =
        palette.first_visible + static_cast<std::uint32_t>(window_index);
    return hit;
}

RegionHit panel_hit(SessionSnapshot const& snapshot, Rect const& panel,
                    std::optional<Rect> const& gutter, int column, int row) {
    if (gutter && contains(*gutter, column, row)) {
        return scrollbar_hit(HitRegion::panel_scrollbar, *gutter, row);
    }
    if (row == panel.y) return {};
    auto const& tree = snapshot.sections().tree;
    if (tree.providers.empty()) return {};
    auto const& provider = tree.providers.front();
    auto const viewport_row = static_cast<std::size_t>(row - (panel.y + 1));
    if (viewport_row >= provider.visible_node_ids.size()) return {};
    RegionHit hit;
    hit.region = HitRegion::panel;
    hit.node_id = provider.visible_node_ids[viewport_row];
    return hit;
}

}  // namespace

RegionHit hit_test(SessionSnapshot const& snapshot, int column, int row) {
    auto const& shell = snapshot.sections().shell;
    if (column < 0 || row < 0 || column >= shell.viewport.columns ||
        row >= shell.viewport.rows) {
        return {};
    }

    // The side panel and its gutter occupy the leftmost columns, disjoint from
    // the editor/palette pane.
    if (shell.panel && contains(*shell.panel, column, row)) {
        return panel_hit(snapshot, *shell.panel, shell.panel_scrollbar, column,
                         row);
    }

    // The tab bar sits above the editor pane (disjoint from the panel and the
    // pane content), so a tab click resolves here even while the palette
    // overlays the pane below.
    for (auto const& tab : shell.tab_hits) {
        if (contains(tab.rect, column, row)) {
            RegionHit hit;
            hit.region = HitRegion::tab;
            hit.tab_index = tab.index;
            return hit;
        }
    }

    // The palette overlays the editor pane while it is open, so it takes
    // precedence over the editor content in the same rectangle.
    if (shell.palette) {
        auto hit = palette_hit(*shell.palette, column, row);
        if (hit.hit()) return hit;
        // A pane cell not on a palette row or its gutter is inert while the
        // palette is open (the document is not interactive underneath).
        if (contains(shell.palette->rect, column, row) ||
            contains(shell.palette->scrollbar_rect, column, row)) {
            return {};
        }
    }

    if (!shell.panes.empty()) {
        auto const& pane = shell.panes.front();
        if (contains(pane.scrollbar, column, row)) {
            return scrollbar_hit(HitRegion::editor_scrollbar, pane.scrollbar,
                                 row);
        }
        if (contains(pane.content, column, row)) {
            return editor_hit(snapshot, pane.content, column, row);
        }
    }

    return {};
}

}  // namespace ssg
