#include "ssg/platform_files.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ssg {
namespace {

[[noreturn]] void throw_errno(std::string_view operation,
                              const std::filesystem::path& path) {
    throw std::system_error(errno, std::generic_category(),
                            std::string(operation) + ": " + path.string());
}

void close_noexcept(std::intptr_t& handle) noexcept {
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
            throw_errno("create replacement temporary file", target);
        }
        path_ = storage_.data();
        if (fchmod(descriptor_, mode) != 0) {
            const int saved = errno;
            close(descriptor_);
            descriptor_ = -1;
            unlink(path_.c_str());
            errno = saved;
            throw_errno("set replacement permissions", target);
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

    void close_for_publish() {
        if (close(descriptor_) != 0) {
            descriptor_ = -1;
            throw_errno("close replacement temporary file", path_);
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

FileIdentity file_identity(const std::filesystem::path& path) {
    struct stat status {};
    if (stat(path.c_str(), &status) != 0) {
        throw_errno("read file identity", path);
    }
    return {static_cast<std::uint64_t>(status.st_dev),
            {static_cast<std::uint64_t>(status.st_ino), 0}};
}

ExclusiveFileLock::ExclusiveFileLock(std::intptr_t native_handle) noexcept
    : native_handle_(native_handle) {}

ExclusiveFileLock::~ExclusiveFileLock() {
    close_noexcept(native_handle_);
}

ExclusiveFileLock::ExclusiveFileLock(ExclusiveFileLock&& other) noexcept
    : native_handle_(std::exchange(other.native_handle_, -1)) {}

ExclusiveFileLock& ExclusiveFileLock::operator=(ExclusiveFileLock&& other) noexcept {
    if (this != &other) {
        close_noexcept(native_handle_);
        native_handle_ = std::exchange(other.native_handle_, -1);
    }
    return *this;
}

std::optional<ExclusiveFileLock> try_lock_file(
    const std::filesystem::path& path) {
    const int descriptor = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        throw_errno("open lock file", path);
    }
    if (fchmod(descriptor, 0600) != 0) {
        const int saved = errno;
        close(descriptor);
        errno = saved;
        throw_errno("set lock permissions", path);
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
    throw_errno("acquire file lock", path);
}

void set_owner_only_permissions(const std::filesystem::path& path) {
    struct stat status {};
    if (stat(path.c_str(), &status) != 0) {
        throw_errno("read permissions", path);
    }
    const mode_t mode = S_ISDIR(status.st_mode) ? 0700 : 0600;
    if (chmod(path.c_str(), mode) != 0) {
        throw_errno("set owner-only permissions", path);
    }
}

std::filesystem::path user_cache_root(std::string_view application_name) {
    const auto validation = validate_workspace_relative_path(
        application_name, PathSyntax::Linux);
    if (!validation.valid() || application_name.find('/') != std::string_view::npos) {
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
    return base / std::filesystem::path{application_name};
}

void replace_file_atomically(const std::filesystem::path& target,
                             std::span<const std::byte> contents) {
    mode_t mode = 0600;
    struct stat status {};
    if (stat(target.c_str(), &status) == 0) {
        mode = status.st_mode & 0777;
    } else if (errno != ENOENT) {
        throw_errno("read replacement target permissions", target);
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
            throw_errno("write replacement temporary file", target);
        }
        if (count == 0) {
            throw std::runtime_error("replacement write made no progress: " +
                                     target.string());
        }
        written += static_cast<std::size_t>(count);
    }
    if (fsync(temporary.descriptor()) != 0) {
        throw_errno("flush replacement temporary file", target);
    }
    temporary.close_for_publish();
    if (rename(temporary.path().c_str(), target.c_str()) != 0) {
        throw_errno("publish replacement file", target);
    }
    temporary.published();

    const auto parent = target.parent_path().empty()
                            ? std::filesystem::path{"."}
                            : target.parent_path();
    const int directory =
        open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        throw_errno("open replacement directory", parent);
    }
    if (fsync(directory) != 0) {
        const int saved = errno;
        close(directory);
        errno = saved;
        throw_errno("flush replacement directory", parent);
    }
    close(directory);
}

} // namespace ssg
