#pragma once

#include "ssg/CommandRegistry.h"
#include "ssg/Document.h"
#include "ssg/platform_files.h"
#include "ssg/RecoveryActions.h"
#include "ssg/ScratchJournal.h"
#include "ssg/TextCodec.h"

#include <compare>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

class FileDocumentId {
public:
    explicit constexpr FileDocumentId(std::uint64_t value = 0) noexcept
        : value_{value} {}
    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }
    constexpr auto operator<=>(const FileDocumentId&) const noexcept = default;

private:
    std::uint64_t value_;
};

class WorkspaceReplacementId {
public:
    explicit constexpr WorkspaceReplacementId(
        std::uint64_t value = 0) noexcept
        : value_{value} {}
    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }
    constexpr auto operator<=>(const WorkspaceReplacementId&) const noexcept =
        default;

private:
    std::uint64_t value_;
};

enum class FileContentKind : std::uint8_t {
    Text,
    Binary,
    DecodeFailure,
};

enum class WorkspaceError : std::uint8_t {
    None,
    InvalidWorkspace,
    InvalidPath,
    PathOutsideWorkspace,
    NotFound,
    AlreadyOpen,
    ReadOnly,
    CapabilityDenied,
    DecodeFailed,
    IoFailed,
    RecoveryFailed,
    PartialFailure,
};

struct WorkspaceFailure {
    std::optional<FileDocumentId> document;
    WorkspaceError error = WorkspaceError::None;
    std::string message;
    friend bool operator==(const WorkspaceFailure&,
                           const WorkspaceFailure&) = default;
};

struct WorkspaceResult {
    WorkspaceError error = WorkspaceError::None;
    std::string message;
    std::optional<FileDocumentId> document;
    std::optional<RecoveryRecordId> compensation;
    std::optional<WorkspaceReplacementId> workspaceCompensation;
    std::vector<WorkspaceFailure> failures;

    [[nodiscard]] bool accepted() const noexcept {
        return error == WorkspaceError::None;
    }
};

struct WorkspaceDocumentState {
    FileDocumentId id;
    JournalDocumentKey key;
    std::string displayLabel;
    FileContentKind contentKind = FileContentKind::Text;
    TextEncodingStatus encoding;
    bool dirty = false;

    friend bool operator==(const WorkspaceDocumentState&,
                           const WorkspaceDocumentState&) = default;
};

class Workspace {
public:
    [[nodiscard]] static Workspace create(
        const std::filesystem::path& root, RecoveryActions& recovery);

    ~Workspace();
    Workspace(Workspace&&) noexcept;
    Workspace& operator=(Workspace&&) noexcept;
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] std::vector<FileDocumentId> documents() const;
    [[nodiscard]] std::optional<WorkspaceDocumentState> state(
        FileDocumentId document) const;
    [[nodiscard]] const Document& document(FileDocumentId document) const;
    [[nodiscard]] TransactionResult apply(
        FileDocumentId document, const EditTransaction& transaction);
    [[nodiscard]] std::vector<std::string> recentFiles() const;

    [[nodiscard]] WorkspaceResult openDirectory(
        const std::filesystem::path& path);
    [[nodiscard]] WorkspaceResult restoreWorkspace(
        WorkspaceReplacementId replacement);

    [[nodiscard]] WorkspaceResult newDocument(
        std::string_view suggestedLabel = {});
    [[nodiscard]] WorkspaceResult openFile(std::string_view path);
    [[nodiscard]] WorkspaceResult openRecent(std::size_t index);
    [[nodiscard]] WorkspaceResult openDroppedContent(
        const InvocationPrincipal& principal,
        std::span<const std::uint8_t> bytes,
        std::string_view suggestedLabel);

    [[nodiscard]] WorkspaceResult save(FileDocumentId document);
    [[nodiscard]] WorkspaceResult saveAll();
    [[nodiscard]] WorkspaceResult saveAs(FileDocumentId document,
                                          std::string_view path);
    [[nodiscard]] WorkspaceResult reload(FileDocumentId document);
    [[nodiscard]] WorkspaceResult reopenWithEncoding(
        FileDocumentId document, TextEncoding encoding);
    [[nodiscard]] WorkspaceResult setEncoding(
        FileDocumentId document, TextEncoding encoding);
    [[nodiscard]] WorkspaceResult setLineEnding(
        FileDocumentId document, LineEnding lineEnding);
    [[nodiscard]] WorkspaceResult setFinalNewline(
        FileDocumentId document, bool finalNewline);
    [[nodiscard]] WorkspaceResult renameFile(FileDocumentId document,
                                              std::string_view path);
    [[nodiscard]] WorkspaceResult deleteFile(FileDocumentId document);
    [[nodiscard]] WorkspaceResult newDirectory(std::string_view path);
    [[nodiscard]] WorkspaceResult restore(
        const RecoveryRecordId& compensation);

private:
    class Impl;
    explicit Workspace(std::unique_ptr<Impl> implementation) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
