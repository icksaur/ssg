#pragma once

#include "ssg/RecoveryManager.h"
#include "ssg/ScratchStore.h"
#include "ssg/Workspace.h"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
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
    Document = 0,
    LiveDiff = 1,
    ReadOnlyOutput = 2,
    SearchResults = 3,
    TreeView = 4,
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
    std::optional<ScratchDurability> recovery;

    friend bool operator==(const TabState&, const TabState&) = default;
};

struct TabViewState {
    std::vector<TabState> tabs;
    std::optional<TabId> active;

    friend bool operator==(const TabViewState&, const TabViewState&) = default;
};

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

struct TabCloseOutcome {
    TabId tab;
    TabLifecycleResult result;
};

struct TabReopenRequest {
    TabState tab;
    RecoveryRecordId compensation;
    std::size_t index = 0;
};

struct TabManagerConfig {
    std::size_t maximumRecentlyClosed = 32;
};

class TabManager {
public:
    explicit TabManager(TabManagerConfig config = {});
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
        std::optional<ScratchDurability> recovery = std::nullopt);
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
        std::optional<ScratchDurability> recovery);

    // Removes every tab for a document that NO LONGER EXISTS, without running
    // the ordinary close path. Distinct from close(): closing flushes a document
    // and records it as reopenable, and neither is meaningful once the file and
    // its workspace entry are gone -- that path would simply fail on the missing
    // document and leave the tab stranded.
    //
    // Returns the number of tabs removed.
    std::size_t dropDocument(FileDocumentId document);

    [[nodiscard]] TabResult activate(TabId tab);
    [[nodiscard]] TabResult next();
    [[nodiscard]] TabResult previous();
    [[nodiscard]] TabResult moveLeft(TabId tab);
    [[nodiscard]] TabResult moveRight(TabId tab);

    [[nodiscard]] TabResult close(TabId tab, TabLifecycleResult result);
    [[nodiscard]] TabResult closeOthers(TabId tab,
                                        std::vector<TabCloseOutcome> outcomes);
    [[nodiscard]] TabResult closeAll(std::vector<TabCloseOutcome> outcomes);
    [[nodiscard]] std::variant<TabResult, TabReopenRequest> beginReopenClosed();
    [[nodiscard]] TabResult finishReopenClosed(TabReopenRequest request,
                                               TabLifecycleResult result);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
