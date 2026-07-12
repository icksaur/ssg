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
    document_close,
    document_reload,
    file_overwrite,
    path_rename,
    path_delete,
    workspace_replace,
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
    durability_failed,
    budget_exceeded,
    preparation_failed,
    action_failed,
    action_and_rollback_failed,
    restoration_failed,
    cleanup_failed,
    record_not_found,
    record_kind_mismatch,
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
    prepare_artifact,
    install_record,
    publish_record,
    mutate_document,
    mutate_filesystem,
    rollback_document,
    rollback_filesystem,
    restore_document,
    restore_filesystem,
    cleanup_record,
};

class RecoveryFaultInjector {
public:
    virtual ~RecoveryFaultInjector() = default;

    // A repeated step denotes another independently fallible part of the same
    // action. Throwing injects failure before that part begins.
    // The injector must outlive RecoveryActions.
    virtual void before_step(RecoveryStep step) = 0;
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

    [[nodiscard]] RecoveryActionResult close_document(
        std::optional<JournalDocument>& document,
        ScratchStore& scratch,
        std::chrono::milliseconds durability_timeout);
    [[nodiscard]] RecoveryActionResult reload_document(
        std::optional<JournalDocument>& document,
        JournalDocument replacement);
    [[nodiscard]] RecoveryActionResult overwrite_file(
        const std::filesystem::path& path,
        std::span<const std::byte> replacement);
    [[nodiscard]] RecoveryActionResult rename_path(
        const std::filesystem::path& source,
        const std::filesystem::path& destination);
    [[nodiscard]] RecoveryActionResult delete_path(
        const std::filesystem::path& path);

    // Replaces the workspace with a copy of replacement while leaving
    // replacement itself unchanged.
    [[nodiscard]] RecoveryActionResult replace_workspace(
        const std::filesystem::path& workspace,
        const std::filesystem::path& replacement);

    [[nodiscard]] RecoveryRestoreResult restore_document(
        const RecoveryRecordId& record,
        std::optional<JournalDocument>& document);
    [[nodiscard]] RecoveryRestoreResult restore_filesystem(
        const RecoveryRecordId& record);

private:
    class Impl;
    explicit RecoveryActions(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace ssg
