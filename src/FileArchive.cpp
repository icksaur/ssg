#include <ssg/FileArchive.h>

#include <ssg/platform_files.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace ssg {
namespace {

constexpr std::size_t kTimestampLength = 15;  // YYYYMMDDTHHMMSS

// The source must live inside the workspace, both so the archived layout mirrors
// the workspace and so a delete cannot be tricked into copying arbitrary files
// into the user's project.
std::optional<std::filesystem::path> relativeToWorkspace(
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& source) {
    std::error_code code;
    const auto root = std::filesystem::weakly_canonical(workspaceRoot, code);
    if (code) return std::nullopt;
    const auto target = std::filesystem::weakly_canonical(source, code);
    if (code) return std::nullopt;

    const auto relative = std::filesystem::relative(target, root, code);
    if (code || relative.empty()) return std::nullopt;
    // `relative` starting with ".." means the target escaped the workspace.
    if (!relative.begin()->compare("..")) return std::nullopt;
    return relative;
}

}  // namespace

FileArchive::FileArchive(std::filesystem::path root)
    : root_(std::move(root)) {}

std::string FileArchive::entryDirectoryName(
    std::chrono::system_clock::time_point moment, std::size_t counter) {
    const auto seconds = std::chrono::system_clock::to_time_t(moment);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::ostringstream name;
    name << std::put_time(&utc, "%Y%m%dT%H%M%S") << '-' << std::setfill('0')
         << std::setw(4) << counter;
    return name.str();
}

std::optional<std::chrono::system_clock::time_point>
FileArchive::entryTimestamp(std::string_view directoryName) {
    if (directoryName.size() < kTimestampLength) return std::nullopt;
    // A separator must follow the fixed-width stamp, or this is some other
    // name that merely happens to start with digits.
    if (directoryName.size() > kTimestampLength &&
        directoryName[kTimestampLength] != '-') {
        return std::nullopt;
    }

    std::tm utc{};
    std::istringstream stamp{
        std::string{directoryName.substr(0, kTimestampLength)}};
    stamp >> std::get_time(&utc, "%Y%m%dT%H%M%S");
    if (stamp.fail()) return std::nullopt;

#ifdef _WIN32
    const auto seconds = _mkgmtime(&utc);
#else
    const auto seconds = timegm(&utc);
#endif
    if (seconds == static_cast<std::time_t>(-1)) return std::nullopt;
    return std::chrono::system_clock::from_time_t(seconds);
}

FileArchiveResult FileArchive::archive(
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& source) {
    const auto relative = relativeToWorkspace(workspaceRoot, source);
    if (!relative) {
        return {false, {}, "archive source is outside the workspace"};
    }

    std::error_code code;
    if (!std::filesystem::is_regular_file(source, code) || code) {
        return {false, {}, "archive source is not a regular file"};
    }

    std::filesystem::create_directories(root_, code);
    if (code) {
        return {false, {}, "could not create the archive root: " +
                               code.message()};
    }

    // Claim an entry directory that DID NOT already exist. A second process
    // deleting in the same second starts its counter at zero too, and
    // create_directories succeeds silently on an existing path -- so without
    // this the two deletions would merge into one entry, and the cleanup below
    // could then delete the other process's archived files. create_directory
    // returns false when the directory already exists, which is the signal.
    const auto now = std::chrono::system_clock::now();
    std::filesystem::path entry;
    bool claimed = false;
    for (std::size_t attempt = 0; attempt < 10000 && !claimed; ++attempt) {
        entry = root_ / entryDirectoryName(now, counter_++);
        claimed = std::filesystem::create_directory(entry, code) && !code;
        if (code) {
            return {false, {}, "could not create archive entry: " +
                                   code.message()};
        }
    }
    if (!claimed) {
        return {false, {}, "could not claim a unique archive entry"};
    }

    const auto destination = entry / *relative;
    std::filesystem::create_directories(destination.parent_path(), code);
    if (code) {
        std::error_code ignored;
        std::filesystem::remove_all(entry, ignored);
        return {false, {}, "could not create archive directory: " +
                               code.message()};
    }

    // Durable before returning, so the caller's unlink can never run while the
    // only other copy is still in a write cache.
    const auto copied = copyFileDurably(source, destination);
    if (!copied.ok()) {
        // Safe because `entry` is one WE created above and no other process can
        // be using it.
        std::error_code ignored;
        std::filesystem::remove_all(entry, ignored);
        return {false, {}, "could not write archive copy: " + copied.message};
    }

    // copyFileDurably flushes the file and its immediate parent. The rest of
    // the chain -- the entry directory, any subdirectories of the relative
    // path, and the archive root -- is newly created and just as unflushed, so
    // a crash could take the whole subtree while the caller believed the copy
    // was safe and went on to unlink the original.
    for (auto directory = destination.parent_path();
         directory != root_.parent_path() && !directory.empty();
         directory = directory.parent_path()) {
        if (const auto synced = syncDirectory(directory); !synced.ok()) {
            std::error_code ignored;
            std::filesystem::remove_all(entry, ignored);
            return {false, {}, "could not flush the archive to disk: " +
                                   synced.message};
        }
        if (directory == root_) break;
    }
    return {true, destination, {}};
}

FileArchivePruneReport FileArchive::prune(
    std::chrono::system_clock::time_point now, std::chrono::hours maxAge) {
    FileArchivePruneReport report;

    std::error_code code;
    if (!std::filesystem::exists(root_, code) || code) {
        // Nothing has been deleted yet. Not an error.
        return report;
    }

    std::vector<std::filesystem::path> expired;
    std::filesystem::directory_iterator entries{root_, code};
    if (code) {
        report.message = "could not read the archive: " + code.message();
        return report;
    }
    for (const auto& entry : entries) {
        if (!entry.is_directory(code) || code) continue;
        const auto timestamp = entryTimestamp(entry.path().filename().string());
        if (!timestamp) {
            ++report.retainedUnparseable;
            continue;
        }
        if (*timestamp > now) {
            // Clock skew, or an archive restored from a backup. Retained rather
            // than pruned: deleting a user's only copy of a deleted file
            // because a clock disagreed would be the very loss this feature
            // exists to prevent. Reported so it is not silently immortal.
            ++report.retainedFutureDated;
            continue;
        }
        if (now - *timestamp > maxAge) expired.push_back(entry.path());
    }

    for (const auto& entry : expired) {
        std::error_code removeError;
        std::filesystem::remove_all(entry, removeError);
        if (removeError) {
            report.message = "could not remove an expired archive entry: " +
                             removeError.message();
            continue;
        }
        ++report.removed;
    }
    return report;
}

}  // namespace ssg
