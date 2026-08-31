#include "ssg/Layout.h"

#include <cstdint>
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
    out.push_back({node.id, frame, content, node.scroll});
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

}  // namespace ssg
