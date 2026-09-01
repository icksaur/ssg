#pragma once

#include <ssg/detail/generated/semantic_wire_manifest.h>

#include "ssg/RecoveryManager.h"
#include "ssg/Workspace.h"

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
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_TAB_KIND_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};

enum class TabRecoveryBadge : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_TAB_RECOVERY_BADGE_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_TAB_KIND_ENUMERATORS
#undef SSG_TAB_RECOVERY_BADGE_ENUMERATORS

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
    std::optional<FileDocumentId> reopenedDocument;
    std::optional<JournalDocumentKey> reopenedDocumentKey;
    bool durable = false;
    // An ephemeral tab (e.g. a read-only help/output tab) is regenerable and is
    // deliberately NOT journaled for reopen, so it legitimately closes without a
    // compensation record. closeAt accepts a missing compensation only when this
    // is set, and does not add the tab to the reopen-closed history.
    bool ephemeral = false;

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
    // Syncs the whole of a tab's document-derived state, INCLUDING its identity
    // and label. Taking them here rather than only mode/dirty/badge is what
    // keeps a renamed or saved-as document's tab title correct: a partial sync
    // silently leaves the old name on screen while the bytes live elsewhere.
    [[nodiscard]] TabResult updateDocument(
        FileDocumentId document, JournalDocumentKey identity,
        std::string_view label, DocumentMode mode, bool dirty,
        TabRecoveryBadge recovery);

    // Removes every tab for a document that NO LONGER EXISTS, without running
    // the close lifecycle. Distinct from close(): closing flushes a document
    // and records it as reopenable, and neither is meaningful once the file and
    // its workspace entry are gone -- the lifecycle would simply fail on the
    // missing document and leave the tab stranded.
    //
    // Returns the number of tabs removed.
    std::size_t dropDocument(FileDocumentId document);

    [[nodiscard]] TabResult activate(TabId tab);    [[nodiscard]] TabResult next();
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
