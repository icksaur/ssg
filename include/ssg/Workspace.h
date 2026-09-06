#pragma once

#include <ssg/CommandCatalog.h>
#include <ssg/Document.h>
#include <ssg/platform_files.h>
#include <ssg/FileArchive.h>
#include <ssg/RecoveryManager.h>
#include <ssg/ScratchJournal.h>
#include <ssg/TextCodec.h>

#include <compare>
#include <cstdint>
#include <filesystem>
#include <functional>
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

// The tab title for a document that has never been named. Defined once so the
// library and its clients cannot disagree about what an unnamed buffer is
// called.
inline constexpr std::string_view kNewBufferLabel = "[new buffer]";

class Workspace {
public:
    [[nodiscard]] static Workspace create(
        const std::filesystem::path& root, RecoveryManager& recovery,
        std::optional<std::filesystem::path> archiveRoot = std::nullopt);

    // Removes archive entries older than kFileArchiveRetention. Housekeeping:
    // a failure here is reported but never prevents using the workspace.
    [[nodiscard]] FileArchivePruneReport pruneArchive(
        std::chrono::system_clock::time_point now =
            std::chrono::system_clock::now());

    ~Workspace();
    Workspace(Workspace&&) noexcept;
    Workspace& operator=(Workspace&&) noexcept;
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    // Installs a callback invoked with the relative path each time this workspace
    // writes a file (save/save-as). The runtime uses it to correlate SSG's own
    // writes against filesystem-watcher events so a self-save is never reported as
    // an external modification. A library-internal seam; a client never sets it.
    void setSaveObserver(std::function<void(const std::filesystem::path&)> observer);
    [[nodiscard]] std::vector<FileDocumentId> documents() const;
    [[nodiscard]] std::optional<WorkspaceDocumentState> state(
        FileDocumentId document) const;
    // The disk baseline captured when the document was opened or last saved (the
    // state its edits branch from), or nullopt for an untitled buffer or an
    // unknown id. Used by draft recovery to detect an external change on reopen.
    [[nodiscard]] std::optional<DraftBaseline> baselineFor(
        FileDocumentId document) const;
    // Whether an observed disk state equals the document's authoritative external
    // baseline (the state its edits branch from, as advanced by a keep_buffer
    // dismissal). `observedContent` is the raw disk bytes, or nullopt when the file
    // was observed absent. A match means the change was already adopted or
    // dismissed, so the reconcile skips it instead of re-raising. An
    // unreadable/non-regular (Unknown) observation is never passed here -- the
    // caller raises directly, never skips.
    [[nodiscard]] bool matchesExternalBaseline(
        FileDocumentId document,
        const std::optional<std::string>& observedContent) const;
    [[nodiscard]] bool commitExternalDismissal(
        FileDocumentId document, bool removed,
        const std::optional<std::string>& dismissedContent);
    // The literal bytes read from disk when the document was opened or last
    // reloaded — the same bytes the baseline hash was computed over. Used by
    // draft recovery to classify a recovered draft against the current disk
    // file. nullopt for an unknown id.
    [[nodiscard]] std::optional<std::string> rawDiskContent(
        FileDocumentId document) const;
    // Replaces a freshly-opened saved document's buffer with a recovered draft's
    // content, leaving the persisted (disk) baseline untouched so the document
    // reports dirty exactly when the draft differs from disk — dirtiness is
    // derived, never set. Returns false for an untitled, non-text, or unknown
    // document. Intended for the single-file draft reopen path only.
    [[nodiscard]] bool restoreDraft(FileDocumentId document,
                                    std::string_view draftContent);
    [[nodiscard]] const Document* tryDocument(FileDocumentId document) const noexcept;
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
    [[nodiscard]] WorkspaceResult openVirtualDocument(
        std::string_view suggestedLabel, std::string_view initialText,
        DocumentMode mode);
    [[nodiscard]] WorkspaceResult openFile(std::string_view path);
    [[nodiscard]] WorkspaceResult openRecent(std::size_t index);
    [[nodiscard]] WorkspaceResult openDroppedContent(
        std::span<const std::uint8_t> bytes,
        std::string_view suggestedLabel);

    [[nodiscard]] WorkspaceResult save(FileDocumentId document);
    [[nodiscard]] WorkspaceResult saveAll();
    [[nodiscard]] WorkspaceResult saveAs(FileDocumentId document,
                                          std::string_view path);
    [[nodiscard]] WorkspaceResult reload(FileDocumentId document);
    // Reversibly replaces an open saved document's buffer with the GIVEN content
    // (never a fresh disk read), rebaselining to those bytes and recording a
    // compensation, so the external-modification reload commits exactly the bytes
    // the conflict was raised about. Unlike reload(), which re-reads disk and could
    // commit different bytes if disk changed again between raise and reload.
    [[nodiscard]] WorkspaceResult reloadWithContent(FileDocumentId document,
                                                    std::string content);
    // Adopts an externally observed rename of an ALREADY-MOVED file: rekeys the
    // open document to `newPath`, relabels it, and rebaselines it to the new path's
    // disk `content`, recording a compensation. It performs NO filesystem move (the
    // file already moved on disk) -- unlike renameFile(), which moves the file.
    // `replaceBuffer` replaces the visible buffer with `content` (a clean document
    // that simply follows its file); when false the buffer is left untouched (a
    // dirty document whose unsaved edits are preserved while its baseline follows
    // to the new path). The open document follows its file instead of raising a
    // spurious removed/created pair.
    [[nodiscard]] WorkspaceResult adoptExternalRename(FileDocumentId document,
                                                      std::string_view newPath,
                                                      std::string content,
                                                      bool replaceBuffer);
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
    [[nodiscard]] WorkspaceResult removeDocument(FileDocumentId document);
    [[nodiscard]] WorkspaceResult newDirectory(std::string_view path);
    [[nodiscard]] WorkspaceResult restore(
        const RecoveryRecordId& compensation);

private:
    class Impl;
    explicit Workspace(std::unique_ptr<Impl> implementation) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
