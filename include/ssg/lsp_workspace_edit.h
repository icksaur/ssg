#pragma once

#include <ssg/lsp_sync.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

struct LspWorkspaceEditCommandDescriptor {
    std::string_view id;
    bool user_navigation = false;
    friend bool operator==(const LspWorkspaceEditCommandDescriptor&,
                           const LspWorkspaceEditCommandDescriptor&) = default;
};

class LspWorkspaceEditCommandSet {
public:
    [[nodiscard]] const std::array<LspWorkspaceEditCommandDescriptor, 1>&
    descriptors() const noexcept {
        return descriptors_;
    }

private:
    const std::array<LspWorkspaceEditCommandDescriptor, 1> descriptors_{
        {{"rename.symbol", false}}};
};

[[nodiscard]] LspWorkspaceEditCommandSet lsp_workspace_edit_command_set();

enum class LspWorkspaceDocumentError : std::uint8_t {
    none,
    unknown_document,
    stale_revision,
    write_failed,
};

struct LspWorkspaceDocumentWriteResult {
    Revision revision{0};
    LspWorkspaceDocumentError error = LspWorkspaceDocumentError::none;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspWorkspaceDocumentError::none;
    }
};

class LspWorkspaceEditDocuments {
public:
    virtual ~LspWorkspaceEditDocuments() = default;
    [[nodiscard]] virtual std::optional<LspDocumentSnapshot> snapshot(
        std::string_view uri) const = 0;
    [[nodiscard]] virtual LspWorkspaceDocumentWriteResult apply(
        std::string uri, Revision expected_revision, std::string text) = 0;
};

enum class LspWorkspaceFileNodeKind : std::uint8_t {
    missing,
    file,
    directory,
};

struct LspWorkspaceFileNode {
    LspWorkspaceFileNodeKind kind = LspWorkspaceFileNodeKind::missing;
    std::string content;
    friend bool operator==(const LspWorkspaceFileNode&,
                           const LspWorkspaceFileNode&) = default;
};

enum class LspWorkspaceFileError : std::uint8_t {
    none,
    not_found,
    already_exists,
    invalid_operation,
    io_error,
};

struct LspWorkspaceFileResult {
    LspWorkspaceFileError error = LspWorkspaceFileError::none;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspWorkspaceFileError::none;
    }
};

class LspWorkspaceFileOperations {
public:
    virtual ~LspWorkspaceFileOperations() = default;
    [[nodiscard]] virtual LspWorkspaceFileResult snapshot(
        std::string_view uri, LspWorkspaceFileNode& node) const = 0;
    [[nodiscard]] virtual LspWorkspaceFileResult create_file(
        std::string uri, bool overwrite) = 0;
    [[nodiscard]] virtual LspWorkspaceFileResult write_file(
        std::string uri, std::string content) = 0;
    [[nodiscard]] virtual LspWorkspaceFileResult rename_path(
        std::string old_uri, std::string new_uri, bool overwrite) = 0;
    [[nodiscard]] virtual LspWorkspaceFileResult delete_path(
        std::string uri, bool recursive) = 0;
    [[nodiscard]] virtual LspWorkspaceFileResult restore_path(
        std::string uri, const LspWorkspaceFileNode& node) = 0;
};

enum class LspWorkspaceEditError : std::uint8_t {
    none,
    malformed_edit,
    unknown_document,
    stale_revision,
    invalid_position,
    overlapping_edits,
    file_conflict,
    apply_failed,
    rollback_failed,
};

enum class LspWorkspaceEditRecoveryKind : std::uint8_t {
    document_text,
    restore_path,
    write_file,
    delete_file,
    rename_file,
};

struct LspWorkspaceEditRecoveryOperation {
    LspWorkspaceEditRecoveryKind kind =
        LspWorkspaceEditRecoveryKind::document_text;
    std::string uri;
    std::string secondary_uri;
    Revision expected_revision{0};
    std::string text;
    LspWorkspaceFileNode node;
    bool recursive = false;
    bool overwrite = false;
    friend bool operator==(const LspWorkspaceEditRecoveryOperation&,
                           const LspWorkspaceEditRecoveryOperation&) = default;
};

struct LspWorkspaceEditRecoveryRecord {
    std::vector<LspWorkspaceEditRecoveryOperation> operations;
    friend bool operator==(const LspWorkspaceEditRecoveryRecord&,
                           const LspWorkspaceEditRecoveryRecord&) = default;
};

struct LspWorkspaceEditApplyResult {
    LspWorkspaceEditError error = LspWorkspaceEditError::none;
    std::string message;
    std::optional<LspWorkspaceEditRecoveryRecord> recovery;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspWorkspaceEditError::none;
    }
};

struct LspWorkspaceEditConfig {
    std::size_t maximum_json_depth = 64;
};

class LspWorkspaceEditApplier {
public:
    LspWorkspaceEditApplier(LspWorkspaceEditDocuments& documents,
                            LspWorkspaceFileOperations& files,
                            LspWorkspaceEditConfig config = {});

    [[nodiscard]] LspWorkspaceEditApplyResult apply(
        std::string_view workspace_edit_json);
    [[nodiscard]] LspWorkspaceEditApplyResult recover(
        const LspWorkspaceEditRecoveryRecord& recovery);

private:
    LspWorkspaceEditDocuments* documents_ = nullptr;
    LspWorkspaceFileOperations* files_ = nullptr;
    LspWorkspaceEditConfig config_;
};

enum class LspRenameError : std::uint8_t {
    none,
    sync_error,
    unknown_document,
    stale_revision,
    invalid_position,
    invalid_argument,
};

struct LspRenameRequestResult {
    std::uint64_t request_id = 0;
    LspRenameError error = LspRenameError::none;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return request_id != 0 && error == LspRenameError::none;
    }
};

enum class LspRenamePublishResult : std::uint8_t {
    accepted,
    cancelled,
    superseded,
    stale_revision,
    malformed_response,
    server_error,
    edit_rejected,
};

struct LspRenamePublication {
    std::uint64_t request_id = 0;
    LspRenamePublishResult result = LspRenamePublishResult::accepted;
    std::string message;
    std::optional<LspWorkspaceEditRecoveryRecord> recovery;
    friend bool operator==(const LspRenamePublication&,
                           const LspRenamePublication&) = default;
};

struct LspRenamePollResult {
    LspSyncError error = LspSyncError::none;
    std::string message;
    std::vector<LspRenamePublication> publications;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspSyncError::none;
    }
};

class LspWorkspaceEditController {
public:
    LspWorkspaceEditController(LspSyncClient& client,
                               LspWorkspaceEditApplier& applier);

    [[nodiscard]] LspRenameRequestResult request_rename(
        std::string uri, Revision revision, ByteOffset position,
        std::string new_name);
    [[nodiscard]] LspRenamePollResult poll(Revision current_revision);

private:
    enum class Disposition : std::uint8_t { active, cancelled, superseded };
    struct Pending {
        std::string uri;
        Revision revision{0};
        Disposition disposition = Disposition::active;
    };

    void supersede();

    LspSyncClient* client_ = nullptr;
    LspWorkspaceEditApplier* applier_ = nullptr;
    std::map<std::uint64_t, Pending> pending_;
    std::uint64_t active_id_ = 0;
};

} // namespace ssg
