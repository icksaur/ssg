#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace ssg {

struct FileArchiveResult {
    bool accepted = false;
    // Where the copy landed, when accepted.
    std::filesystem::path archivedPath;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return accepted; }
};

struct FileArchivePruneReport {
    std::size_t removed = 0;
    // Entries left alone because their name could not be read as a timestamp.
    // Reported rather than deleted: an unreadable entry is more likely a bug
    // than garbage, and removing it would be a second way to lose data in a
    // feature whose entire purpose is preventing the first.
    std::size_t retainedUnparseable = 0;
    // Entries dated in the future -- clock skew, or an archive restored from a
    // backup. Retained for the same reason as unparseable entries, and counted
    // so the condition is visible rather than a silently immortal entry.
    std::size_t retainedFutureDated = 0;
    // Housekeeping failures. Non-empty does NOT mean the caller should fail
    // (see the FileArchive contract).
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return message.empty(); }
};

// CONTRACT
// FileArchive: an archive housekeeping failure is never fatal to the caller —
//   a corrupt or unreadable archive entry must never stop a user opening their
//   workspace. Delete is deliberately unconfirmed, so the archive is what makes
//   the absence of a confirmation safe.
// The durable home for deleted files.
//
// Distinct from the recovery store on purpose. Recovery is a bounded undo ring
// (32 records / 64 MiB, evicting oldest-first), which is right for undo and
// wrong as the only surviving copy of something the user deleted: its lifetime
// would depend on how many unrelated edits happened afterwards.
class FileArchive {
public:
    explicit FileArchive(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

    // Copies `source` into the archive under its workspace-relative path, and
    // returns only once that copy is durable -- including its directory entry.
    // A copy, not a move: the workspace and the archive may be on different
    // filesystems, and a move that failed partway would leave the file neither
    // in place nor archived.
    //
    // The caller must not unlink the original until this succeeds. That
    // ordering is the whole guarantee: a delete never drops the number of
    // copies below one.
    [[nodiscard]] FileArchiveResult archive(
        const std::filesystem::path& workspaceRoot,
        const std::filesystem::path& source);

    // Removes entries older than `maxAge`. Age comes from the timestamp in the
    // entry's own directory name rather than from filesystem metadata, so it
    // survives a copy of the archive and is decidable in a test without
    // manipulating clocks.
    [[nodiscard]] FileArchivePruneReport prune(
        std::chrono::system_clock::time_point now,
        std::chrono::hours maxAge);

    // `<utc-timestamp>-<counter>`, chosen to sort chronologically so the
    // archive is browsable with ls. The counter separates deletions within the
    // same second.
    [[nodiscard]] static std::string entryDirectoryName(
        std::chrono::system_clock::time_point moment, std::size_t counter);

    // The inverse of entryDirectoryName; nullopt when the name was not written
    // by this code.
    [[nodiscard]] static std::optional<std::chrono::system_clock::time_point>
    entryTimestamp(std::string_view directoryName);

private:
    std::filesystem::path root_;
    std::size_t counter_ = 0;
};

// How long a deleted file stays recoverable. Age-based rather than count-based
// because the failure this guards against is "I deleted it last week and now I
// need it", which a count bound cannot express.
inline constexpr std::chrono::hours kFileArchiveRetention{24 * 14};

}  // namespace ssg
