#include "ssg/Layout.h"

namespace ssg {

const SolvedBox* SolvedLayout::find(std::string_view id) const noexcept {
    for (const auto& box : boxes) {
        if (box.id == id) return &box;
    }
    return nullptr;
}

namespace {

// Places `node` at `frame`, emits its box, then lays its children within the
// content rect (frame minus inset). Sets `ok` to false and stops when a
// container's Exact children cannot fit. Recursive, depth-bounded by the tree.
void solveNode(const LayoutNode& node, Rect frame, std::vector<SolvedBox>& out,
               bool& ok) {
    out.push_back({node.id, node.kind, frame});
    if (node.children.empty()) return;

    const Rect content{
        frame.x + node.inset.left(),
        frame.y + node.inset.top(),
        frame.width - node.inset.left() - node.inset.right(),
        frame.height - node.inset.top() - node.inset.bottom(),
    };
    const bool row = node.axis == Axis::Row;
    const int extent = row ? content.width : content.height;

    int exactTotal = 0;
    int flexCount = 0;
    for (const auto& child : node.children) {
        if (child.size.kind() == SizeKind::Exact) {
            exactTotal += child.size.extent();
        } else {
            ++flexCount;
        }
    }
    if (exactTotal > extent) {
        ok = false;
        return;
    }

    // Equal split of the remainder among the flex children; cells that do not
    // divide evenly go to the LAST flex child (matches the old pane rule
    // `rect.width - firstWidth`). A container may legally have no flex child, in
    // which case the remainder is simply unused.
    const int remainder = extent - exactTotal;
    const int flexBase = flexCount > 0 ? remainder / flexCount : 0;
    const int flexExtra = flexCount > 0 ? remainder % flexCount : 0;

    int cursor = row ? content.x : content.y;
    int flexSeen = 0;
    for (const auto& child : node.children) {
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
        solveNode(child, childFrame, out, ok);
        if (!ok) return;
        cursor += mainSize;
    }
}

}  // namespace

std::optional<SolvedLayout> solveLayout(const LayoutNode& root, Rect bounds) {
    SolvedLayout layout;
    bool ok = true;
    solveNode(root, bounds, layout.boxes, ok);
    if (!ok) return std::nullopt;
    return layout;
}

}  // namespace ssg
