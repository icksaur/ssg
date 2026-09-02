#include "status_queue.h"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace ssg {
namespace {

bool validItem(const StatusItem& item) {
    if (item.id.value() == 0 || item.text.empty()) {
        return false;
    }
    std::set<std::string_view> actionIds;
    return std::all_of(
        item.actions.begin(), item.actions.end(),
        [&](const UiAction& action) {
            return !action.id.empty() && !action.label.empty() &&
                   !action.commandId.empty() &&
                   actionIds.insert(action.id).second;
        });
}

std::string actionNodeId(StatusId statusId, std::uint64_t generation,
                         std::string_view actionId) {
    constexpr std::array<char, 16> digits{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string id = "footer.status_action/" +
                     std::to_string(statusId.value()) + "/" +
                     std::to_string(generation) + "/";
    id.reserve(id.size() + actionId.size() * 2);
    for (const unsigned char byte : actionId) {
        id.push_back(digits[byte >> 4]);
        id.push_back(digits[byte & 0x0f]);
    }
    return id;
}

} // namespace

StatusEnqueueResult StatusQueue::enqueue(StatusItem item) {
    if (!validItem(item)) {
        return {};
    }

    std::optional<StatusId> evicted;
    const auto sameId = std::find_if(
        entries_.begin(), entries_.end(), [&](const Entry& entry) {
            return entry.item.id == item.id;
        });
    if (sameId != entries_.end()) {
        entries_.erase(sameId);
    } else if (entries_.size() == kCapacity) {
        const auto worst = std::max_element(
            entries_.begin(), entries_.end(),
            [](const Entry& left, const Entry& right) {
                if (left.item.priority != right.item.priority) {
                    return left.item.priority < right.item.priority;
                }
                return left.generation > right.generation;
            });
        if (item.priority >= worst->item.priority) {
            return {};
        }
        evicted = worst->item.id;
        entries_.erase(worst);
    }

    const std::uint64_t generation = nextGeneration_++;
    entries_.push_back({std::move(item), generation});
    std::sort(entries_.begin(), entries_.end(),
              [](const Entry& left, const Entry& right) {
                  if (left.item.priority != right.item.priority) {
                      return left.item.priority < right.item.priority;
                  }
                  return left.generation < right.generation;
              });
    selected_ = 0;
    return {true, generation, evicted};
}

void StatusQueue::next() noexcept {
    if (!entries_.empty()) {
        selected_ = (selected_ + 1) % entries_.size();
    }
}

void StatusQueue::previous() noexcept {
    if (!entries_.empty()) {
        selected_ =
            selected_ == 0 ? entries_.size() - 1 : selected_ - 1;
    }
}

void StatusQueue::dismiss() noexcept {
    if (entries_.empty()) {
        return;
    }
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(selected_));
    if (entries_.empty()) {
        selected_ = 0;
    } else if (selected_ >= entries_.size()) {
        selected_ = entries_.size() - 1;
    }
}

StatusViewState StatusQueue::viewState() const {
    StatusViewState view;
    view.selected = selected_;
    view.items.reserve(entries_.size());
    for (const auto& entry : entries_) {
        view.items.push_back(
            {entry.item.id, entry.item.priority, entry.generation,
             entry.item.text, entry.item.actions});
    }
    return view;
}

StatusFooterProjection StatusQueue::footerProjection() const {
    StatusFooterProjection projection;
    if (entries_.empty()) {
        return projection;
    }

    const auto& selected = entries_[selected_];
    projection.value =
        selected.item.text + " " + std::to_string(selected_ + 1) + "/" +
        std::to_string(entries_.size());
    projection.actions.reserve(selected.item.actions.size());
    for (const auto& action : selected.item.actions) {
        projection.actions.push_back(action);
    }
    return projection;
}

std::vector<StatusActionNode> StatusQueue::actionNodes() const {
    return projectStatusActionNodes(viewState());
}

std::vector<StatusActionNode> projectStatusActionNodes(
    const StatusViewState& status) {
    std::vector<StatusActionNode> nodes;
    if (status.items.empty() || status.selected >= status.items.size()) {
        return nodes;
    }
    const auto& selected = status.items[status.selected];
    nodes.reserve(selected.actions.size());
    for (const auto& action : selected.actions) {
        nodes.push_back(
            {UiNodeId{actionNodeId(selected.id, selected.generation,
                                   action.id)},
             action.label, action.commandId});
    }
    return nodes;
}

PromptStatusDelta PromptStatusDeltaCodec::derive(
    const PromptStatusViewState& before, const PromptStatusViewState& after) {
    if (before == after) {
        return {};
    }
    return {true, after};
}

} // namespace ssg
