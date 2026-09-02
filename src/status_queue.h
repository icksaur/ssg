#pragma once

#include <ssg/StatusQueue.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ssg {

struct StatusEnqueueResult {
    bool accepted = false;
    std::uint64_t generation = 0;
    std::optional<StatusId> evicted;
};

struct StatusFooterProjection {
    std::string value;
    std::vector<UiAction> actions;
    friend bool operator==(const StatusFooterProjection&,
                           const StatusFooterProjection&) = default;
};

class StatusQueue {
public:
    static constexpr std::size_t kCapacity = 16;

    [[nodiscard]] StatusEnqueueResult enqueue(StatusItem item);
    void next() noexcept;
    void previous() noexcept;
    void dismiss() noexcept;
    [[nodiscard]] StatusViewState viewState() const;
    [[nodiscard]] StatusFooterProjection footerProjection() const;
    [[nodiscard]] std::vector<StatusActionNode> actionNodes() const;

private:
    struct Entry {
        StatusItem item;
        std::uint64_t generation = 0;
    };

    std::vector<Entry> entries_;
    std::size_t selected_ = 0;
    std::uint64_t nextGeneration_ = 1;
};

}  // namespace ssg
