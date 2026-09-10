#include <ssg/HitTester.h>

#include <algorithm>
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

RegionHit editorHit(GridPresentation const& snapshot, Rect const& content,
                     int column, int row) {
    auto const& viewport = snapshot.viewport;
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
    if (contains(panel.query, column, row)) {
        RegionHit hit;
        hit.region = HitRegion::PanelQuery;
        return hit;
    }
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

RegionHit uiRegionHit(const SolvedUiRegion& surface, HitRegion fieldRegion,
                    int column, int row) {
    if (surface.input &&
        (contains(surface.input->query, column, row) ||
         (surface.input->ghost &&
          contains(*surface.input->ghost, column, row)))) {
        RegionHit hit;
        hit.region = fieldRegion;
        hit.fieldId = "input_line.query";
        return hit;
    }
    for (const auto& item : surface.items) {
        if (!contains(item.rect, column, row)) continue;
        RegionHit hit;
        hit.fieldId = item.nodeId.value();
        hit.region = fieldRegion;
        hit.commandId = item.command;
        return hit;
    }
    return {};
}

const UiNode* nodeById(const UiNode& node, std::string_view id) {
    if (node.id.value() == id) return &node;
    const auto* container = std::get_if<UiContainer>(&node.content);
    if (!container) return nullptr;
    for (const auto& child : container->children) {
        if (const auto* found = nodeById(child, id)) return found;
    }
    return nullptr;
}

RegionHit promptHit(const GridPresentation& snapshot, const UiNode& node,
                    int column, int row) {
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children) {
            RegionHit hit = promptHit(snapshot, child, column, row);
            if (hit.region != HitRegion::None) return hit;
        }
        return {};
    }
    const auto* leaf = std::get_if<UiLeaf>(&node.content);
    if (!leaf || !node.resolved) return {};
    const bool actionable =
        (leaf->widget.kind == WidgetKind::TextInput &&
         node.resolved->active.has_value()) ||
        ((leaf->widget.kind == WidgetKind::Checkbox ||
          leaf->widget.kind == WidgetKind::Field) &&
         node.resolved->command && !node.resolved->command->empty());
    const auto* solved = snapshot.layout.find(node.id);
    if (!actionable || !solved ||
        !contains(solved->rect, column, row)) {
        return {};
    }
    RegionHit hit;
    hit.region = HitRegion::FooterField;
    hit.fieldId = node.id.value();
    return hit;
}

}  // namespace

RegionHit HitTester::at(int column, int row) const {
    auto const& snapshot = snapshot_;
    const auto* root =
        snapshot.layout.find(UiNodeId{std::string{kRootNodeId}});
    if (!root || column < root->rect.x || row < root->rect.y ||
        column >= root->rect.right() || row >= root->rect.bottom()) {
        return {};
    }

    if (snapshot.promptStatus.activeKind &&
        promptFocusRegion(*snapshot.promptStatus.activeKind) ==
            PromptRegion::Footer) {
        const auto* prompt =
            snapshot.layout.find(UiNodeId{std::string{kFooterPromptNodeId}});
        if (!prompt) {
            throw std::logic_error(
                "HitTester: prompt has no solved UI node");
        }
        if (contains(prompt->rect, column, row)) {
            const auto* promptSchema = nodeById(
                snapshot.uiTree.root,
                kFooterPromptNodeId);
            return promptSchema
                       ? promptHit(snapshot, *promptSchema, column, row)
                       : RegionHit{};
        }
    }

    if (snapshot.notice) {
        const auto* node =
            snapshot.layout.find(UiNodeId{std::string{kNoticeNodeId}});
        if (!node) {
            throw std::logic_error(
                "HitTester: notice has no solved UI node");
        }
        if (contains(node->rect, column, row)) {
            const auto solved =
                solveNoticeSurface(*snapshot.notice, node->rect);
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

    if (!snapshot.externalModification.files.empty()) {
        const auto* node = snapshot.layout.find(
            UiNodeId{std::string{kExternalModNodeId}});
        if (!node) {
            throw std::logic_error(
                "HitTester: external modification has no solved UI node");
        }
        if (contains(node->rect, column, row)) {
            const auto solved = solveExternalModificationSurface(
                snapshot.externalModification, node->rect);
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

    if (const auto* node = snapshot.layout.find(
            UiNodeId{std::string{kTabBarNodeId}});
        node && contains(node->rect, column, row)) {
        const auto solved = solveTabBar(
            snapshot.tabs, snapshot.style.tab,
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
    if (const auto* node = snapshot.layout.find(
            UiNodeId{std::string{kFindResultsViewportNodeId}})) {
        const auto solved = solvePaletteSurface(
            snapshot.palette, node->rect,
            snapshot.style.dimensions.scrollbarGutterWidth);
        auto hit = paletteHit(solved, column, row);
        if (hit.hit()) return hit;
        if (contains(solved.rect, column, row)) return {};
    }

    if (snapshot.header &&
        contains(snapshot.header->rect, column, row)) {
        return uiRegionHit(*snapshot.header, HitRegion::HeaderField,
                         column, row);
    }
    if (snapshot.footer &&
        contains(snapshot.footer->rect, column, row)) {
        return uiRegionHit(*snapshot.footer, HitRegion::FooterField,
                         column, row);
    }

    for (const auto id : {kHeaderNodeId, kFooterNodeId}) {
        const auto* node =
            snapshot.layout.find(UiNodeId{std::string{id}});
        if (node && contains(node->rect, column, row)) return {};
    }

    if (snapshot.panel &&
        contains(snapshot.panel->rect, column, row)) {
        return panelHit(*snapshot.panel, column, row);
    }

    if (snapshot.document) {
        const auto& document = *snapshot.document;
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
        if (!snapshot_.document) return std::nullopt;
        return make(snapshot_.document->scrollbarGutter,
                    snapshot_.viewport.scrollbar);
    case HitRegion::PanelScrollbar: {
        if (!snapshot_.panel || !snapshot_.panel->scrollbarGutter) {
            return std::nullopt;
        }
        return make(*snapshot_.panel->scrollbarGutter,
                    snapshot_.panel->scrollbar);
    }
    case HitRegion::PaletteScrollbar:
        if (const auto* node = snapshot_.layout.find(
                UiNodeId{std::string{kFindResultsViewportNodeId}})) {
            const auto solved = solvePaletteSurface(
                snapshot_.palette, node->rect,
                snapshot_.style.dimensions.scrollbarGutterWidth);
            return make(solved.scrollbar, snapshot_.palette.scrollbar);
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

}  // namespace ssg
