#include "ssg/GitMetadataWatcher.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace ssg {
namespace {

std::system_error windowsError(const char* operation) {
    return std::system_error(
        static_cast<int>(::GetLastError()), std::system_category(), operation);
}

class WindowsGitMetadataWatcher final : public GitMetadataWatcher {
public:
    explicit WindowsGitMetadataWatcher(
        std::vector<std::filesystem::path> directories) {
        replaceDirectories(std::move(directories));
    }

    ~WindowsGitMetadataWatcher() override { clear(); }

    bool poll(std::chrono::milliseconds timeout) override {
        if (timeout.count() < 0) {
            throw std::invalid_argument("Git metadata watcher timeout must be non-negative");
        }
        if (watches_.empty()) return false;
        std::vector<HANDLE> events;
        events.reserve(watches_.size());
        for (const auto& watch : watches_) events.push_back(watch.event);
        const auto bounded = static_cast<DWORD>(std::min<std::int64_t>(
            timeout.count(), std::numeric_limits<DWORD>::max() - 1));
        const auto wait = ::WaitForMultipleObjects(
            static_cast<DWORD>(events.size()), events.data(), FALSE, bounded);
        if (wait == WAIT_TIMEOUT) return false;
        if (wait == WAIT_FAILED) throw windowsError("failed to poll Git metadata watcher");
        bool dirty = false;
        for (auto& watch : watches_) {
            DWORD bytes = 0;
            if (!::GetOverlappedResult(watch.directory, &watch.overlapped, &bytes,
                                       FALSE)) {
                const auto error = ::GetLastError();
                if (error == ERROR_IO_INCOMPLETE) continue;
                if (error == ERROR_NOTIFY_ENUM_DIR) {
                    healthy_ = false;
                    dirty = true;
                }
                else throw windowsError("failed to read Git metadata watcher");
            } else if (bytes == 0) {
                healthy_ = false;
                dirty = true;
            } else {
                dirty = dirty || relevant(watch, bytes);
            }
            start(watch);
        }
        return dirty;
    }

    bool healthy() const noexcept override { return healthy_; }

    void replaceDirectories(
        std::vector<std::filesystem::path> directories) override {
        clear();
        try {
            for (auto& directory : directories) {
                directory = std::filesystem::canonical(directory);
                add(directory, false);
                if (std::filesystem::is_directory(directory / "refs")) {
                    add(directory / "refs", true);
                }
            }
            healthy_ = true;
        } catch (...) {
            clear();
            throw;
        }
    }

private:
    struct Watch {
        HANDLE directory = INVALID_HANDLE_VALUE;
        HANDLE event = nullptr;
        OVERLAPPED overlapped{};
        std::vector<std::byte> buffer{64 * 1024};
        bool refs = false;
    };

    void clear() {
        for (auto& watch : watches_) {
            (void)::CancelIoEx(watch.directory, &watch.overlapped);
            if (watch.event) (void)::CloseHandle(watch.event);
            if (watch.directory != INVALID_HANDLE_VALUE) {
                (void)::CloseHandle(watch.directory);
            }
        }
        watches_.clear();
    }

    void add(const std::filesystem::path& path, bool refs) {
        watches_.emplace_back();
        auto& watch = watches_.back();
        watch.refs = refs;
        watch.directory = ::CreateFileW(
            path.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (watch.directory == INVALID_HANDLE_VALUE) {
            throw windowsError("failed to open Git metadata directory");
        }
        watch.event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!watch.event) {
            (void)::CloseHandle(watch.directory);
            watches_.pop_back();
            throw windowsError("failed to create Git metadata event");
        }
        start(watch);
    }

    static void start(Watch& watch) {
        watch.overlapped = {};
        watch.overlapped.hEvent = watch.event;
        ::ResetEvent(watch.event);
        const auto filter = FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
            FILE_NOTIFY_CHANGE_SIZE;
        if (!::ReadDirectoryChangesW(
                watch.directory, watch.buffer.data(),
                static_cast<DWORD>(watch.buffer.size()), watch.refs, filter,
                nullptr, &watch.overlapped, nullptr)) {
            throw windowsError("failed to arm Git metadata watcher");
        }
    }

    static bool relevant(const Watch& watch, DWORD bytes) {
        const auto* current =
            reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(watch.buffer.data());
        for (DWORD offset = 0; offset < bytes;) {
            const std::filesystem::path path{
                std::wstring{current->FileName,
                             current->FileNameLength / sizeof(wchar_t)}};
            if (watch.refs || path == "HEAD" || path == "index" ||
                path == "packed-refs" || path == "commondir") {
                return true;
            }
            if (current->NextEntryOffset == 0) break;
            offset += current->NextEntryOffset;
            current = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                reinterpret_cast<const std::byte*>(current) +
                current->NextEntryOffset);
        }
        return false;
    }

    std::deque<Watch> watches_;
    bool healthy_ = true;
};

}  // namespace

std::unique_ptr<GitMetadataWatcher> makePlatformGitMetadataWatcher(
    std::vector<std::filesystem::path> directories) {
    return std::make_unique<WindowsGitMetadataWatcher>(std::move(directories));
}

}  // namespace ssg
