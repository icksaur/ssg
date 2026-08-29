#include "ssg/GitMetadataWatcher.h"

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/inotify.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ssg {
namespace {

class LinuxGitMetadataWatcher final : public GitMetadataWatcher {
public:
    explicit LinuxGitMetadataWatcher(std::vector<std::filesystem::path> directories) {
        descriptor_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (descriptor_ == -1) {
            throw std::system_error(errno, std::generic_category(),
                                    "failed to create Git metadata watcher");
        }
        try {
            replaceDirectories(std::move(directories));
        } catch (...) {
            (void)::close(descriptor_);
            descriptor_ = -1;
            throw;
        }
    }

    ~LinuxGitMetadataWatcher() override {
        if (descriptor_ != -1) {
            (void)::close(descriptor_);
        }
    }

    bool poll(std::chrono::milliseconds timeout) override {
        if (timeout.count() < 0) {
            throw std::invalid_argument("Git metadata watcher timeout must be non-negative");
        }
        pollfd request{descriptor_, POLLIN, 0};
        const auto ready = ::poll(
            &request, 1, static_cast<int>(std::min<std::int64_t>(
                             timeout.count(), std::numeric_limits<int>::max())));
        if (ready == -1 && errno != EINTR) {
            throw std::system_error(errno, std::generic_category(),
                                    "failed to poll Git metadata watcher");
        }
        if (ready <= 0) {
            return false;
        }
        bool dirty = false;
        alignas(inotify_event) std::byte buffer[64 * 1024];
        for (;;) {
            const auto count = ::read(descriptor_, buffer, sizeof buffer);
            if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            if (count == -1) {
                if (errno == EINTR) continue;
                throw std::system_error(errno, std::generic_category(),
                                        "failed to read Git metadata watcher");
            }
            if (count == 0) break;
            for (std::size_t offset = 0; offset < static_cast<std::size_t>(count);) {
                const auto& event =
                    *reinterpret_cast<const inotify_event*>(buffer + offset);
                dirty = handle(event) || dirty;
                offset += sizeof(inotify_event) + event.len;
            }
        }
        return dirty;
    }

    bool healthy() const noexcept override { return healthy_; }

    void replaceDirectories(
        std::vector<std::filesystem::path> directories) override {
        for (const auto& [descriptor, _] : watches_) {
            (void)::inotify_rm_watch(descriptor_, descriptor);
        }
        watches_.clear();
        for (auto& directory : directories) {
            directory = std::filesystem::canonical(directory);
            addRoot(directory);
        }
        healthy_ = true;
    }

private:
    enum class Scope { Root, Refs };
    static constexpr std::uint32_t kWatchMask =
        IN_CREATE | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE |
        IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF |
        IN_UNMOUNT;

    void addWatch(const std::filesystem::path& directory, Scope scope) {
        const auto watch = ::inotify_add_watch(descriptor_, directory.c_str(), kWatchMask);
        if (watch == -1) {
            throw std::system_error(errno, std::generic_category(),
                                    "failed to watch Git metadata directory");
        }
        watches_.insert_or_assign(watch, std::pair{directory, scope});
    }

    void addRefs(const std::filesystem::path& directory) {
        if (!std::filesystem::is_directory(directory)) return;
        addWatch(directory, Scope::Refs);
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_directory()) addWatch(entry.path(), Scope::Refs);
        }
    }

    void addRoot(const std::filesystem::path& directory) {
        addWatch(directory, Scope::Root);
        addRefs(directory / "refs");
    }

    bool handle(const inotify_event& event) {
        if ((event.mask & IN_Q_OVERFLOW) != 0) return true;
        const auto found = watches_.find(event.wd);
        if (found == watches_.end()) return true;
        const auto [directory, scope] = found->second;
        if ((event.mask & (IN_IGNORED | IN_UNMOUNT | IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
            watches_.erase(found);
            healthy_ = false;
            return true;
        }
        if (event.len == 0) return false;
        const auto path = directory / event.name;
        if (scope == Scope::Refs) {
            if ((event.mask & IN_ISDIR) != 0 &&
                (event.mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
                try {
                    addRefs(path);
                } catch (const std::system_error&) {
                    healthy_ = false;
                    return true;
                }
            }
            return true;
        }
        const std::string_view name{event.name};
        if (name == "HEAD" || name == "index" || name == "packed-refs" ||
            name == "commondir") {
            return true;
        }
        if (name == "refs" && (event.mask & IN_ISDIR) != 0 &&
            (event.mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
            try {
                addRefs(path);
            } catch (const std::system_error&) {
                healthy_ = false;
                return true;
            }
            return true;
        }
        return false;
    }

    int descriptor_ = -1;
    std::map<int, std::pair<std::filesystem::path, Scope>> watches_;
    bool healthy_ = true;
};

}  // namespace

std::unique_ptr<GitMetadataWatcher> makePlatformGitMetadataWatcher(
    std::vector<std::filesystem::path> directories) {
    return std::make_unique<LinuxGitMetadataWatcher>(std::move(directories));
}

}  // namespace ssg
