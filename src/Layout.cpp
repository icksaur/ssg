#include <ssg/Layout.h>

#include <ssg/GraphemeLayout.h>
#include <ssg/Viewport.h>
#include <ssg/EditorSession.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace ssg {
namespace {

struct PaneCellFrame {
    PaneId id;
    Rect rect;
};

void requireSplitNode(const PaneTopologyNode& node) {
    if (node.children.size() != 2) {
        throw std::logic_error{
            "pane topology split must have exactly two children"};
    }
}

bool canLayoutPanes(const PaneTopologyNode& node, Rect rect) {
    if (node.isLeaf()) return rect.width >= 2 && rect.height >= 1;
    requireSplitNode(node);
    if (node.axis == SplitAxis::Vertical) {
        const int firstWidth = rect.width / 2;
        return canLayoutPanes(
                   node.children[0],
                   {rect.x, rect.y, firstWidth, rect.height}) &&
               canLayoutPanes(
                   node.children[1],
                   {rect.x + firstWidth, rect.y,
                    rect.width - firstWidth, rect.height});
    }
    const int firstHeight = rect.height / 2;
    return canLayoutPanes(
               node.children[0],
               {rect.x, rect.y, rect.width, firstHeight}) &&
           canLayoutPanes(
               node.children[1],
               {rect.x, rect.y + firstHeight, rect.width,
                rect.height - firstHeight});
}

void projectPaneCells(const PaneTopologyNode& node, Rect rect,
                      std::vector<PaneCellFrame>& output) {
    if (node.isLeaf()) {
        output.push_back({node.id, rect});
        return;
    }
    requireSplitNode(node);
    if (node.axis == SplitAxis::Vertical) {
        const int firstWidth = rect.width / 2;
        projectPaneCells(node.children[0],
                         {rect.x, rect.y, firstWidth, rect.height}, output);
        projectPaneCells(node.children[1],
                         {rect.x + firstWidth, rect.y,
                          rect.width - firstWidth, rect.height},
                         output);
        return;
    }
    const int firstHeight = rect.height / 2;
    projectPaneCells(node.children[0],
                     {rect.x, rect.y, rect.width, firstHeight}, output);
    projectPaneCells(node.children[1],
                     {rect.x, rect.y + firstHeight, rect.width,
                      rect.height - firstHeight},
                     output);
}

double centerX(const Rect& rect) { return rect.x + rect.width / 2.0; }
double centerY(const Rect& rect) { return rect.y + rect.height / 2.0; }

}  // namespace

const SolvedGridNode* SolvedGridTree::find(
    const UiNodeId& id) const noexcept {
    for (const auto& node : nodes) {
        if (node.id == id) return &node;
    }

    return nullptr;
}

SolvedNoticeSurface solveNoticeSurface(const NoticeView& notice, Rect rect) {
    SolvedNoticeSurface solved{rect, {}};
    int actionX = rect.right();
    for (auto it = notice.actions.rbegin(); it != notice.actions.rend(); ++it) {
        std::string text = "[" + it->label + "]";
        const auto width = static_cast<int>(
            GraphemeLayout{}.computeRun(text).totalCells);
        actionX -= width;
        if (actionX < rect.x) break;
        solved.actions.push_back(
            {it->id, std::move(text), {actionX, rect.y, width, 1}});
        --actionX;
    }
    return solved;
}

GridSize measureExternalModificationSurface(
    const ExternalModificationViewState& external) {
    constexpr int kMaxListRows = 4;
    if (external.files.empty()) return {};
    return {1, 1 + static_cast<int>(std::min(
                       external.files.size(),
                       static_cast<std::size_t>(kMaxListRows)))};
}

SolvedExternalModificationSurface solveExternalModificationSurface(
    const ExternalModificationViewState& external, Rect rect) {
    SolvedExternalModificationSurface solved{
        rect, {rect.x, rect.y, rect.width, rect.height > 0 ? 1 : 0}, {}};
    if (rect.height <= 1 || external.files.empty()) return solved;

    constexpr int kMaxListRows = 4;
    const std::size_t total = external.files.size();
    std::optional<std::size_t> selected;
    if (external.selected) {
        const auto found =
            std::find_if(external.files.begin(), external.files.end(),
                         [&](const ExternalDocumentView& file) {
                             return file.id == *external.selected;
                         });
        if (found != external.files.end()) {
            selected = static_cast<std::size_t>(
                std::distance(external.files.begin(), found));
        }
    }
    const int listRows = std::min(
        static_cast<int>(std::min(
            total, static_cast<std::size_t>(kMaxListRows))),
        rect.height - 1);
    const bool overflow =
        total > static_cast<std::size_t>(listRows) && listRows > 1;
    const int shown = overflow ? listRows - 1 : listRows;
    const std::size_t half = static_cast<std::size_t>(shown / 2);
    const std::size_t selectionIndex = selected.value_or(0);
    const std::size_t firstVisible = std::min(
        selectionIndex > half ? selectionIndex - half : 0,
        total - static_cast<std::size_t>(shown));
    int y = rect.y + 1;
    for (int index = 0; index < shown; ++index) {
        const auto& file =
            external.files[firstVisible + static_cast<std::size_t>(index)];
        SolvedExternalModificationRow row{
            file.id, file.statusLabel + " " + file.path.string(),
            {rect.x, y, rect.width, 1},
            selected &&
                firstVisible + static_cast<std::size_t>(index) == *selected,
            {}};
        int actionX = rect.right();
        for (auto action = file.actions.rbegin();
             action != file.actions.rend(); ++action) {
            std::string text = "[" + action->label + "]";
            const int width = static_cast<int>(
                GraphemeLayout{}.computeRun(text).totalCells);
            actionX -= width;
            if (actionX < rect.x) break;
            row.actions.push_back(
                {file.id, action->command, std::move(text),
                 {actionX, y, width, 1}});
            --actionX;
        }
        solved.rows.push_back(std::move(row));
        ++y;
    }
    if (overflow) {
        solved.rows.push_back(
            {std::nullopt,
             "+" +
                 std::to_string(total - static_cast<std::size_t>(shown)) +
                 " more",
             {rect.x, y, rect.width, 1}, false, {}});
    }
    return solved;
}

std::string gridTabTitle(const TabState& tab, const TabGlyphs& glyphs) {
    if (tab.kind == TabKind::LiveDiff) {
        return glyphs.liveDiffPrefix + tab.label;
    }
    if (tab.mode == DocumentMode::ReadOnly) {
        return tab.label + glyphs.readOnlySuffix;
    }
    return tab.label;
}

std::string gridTabDisplay(std::string_view title, bool dirty,
                           const TabGlyphs& glyphs) {
    std::string display = glyphs.leftEdge + std::string{title};
    if (dirty) display += glyphs.dirtySuffix;
    display += glyphs.rightEdge;
    return display;
}

SolvedTabBar solveTabBar(const TabViewState& tabs,
                         const TabGlyphs& glyphs, Rect rect) {
    SolvedTabBar solved{rect, {}, {}};
    if (rect.width <= 0 || rect.height <= 0) return solved;
    const auto display = [&](const TabState& tab) {
        return gridTabDisplay(gridTabTitle(tab, glyphs), tab.dirty,
                              glyphs);
    };
    const auto width = [&](const TabState& tab) {
        return std::max(
            1, static_cast<int>(
                   GraphemeLayout{}.computeRun(display(tab)).totalCells));
    };
    const int separatorWidth = static_cast<int>(
        GraphemeLayout{}.computeRun(glyphs.separator).totalCells);
    std::size_t active = 0;
    if (tabs.active) {
        const auto found =
            std::find_if(tabs.tabs.begin(), tabs.tabs.end(),
                         [&](const TabState& tab) {
                             return tab.id == *tabs.active;
                         });
        if (found != tabs.tabs.end()) {
            active = static_cast<std::size_t>(
                std::distance(tabs.tabs.begin(), found));
        }
    }
    std::size_t first = 0;
    int used = 0;
    for (std::size_t index = 0;
         index < tabs.tabs.size() && index <= active; ++index) {
        used += width(tabs.tabs[index]);
    }
    used += static_cast<int>(active) * separatorWidth;
    while (first < active && used > rect.width) {
        used -= width(tabs.tabs[first]) + separatorWidth;
        ++first;
    }

    int x = rect.x;
    bool placedAny = false;
    for (std::size_t index = first; index < tabs.tabs.size(); ++index) {
        const int separatorReserve =
            placedAny && separatorWidth > 0
                ? std::min(rect.right() - x, separatorWidth)
                : 0;
        const int chipX = x + separatorReserve;
        std::string text = display(tabs.tabs[index]);
        const int chipWidth = std::min(
            rect.right() - chipX,
            std::max(1, static_cast<int>(
                            GraphemeLayout{}.computeRun(text).totalCells)));
        if (chipWidth <= 0) break;
        if (separatorReserve > 0) {
            solved.separators.push_back(
                {glyphs.separator,
                 {x, rect.y, separatorReserve, rect.height}});
        }
        solved.tabs.push_back(
            {index, tabs.tabs[index].id, std::move(text),
             {chipX, rect.y, chipWidth, rect.height},
             tabs.active && tabs.tabs[index].id == *tabs.active});
        x = chipX + chipWidth;
        placedAny = true;
    }
    return solved;
}

SolvedPaletteSurface solvePaletteSurface(const PaletteReport& palette,
                                         Rect rect,
                                         int scrollbarWidth) {
    const int gutter = std::clamp(scrollbarWidth, 0, rect.width);
    SolvedPaletteSurface solved{
        rect,
        {rect.x, rect.y, rect.width - gutter, rect.height},
        {rect.right() - gutter, rect.y, gutter, rect.height},
        {}};
    const auto visible = std::min(
        palette.rows.size(),
        static_cast<std::size_t>(std::max(rect.height, 0)));
    solved.visibleRows.reserve(visible);
    for (std::size_t index = 0; index < visible; ++index) {
        const auto absolute = palette.firstVisible +
                              static_cast<std::uint32_t>(index);
        solved.visibleRows.push_back(
            {index, absolute,
             {solved.rows.x, solved.rows.y + static_cast<int>(index),
              solved.rows.width, 1},
             palette.selected && *palette.selected == absolute});
    }
    return solved;
}

SolvedPanelSurface solvePanelSurface(const TreeViewState& tree,
                                     const SolvedGridNode& panel,
                                     std::uint32_t firstVisible,
                                     bool revealSelection,
                                     const Style& style) {
    SolvedPanelSurface solved{
        panel.rect,
        {panel.rect.x, panel.rect.y, panel.rect.width,
         panel.rect.height > 0 ? 1 : 0},
        {},
        std::nullopt,
        0,
        {},
        {}};
    const auto* provider = activeTreeProvider(tree);
    if (provider == nullptr || panel.rect.width <= 0 || panel.rect.height <= 0) {
        return solved;
    }

    solved.providerText = std::string{treeProviderLabel(provider->kind)};
    const auto contentRows =
        static_cast<std::uint32_t>(std::max(panel.rect.height - 1, 0));
    const int gutterWidth =
        panel.scroll == ScrollAxis::Vertical
            ? std::clamp(style.dimensions.scrollbarGutterWidth, 0,
                         panel.rect.width)
            : 0;
    if (contentRows > 0 && gutterWidth > 0 &&
        panel.rect.width > gutterWidth) {
        solved.scrollbarGutter =
            Rect{panel.rect.right() - gutterWidth, panel.rect.y + 1,
                 gutterWidth, static_cast<int>(contentRows)};
    }
    const int contentRight = solved.scrollbarGutter
                                 ? solved.scrollbarGutter->x
                                 : panel.rect.right();
    std::optional<std::uint32_t> selectedIndex;
    if (provider->selected) {
        const auto selected = std::ranges::find(
            provider->nodes, *provider->selected,
            [](const TreeNodeView& row) { return row.node.id; });
        if (selected != provider->nodes.end()) {
            selectedIndex = static_cast<std::uint32_t>(
                std::distance(provider->nodes.begin(), selected));
        }
    }
    const auto window = listScrollView(
        static_cast<std::uint32_t>(provider->nodes.size()), contentRows,
        firstVisible, selectedIndex, revealSelection);
    solved.firstVisible = window.firstVisible;
    solved.scrollbar = window.scrollbar;
    solved.rows.reserve(window.visibleCount);
    for (std::uint32_t row = 0; row < window.visibleCount; ++row) {
        const auto absolute = window.firstVisible + row;
        const auto& view = provider->nodes[absolute];
        std::string text(view.depth * style.tree.indentPerDepth, ' ');
        if (view.node.expandable) {
            text += view.expanded ? style.tree.expanded : style.tree.collapsed;
        }
        text += view.node.label;
        solved.rows.push_back(
            {absolute,
             view.node.id,
             std::move(text),
             {panel.rect.x, panel.rect.y + 1 + static_cast<int>(row),
              contentRight - panel.rect.x, 1},
             provider->selected && view.node.id == *provider->selected,
             view.node.kind == TreeNodeKind::Directory});
    }
    return solved;
}

SolvedDocumentSurface solveDocumentSurface(
    const SolvedGridNode& viewport, const PaneTopology& topology,
    bool lineNumbers,
    std::uint32_t logicalLineCount, const StyleDimensions& dimensions) {
    const int scrollbarWidth =
        viewport.scroll == ScrollAxis::Vertical
            ? std::clamp(dimensions.scrollbarGutterWidth, 0,
                         viewport.rect.width)
            : 0;
    const int requestedLineNumberWidth =
        lineNumbers
            ? static_cast<int>(
                  std::to_string(std::max(logicalLineCount, 1u)).size()) +
                  1
            : 0;
    SolvedDocumentSurface solved;
    solved.rect = viewport.rect;
    std::vector<PaneCellFrame> frames;
    if (canLayoutPanes(topology.root(), viewport.rect)) {
        projectPaneCells(topology.root(), viewport.rect, frames);
    } else {
        frames.push_back({topology.activePane(), viewport.rect});
    }
    solved.panes.reserve(frames.size());
    for (const auto& pane : frames) {
        int lineNumberWidth = requestedLineNumberWidth;
        if (lineNumberWidth > 0 &&
            pane.rect.width - scrollbarWidth - lineNumberWidth <
                dimensions.editorMinimumWidth) {
            lineNumberWidth = 0;
        }
        solved.panes.push_back({
            pane.id,
            pane.rect,
            {pane.rect.x + lineNumberWidth, pane.rect.y,
             pane.rect.width - scrollbarWidth - lineNumberWidth,
             pane.rect.height},
            {pane.rect.right() - scrollbarWidth, pane.rect.y,
             scrollbarWidth, pane.rect.height},
            lineNumberWidth > 0
                ? Rect{pane.rect.x, pane.rect.y, lineNumberWidth,
                       pane.rect.height}
                : Rect{},
        });
    }
    const auto active = std::ranges::find(
        solved.panes, topology.activePane(), &SolvedDocumentPane::id);
    solved.activePaneIndex =
        active == solved.panes.end()
            ? 0
            : static_cast<std::size_t>(
                  std::distance(solved.panes.begin(), active));
    solved.content = solved.panes.front().content;
    solved.scrollbarGutter = solved.panes.front().scrollbarGutter;
    solved.lineNumbers = solved.panes.front().lineNumbers;
    return solved;
}

std::optional<PaneId> paneInDirection(
    const SolvedDocumentSurface& surface, PaneDirection direction) noexcept {
    if (surface.activePaneIndex >= surface.panes.size()) return std::nullopt;
    const auto& current = surface.panes[surface.activePaneIndex];
    const double currentX = centerX(current.frame);
    const double currentY = centerY(current.frame);
    const SolvedDocumentPane* best = nullptr;
    double bestDistance = std::numeric_limits<double>::max();
    for (const auto& candidate : surface.panes) {
        if (candidate.id == current.id) continue;
        const double dx = centerX(candidate.frame) - currentX;
        const double dy = centerY(candidate.frame) - currentY;
        const bool eligible =
            (direction == PaneDirection::Left && dx < 0) ||
            (direction == PaneDirection::Right && dx > 0) ||
            (direction == PaneDirection::Up && dy < 0) ||
            (direction == PaneDirection::Down && dy > 0);
        if (!eligible) continue;
        const double distance = dx * dx + dy * dy;
        if (distance < bestDistance) {
            best = &candidate;
            bestDistance = distance;
        }
    }
    return best == nullptr ? std::nullopt
                           : std::optional<PaneId>{best->id};
}

namespace {

void solveNode(const LayoutNode& node, Rect frame,
               std::vector<SolvedGridNode>& out,
               std::set<UiNodeId>& identities, bool& ok) {
    if (node.id.empty() || !identities.insert(node.id).second) {
        throw std::invalid_argument(
            "solveGridTree: node identities must be nonempty and unique");
    }
    const auto contentWidth =
        static_cast<std::int64_t>(frame.width) - node.inset.left() -
        node.inset.right();
    const auto contentHeight =
        static_cast<std::int64_t>(frame.height) - node.inset.top() -
        node.inset.bottom();
    if (frame.width < 0 || frame.height < 0 || contentWidth < 0 ||
        contentHeight < 0) {
        ok = false;
        return;
    }
    const Rect content{frame.x + node.inset.left(),
                      frame.y + node.inset.top(),
                      static_cast<int>(contentWidth),
                      static_cast<int>(contentHeight)};
    out.push_back({node.id, frame, content, node.scroll, {}, {}, {}});
    if (node.children.empty()) return;

    const bool row = node.axis == Axis::Row;
    const int extent = row ? content.width : content.height;

    std::vector<bool> active(node.children.size(), true);
    const auto floor = [](const Size& size) -> std::int64_t {
        switch (size.kind()) {
        case SizeKind::Exact:
            return size.extent();
        case SizeKind::Flex:
            return 0;
        case SizeKind::Responsive:
            return size.minimum();
        case SizeKind::Auto:
            break;
        }
        throw std::invalid_argument(
            "solveGridTree: Auto size is not supported by the grid solver");
    };
    for (const auto& child : node.children) {
        // The grid box solver distributes Exact and Flex space; it does not do
        // intrinsic (Auto) content sizing. Auto reaching here is a misuse (a
        // content-aware projection such as projectUiRegion resolves Auto), so
        // fail with a distinct error rather than the nullopt that means "no fit".
        if (child.size.kind() == SizeKind::Auto) {
            throw std::invalid_argument(
                "solveGridTree: Auto size is not supported by the grid solver");
        }
    }

    const auto required = [&] {
        std::int64_t total = 0;
        std::size_t count = 0;
        for (std::size_t index = 0; index < node.children.size(); ++index) {
            if (!active[index]) continue;
            total += floor(node.children[index].size);
            ++count;
        }
        if (count > 1) {
            total += static_cast<std::int64_t>(node.gap.extent()) *
                     static_cast<std::int64_t>(count - 1);
        }
        return total;
    };
    while (required() > extent) {
        auto drop = node.children.size();
        for (std::size_t index = node.children.size(); index-- > 0;) {
            if (active[index] &&
                node.children[index].size.kind() == SizeKind::Responsive &&
                node.children[index].size.optional()) {
                drop = index;
                break;
            }
        }
        if (drop == node.children.size()) {
            ok = false;
            return;
        }
        active[drop] = false;
    }

    std::size_t activeCount = 0;
    std::int64_t floorTotal = 0;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        if (!active[index]) continue;
        ++activeCount;
        floorTotal += floor(node.children[index].size);
    }
    const auto gapTotal =
        activeCount > 1
            ? static_cast<std::int64_t>(node.gap.extent()) *
                  static_cast<std::int64_t>(activeCount - 1)
            : 0;
    std::vector<int> allocations(node.children.size(), 0);
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        if (active[index]) {
            allocations[index] =
                static_cast<int>(floor(node.children[index].size));
        }
    }
    std::int64_t remainder = extent - gapTotal - floorTotal;

    std::int64_t preferredTotal = 0;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        if (!active[index]) continue;
        const auto& size = node.children[index].size;
        if (size.kind() == SizeKind::Responsive && size.optional()) {
            preferredTotal += size.extent() - size.minimum();
        }
    }
    const auto preferredAllocation = std::min(remainder, preferredTotal);
    std::int64_t preferredDistributed = 0;
    if (preferredTotal > 0) {
        for (std::size_t index = 0; index < node.children.size(); ++index) {
            if (!active[index]) continue;
            const auto& size = node.children[index].size;
            if (size.kind() != SizeKind::Responsive || !size.optional()) {
                continue;
            }
            const auto range = size.extent() - size.minimum();
            const auto share = preferredAllocation * range / preferredTotal;
            allocations[index] += static_cast<int>(share);
            preferredDistributed += share;
        }
        auto extra = preferredAllocation - preferredDistributed;
        for (std::size_t index = node.children.size();
             index-- > 0 && extra > 0;) {
            if (!active[index]) continue;
            const auto& size = node.children[index].size;
            if (size.kind() != SizeKind::Responsive || !size.optional()) {
                continue;
            }
            const auto capacity = size.extent() - allocations[index];
            const auto add = std::min<std::int64_t>(capacity, extra);
            allocations[index] += static_cast<int>(add);
            extra -= add;
        }
    }
    remainder -= preferredAllocation;

    int growthTotal = 0;
    std::size_t finalGrowing = node.children.size();
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        if (!active[index]) continue;
        const auto& size = node.children[index].size;
        const int growth =
            size.kind() == SizeKind::Flex
                ? 1
                : (size.kind() == SizeKind::Responsive ? size.growth() : 0);
        growthTotal += growth;
        if (growth > 0) finalGrowing = index;
    }
    std::int64_t growthDistributed = 0;
    if (growthTotal > 0) {
        for (std::size_t index = 0; index < node.children.size(); ++index) {
            if (!active[index]) continue;
            const auto& size = node.children[index].size;
            const int growth =
                size.kind() == SizeKind::Flex
                    ? 1
                    : (size.kind() == SizeKind::Responsive ? size.growth() : 0);
            if (growth == 0) continue;
            const auto share = remainder * growth / growthTotal;
            allocations[index] += static_cast<int>(share);
            growthDistributed += share;
        }
        allocations[finalGrowing] +=
            static_cast<int>(remainder - growthDistributed);
    }

    int cursor = row ? content.x : content.y;
    std::size_t placed = 0;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        if (!active[index]) continue;
        const auto& child = node.children[index];
        const int mainSize = allocations[index];
        const Rect childFrame =
            row ? Rect{cursor, content.y, mainSize, content.height}
                : Rect{content.x, cursor, content.width, mainSize};
        solveNode(child, childFrame, out, identities, ok);
        if (!ok) return;
        cursor += mainSize;
        ++placed;
        if (placed < activeCount) {
            cursor += node.gap.extent();
        }
    }
}

}  // namespace

std::optional<SolvedGridTree> solveGridTree(const LayoutNode& root,
                                            Rect bounds) {
    SolvedGridTree layout;
    std::set<UiNodeId> identities;
    bool ok = true;
    solveNode(root, bounds, layout.nodes, identities, ok);
    if (!ok) return std::nullopt;
    return layout;
}

namespace {

struct UiNodeMetadata {
    ResolvedUiNodeStyle style;
    std::optional<WidgetDescriptor> widget;
    std::optional<UiLeafState> leafState;
};

struct LoweredUiNode {
    LayoutNode layout;
    GridSize natural;
};

std::int64_t naturalMain(const LoweredUiNode& node, Axis axis) {
    if (node.layout.size.kind() == SizeKind::Exact) {
        return node.layout.size.extent();
    }
    if (node.layout.size.kind() == SizeKind::Flex) return 0;
    return axis == Axis::Row ? node.natural.columns : node.natural.rows;
}

std::int64_t naturalCross(const LoweredUiNode& node, Axis axis) {
    return axis == Axis::Row ? node.natural.rows : node.natural.columns;
}

std::optional<LoweredUiNode> lowerUiNode(
    const UiNode& node, Axis parentAxis, bool ancestorsVisible,
    const std::map<UiNodeId, GridSize>& intrinsic,
    ResolvedUiNodeStyle inherited,
    std::map<UiNodeId, UiNodeMetadata>& metadata, std::string& error) {
    const bool visible = ancestorsVisible && node.visible;
    if (!visible) return std::nullopt;

    inherited = node.style.resolve(inherited);

    LayoutNode layout{node.id, node.size};
    GridSize natural{};
    UiNodeMetadata nodeMetadata;
    nodeMetadata.style = inherited;
    if (const auto* leaf = std::get_if<UiLeaf>(&node.content)) {
        nodeMetadata.widget = leaf->widget;
        nodeMetadata.leafState = node.resolved;
        if (node.size.kind() == SizeKind::Auto) {
            const auto measured = intrinsic.find(node.id);
            if (measured == intrinsic.end() || measured->second.columns < 0 ||
                measured->second.rows < 0) {
                error = "missing intrinsic grid size for Auto node \"" +
                        node.id.value() + "\"";
                return std::nullopt;
            }
            natural = measured->second;
        }
    } else {
        const auto& container = std::get<UiContainer>(node.content);
        layout.axis = container.axis;
        layout.inset = container.inset;
        layout.gap = container.gap;
        layout.scroll = container.scroll;

        std::int64_t main = 0;
        std::int64_t cross = 0;
        for (const auto& child : container.children) {
            auto lowered = lowerUiNode(
                child, container.axis, visible, intrinsic,
                inherited, metadata, error);
            if (!error.empty()) return std::nullopt;
            if (!lowered) continue;
            main += naturalMain(*lowered, container.axis);
            cross = std::max(cross, naturalCross(*lowered, container.axis));
            layout.children.push_back(std::move(lowered->layout));
        }
        if (layout.children.size() > 1) {
            main += static_cast<std::int64_t>(container.gap.extent()) *
                    static_cast<std::int64_t>(layout.children.size() - 1);
        }
        const auto width =
            container.axis == Axis::Row
                ? main + container.inset.left() + container.inset.right()
                : cross + container.inset.left() + container.inset.right();
        const auto height =
            container.axis == Axis::Row
                ? cross + container.inset.top() + container.inset.bottom()
                : main + container.inset.top() + container.inset.bottom();
        if (width > std::numeric_limits<int>::max() ||
            height > std::numeric_limits<int>::max()) {
            error = "intrinsic grid size exceeds the geometry range at node \"" +
                    node.id.value() + "\"";
            return std::nullopt;
        }
        natural = {static_cast<int>(width), static_cast<int>(height)};
    }

    if (node.size.kind() == SizeKind::Auto) {
        layout.size = Size::exact(parentAxis == Axis::Row
                                      ? natural.columns
                                      : natural.rows);
    }
    metadata.emplace(node.id, std::move(nodeMetadata));
    return LoweredUiNode{std::move(layout), natural};
}

}  // namespace

SolveUiFrameResult solveUiFrame(
    const UiSchema& schema,
    const std::vector<GridIntrinsicSize>& intrinsicSizes, Rect bounds) {
    std::map<UiNodeId, GridSize> intrinsic;
    for (const auto& size : intrinsicSizes) {
        if (!intrinsic.emplace(size.id, size.size).second) {
            return {std::nullopt,
                    "intrinsic sizes contain duplicate node identities"};
        }
    }

    std::map<UiNodeId, UiNodeMetadata> metadata;
    std::string error;
    auto lowered = lowerUiNode(schema.root, Axis::Column, true,
                               intrinsic, {}, metadata, error);
    if (!error.empty()) return {std::nullopt, std::move(error)};
    if (!lowered) {
        return {std::nullopt, "UI root is absent"};
    }

    auto solved = solveGridTree(lowered->layout, bounds);
    if (!solved) return {std::nullopt, "UI frame does not fit grid bounds"};
    for (auto& node : solved->nodes) {
        const auto found = metadata.find(node.id);
        if (found == metadata.end()) {
            return {std::nullopt, "solved node has no UI metadata"};
        }
        node.style = found->second.style;
        node.widget = found->second.widget;
        node.leafState = found->second.leafState;
    }
    return {std::move(solved), {}};
}

}  // namespace ssg
