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

[[nodiscard]] LspWorkspaceEditCommandSet lspWorkspaceEditCommandSet();

enum class LspWorkspaceDocumentError : std::uint8_t {
    None,
    UnknownDocument,
    StaleRevision,
    WriteFailed,
};

struct LspWorkspaceDocumentWriteResult {
    Revision revision{0};
    LspWorkspaceDocumentError error = LspWorkspaceDocumentError::None;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspWorkspaceDocumentError::None;
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
    Missing,
    File,
    Directory,
};

struct LspWorkspaceFileNode {
    LspWorkspaceFileNodeKind kind = LspWorkspaceFileNodeKind::Missing;
    std::string content;
    friend bool operator==(const LspWorkspaceFileNode&,
                           const LspWorkspaceFileNode&) = default;
};

enum class LspWorkspaceFileError : std::uint8_t {
    None,
    NotFound,
    AlreadyExists,
    InvalidOperation,
    IoError,
};

struct LspWorkspaceFileResult {
    LspWorkspaceFileError error = LspWorkspaceFileError::None;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspWorkspaceFileError::None;
    }
};

class LspWorkspaceFileOperations {
public:
    virtual ~LspWorkspaceFileOperations() = default;
    [[nodiscard]] virtual LspWorkspaceFileResult snapshot(
        std::string_view uri, LspWorkspaceFileNode& node) const = 0;
    [[nodiscard]] virtual LspWorkspaceFileResult createFile(
        std::string uri, bool overwrite) = 0;
    [[nodiscard]] virtual LspWorkspaceFileResult writeFile(
        std::string uri, std::string content) = 0;
    [[nodiscard]] virtual LspWorkspaceFileResult renamePath(
        std::string old_uri, std::string new_uri, bool overwrite) = 0;
    [[nodiscard]] virtual LspWorkspaceFileResult deletePath(
        std::string uri, bool recursive) = 0;
    [[nodiscard]] virtual LspWorkspaceFileResult restorePath(
        std::string uri, const LspWorkspaceFileNode& node) = 0;
};

enum class LspWorkspaceEditError : std::uint8_t {
    None,
    MalformedEdit,
    UnknownDocument,
    StaleRevision,
    InvalidPosition,
    OverlappingEdits,
    FileConflict,
    ApplyFailed,
    RollbackFailed,
};

enum class LspWorkspaceEditRecoveryKind : std::uint8_t {
    DocumentText,
    RestorePath,
    WriteFile,
    DeleteFile,
    RenameFile,
};

struct LspWorkspaceEditRecoveryOperation {
    LspWorkspaceEditRecoveryKind kind =
        LspWorkspaceEditRecoveryKind::DocumentText;
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
    LspWorkspaceEditError error = LspWorkspaceEditError::None;
    std::string message;
    std::optional<LspWorkspaceEditRecoveryRecord> recovery;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspWorkspaceEditError::None;
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
    None,
    SyncError,
    UnknownDocument,
    StaleRevision,
    InvalidPosition,
    InvalidArgument,
};

struct LspRenameRequestResult {
    std::uint64_t request_id = 0;
    LspRenameError error = LspRenameError::None;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return request_id != 0 && error == LspRenameError::None;
    }
};

enum class LspRenamePublishResult : std::uint8_t {
    Accepted,
    Cancelled,
    Superseded,
    StaleRevision,
    MalformedResponse,
    ServerError,
    EditRejected,
};

struct LspRenamePublication {
    std::uint64_t request_id = 0;
    LspRenamePublishResult result = LspRenamePublishResult::Accepted;
    std::string message;
    std::optional<LspWorkspaceEditRecoveryRecord> recovery;
    friend bool operator==(const LspRenamePublication&,
                           const LspRenamePublication&) = default;
};

struct LspRenamePollResult {
    LspSyncError error = LspSyncError::None;
    std::string message;
    std::vector<LspRenamePublication> publications;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspSyncError::None;
    }
};

class LspWorkspaceEditController {
public:
    LspWorkspaceEditController(LspSyncClient& client,
                               LspWorkspaceEditApplier& applier);

    [[nodiscard]] LspRenameRequestResult requestRename(
        std::string uri, Revision revision, ByteOffset position,
        std::string new_name);
    [[nodiscard]] LspRenamePollResult poll(Revision current_revision);

private:
    enum class Disposition : std::uint8_t { Active, Cancelled, Superseded };
    struct Pending {
        std::string uri;
        Revision revision{0};
        Disposition disposition = Disposition::Active;
    };

    void supersede();

    LspSyncClient* client_ = nullptr;
    LspWorkspaceEditApplier* applier_ = nullptr;
    std::map<std::uint64_t, Pending> pending_;
    std::uint64_t active_id_ = 0;
};

} // namespace ssg
