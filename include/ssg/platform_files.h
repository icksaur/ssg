#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

enum class PathSyntax {
    Linux,
    Windows,
};

enum class LongPathPolicy {
    Legacy,
    Extended,
};

enum class PathError {
    None,
    Empty,
    Absolute,
    Traversal,
    InvalidUtf8,
    InvalidCharacter,
    ReservedName,
    TrailingDotOrSpace,
    ComponentTooLong,
    PathTooLong,
};

struct PathValidation {
    PathError error = PathError::None;
    std::size_t componentIndex = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return error == PathError::None;
    }
};

[[nodiscard]] PathValidation validateWorkspaceRelativePath(
    std::string_view path,
    PathSyntax syntax,
    LongPathPolicy longPaths = LongPathPolicy::Legacy) noexcept;

struct FileIdentity {
    std::uint64_t volume = 0;
    std::array<std::uint64_t, 2> file{};

    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

enum class FileKind : std::uint8_t {
    Regular,
    Directory,
    Symlink,
    Other,
};

struct FileStat {
    FileKind kind;
    std::uintmax_t size;
    std::filesystem::file_time_type mtime;
    FileIdentity identity;
};

// Missing is the only absence. Other metadata failures throw so callers cannot
// confuse an inaccessible path with one that does not exist.
[[nodiscard]] std::optional<FileStat> statFile(
    const std::filesystem::path& path);

class ExclusiveFileLock {
public:
    ~ExclusiveFileLock();
    ExclusiveFileLock(ExclusiveFileLock&& other) noexcept;
    ExclusiveFileLock& operator=(ExclusiveFileLock&& other) noexcept;

    ExclusiveFileLock(const ExclusiveFileLock&) = delete;
    ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;

private:
    explicit ExclusiveFileLock(std::intptr_t nativeHandle) noexcept;
    friend std::optional<ExclusiveFileLock> tryLockFile(
        const std::filesystem::path&);

    std::intptr_t nativeHandle_ = -1;
};

// Returns no value only when another live handle owns the lock. Other failures
// throw std::system_error so contention cannot hide an I/O or permission error.
[[nodiscard]] std::optional<ExclusiveFileLock> tryLockFile(
    const std::filesystem::path& path);

void setOwnerOnlyPermissions(const std::filesystem::path& path);

[[nodiscard]] std::filesystem::path userCacheRoot(
    std::string_view applicationName);

// The user's own per-application CONFIGURATION root -- distinct from
// userCacheRoot() above, which is LOCAL/disposable (Linux `~/.cache`,
// Windows `%LOCALAPPDATA%`). Config is the thing a user backs up, syncs,
// and hand-edits (Linux XDG `~/.config`, Windows ROAMING `%APPDATA%`), so
// it deliberately resolves to a different root than the cache primitive
// even though both mirror the same XDG-style env-var-with-fallback shape.
[[nodiscard]] std::filesystem::path userConfigRoot(
    std::string_view applicationName);

// The user's own per-application STATE root -- distinct from userCacheRoot()
// (LOCAL/disposable: Linux `~/.cache`, Windows `%LOCALAPPDATA%`) and from
// userConfigRoot() (backed up/synced/hand-edited: Linux `~/.config`, Windows
// ROAMING `%APPDATA%`). State is data the app owns and wants to survive a
// restart but that a user neither hand-edits nor needs synced -- draft
// recovery scratch, session remnants (Linux XDG `~/.local/state`, Windows
// LOCAL `%LOCALAPPDATA%`). It resolves to a different root than both other
// primitives even though all three mirror the same XDG-style
// env-var-with-fallback shape.
[[nodiscard]] std::filesystem::path userStateRoot(
    std::string_view applicationName);

void replaceFileAtomically(
    const std::filesystem::path& target,
    std::span<const std::byte> contents);

enum class FileIoStatus : std::uint8_t {
    Ok,
    NotFound,
    AlreadyExists,
    IoError,
};

// The outcome of a filesystem operation that has routine, expected failures a
// caller must branch on. AlreadyExists in particular is not exceptional: it is
// how the clash rule reports a refused overwrite.
struct FileIoResult {
    FileIoStatus status = FileIoStatus::IoError;
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return status == FileIoStatus::Ok;
    }
};

struct FileReadResult {
    FileIoStatus status = FileIoStatus::IoError;
    // Unsigned bytes rather than std::byte because text decoding -- the consumer of
    // very nearly every read here -- takes span<const std::uint8_t>. Matching it
    // keeps the large-file open path copy-free. The write side stays on
    // std::byte to match replaceFileAtomically; converting a span on write is
    // free, whereas converting a buffer on read is not.
    std::vector<std::uint8_t> bytes;
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return status == FileIoStatus::Ok;
    }
};

enum class DirectoryTraversal : std::uint8_t {
    Children,
    Recursive,
};

struct DirectoryListResult {
    FileIoStatus status = FileIoStatus::IoError;
    std::vector<std::filesystem::directory_entry> entries;
    bool complete = false;
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return status == FileIoStatus::Ok;
    }
};

// Reads a whole file. There is deliberately NO overload that turns a path
// straight into a string or a byte vector: a missing file must never be
// expressible as empty content, so every caller is forced through a status it
// has to look at.
[[nodiscard]] FileReadResult readFile(const std::filesystem::path& path);

// Creates a new file, failing with AlreadyExists when the path is taken. The
// exclusion is performed by the filesystem itself (O_CREAT|O_EXCL, CREATE_NEW)
// rather than by a preceding existence check, so a concurrent creator cannot
// be silently clobbered.
// CONTRACT
// createFileExclusively: the clash check is the filesystem's own atomic
//   creation, never a preceding existence probe, so the seam offers no exists()
//   helper — one would only ever build the racy pre-check this replaces. For the
//   same reason readFile exposes no path-to-string/bytes overload: a missing
//   file must never be expressible as empty content.
[[nodiscard]] FileIoResult createFileExclusively(
    const std::filesystem::path& target,
    std::span<const std::byte> contents);

[[nodiscard]] FileIoResult appendFileDurably(
    const std::filesystem::path& target,
    std::span<const std::byte> contents);

[[nodiscard]] FileIoResult syncFile(const std::filesystem::path& path);

[[nodiscard]] FileIoResult renamePathDurably(
    const std::filesystem::path& source,
    const std::filesystem::path& destination);

// seam-exempt: naming the unsafe call in prose, not calling it
// Renames without replacing an existing destination. std::filesystem::rename
// silently replaces, so it must not be used where a clash has to be refused.
[[nodiscard]] FileIoResult renameFileNoClobber(
    const std::filesystem::path& source,
    const std::filesystem::path& destination);

// Reports a missing file as NotFound rather than as a generic failure, so a
// caller can tell "already gone" from "not allowed to remove".
[[nodiscard]] FileIoResult removeFile(const std::filesystem::path& path);

// Creates the full missing directory chain and flushes each new directory and
// its parent. AlreadyExists means the complete chain was already present.
[[nodiscard]] FileIoResult createDirectoriesDurably(
    const std::filesystem::path& path);

// Creates a durable directory chain, treating an existing directory as success.
[[nodiscard]] FileIoResult ensureDirectory(
    const std::filesystem::path& path);

// Removes a file or directory tree without following directory symlinks.
// NotFound means there was nothing to remove.
[[nodiscard]] FileIoResult removeTree(const std::filesystem::path& path);

// Removes a file or directory tree, treating an absent path as success.
[[nodiscard]] FileIoResult removeTreeIfPresent(
    const std::filesystem::path& path);

// Recursive listings do not follow directory symlinks. A successful result
// with complete == false is a bounded or partially inaccessible traversal.
[[nodiscard]] DirectoryListResult listDirectory(
    const std::filesystem::path& path,
    DirectoryTraversal traversal = DirectoryTraversal::Children,
    std::size_t maximumEntries = std::numeric_limits<std::size_t>::max());

// Flushes a DIRECTORY's own entry to disk. Writing a file durably only makes
// the file and its immediate parent survive a crash; when a caller has just
// created a chain of directories, every link in that chain needs this or the
// whole subtree can vanish while the caller believes the copy is safe.
[[nodiscard]] FileIoResult syncDirectory(const std::filesystem::path& path);

// Copies to a destination that must not already exist, and returns only once
// the copy is durable -- including the directory entry, not merely the bytes.
// An archive built on a copy whose parent entry is unsynced can lose the file
// to a crash while having reported success.
[[nodiscard]] FileIoResult copyFileDurably(
    const std::filesystem::path& source,
    const std::filesystem::path& destination);

// Forces a chosen seam primitive to fail so that failure handling is reachable
// from a unit test. Inert unless installed; the default path costs one null
// check. Modeled on RecoveryFaultInjector, whose lifetime rule it shares: the
// injector must outlive its installation.
class FileIoFaultInjector {
public:
    virtual ~FileIoFaultInjector() = default;

    // Returning a non-Ok status makes the named operation fail with it before
    // the operation touches the filesystem. `operation` is the seam function
    // name; `path` is its primary target.
    [[nodiscard]] virtual FileIoStatus beforeOperation(
        std::string_view operation, const std::filesystem::path& path) = 0;
};

// Installs (or, with nullptr, removes) the process-wide injector and returns
// the previous one. Test-only; production never calls it.
//
// Not synchronized: install and uninstall must not race with seam calls on
// other threads. Tests install around a single-threaded section and restore
// afterwards, which is the only supported use.
FileIoFaultInjector* installFileIoFaultInjector(
    FileIoFaultInjector* injector) noexcept;

} // namespace ssg
