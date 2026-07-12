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
    document,
    live_diff,
    read_only_output,
    search_results,
    tree_view,
};

enum class TabRecoveryBadge : std::uint8_t {
    none,
    pending,
    durable,
    failed,
};

struct TabState {
    TabId id;
    TabKind kind = TabKind::document;
    std::optional<FileDocumentId> document;
    std::optional<JournalDocumentKey> document_key;
    std::string content_identity;
    std::string label;
    DocumentMode mode = DocumentMode::edit;
    bool dirty = false;
    TabRecoveryBadge recovery = TabRecoveryBadge::none;

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

[[nodiscard]] TabDelta derive_tab_delta(const TabViewState& base,
                                        const TabViewState& target);
[[nodiscard]] TabReplayResult replay_tab_delta(const TabViewState& base,
                                               const TabDelta& delta);

enum class TabCommand : std::uint8_t {
    close,
    close_others,
    close_all,
    reopen_closed,
    next,
    previous,
    activate,
    move_left,
    move_right,
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
        {"tab.close", TabCommand::close},
        {"tab.close_others", TabCommand::close_others},
        {"tab.close_all", TabCommand::close_all},
        {"tab.reopen_closed", TabCommand::reopen_closed},
        {"tab.next", TabCommand::next},
        {"tab.previous", TabCommand::previous},
        {"tab.activate", TabCommand::activate},
        {"tab.move_left", TabCommand::move_left},
        {"tab.move_right", TabCommand::move_right},
    }};
};

[[nodiscard]] TabManagementCommandSet tab_management_command_set();

enum class TabError : std::uint8_t {
    none,
    invalid_argument,
    not_found,
    no_tabs,
    no_recently_closed,
    lifecycle_failed,
    durability_failed,
};

struct TabFailure {
    TabId tab;
    TabError error = TabError::none;
    std::string message;

    friend bool operator==(const TabFailure&, const TabFailure&) = default;
};

struct TabResult {
    TabError error = TabError::none;
    std::string message;
    std::optional<TabId> tab;
    std::vector<TabFailure> failures;

    [[nodiscard]] bool accepted() const noexcept {
        return error == TabError::none;
    }
};

struct TabLifecycleResult {
    TabError error = TabError::none;
    std::string message;
    std::optional<RecoveryRecordId> compensation;
    bool durable = false;

    [[nodiscard]] bool accepted() const noexcept {
        return error == TabError::none;
    }
};

class TabLifecycle {
public:
    virtual ~TabLifecycle() = default;
    [[nodiscard]] virtual TabLifecycleResult close(
        const TabState& tab, std::chrono::milliseconds durability_timeout) = 0;
    [[nodiscard]] virtual TabLifecycleResult reopen(
        const TabState& tab, const RecoveryRecordId& compensation) = 0;
};

struct TabManagerConfig {
    std::size_t maximum_recently_closed = 32;
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

    [[nodiscard]] const TabViewState& view_state() const noexcept;
    [[nodiscard]] std::size_t recently_closed_count() const noexcept;

    [[nodiscard]] TabResult open_document(
        FileDocumentId document, JournalDocumentKey identity,
        std::string_view label, DocumentMode mode, bool dirty,
        TabRecoveryBadge recovery = TabRecoveryBadge::none);
    [[nodiscard]] TabResult open_content(TabKind kind,
                                         std::string_view content_identity,
                                         std::string_view label,
                                         DocumentMode mode);
    [[nodiscard]] TabResult update_document(
        FileDocumentId document, DocumentMode mode, bool dirty,
        TabRecoveryBadge recovery);

    [[nodiscard]] TabResult activate(TabId tab);
    [[nodiscard]] TabResult next();
    [[nodiscard]] TabResult previous();
    [[nodiscard]] TabResult move_left(TabId tab);
    [[nodiscard]] TabResult move_right(TabId tab);

    [[nodiscard]] TabResult close(
        TabId tab, std::chrono::milliseconds durability_timeout);
    [[nodiscard]] TabResult close_others(
        TabId tab, std::chrono::milliseconds durability_timeout);
    [[nodiscard]] TabResult close_all(
        std::chrono::milliseconds durability_timeout);
    [[nodiscard]] TabResult reopen_closed();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
