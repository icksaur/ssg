#include "ssg/Layout.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace ssg {

const SolvedGridNode* SolvedGridTree::find(
    const UiNodeId& id) const noexcept {
    for (const auto& node : nodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
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

    std::int64_t exactTotal = 0;
    int flexCount = 0;
    for (const auto& child : node.children) {
        // The grid box solver distributes Exact and Flex space; it does not do
        // intrinsic (Auto) content sizing. Auto reaching here is a misuse (a
        // content-aware lowering such as lowerUiChromeRegion resolves Auto), so
        // fail with a distinct error rather than the nullopt that means "no fit".
        if (child.size.kind() == SizeKind::Auto) {
            throw std::invalid_argument(
                "solveGridTree: Auto size is not supported by the grid solver");
        }
        if (child.size.kind() == SizeKind::Exact) {
            exactTotal += child.size.extent();
        } else {
            ++flexCount;
        }
    }
    const auto gapTotal =
        node.children.size() > 1
            ? static_cast<std::int64_t>(node.gap.extent()) *
                  static_cast<std::int64_t>(node.children.size() - 1)
            : 0;
    if (exactTotal + gapTotal > extent) {
        ok = false;
        return;
    }

    // Equal split of the remainder among the flex children; cells that do not
    // divide evenly go to the LAST flex child (matches the old pane rule
    // `rect.width - firstWidth`). A container may legally have no flex child, in
    // which case the remainder is simply unused.
    const int remainder =
        extent - static_cast<int>(exactTotal + gapTotal);
    const int flexBase = flexCount > 0 ? remainder / flexCount : 0;
    const int flexExtra = flexCount > 0 ? remainder % flexCount : 0;

    int cursor = row ? content.x : content.y;
    int flexSeen = 0;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        const auto& child = node.children[index];
        int mainSize = 0;
        if (child.size.kind() == SizeKind::Exact) {
            mainSize = child.size.extent();
        } else {
            ++flexSeen;
            mainSize = flexBase + (flexSeen == flexCount ? flexExtra : 0);
        }
        const Rect childFrame =
            row ? Rect{cursor, content.y, mainSize, content.height}
                : Rect{content.x, cursor, content.width, mainSize};
        solveNode(child, childFrame, out, identities, ok);
        if (!ok) return;
        cursor += mainSize;
        if (index + 1 < node.children.size()) {
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
    const UiNode& node, Axis parentAxis, bool ancestorsPresent,
    const std::map<UiNodeId, bool>& presence,
    const std::map<UiNodeId, UiLeafState>& state,
    const std::map<UiNodeId, GridSize>& intrinsic,
    ResolvedUiNodeStyle inherited,
    std::map<UiNodeId, UiNodeMetadata>& metadata, std::string& error) {
    const auto present = presence.find(node.id);
    if (!ancestorsPresent || present == presence.end() || !present->second) {
        return std::nullopt;
    }

    if (node.style.foreground) inherited.foreground = node.style.foreground;
    if (node.style.background) inherited.background = node.style.background;

    LayoutNode layout{node.id, node.size};
    GridSize natural{};
    UiNodeMetadata nodeMetadata;
    nodeMetadata.style = inherited;
    if (const auto* leaf = std::get_if<UiLeaf>(&node.content)) {
        nodeMetadata.widget = leaf->widget;
        if (const auto found = state.find(node.id); found != state.end()) {
            nodeMetadata.leafState = found->second;
        }
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
                child, container.axis, true, presence, state, intrinsic,
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
    const ValidatedSchema& schema, const UiStateSection& stateSection,
    const UiPresenceSection& presenceSection,
    const ClientUiProfile& profile,
    const std::vector<GridIntrinsicSize>& intrinsicSizes, Rect bounds) {
    if (stateSection.generation != schema.generation() ||
        presenceSection.generation != schema.generation() ||
        stateSection.nodes.size() != schema.nodeIds().size() ||
        presenceSection.nodes.size() != schema.nodeIds().size()) {
        return {std::nullopt,
                "UI schema, state, and presence do not correspond"};
    }

    std::map<UiNodeId, UiLeafState> state;
    std::set<UiNodeId> stateIds;
    for (const auto& node : stateSection.nodes) {
        if (!stateIds.insert(node.id).second) {
            return {std::nullopt, "UI state contains duplicate node identities"};
        }
        if (node.leaf) state.emplace(node.id, *node.leaf);
    }
    std::map<UiNodeId, bool> presence;
    for (const auto& record : presenceSection.nodes) {
        if (!presence.emplace(record.id, record.present).second) {
            return {std::nullopt,
                    "UI presence contains duplicate node identities"};
        }
    }
    if (stateIds != schema.nodeIds()) {
        return {std::nullopt, "UI state does not correspond to schema"};
    }
    std::set<UiNodeId> presenceIds;
    for (const auto& [id, present] : presence) {
        (void)present;
        presenceIds.insert(id);
    }
    if (presenceIds != schema.nodeIds()) {
        return {std::nullopt, "UI presence does not correspond to schema"};
    }

    std::map<UiNodeId, GridSize> intrinsic;
    for (const auto& size : intrinsicSizes) {
        if (!intrinsic.emplace(size.id, size.size).second) {
            return {std::nullopt,
                    "intrinsic sizes contain duplicate node identities"};
        }
    }

    std::string profileError;
    const auto inspectProfile = [&](const auto& self, const UiNode& node) -> void {
        if (!profileError.empty()) return;
        if (const auto* leaf = std::get_if<UiLeaf>(&node.content)) {
            if (!profile.supports(leaf->widget.kind)) {
                profileError = "grid profile does not support node \"" +
                               node.id.value() + "\" widget " +
                               std::string{widgetKindName(leaf->widget.kind)};
                return;
            }
            if (leaf->widget.surface &&
                !profile.supports(*leaf->widget.surface)) {
                profileError = "grid profile does not support node \"" +
                               node.id.value() + "\" surface " +
                               std::string{
                                   viewSurfaceName(*leaf->widget.surface)};
            }
            return;
        }
        for (const auto& child :
             std::get<UiContainer>(node.content).children) {
            self(self, child);
        }
    };
    inspectProfile(inspectProfile, schema.schema().root);
    if (!profileError.empty()) return {std::nullopt, std::move(profileError)};

    std::map<UiNodeId, UiNodeMetadata> metadata;
    std::string error;
    auto lowered = lowerUiNode(schema.schema().root, Axis::Column, true,
                               presence, state, intrinsic, {}, metadata, error);
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
