#pragma once

#include "ssg/PromptSurface.h"
#include "ssg/StatusActionInvocation.h"
#include "ssg/UiTree.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

enum class StatusPriority : std::uint8_t {
    Error,
    Warning,
    Information,
    Progress,
};

struct StatusAction {
    std::string id;
    std::string accessibleLabel;
    std::string commandId;
    friend bool operator==(const StatusAction&, const StatusAction&) = default;
};

struct StatusItem {
    StatusId id;
    StatusPriority priority = StatusPriority::Information;
    std::string text;
    std::vector<StatusAction> actions;
    friend bool operator==(const StatusItem&, const StatusItem&) = default;
};

struct StatusItemView {
    StatusId id;
    StatusPriority priority = StatusPriority::Information;
    std::uint64_t generation = 0;
    std::string accessibleLabel;
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

enum class StatusActionError : std::uint8_t {
    None,
    Stale,
    UnknownAction,
};

struct StatusActionResult {
    StatusActionError error = StatusActionError::None;
    std::optional<std::string> commandId;
    [[nodiscard]] bool accepted() const noexcept {
        return error == StatusActionError::None && commandId.has_value();
    }
};

struct StatusFooterProjection {
    std::string value;
    std::vector<StatusAction> actions;
    friend bool operator==(const StatusFooterProjection&,
                           const StatusFooterProjection&) = default;
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

class StatusQueue {
public:
    static constexpr std::size_t kCapacity = 16;

    [[nodiscard]] StatusEnqueueResult enqueue(StatusItem item);
    void next() noexcept;
    void previous() noexcept;
    void dismiss() noexcept;
    [[nodiscard]] StatusActionResult invokeAction(
        const StatusActionInvocation& invocation) const;
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

struct PromptStatusViewState {
    StatusViewState status;
    // The active prompt's KIND, authoritative and independent of any footer
    // layout view (which is grid projection and now lives in PresentationSnapshot).
    // A header-hosted prompt (the palette / file finder, whose query renders in
    // the header input line) publishes its kind here with no footer view at all,
    // so a client detects "which prompt is open" from state, never from a
    // rendering artifact. nullopt when no prompt is active.
    std::optional<PromptKind> activeKind;
    friend bool operator==(const PromptStatusViewState&,
                           const PromptStatusViewState&) = default;
};

struct PromptStatusDelta {
    bool changed = false;
    std::optional<PromptStatusViewState> replacement;
    friend bool operator==(const PromptStatusDelta&,
                           const PromptStatusDelta&) = default;
};

class PromptStatusDeltaCodec {
public:
    [[nodiscard]] PromptStatusDelta derive(const PromptStatusViewState& before,
                                           const PromptStatusViewState& after);
};

} // namespace ssg
