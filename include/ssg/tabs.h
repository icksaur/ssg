#pragma once

#include "ssg/recovery.h"
#include "ssg/workspace.h"

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

class TabId {
public:
    explicit constexpr TabId(std::uint64_t value = 0) noexcept : value_{value} {}
    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }
    constexpr auto operator<=>(const TabId&) const noexcept = default;

private:
    std::uint64_t value_;
};

enum class TabKind : std::uint8_t {
    Document,
    LiveDiff,
    ReadOnlyOutput,
    SearchResults,
    TreeView,
};

enum class TabRecoveryBadge : std::uint8_t {
    None,
    Pending,
    Durable,
    Failed,
};

struct TabState {
    TabId id;
    TabKind kind = TabKind::Document;
    std::optional<FileDocumentId> document;
    std::optional<JournalDocumentKey> documentKey;
    std::string contentIdentity;
    std::string label;
    DocumentMode mode = DocumentMode::Edit;
    bool dirty = false;
    TabRecoveryBadge recovery = TabRecoveryBadge::None;

    friend bool operator==(const TabState&, const TabState&) = default;
};

struct TabViewState {
    std::vector<TabState> tabs;
    std::optional<TabId> active;

    friend bool operator==(const TabViewState&, const TabViewState&) = default;
};

struct TabDelta {
    std::optional<TabViewState> state;

    friend bool operator==(const TabDelta&, const TabDelta&) = default;
};

struct TabReplayResult {
    std::optional<TabViewState> state;
    std::string error;

    [[nodiscard]] bool accepted() const noexcept { return error.empty(); }
};

[[nodiscard]] TabDelta deriveTabDelta(const TabViewState& base,
                                        const TabViewState& target);
[[nodiscard]] TabReplayResult replayTabDelta(const TabViewState& base,
                                               const TabDelta& delta);

enum class TabCommand : std::uint8_t {
    Close,
    CloseOthers,
    CloseAll,
    ReopenClosed,
    Next,
    Previous,
    Activate,
    MoveLeft,
    MoveRight,
};

struct TabCommandDescriptor {
    std::string_view id;
    TabCommand command;

    friend bool operator==(const TabCommandDescriptor&,
                           const TabCommandDescriptor&) = default;
};

class TabManagementCommandSet {
public:
    [[nodiscard]] const std::array<TabCommandDescriptor, 9>& descriptors()
        const noexcept {
        return descriptors_;
    }

private:
    const std::array<TabCommandDescriptor, 9> descriptors_{{
        {"tab.close", TabCommand::Close},
        {"tab.close_others", TabCommand::CloseOthers},
        {"tab.close_all", TabCommand::CloseAll},
        {"tab.reopen_closed", TabCommand::ReopenClosed},
        {"tab.next", TabCommand::Next},
        {"tab.previous", TabCommand::Previous},
        {"tab.activate", TabCommand::Activate},
        {"tab.move_left", TabCommand::MoveLeft},
        {"tab.move_right", TabCommand::MoveRight},
    }};
};

[[nodiscard]] TabManagementCommandSet tabManagementCommandSet();

enum class TabError : std::uint8_t {
    None,
    InvalidArgument,
    NotFound,
    NoTabs,
    NoRecentlyClosed,
    LifecycleFailed,
    DurabilityFailed,
};

struct TabFailure {
    TabId tab;
    TabError error = TabError::None;
    std::string message;

    friend bool operator==(const TabFailure&, const TabFailure&) = default;
};

struct TabResult {
    TabError error = TabError::None;
    std::string message;
    std::optional<TabId> tab;
    std::vector<TabFailure> failures;

    [[nodiscard]] bool accepted() const noexcept {
        return error == TabError::None;
    }
};

struct TabLifecycleResult {
    TabError error = TabError::None;
    std::string message;
    std::optional<RecoveryRecordId> compensation;
    bool durable = false;

    [[nodiscard]] bool accepted() const noexcept {
        return error == TabError::None;
    }
};

class TabLifecycle {
public:
    virtual ~TabLifecycle() = default;
    [[nodiscard]] virtual TabLifecycleResult close(
        const TabState& tab, std::chrono::milliseconds durabilityTimeout) = 0;
    [[nodiscard]] virtual TabLifecycleResult reopen(
        const TabState& tab, const RecoveryRecordId& compensation) = 0;
};

struct TabManagerConfig {
    std::size_t maximumRecentlyClosed = 32;
};

class TabManager {
public:
    explicit TabManager(TabLifecycle& lifecycle,
                        TabManagerConfig config = {});
    ~TabManager();

    TabManager(const TabManager&) = delete;
    TabManager& operator=(const TabManager&) = delete;
    TabManager(TabManager&&) noexcept;
    TabManager& operator=(TabManager&&) noexcept;

    [[nodiscard]] const TabViewState& viewState() const noexcept;
    [[nodiscard]] std::size_t recentlyClosedCount() const noexcept;

    [[nodiscard]] TabResult openDocument(
        FileDocumentId document, JournalDocumentKey identity,
        std::string_view label, DocumentMode mode, bool dirty,
        TabRecoveryBadge recovery = TabRecoveryBadge::None);
    [[nodiscard]] TabResult openContent(TabKind kind,
                                         std::string_view contentIdentity,
                                         std::string_view label,
                                         DocumentMode mode);
    [[nodiscard]] TabResult updateDocument(
        FileDocumentId document, DocumentMode mode, bool dirty,
        TabRecoveryBadge recovery);

    [[nodiscard]] TabResult activate(TabId tab);
    [[nodiscard]] TabResult next();
    [[nodiscard]] TabResult previous();
    [[nodiscard]] TabResult moveLeft(TabId tab);
    [[nodiscard]] TabResult moveRight(TabId tab);

    [[nodiscard]] TabResult close(
        TabId tab, std::chrono::milliseconds durabilityTimeout);
    [[nodiscard]] TabResult closeOthers(
        TabId tab, std::chrono::milliseconds durabilityTimeout);
    [[nodiscard]] TabResult closeAll(
        std::chrono::milliseconds durabilityTimeout);
    [[nodiscard]] TabResult reopenClosed();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
