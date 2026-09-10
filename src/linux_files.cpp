#include <ssg/platform_files.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ssg {
namespace {

[[noreturn]] void throwErrno(std::string_view operation,
                              const std::filesystem::path& path) {
    throw std::system_error(errno, std::generic_category(),
                            std::string(operation) + ": " + path.string());
}

void closeNoexcept(std::intptr_t& handle) noexcept {
    if (handle >= 0) {
        const auto fd = static_cast<int>(handle);
        flock(fd, LOCK_UN);
        close(fd);
        handle = -1;
    }
}

class TemporaryFile {
public:
    TemporaryFile(const std::filesystem::path& target, mode_t mode) {
        auto pattern =
            (target.parent_path() / (target.filename().string() + ".ssg-tmp-XXXXXX"))
                .string();
        storage_.assign(pattern.begin(), pattern.end());
        storage_.push_back('\0');
        descriptor_ = mkstemp(storage_.data());
        if (descriptor_ < 0) {
            throwErrno("create replacement temporary file", target);
        }
        path_ = storage_.data();
        if (fchmod(descriptor_, mode) != 0) {
            const int saved = errno;
            close(descriptor_);
            descriptor_ = -1;
            unlink(path_.c_str());
            errno = saved;
            throwErrno("set replacement permissions", target);
        }
    }

    ~TemporaryFile() {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
        if (!path_.empty()) {
            unlink(path_.c_str());
        }
    }

    int descriptor() const noexcept { return descriptor_; }
    const std::filesystem::path& path() const noexcept { return path_; }

    void closeForPublish() {
        if (close(descriptor_) != 0) {
            descriptor_ = -1;
            throwErrno("close replacement temporary file", path_);
        }
        descriptor_ = -1;
    }

    void published() noexcept { path_.clear(); }

private:
    std::vector<char> storage_;
    std::filesystem::path path_;
    int descriptor_ = -1;
};

} // namespace

std::optional<FileStat> statFile(const std::filesystem::path& path,
                                 SymlinkMode symlinks) {
    struct stat status {};
    const auto inspect =
        symlinks == SymlinkMode::Follow ? ::stat : ::lstat;
    if (inspect(path.c_str(), &status) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) return std::nullopt;
        if (symlinks == SymlinkMode::Follow && errno == ELOOP) {
            return std::nullopt;
        }
        throwErrno("read file metadata", path);
    }
    FileKind kind = FileKind::Other;
    if (S_ISREG(status.st_mode)) {
        kind = FileKind::Regular;
    } else if (S_ISDIR(status.st_mode)) {
        kind = FileKind::Directory;
    } else if (S_ISLNK(status.st_mode)) {
        kind = FileKind::Symlink;
    }
    const auto systemTime =
        std::chrono::system_clock::time_point{
            std::chrono::seconds{status.st_mtim.tv_sec}} +
        std::chrono::nanoseconds{status.st_mtim.tv_nsec};
    return FileStat{
        kind,
        static_cast<std::uintmax_t>(status.st_size),
        std::chrono::file_clock::from_sys(systemTime),
        {static_cast<std::uint64_t>(status.st_dev),
         {static_cast<std::uint64_t>(status.st_ino), 0}}};
}

ExclusiveFileLock::ExclusiveFileLock(std::intptr_t nativeHandle) noexcept
    : nativeHandle_(nativeHandle) {}

ExclusiveFileLock::~ExclusiveFileLock() {
    closeNoexcept(nativeHandle_);
}

ExclusiveFileLock::ExclusiveFileLock(ExclusiveFileLock&& other) noexcept
    : nativeHandle_(std::exchange(other.nativeHandle_, -1)) {}

ExclusiveFileLock& ExclusiveFileLock::operator=(ExclusiveFileLock&& other) noexcept {
    if (this != &other) {
        closeNoexcept(nativeHandle_);
        nativeHandle_ = std::exchange(other.nativeHandle_, -1);
    }
    return *this;
}

std::optional<ExclusiveFileLock> tryLockFile(
    const std::filesystem::path& path) {
    const int descriptor = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        throwErrno("open lock file", path);
    }
    if (fchmod(descriptor, 0600) != 0) {
        const int saved = errno;
        close(descriptor);
        errno = saved;
        throwErrno("set lock permissions", path);
    }
    if (flock(descriptor, LOCK_EX | LOCK_NB) == 0) {
        return ExclusiveFileLock{descriptor};
    }
    const int saved = errno;
    close(descriptor);
    if (saved == EWOULDBLOCK || saved == EAGAIN) {
        return std::nullopt;
    }
    errno = saved;
    throwErrno("acquire file lock", path);
}

void setOwnerOnlyPermissions(const std::filesystem::path& path) {
    struct stat status {};
    if (stat(path.c_str(), &status) != 0) {
        throwErrno("read permissions", path);
    }
    const mode_t mode = S_ISDIR(status.st_mode) ? 0700 : 0600;
    if (chmod(path.c_str(), mode) != 0) {
        throwErrno("set owner-only permissions", path);
    }
}

std::filesystem::path userCacheRoot(std::string_view applicationName) {
    const auto validation = validateWorkspaceRelativePath(
        applicationName, PathSyntax::Linux);
    if (!validation.valid() || applicationName.find('/') != std::string_view::npos) {
        throw std::invalid_argument("cache application name must be one valid component");
    }

    std::filesystem::path base;
    if (const char* xdg = std::getenv("XDG_CACHE_HOME");
        xdg != nullptr && *xdg != '\0' && std::filesystem::path{xdg}.is_absolute()) {
        base = xdg;
    } else if (const char* home = std::getenv("HOME");
               home != nullptr && *home != '\0') {
        base = std::filesystem::path{home} / ".cache";
    } else {
        throw std::runtime_error("cannot resolve user cache root: HOME is unset");
    }
    return base / std::filesystem::path{applicationName};
}

std::filesystem::path userConfigRoot(std::string_view applicationName) {
    const auto validation = validateWorkspaceRelativePath(
        applicationName, PathSyntax::Linux);
    if (!validation.valid() || applicationName.find('/') != std::string_view::npos) {
        throw std::invalid_argument("config application name must be one valid component");
    }

    std::filesystem::path base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME");
        xdg != nullptr && *xdg != '\0' && std::filesystem::path{xdg}.is_absolute()) {
        base = xdg;
    } else if (const char* home = std::getenv("HOME");
               home != nullptr && *home != '\0') {
        base = std::filesystem::path{home} / ".config";
    } else {
        throw std::runtime_error("cannot resolve user config root: HOME is unset");
    }
    return base / std::filesystem::path{applicationName};
}

std::filesystem::path userStateRoot(std::string_view applicationName) {
    const auto validation = validateWorkspaceRelativePath(
        applicationName, PathSyntax::Linux);
    if (!validation.valid() || applicationName.find('/') != std::string_view::npos) {
        throw std::invalid_argument("state application name must be one valid component");
    }

    std::filesystem::path base;
    if (const char* xdg = std::getenv("XDG_STATE_HOME");
        xdg != nullptr && *xdg != '\0' && std::filesystem::path{xdg}.is_absolute()) {
        base = xdg;
    } else if (const char* home = std::getenv("HOME");
               home != nullptr && *home != '\0') {
        base = std::filesystem::path{home} / ".local" / "state";
    } else {
        throw std::runtime_error("cannot resolve user state root: HOME is unset");
    }
    return base / std::filesystem::path{applicationName};
}

void replaceFileAtomically(const std::filesystem::path& target,
                             std::span<const std::byte> contents) {
    mode_t mode = 0600;
    struct stat status {};
    if (stat(target.c_str(), &status) == 0) {
        mode = status.st_mode & 0777;
    } else if (errno != ENOENT) {
        throwErrno("read replacement target permissions", target);
    }

    TemporaryFile temporary(target, mode);
    std::size_t written = 0;
    while (written < contents.size()) {
        const auto count =
            write(temporary.descriptor(), contents.data() + written,
                  contents.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throwErrno("write replacement temporary file", target);
        }
        if (count == 0) {
            throw std::runtime_error("replacement write made no progress: " +
                                     target.string());
        }
        written += static_cast<std::size_t>(count);
    }
    if (fsync(temporary.descriptor()) != 0) {
        throwErrno("flush replacement temporary file", target);
    }
    temporary.closeForPublish();
    if (rename(temporary.path().c_str(), target.c_str()) != 0) {
        throwErrno("publish replacement file", target);
    }
    temporary.published();

    const auto parent = target.parent_path().empty()
                            ? std::filesystem::path{"."}
                            : target.parent_path();
    const int directory =
        open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        throwErrno("open replacement directory", parent);
    }
    if (fsync(directory) != 0) {
        const int saved = errno;
        close(directory);
        errno = saved;
        throwErrno("flush replacement directory", parent);
    }
    close(directory);
}

namespace {

FileIoStatus statusForErrno(int code) noexcept {
    switch (code) {
        case ENOENT:
            return FileIoStatus::NotFound;
        case EEXIST:
        // renameat2 reports a non-empty existing destination this way, which is
        // still "the name is taken". ENOTDIR is deliberately NOT here: a path
        // component that is not a directory is a structural error, and calling
        // it NotFound would let callers that treat absence as benign swallow it.
        case ENOTEMPTY:
            return FileIoStatus::AlreadyExists;
        default:
            return FileIoStatus::IoError;
    }
}

std::string describeErrno(int code, std::string_view operation,
                          const std::filesystem::path& path) {
    return std::string(operation) + ": " + path.string() + ": " +
           std::generic_category().message(code);
}

FileIoResult errnoFailure(int code, std::string_view operation,
                          const std::filesystem::path& path) {
    return {statusForErrno(code), describeErrno(code, operation, path)};
}

// fsync of the directory entry, so a freshly created or renamed file survives a
// crash. Bytes reaching the disk is not enough if the name pointing at them has
// not.
[[nodiscard]] FileIoResult syncParentDirectory(
    const std::filesystem::path& target) {
    const auto parent = target.parent_path().empty()
                            ? std::filesystem::path{"."}
                            : target.parent_path();
    const int directory =
        open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        return errnoFailure(errno, "open parent directory", parent);
    }
    if (fsync(directory) != 0) {
        const int saved = errno;
        close(directory);
        return errnoFailure(saved, "flush parent directory", parent);
    }
    close(directory);
    return {FileIoStatus::Ok, {}};
}

[[nodiscard]] FileIoResult writeAll(int descriptor,
                                    std::span<const std::byte> contents,
                                    const std::filesystem::path& target) {
    std::size_t written = 0;
    while (written < contents.size()) {
        const auto count = write(descriptor, contents.data() + written,
                                 contents.size() - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            return errnoFailure(errno, "write file", target);
        }
        if (count == 0) {
            return {FileIoStatus::IoError,
                    "write made no progress: " + target.string()};
        }
        written += static_cast<std::size_t>(count);
    }
    return {FileIoStatus::Ok, {}};
}

// Removes a file we created, but ONLY if the name still refers to the very file
// our descriptor holds. Unlinking by name alone would delete whatever now sits
// at that path -- possibly another process's file, created after ours was
// replaced. Comparing device+inode makes the cleanup refuse to destroy a
// stranger's data.
void unlinkIfStillOurs(int descriptor,
                       const std::filesystem::path& path) noexcept {
    struct stat byDescriptor {};
    struct stat byName {};
    if (fstat(descriptor, &byDescriptor) != 0) return;
    if (stat(path.c_str(), &byName) != 0) return;
    if (byDescriptor.st_dev != byName.st_dev ||
        byDescriptor.st_ino != byName.st_ino) {
        return;
    }
    unlink(path.c_str());
}

} // namespace

FileReadResult readFile(const std::filesystem::path& path) {

    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        const auto failure = errnoFailure(errno, "open file for reading", path);
        return {failure.status, {}, failure.message};
    }

    struct stat status {};
    if (fstat(descriptor, &status) != 0) {
        const int saved = errno;
        close(descriptor);
        const auto failure = errnoFailure(saved, "stat file for reading", path);
        return {failure.status, {}, failure.message};
    }
    if (S_ISDIR(status.st_mode)) {
        close(descriptor);
        return {FileIoStatus::IoError, {},
                "path is a directory, not a file: " + path.string()};
    }

    std::vector<std::uint8_t> bytes;
    if (S_ISREG(status.st_mode) && status.st_size > 0) {
        bytes.reserve(static_cast<std::size_t>(status.st_size));
    }

    // Read to EOF in chunks: size from the stat hint but never trust it, since
    // the file may grow or shrink under us.
    std::array<std::uint8_t, 64 * 1024> buffer{};
    for (;;) {
        const auto count = read(descriptor, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            const int saved = errno;
            close(descriptor);
            const auto failure = errnoFailure(saved, "read file", path);
            return {failure.status, {}, failure.message};
        }
        if (count == 0) break;
        bytes.insert(bytes.end(), buffer.data(),
                     buffer.data() + static_cast<std::size_t>(count));
    }
    close(descriptor);
    return {FileIoStatus::Ok, std::move(bytes), {}};
}

FileIoResult createFileExclusively(const std::filesystem::path& target,
                                   std::span<const std::byte> contents) {

    // O_EXCL is what makes the clash rule race-free: the kernel, not this
    // process, decides whether the name was already taken.
    const int descriptor =
        open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return errnoFailure(errno, "create file", target);
    }

    // Cleanup happens while the descriptor is still open so the file can be
    // identified, never by name alone.
    if (auto written = writeAll(descriptor, contents, target); !written.ok()) {
        unlinkIfStillOurs(descriptor, target);
        close(descriptor);
        return written;
    }
    if (fsync(descriptor) != 0) {
        const int saved = errno;
        unlinkIfStillOurs(descriptor, target);
        close(descriptor);
        return errnoFailure(saved, "flush created file", target);
    }
    if (close(descriptor) != 0) {
        return errnoFailure(errno, "close created file", target);
    }
    return syncParentDirectory(target);
}

FileIoResult appendFileDurably(const std::filesystem::path& target,
                               std::span<const std::byte> contents) {

    bool created = false;
    int descriptor =
        open(target.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC,
             0600);
    if (descriptor >= 0) {
        created = true;
    } else if (errno == EEXIST) {
        descriptor = open(target.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    }
    if (descriptor < 0) {
        return errnoFailure(errno, "open file for append", target);
    }
    if (auto written = writeAll(descriptor, contents, target); !written.ok()) {
        close(descriptor);
        return written;
    }
    if (fsync(descriptor) != 0) {
        const int saved = errno;
        close(descriptor);
        return errnoFailure(saved, "flush appended file", target);
    }
    if (close(descriptor) != 0) {
        return errnoFailure(errno, "close appended file", target);
    }
    return created ? syncParentDirectory(target)
                   : FileIoResult{FileIoStatus::Ok, {}};
}

FileIoResult syncFile(const std::filesystem::path& path) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return errnoFailure(errno, "open file for flush", path);
    }
    if (fsync(descriptor) != 0) {
        const int saved = errno;
        close(descriptor);
        return errnoFailure(saved, "flush file", path);
    }
    if (close(descriptor) != 0) {
        return errnoFailure(errno, "close flushed file", path);
    }
    return {FileIoStatus::Ok, {}};
}

FileIoResult renamePathDurably(const std::filesystem::path& source,
                               const std::filesystem::path& destination) {
    if (rename(source.c_str(), destination.c_str()) != 0) {
        return errnoFailure(errno, "rename path", destination);
    }
    if (auto result = syncParentDirectory(source); !result.ok()) {
        return result;
    }
    if (source.parent_path().lexically_normal() !=
        destination.parent_path().lexically_normal()) {
        return syncParentDirectory(destination);
    }
    return {FileIoStatus::Ok, {}};
}

FileIoResult renameFileNoClobber(const std::filesystem::path& source,
                                 const std::filesystem::path& destination) {

    // renameat2 with RENAME_NOREPLACE lets the kernel enforce non-replacement
    // atomically. It has existed since Linux 3.15, so the fallback below is
    // reached only on an ancient kernel or a filesystem that does not implement
    // it.
#if defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
    if (syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD,
                destination.c_str(), RENAME_NOREPLACE) == 0) {
        return syncParentDirectory(destination);
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return errnoFailure(errno, "rename file", destination);
    }
#endif
    // Fallback: link() fails with EEXIST on an occupied destination, so the
    // exclusion still lives in the kernel rather than in a check performed
    // here. It is NOT atomic and it cannot cross filesystems (EXDEV) or
    // preserve symlink identity, so it is strictly a last resort.
    if (link(source.c_str(), destination.c_str()) != 0) {
        return errnoFailure(errno, "rename file", destination);
    }
    if (unlink(source.c_str()) != 0) {
        // Both names now exist. Leaving the duplicate is the safe outcome:
        // unlinking the destination to "roll back" could delete a file another
        // process put there, and losing data is worse than an extra copy the
        // caller is told about.
        return {statusForErrno(errno),
                "renamed file left duplicated because the source could not be "
                "removed: " +
                    source.string() + ": " +
                    std::generic_category().message(errno)};
    }
    return syncParentDirectory(destination);
}

FileIoResult createDirectoriesDurably(const std::filesystem::path& path) {
    std::error_code error;
    std::vector<std::filesystem::path> missing;
    auto current = path;
    while (!current.empty() && !std::filesystem::exists(current, error)) {
        if (error) {
            return {statusForErrno(error.value()), error.message()};
        }
        missing.push_back(current);
        current = current.parent_path();
    }
    if (error) return {statusForErrno(error.value()), error.message()};
    if (missing.empty()) {
        return {FileIoStatus::AlreadyExists, "directory already exists"};
    }
    bool createdLeaf = false;
    for (auto iterator = missing.rbegin(); iterator != missing.rend();
         ++iterator) {
        const bool created = std::filesystem::create_directory(*iterator, error);
        if (error) return {statusForErrno(error.value()), error.message()};
        if (*iterator == path) createdLeaf = created;
    }
    for (const auto& directory : missing) {
        if (const auto synced = syncDirectory(directory); !synced.ok()) {
            return synced;
        }
    }
    if (!current.empty()) {
        if (const auto synced = syncDirectory(current); !synced.ok()) {
            return synced;
        }
    }
    return createdLeaf
               ? FileIoResult{FileIoStatus::Ok, {}}
               : FileIoResult{FileIoStatus::AlreadyExists,
                              "directory already exists"};
}

FileIoResult removeTree(const std::filesystem::path& path) {
    std::error_code error;
    const auto removed = std::filesystem::remove_all(path, error);
    if (error) return {statusForErrno(error.value()), error.message()};
    if (removed == 0) return {FileIoStatus::NotFound, "path not found"};
    return {FileIoStatus::Ok, {}};
}

DirectoryListResult listDirectory(const std::filesystem::path& path,
                                  DirectoryTraversal traversal,
                                  std::size_t maximumEntries) {
    DirectoryListResult result{FileIoStatus::Ok, {}, true, {}};
    std::error_code error;
    const auto append = [&](const auto& entry) {
        if (result.entries.size() == maximumEntries) {
            result.complete = false;
            return false;
        }
        result.entries.push_back(entry);
        return true;
    };
    if (traversal == DirectoryTraversal::Children) {
        std::filesystem::directory_iterator current{
            path, std::filesystem::directory_options::skip_permission_denied,
            error};
        const std::filesystem::directory_iterator end;
        if (error) {
            return {statusForErrno(error.value()), {}, false, error.message()};
        }
        for (; current != end; current.increment(error)) {
            if (error) {
                result.complete = false;
                if (result.message.empty()) result.message = error.message();
                error.clear();
                continue;
            }
            if (!append(*current)) break;
        }
    } else {
        std::filesystem::recursive_directory_iterator current{
            path, std::filesystem::directory_options::skip_permission_denied,
            error};
        const std::filesystem::recursive_directory_iterator end;
        if (error) {
            return {statusForErrno(error.value()), {}, false, error.message()};
        }
        for (; current != end; current.increment(error)) {
            if (error) {
                result.complete = false;
                if (result.message.empty()) result.message = error.message();
                error.clear();
                continue;
            }
            if (!append(*current)) break;
        }
    }
    return result;
}

FileIoResult syncDirectory(const std::filesystem::path& path) {

    const int directory = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        return errnoFailure(errno, "open directory for flush", path);
    }
    if (fsync(directory) != 0) {
        const int saved = errno;
        close(directory);
        return errnoFailure(saved, "flush directory", path);
    }
    close(directory);
    return {FileIoStatus::Ok, {}};
}

FileIoResult copyFileDurably(const std::filesystem::path& source,
                             const std::filesystem::path& destination) {

    auto contents = readFile(source);
    if (!contents.ok()) {
        return {contents.status, contents.message};
    }
    return createFileExclusively(
        destination,
        std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(contents.bytes.data()),
            contents.bytes.size()});
}

} // namespace ssg
