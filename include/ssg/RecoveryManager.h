#pragma once

#include <ssg/DocumentKey.h>
#include <ssg/types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

struct RecoveryConfig {
    std::size_t maximumRecords = 32;
    std::uintmax_t maximumBytes = 64U * 1024U * 1024U;
};

enum class RecoveryRecordKind : std::uint8_t {
    DocumentClose = 0,
    PathRename = 3,
    PathDelete = 4,
};

class RecoveryRecordId {
public:
    explicit RecoveryRecordId(std::string value);

    [[nodiscard]] std::string_view value() const noexcept { return value_; }

    friend bool operator==(const RecoveryRecordId&,
                           const RecoveryRecordId&) = default;

private:
    std::string value_;
};

struct RecoveryRecord {
    RecoveryRecordId id;
    RecoveryRecordKind kind;
    std::uintmax_t storedBytes;
    std::optional<DocumentKey> document;
    std::vector<std::filesystem::path> affectedPaths;

    friend bool operator==(const RecoveryRecord&, const RecoveryRecord&) =
        default;
};

struct ClosedDocumentSnapshot {
    DocumentKey key;
    DocumentMode mode = DocumentMode::Edit;
    bool dirty = false;
    std::string utf8Content;

    friend bool operator==(const ClosedDocumentSnapshot&,
                           const ClosedDocumentSnapshot&) = default;
};

enum class RecoveryErrorCode : std::uint8_t {
    BudgetExceeded,
    PreparationFailed,
    ActionFailed,
    ActionAndRollbackFailed,
    RestorationFailed,
    CleanupFailed,
    RecordNotFound,
    RecordKindMismatch,
};

struct RecoveryError {
    RecoveryErrorCode code;
    std::string message;
    std::string rollbackFailure;

    friend bool operator==(const RecoveryError&, const RecoveryError&) =
        default;
};

struct RecoveryActionResult {
    std::optional<RecoveryRecordId> compensation;
    std::optional<RecoveryError> error;

    [[nodiscard]] bool accepted() const noexcept { return !error.has_value(); }
};

struct RecoveryRestoreResult {
    std::optional<RecoveryError> error;

    [[nodiscard]] bool accepted() const noexcept { return !error.has_value(); }
};

class RecoveryManager {
public:
    [[nodiscard]] static RecoveryManager create(
        const std::filesystem::path& recoveryRoot,
        RecoveryConfig config = {});

    ~RecoveryManager();
    RecoveryManager(RecoveryManager&&) noexcept;
    RecoveryManager& operator=(RecoveryManager&&) noexcept;
    RecoveryManager(const RecoveryManager&) = delete;
    RecoveryManager& operator=(const RecoveryManager&) = delete;

    [[nodiscard]] RecoveryActionResult closeDocument(
        std::optional<ClosedDocumentSnapshot>& document);
    // Renames only when the destination is free, with the exclusion enforced by
    // the filesystem rather than by a preceding check.
    [[nodiscard]] RecoveryActionResult renamePathNoClobber(
        const std::filesystem::path& source,
        const std::filesystem::path& destination);
    [[nodiscard]] RecoveryActionResult deletePath(
        const std::filesystem::path& path);


    [[nodiscard]] RecoveryRestoreResult restoreDocument(
        const RecoveryRecordId& record,
        std::optional<ClosedDocumentSnapshot>& document);
private:
    class Impl;
    explicit RecoveryManager(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace ssg
