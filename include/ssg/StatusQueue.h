#pragma once

#include <ssg/PromptSurface.h>
#include <ssg/UiTree.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

class StatusId {
public:
    explicit constexpr StatusId(std::uint64_t value = 0) noexcept
        : value_(value) {}
    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }
    constexpr auto operator<=>(const StatusId&) const = default;

private:
    std::uint64_t value_;
};

enum class StatusPriority : std::uint8_t {
    Error = 0,
    Warning = 1,
    Information = 2,
    Progress = 3,
};

struct UiAction {
    std::string id;
    std::string label;
    std::string commandId;
    friend bool operator==(const UiAction&, const UiAction&) = default;
};

struct StatusItem {
    StatusId id;
    StatusPriority priority = StatusPriority::Information;
    std::string text;
    std::vector<UiAction> actions;
    friend bool operator==(const StatusItem&, const StatusItem&) = default;
};

struct StatusItemView {
    StatusId id;
    StatusPriority priority = StatusPriority::Information;
    std::uint64_t generation = 0;
    std::string accessibleLabel;
    std::vector<UiAction> actions;
    friend bool operator==(const StatusItemView&,
                           const StatusItemView&) = default;
};

struct StatusViewState {
    std::vector<StatusItemView> items;
    std::size_t selected = 0;
    friend bool operator==(const StatusViewState&,
                           const StatusViewState&) = default;
};

struct StatusActionNode {
    UiNodeId id;
    std::string accessibleLabel;
    std::string commandId;
    friend bool operator==(const StatusActionNode&,
                           const StatusActionNode&) = default;
};

[[nodiscard]] std::vector<StatusActionNode> projectStatusActionNodes(
    const StatusViewState& status);

struct PromptStatusViewState {
    StatusViewState status;
    // The active prompt's kind is authoritative and independent of layout.
    // A header-hosted prompt (the palette / file finder, whose query renders in
    // the header input line) publishes its kind here with no footer view at all,
    // so a client detects "which prompt is open" from state, never from a
    // rendering artifact. nullopt when no prompt is active.
    std::optional<PromptKind> activeKind;
    friend bool operator==(const PromptStatusViewState&,
                           const PromptStatusViewState&) = default;
};

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

} // namespace ssg
