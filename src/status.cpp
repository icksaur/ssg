#include "ssg/status.h"

#include <algorithm>
#include <utility>

namespace ssg {
namespace {

bool validItem(const StatusItem& item) {
    if (item.id.value() == 0 || item.text.empty()) {
        return false;
    }
    return std::all_of(item.actions.begin(), item.actions.end(),
                       [](const StatusAction& action) {
                           return !action.id.empty() &&
                                  !action.accessible_label.empty() &&
                                  !action.command_id.empty();
                       });
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

    const std::uint64_t generation = next_generation_++;
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

StatusActionResult StatusQueue::invokeAction(
    const StatusActionInvocation& invocation) const {
    if (entries_.empty()) {
        return {StatusActionError::Stale, std::nullopt};
    }
    const auto& selected = entries_[selected_];
    if (selected.item.id != invocation.status_id ||
        selected.generation != invocation.generation) {
        return {StatusActionError::Stale, std::nullopt};
    }
    const auto action = std::find_if(
        selected.item.actions.begin(), selected.item.actions.end(),
        [&](const StatusAction& candidate) {
            return candidate.id == invocation.action_id;
        });
    if (action == selected.item.actions.end()) {
        return {StatusActionError::UnknownAction, std::nullopt};
    }
    return {StatusActionError::None, action->command_id};
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
        projection.actions.push_back({action.id, action.accessible_label});
    }
    return projection;
}

PromptStatusDelta derivePromptStatusDelta(
    const PromptStatusViewState& before, const PromptStatusViewState& after) {
    if (before == after) {
        return {};
    }
    return {true, after};
}

} // namespace ssg
