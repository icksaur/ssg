#include "ssg/ShellState.h"

#include <limits>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

namespace ssg {
namespace {

struct PaneNode {
    PaneId id;
    SplitAxis axis = SplitAxis::Vertical;
    std::unique_ptr<PaneNode> first;
    std::unique_ptr<PaneNode> second;

    [[nodiscard]] bool leaf() const noexcept { return !first; }
};

void collectIds(const PaneNode& node, std::vector<PaneId>& ids) {
    if (node.leaf()) {
        ids.push_back(node.id);
        return;
    }
    collectIds(*node.first, ids);
    collectIds(*node.second, ids);
}

PaneNode* findLeaf(PaneNode& node, PaneId id) {
    if (node.leaf()) return node.id == id ? &node : nullptr;
    if (auto* found = findLeaf(*node.first, id)) return found;
    return findLeaf(*node.second, id);
}

bool removeLeaf(std::unique_ptr<PaneNode>& node, PaneId id) {
    if (!node || node->leaf()) return false;
    if (node->first->leaf() && node->first->id == id) {
        node = std::move(node->second);
        return true;
    }
    if (node->second->leaf() && node->second->id == id) {
        node = std::move(node->first);
        return true;
    }
    return removeLeaf(node->first, id) || removeLeaf(node->second, id);
}

bool canLayout(const PaneNode& node, Rect rect) {
    if (node.leaf()) return rect.width >= 2 && rect.height >= 1;
    if (node.axis == SplitAxis::Vertical) {
        const int firstWidth = rect.width / 2;
        return canLayout(*node.first,
                         {rect.x, rect.y, firstWidth, rect.height}) &&
               canLayout(*node.second,
                         {rect.x + firstWidth, rect.y,
                          rect.width - firstWidth, rect.height});
    }
    const int firstHeight = rect.height / 2;
    return canLayout(*node.first,
                     {rect.x, rect.y, rect.width, firstHeight}) &&
           canLayout(*node.second,
                     {rect.x, rect.y + firstHeight, rect.width,
                      rect.height - firstHeight});
}

void layoutPaneFrames(const PaneNode& node, Rect rect,
                      std::vector<PaneFrame>& output) {
    if (node.leaf()) {
        output.push_back({node.id, rect});
        return;
    }
    if (node.axis == SplitAxis::Vertical) {
        const int firstWidth = rect.width / 2;
        layoutPaneFrames(*node.first,
                         {rect.x, rect.y, firstWidth, rect.height}, output);
        layoutPaneFrames(*node.second,
                         {rect.x + firstWidth, rect.y,
                          rect.width - firstWidth, rect.height},
                         output);
        return;
    }
    const int firstHeight = rect.height / 2;
    layoutPaneFrames(*node.first,
                     {rect.x, rect.y, rect.width, firstHeight}, output);
    layoutPaneFrames(*node.second,
                     {rect.x, rect.y + firstHeight, rect.width,
                      rect.height - firstHeight},
                     output);
}

const PaneFrame* paneFrame(const std::vector<PaneFrame>& panes, PaneId id) {
    const auto found = std::ranges::find(panes, id, &PaneFrame::id);
    return found == panes.end() ? nullptr : &*found;
}

double centerX(const Rect& rect) { return rect.x + rect.width / 2.0; }
double centerY(const Rect& rect) { return rect.y + rect.height / 2.0; }

}  // namespace

struct ShellState::Impl {
    std::unique_ptr<PaneNode> root =
        std::make_unique<PaneNode>(PaneNode{PaneId{1}});
    PaneId active{1};
    std::uint32_t nextId = 2;
};

ShellState::ShellState() : impl_(std::make_unique<Impl>()) {}
ShellState::~ShellState() = default;
ShellState::ShellState(ShellState&&) noexcept = default;
ShellState& ShellState::operator=(ShellState&&) noexcept = default;

PaneId ShellState::activePane() const noexcept { return impl_->active; }

std::size_t ShellState::paneCount() const noexcept {
    std::vector<PaneId> ids;
    collectIds(*impl_->root, ids);
    return ids.size();
}

std::vector<PaneFrame> ShellState::paneFrames(Rect rect) const {
    std::vector<PaneFrame> frames;
    if (canLayout(*impl_->root, rect)) {
        layoutPaneFrames(*impl_->root, rect, frames);
    } else {
        frames.push_back({impl_->active, rect});
    }
    return frames;
}

PaneId ShellState::splitActive(SplitAxis axis) {
    auto* leaf = findLeaf(*impl_->root, impl_->active);
    const PaneId original = leaf->id;
    const PaneId created{impl_->nextId++};
    leaf->axis = axis;
    leaf->first = std::make_unique<PaneNode>(PaneNode{original});
    leaf->second = std::make_unique<PaneNode>(PaneNode{created});
    impl_->active = created;
    return created;
}

bool ShellState::closeActivePane() {
    if (paneCount() == 1) return false;
    std::vector<PaneId> ids;
    collectIds(*impl_->root, ids);
    const auto active = std::ranges::find(ids, impl_->active);
    const PaneId replacement =
        active + 1 != ids.end() ? *(active + 1) : *(active - 1);
    const bool removed = removeLeaf(impl_->root, impl_->active);
    if (removed) impl_->active = replacement;
    return removed;
}

void ShellState::nextPane() noexcept {
    std::vector<PaneId> ids;
    collectIds(*impl_->root, ids);
    const auto active = std::ranges::find(ids, impl_->active);
    impl_->active = *(active + 1 == ids.end() ? ids.begin() : active + 1);
}

void ShellState::previousPane() noexcept {
    std::vector<PaneId> ids;
    collectIds(*impl_->root, ids);
    const auto active = std::ranges::find(ids, impl_->active);
    impl_->active = active == ids.begin() ? ids.back() : *(active - 1);
}

bool ShellState::focusPane(PaneDirection direction,
                           const std::vector<PaneFrame>& panes) noexcept {
    const auto* current = paneFrame(panes, impl_->active);
    if (!current) return false;
    const double currentX = centerX(current->rect);
    const double currentY = centerY(current->rect);
    const PaneFrame* best = nullptr;
    double bestDistance = std::numeric_limits<double>::max();
    for (const auto& candidate : panes) {
        if (candidate.id == impl_->active) continue;
        const double dx = centerX(candidate.rect) - currentX;
        const double dy = centerY(candidate.rect) - currentY;
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
    if (!best) return false;
    impl_->active = best->id;
    return true;
}

}  // namespace ssg
