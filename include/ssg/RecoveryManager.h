#pragma once

#include <ssg/ScratchStore.h>
#include <ssg/ScratchJournal.h>

#include <chrono>
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
    std::optional<JournalDocumentKey> document;
    std::vector<std::filesystem::path> affectedPaths;

    friend bool operator==(const RecoveryRecord&, const RecoveryRecord&) =
        default;
};

enum class RecoveryErrorCode : std::uint8_t {
    DurabilityFailed,
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

enum class RecoveryStep : std::uint8_t {
    PrepareArtifact,
    InstallRecord,
    PublishRecord,
    MutateDocument,
    MutateFilesystem,
    RollbackDocument,
    RollbackFilesystem,
    RestoreDocument,
    RestoreFilesystem,
    CleanupRecord,
};

class RecoveryFaultInjector {
public:
    virtual ~RecoveryFaultInjector() = default;

    // A repeated step denotes another independently fallible part of the same
    // action. Throwing injects failure before that part begins.
    // The injector must outlive RecoveryManager.
    virtual void beforeStep(RecoveryStep step) = 0;
};

class RecoveryManager {
public:
    [[nodiscard]] static RecoveryManager create(
        const std::filesystem::path& recoveryRoot,
        RecoveryConfig config = {});
    [[nodiscard]] static RecoveryManager create(
        const std::filesystem::path& recoveryRoot,
        RecoveryConfig config,
        RecoveryFaultInjector& faultInjector);

    ~RecoveryManager();
    RecoveryManager(RecoveryManager&&) noexcept;
    RecoveryManager& operator=(RecoveryManager&&) noexcept;
    RecoveryManager(const RecoveryManager&) = delete;
    RecoveryManager& operator=(const RecoveryManager&) = delete;

    [[nodiscard]] std::vector<RecoveryRecord> records() const;

    [[nodiscard]] RecoveryActionResult closeDocument(
        std::optional<JournalDocument>& document,
        ScratchStore& scratch,
        std::chrono::milliseconds durabilityTimeout);
    [[nodiscard]] RecoveryActionResult renamePath(
        const std::filesystem::path& source,
        const std::filesystem::path& destination);

    // Renames only when the destination is free, with the exclusion enforced by
    // the filesystem rather than by a preceding check. Distinct from
    // renamePath, which deliberately REPLACES the destination: replacing is
    // right for an LSP-driven or recovery-internal move, and wrong for a
    // user-facing rename, where clobbering is data loss.
    [[nodiscard]] RecoveryActionResult renamePathNoClobber(
        const std::filesystem::path& source,
        const std::filesystem::path& destination);
    [[nodiscard]] RecoveryActionResult deletePath(
        const std::filesystem::path& path);


    [[nodiscard]] RecoveryRestoreResult restoreDocument(
        const RecoveryRecordId& record,
        std::optional<JournalDocument>& document);
    [[nodiscard]] RecoveryRestoreResult restoreFilesystem(
        const RecoveryRecordId& record);

private:
    class Impl;
    explicit RecoveryManager(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace ssg
