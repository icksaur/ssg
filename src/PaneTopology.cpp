#include <ssg/PaneTopology.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ssg {
namespace {

void collect(const PaneTopologyNode& node, std::vector<PaneId>& panes) {
    if (node.isLeaf()) {
        panes.push_back(node.id);
        return;
    }
    for (const auto& child : node.children) collect(child, panes);
}

PaneTopologyNode* find(PaneTopologyNode& node, PaneId id) {
    if (node.isLeaf()) return node.id == id ? &node : nullptr;
    for (auto& child : node.children) {
        if (auto* found = find(child, id)) return found;
    }
    return nullptr;
}

bool remove(PaneTopologyNode& node, PaneId id) {
    for (auto it = node.children.begin(); it != node.children.end(); ++it) {
        if (it->isLeaf() && it->id == id) {
            auto sibling = std::next(it);
            if (sibling == node.children.end()) sibling = std::prev(it);
            auto replacement = std::move(*sibling);
            node = std::move(replacement);
            return true;
        }
        if (remove(*it, id)) return true;
    }
    return false;
}

bool contains(const PaneTopologyNode& node, PaneId id) {
    if (node.isLeaf()) return node.id == id;
    return std::ranges::any_of(node.children, [id](const auto& child) {
        return contains(child, id);
    });
}

}  // namespace

PaneTopology::PaneTopology(PaneTopologyNode root, PaneId active,
                           std::uint32_t nextId) noexcept
    : root_(std::move(root)), active_(active), nextId_(nextId) {}

PaneTopology PaneTopology::initial() {
    return PaneTopology{PaneTopologyNode{PaneId{1}}, PaneId{1}, 2};
}

std::vector<PaneId> PaneTopology::panes() const {
    std::vector<PaneId> result;
    collect(root_, result);
    return result;
}

bool PaneTopology::contains(PaneId pane) const noexcept {
    return ::ssg::contains(root_, pane);
}

PaneId PaneTopology::splitActive(SplitAxis axis) {
    auto* leaf = find(root_, active_);
    if (leaf == nullptr) {
        throw std::logic_error{"active pane is not in topology"};
    }
    const PaneId original = leaf->id;
    const PaneId created{nextId_++};
    leaf->axis = axis;
    leaf->children = {PaneTopologyNode{original}, PaneTopologyNode{created}};
    active_ = created;
    return created;
}

bool PaneTopology::closeActive() {
    auto ids = panes();
    if (ids.size() == 1) return false;
    const auto active = std::ranges::find(ids, active_);
    const PaneId replacement =
        std::next(active) != ids.end() ? *std::next(active) : *std::prev(active);
    if (!remove(root_, active_)) return false;
    active_ = replacement;
    return true;
}

void PaneTopology::cycle(PaneCycleDirection direction) {
    auto ids = panes();
    const auto active = std::ranges::find(ids, active_);
    if (direction == PaneCycleDirection::Next) {
        active_ = std::next(active) == ids.end() ? ids.front() : *std::next(active);
    } else {
        active_ = active == ids.begin() ? ids.back() : *std::prev(active);
    }
}

bool PaneTopology::focus(PaneId pane) noexcept {
    if (!contains(pane)) return false;
    active_ = pane;
    return true;
}

}  // namespace ssg
