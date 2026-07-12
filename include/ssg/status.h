#pragma once

#include "ssg/prompt.h"

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
    error,
    warning,
    information,
    progress,
};

struct StatusAction {
    std::string id;
    std::string accessible_label;
    std::string command_id;
    friend bool operator==(const StatusAction&, const StatusAction&) = default;
};

struct StatusItem {
    StatusId id;
    StatusPriority priority = StatusPriority::information;
    std::string text;
    std::vector<StatusAction> actions;
    friend bool operator==(const StatusItem&, const StatusItem&) = default;
};

struct StatusItemView {
    StatusId id;
    StatusPriority priority = StatusPriority::information;
    std::uint64_t generation = 0;
    std::string accessible_label;
    std::vector<StatusAction> actions;
    friend bool operator==(const StatusItemView&,
                           const StatusItemView&) = default;
};

struct StatusViewState {
    std::vector<StatusItemView> items;
    std::size_t selected = 0;
    friend bool operator==(const StatusViewState&,
                           const StatusViewState&) = default;
};

struct StatusEnqueueResult {
    bool accepted = false;
    std::uint64_t generation = 0;
    std::optional<StatusId> evicted;
};

struct StatusActionInvocation {
    StatusId status_id;
    std::string action_id;
    std::uint64_t generation = 0;
};

enum class StatusActionError : std::uint8_t {
    none,
    stale,
    unknown_action,
};

struct StatusActionResult {
    StatusActionError error = StatusActionError::none;
    std::optional<std::string> command_id;
    [[nodiscard]] bool accepted() const noexcept {
        return error == StatusActionError::none && command_id.has_value();
    }
};

struct StatusFooterProjection {
    std::string value;
    std::vector<ShellLabel> actions;
    friend bool operator==(const StatusFooterProjection&,
                           const StatusFooterProjection&) = default;
};

class StatusQueue {
public:
    static constexpr std::size_t capacity = 16;

    [[nodiscard]] StatusEnqueueResult enqueue(StatusItem item);
    void next() noexcept;
    void previous() noexcept;
    void dismiss() noexcept;
    [[nodiscard]] StatusActionResult invoke_action(
        const StatusActionInvocation& invocation) const;
    [[nodiscard]] StatusViewState view_state() const;
    [[nodiscard]] StatusFooterProjection footer_projection() const;

private:
    struct Entry {
        StatusItem item;
        std::uint64_t generation = 0;
    };

    std::vector<Entry> entries_;
    std::size_t selected_ = 0;
    std::uint64_t next_generation_ = 1;
};

struct PromptStatusViewState {
    std::optional<PromptViewState> prompt;
    StatusViewState status;
    friend bool operator==(const PromptStatusViewState&,
                           const PromptStatusViewState&) = default;
};

struct PromptStatusDelta {
    bool changed = false;
    std::optional<PromptStatusViewState> replacement;
    friend bool operator==(const PromptStatusDelta&,
                           const PromptStatusDelta&) = default;
};

[[nodiscard]] PromptStatusDelta derive_prompt_status_delta(
    const PromptStatusViewState& before, const PromptStatusViewState& after);

} // namespace ssg
