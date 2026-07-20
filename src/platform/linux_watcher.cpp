#include "ssg/watcher.h"

#include <ssg/startup_audit.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/inotify.h>
#include <unistd.h>
#include <vector>

namespace ssg {
namespace {

std::int64_t modificationTime(const std::filesystem::path& path,
                               std::error_code& error) {
    const auto value = std::filesystem::last_write_time(path, error);
    if (error) {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               value.time_since_epoch()).count();
}

std::optional<WatchFileState> observe(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || status.type() == std::filesystem::file_type::not_found) {
        return std::nullopt;
    }
    std::uint64_t size = 0;
    if (std::filesystem::is_regular_file(status)) {
        size = std::filesystem::file_size(path, error);
        if (error) {
            return std::nullopt;
        }
    }
    const auto modified = modificationTime(path, error);
    if (error) {
        return std::nullopt;
    }
    try {
        return WatchFileState{fileIdentity(path), size, modified};
    } catch (const std::filesystem::filesystem_error&) {
        return std::nullopt;
    } catch (const std::system_error&) {
        return std::nullopt;
    }
}

WorkspaceScan scanWorkspace(const std::filesystem::path& root,
                             std::size_t maximum) {
    WorkspaceScan result;
    std::error_code error;
    std::filesystem::recursive_directory_iterator current(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        return {{}, false};
    }
    for (; current != end; current.increment(error)) {
        if (error) {
            result.complete = false;
            error.clear();
            continue;
        }
        const auto status = current->symlink_status(error);
        if (error) {
            result.complete = false;
            error.clear();
            continue;
        }
        if (std::filesystem::is_symlink(status)) {
            if (std::filesystem::is_directory(status)) {
                current.disable_recursion_pending();
            }
            continue;
        }
        if (result.entries.size() == maximum) {
            return {{}, false};
        }
        if (auto state = observe(current->path())) {
            result.entries.push_back(
                {current->path().lexically_relative(root), *state});
        } else {
            result.complete = false;
        }
    }
    return result;
}

class LinuxFilesystemWatcher final : public FilesystemWatcher {
public:
    LinuxFilesystemWatcher(std::filesystem::path root, WatcherConfig config)
        : root_(std::filesystem::canonical(std::move(root))),
          max_rescan_entries_(config.max_rescan_entries) {
        noteOptionalConstruction(OptionalSubsystem::FilesystemWatcher);
        descriptor_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (descriptor_ == -1) {
            throw std::system_error(errno, std::generic_category(),
                                    "failed to create inotify watcher");
        }
        try {
            addWatchTree(root_);
            const auto initial = scanWorkspace(root_, config.max_rescan_entries);
            if (!initial.complete) {
                throw std::runtime_error(
                    "failed to seed bounded filesystem watcher snapshot");
            }
            normalizer_ = std::make_unique<WatchEventNormalizer>(
                config, initial.entries,
                [this](std::size_t maximum) {
                    return rescanWorkspace(maximum);
                });
        } catch (...) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw;
        }
    }

    ~LinuxFilesystemWatcher() override {
        if (descriptor_ != -1) {
            ::close(descriptor_);
        }
    }

    void registerSave(SaveExpectation expectation) override {
        normalizer_->registerSave(std::move(expectation));
    }

    std::vector<WatchEvent> poll(std::chrono::milliseconds timeout) override {
        if (timeout.count() < 0) {
            throw std::invalid_argument("watcher poll timeout must be non-negative");
        }
        const auto bounded = std::min<std::int64_t>(
            timeout.count(), std::numeric_limits<int>::max());
        pollfd request{descriptor_, POLLIN, 0};
        const auto ready = ::poll(&request, 1, static_cast<int>(bounded));
        if (ready == -1 && errno != EINTR) {
            throw std::system_error(errno, std::generic_category(),
                                    "failed to poll inotify watcher");
        }
        if (ready > 0) {
            readNativeEvents();
        }
        return normalizer_->takeReady(WatchClock::now());
    }

private:
    static constexpr std::uint32_t kWatchMask =
        IN_CREATE | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE |
        IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;

    void addWatch(const std::filesystem::path& directory) {
        const auto descriptor =
            ::inotify_add_watch(descriptor_, directory.c_str(), kWatchMask);
        if (descriptor == -1) {
            throw std::system_error(errno, std::generic_category(),
                                    "failed to add inotify directory watch");
        }
        directories_[descriptor] = directory;
    }

    void addWatchTree(const std::filesystem::path& directory) {
        addWatch(directory);
        std::error_code error;
        std::filesystem::recursive_directory_iterator current(
            directory,
            std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        if (error) {
            throw std::filesystem::filesystem_error(
                "failed to enumerate watcher directories", directory, error);
        }
        for (; current != end; current.increment(error)) {
            if (error) {
                throw std::filesystem::filesystem_error(
                    "failed to enumerate watcher directories", directory, error);
            }
            const auto status = current->symlink_status(error);
            if (error) {
                throw std::filesystem::filesystem_error(
                    "failed to inspect watcher directory", current->path(), error);
            }
            if (std::filesystem::is_symlink(status)) {
                current.disable_recursion_pending();
            } else if (std::filesystem::is_directory(status)) {
                addWatch(current->path());
            }
        }
    }

    WorkspaceScan rescanWorkspace(std::size_t maximum) {
        for (const auto& [descriptor, path] : directories_) {
            (void)path;
            if (::inotify_rm_watch(descriptor_, descriptor) == -1 &&
                errno != EINVAL) {
                return {{}, false};
            }
        }
        directories_.clear();
        try {
            addWatchTree(root_);
        } catch (const std::filesystem::filesystem_error&) {
            return {{}, false};
        } catch (const std::system_error&) {
            return {{}, false};
        }
        return scanWorkspace(root_, maximum);
    }

    void readNativeEvents() {
        alignas(inotify_event) std::byte buffer[64 * 1024];
        for (;;) {
            const auto count = ::read(descriptor_, buffer, sizeof(buffer));
            if (count == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                if (errno == EINTR) {
                    continue;
                }
                throw std::system_error(errno, std::generic_category(),
                                        "failed to read inotify events");
            }
            if (count == 0) {
                return;
            }
            std::size_t offset = 0;
            while (offset < static_cast<std::size_t>(count)) {
                const auto* native =
                    reinterpret_cast<const inotify_event*>(buffer + offset);
                handle(*native);
                offset += sizeof(inotify_event) + native->len;
            }
        }
    }

    void handle(const inotify_event& native) {
        if ((native.mask & IN_Q_OVERFLOW) != 0) {
            normalizer_->push(
                {NativeWatchAction::Overflow, {}, 0, {}}, WatchClock::now());
            return;
        }
        const auto directory = directories_.find(native.wd);
        if (directory == directories_.end()) {
            return;
        }
        if ((native.mask & IN_IGNORED) != 0) {
            directories_.erase(directory);
            return;
        }
        if ((native.mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
            normalizer_->push(
                {NativeWatchAction::Overflow, {}, 0, {}}, WatchClock::now());
            return;
        }
        if (native.len == 0) {
            return;
        }

        const auto absolute = directory->second / native.name;
        const auto relative = absolute.lexically_relative(root_);
        const auto now = WatchClock::now();
        const auto observed = observe(absolute);
        if ((native.mask & IN_ISDIR) != 0 &&
            (native.mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
            if ((native.mask & IN_MOVED_TO) != 0) {
                normalizer_->push(
                    {NativeWatchAction::RenameTo, relative, native.cookie,
                     observed},
                    now);
            } else {
                normalizer_->push(
                    {NativeWatchAction::Create, relative, 0, observed}, now);
            }
            try {
                addWatchTree(absolute);
                const auto subtree =
                    scanWorkspace(absolute, max_rescan_entries_);
                if (!subtree.complete) {
                    normalizer_->push(
                        {NativeWatchAction::Overflow, {}, 0, {}}, now);
                    return;
                }
                for (const auto& entry : subtree.entries) {
                    normalizer_->push(
                        {NativeWatchAction::Create,
                         (absolute / entry.path).lexically_relative(root_),
                         0, entry.state},
                        now);
                }
            } catch (const std::filesystem::filesystem_error&) {
                normalizer_->push(
                    {NativeWatchAction::Overflow, {}, 0, {}}, now);
                return;
            } catch (const std::system_error&) {
                normalizer_->push(
                    {NativeWatchAction::Overflow, {}, 0, {}}, now);
                return;
            }
            return;
        }

        if ((native.mask & IN_MOVED_FROM) != 0) {
            normalizer_->push(
                {NativeWatchAction::RenameFrom, relative, native.cookie, {}}, now);
        } else if ((native.mask & IN_MOVED_TO) != 0) {
            normalizer_->push(
                {NativeWatchAction::RenameTo, relative, native.cookie, observed},
                now);
        } else if ((native.mask & IN_CREATE) != 0) {
            normalizer_->push(
                {NativeWatchAction::Create, relative, 0, observed}, now);
        } else if ((native.mask & IN_DELETE) != 0) {
            normalizer_->push(
                {NativeWatchAction::Remove, relative, 0, {}}, now);
        } else if ((native.mask & (IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE)) != 0) {
            normalizer_->push(
                {NativeWatchAction::Modify, relative, 0, observed}, now);
        }
    }

    std::filesystem::path root_;
    int descriptor_ = -1;
    std::map<int, std::filesystem::path> directories_;
    std::unique_ptr<WatchEventNormalizer> normalizer_;
    std::size_t max_rescan_entries_;
};

} // namespace

std::unique_ptr<FilesystemWatcher> makePlatformFilesystemWatcher(
    const std::filesystem::path& canonicalRoot, WatcherConfig config) {
    return std::make_unique<LinuxFilesystemWatcher>(canonicalRoot, config);
}

} // namespace ssg
