#pragma once

#include "ssg/scratch.h"
#include "ssg/scratch_journal.h"

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
    std::size_t maximum_records = 32;
    std::uintmax_t maximum_bytes = 64U * 1024U * 1024U;
};

enum class RecoveryRecordKind : std::uint8_t {
    DocumentClose,
    DocumentReload,
    FileOverwrite,
    PathRename,
    PathDelete,
    WorkspaceReplace,
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
    std::uintmax_t stored_bytes;
    std::optional<JournalDocumentKey> document;
    std::vector<std::filesystem::path> affected_paths;

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
    std::string rollback_failure;

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
    // The injector must outlive RecoveryActions.
    virtual void beforeStep(RecoveryStep step) = 0;
};

class RecoveryActions {
public:
    [[nodiscard]] static RecoveryActions create(
        const std::filesystem::path& recovery_root,
        RecoveryConfig config = {});
    [[nodiscard]] static RecoveryActions create(
        const std::filesystem::path& recovery_root,
        RecoveryConfig config,
        RecoveryFaultInjector& fault_injector);

    ~RecoveryActions();
    RecoveryActions(RecoveryActions&&) noexcept;
    RecoveryActions& operator=(RecoveryActions&&) noexcept;
    RecoveryActions(const RecoveryActions&) = delete;
    RecoveryActions& operator=(const RecoveryActions&) = delete;

    [[nodiscard]] std::vector<RecoveryRecord> records() const;

    [[nodiscard]] RecoveryActionResult closeDocument(
        std::optional<JournalDocument>& document,
        ScratchStore& scratch,
        std::chrono::milliseconds durability_timeout);
    [[nodiscard]] RecoveryActionResult reloadDocument(
        std::optional<JournalDocument>& document,
        JournalDocument replacement);
    [[nodiscard]] RecoveryActionResult overwriteFile(
        const std::filesystem::path& path,
        std::span<const std::byte> replacement);
    [[nodiscard]] RecoveryActionResult renamePath(
        const std::filesystem::path& source,
        const std::filesystem::path& destination);
    [[nodiscard]] RecoveryActionResult deletePath(
        const std::filesystem::path& path);

    // Replaces the workspace with a copy of replacement while leaving
    // replacement itself unchanged.
    [[nodiscard]] RecoveryActionResult replaceWorkspace(
        const std::filesystem::path& workspace,
        const std::filesystem::path& replacement);

    [[nodiscard]] RecoveryRestoreResult restoreDocument(
        const RecoveryRecordId& record,
        std::optional<JournalDocument>& document);
    [[nodiscard]] RecoveryRestoreResult restoreFilesystem(
        const RecoveryRecordId& record);

private:
    class Impl;
    explicit RecoveryActions(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace ssg
