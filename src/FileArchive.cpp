#include <ssg/FileArchive.h>

#include <ssg/platform_files.h>

#include <algorithm>
#include <system_error>
#include <utility>
#include <vector>

namespace ssg {
namespace {

// The source must live inside the workspace, both so the archived layout mirrors
// the workspace and so a delete cannot be tricked into copying arbitrary files
// into the user's project.
std::optional<std::filesystem::path> relativeToWorkspace(
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& source) {
    std::error_code code;
    const auto root = weaklyCanonicalPath(workspaceRoot, code);
    if (code) return std::nullopt;
    const auto target = weaklyCanonicalPath(source, code);
    if (code) return std::nullopt;

    const auto relative = std::filesystem::relative(target, root, code);
    if (code || relative.empty()) return std::nullopt;
    // `relative` starting with ".." means the target escaped the workspace.
    if (!relative.begin()->compare("..")) return std::nullopt;
    return relative;
}

}  // namespace

FileArchive::FileArchive(std::filesystem::path root)
    : store_(std::move(root)) {}

FileArchiveResult FileArchive::archive(
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& source) {
    const auto relative = relativeToWorkspace(workspaceRoot, source);
    if (!relative) {
        return {false, {}, "archive source is outside the workspace"};
    }

    const auto sourceStat = statFile(source);
    if (!sourceStat || sourceStat->kind != FileKind::Regular) {
        return {false, {}, "archive source is not a regular file"};
    }

    const auto now = std::chrono::system_clock::now();
    const auto claimed = store_.claimEntry(now);
    if (!claimed.ok()) {
        return {false, {}, "could not create archive entry: " +
                               claimed.message};
    }
    const auto& entry = claimed.path;

    const auto destination = entry / *relative;
    const auto destinationCreated = ensureDirectory(destination.parent_path());
    if (!destinationCreated.ok()) {
        (void)removeTreeIfPresent(entry);
        return {false, {}, "could not create archive directory: " +
                               destinationCreated.message};
    }

    // Durable before returning, so the caller's unlink can never run while the
    // only other copy is still in a write cache.
    const auto copied = copyFileDurably(source, destination);
    if (!copied.ok()) {
        // Safe because `entry` is one WE created above and no other process can
        // be using it.
        (void)removeTreeIfPresent(entry);
        return {false, {}, "could not write archive copy: " + copied.message};
    }

    // copyFileDurably flushes the file and its immediate parent. The rest of
    // the chain -- the entry directory, any subdirectories of the relative
    // path, and the archive root -- is newly created and just as unflushed, so
    // a crash could take the whole subtree while the caller believed the copy
    // was safe and went on to unlink the original.
    for (auto directory = destination.parent_path();
         directory != store_.root().parent_path() && !directory.empty();
         directory = directory.parent_path()) {
        if (const auto synced = syncDirectory(directory); !synced.ok()) {
            (void)removeTreeIfPresent(entry);
            return {false, {}, "could not flush the archive to disk: " +
                                   synced.message};
        }
        if (directory == store_.root()) break;
    }
    return {true, destination, {}};
}

FileArchivePruneReport FileArchive::prune(
    std::chrono::system_clock::time_point now, std::chrono::hours maxAge) {
    FileArchivePruneReport report;

    const auto entries = store_.entries();
    if (entries.status == FileIoStatus::NotFound) return report;
    if (!entries.ok() || !entries.complete) {
        report.message = "could not read the archive: " + entries.message;
        return report;
    }
    std::vector<DurableStoreEntry> expired;
    for (const auto& entry : entries.entries) {
        if (!entry.created) {
            ++report.retainedUnparseable;
            continue;
        }
        if (*entry.created > now) {
            // Clock skew, or an archive restored from a backup. Retained rather
            // than pruned: deleting a user's only copy of a deleted file
            // because a clock disagreed would be the very loss this feature
            // exists to prevent. Reported so it is not silently immortal.
            ++report.retainedFutureDated;
            continue;
        }
        if (now - *entry.created > maxAge) expired.push_back(entry);
    }

    for (const auto& entry : expired) {
        const auto removed = removeTreeIfPresent(entry.path);
        if (!removed.ok()) {
            report.message = "could not remove an expired archive entry: " +
                             removed.message;
            continue;
        }
        ++report.removed;
    }
    return report;
}

}  // namespace ssg
