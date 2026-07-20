#include "ssg/FilesystemWatcher.h"

#include <ssg/startup_audit.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace ssg {
namespace {

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
    const auto time = std::filesystem::last_write_time(path, error);
    if (error) {
        return std::nullopt;
    }
    try {
        return WatchFileState{
            file_identity(path), size,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                time.time_since_epoch()).count()};
    } catch (const std::filesystem::filesystem_error&) {
        return std::nullopt;
    } catch (const std::system_error&) {
        return std::nullopt;
    }
}

WorkspaceScan scan_workspace(const std::filesystem::path& root,
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
            current.disable_recursion_pending();
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

std::system_error windows_error(const char* operation) {
    return std::system_error(
        static_cast<int>(::GetLastError()), std::system_category(), operation);
}

class WindowsFilesystemWatcher final : public FilesystemWatcher {
public:
    WindowsFilesystemWatcher(std::filesystem::path root, WatcherConfig config)
        : root_(std::filesystem::canonical(std::move(root))),
          buffer_(64 * 1024),
          max_rescan_entries_(config.max_rescan_entries) {
        note_optional_construction(OptionalSubsystem::filesystem_watcher);
        directory_ = ::CreateFileW(
            root_.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (directory_ == INVALID_HANDLE_VALUE) {
            throw windows_error("failed to open ReadDirectoryChangesW root");
        }
        event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event_ == nullptr) {
            const auto error = windows_error(
                "failed to create ReadDirectoryChangesW event");
            ::CloseHandle(directory_);
            directory_ = INVALID_HANDLE_VALUE;
            throw error;
        }
        const auto initial = scan_workspace(root_, config.max_rescan_entries);
        if (!initial.complete) {
            ::CloseHandle(event_);
            ::CloseHandle(directory_);
            throw std::runtime_error(
                "failed to seed bounded filesystem watcher snapshot");
        }
        try {
            normalizer_ = std::make_unique<WatchEventNormalizer>(
                config, initial.entries,
                [this](std::size_t maximum) {
                    return scan_workspace(root_, maximum);
                });
            start_read();
        } catch (...) {
            ::CloseHandle(event_);
            ::CloseHandle(directory_);
            event_ = nullptr;
            directory_ = INVALID_HANDLE_VALUE;
            throw;
        }
    }

    ~WindowsFilesystemWatcher() override {
        if (pending_) {
            (void)::CancelIoEx(directory_, &overlapped_);
            DWORD ignored = 0;
            (void)::GetOverlappedResult(
                directory_, &overlapped_, &ignored, TRUE);
        }
        if (event_ != nullptr) {
            ::CloseHandle(event_);
        }
        if (directory_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(directory_);
        }
    }

    void register_save(SaveExpectation expectation) override {
        normalizer_->register_save(std::move(expectation));
    }

    std::vector<WatchEvent> poll(std::chrono::milliseconds timeout) override {
        if (timeout.count() < 0) {
            throw std::invalid_argument("watcher poll timeout must be non-negative");
        }
        const auto bounded = static_cast<DWORD>(std::min<std::int64_t>(
            timeout.count(), std::numeric_limits<DWORD>::max() - 1));
        const auto wait = ::WaitForSingleObject(event_, bounded);
        if (wait == WAIT_TIMEOUT) {
            return normalizer_->take_ready(WatchClock::now());
        }
        if (wait != WAIT_OBJECT_0) {
            throw windows_error("failed waiting for ReadDirectoryChangesW");
        }

        DWORD bytes = 0;
        if (!::GetOverlappedResult(
                directory_, &overlapped_, &bytes, FALSE)) {
            const auto error = ::GetLastError();
            pending_ = false;
            if (error == ERROR_NOTIFY_ENUM_DIR) {
                pending_rename_token_.reset();
                start_read();
                normalizer_->push(
                    {NativeWatchAction::overflow, {}, 0, {}},
                    WatchClock::now());
                return normalizer_->take_ready(WatchClock::now());
            }
            throw std::system_error(
                static_cast<int>(error), std::system_category(),
                "failed to complete ReadDirectoryChangesW");
        }
        pending_ = false;
        std::vector<std::byte> completed(
            buffer_.begin(), buffer_.begin() + bytes);
        start_read();
        if (bytes == 0) {
            pending_rename_token_.reset();
            normalizer_->push(
                {NativeWatchAction::overflow, {}, 0, {}}, WatchClock::now());
        } else {
            parse(completed);
        }
        return normalizer_->take_ready(WatchClock::now());
    }

private:
    void start_read() {
        overlapped_ = {};
        overlapped_.hEvent = event_;
        ::ResetEvent(event_);
        const auto filter = FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
        if (!::ReadDirectoryChangesW(
                directory_, buffer_.data(), static_cast<DWORD>(buffer_.size()),
                TRUE, filter, nullptr, &overlapped_, nullptr)) {
            throw windows_error("failed to start ReadDirectoryChangesW");
        }
        pending_ = true;
    }

    void parse(const std::vector<std::byte>& completed) {
        const auto byte_count = static_cast<DWORD>(completed.size());
        DWORD offset = 0;
        while (offset < byte_count) {
            const auto* native =
                reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                    completed.data() + offset);
            const std::wstring name(
                native->FileName,
                native->FileNameLength / sizeof(wchar_t));
            const std::filesystem::path relative{name};
            const auto observed = observe(root_ / relative);
            const auto now = WatchClock::now();
            switch (native->Action) {
            case FILE_ACTION_ADDED:
                normalizer_->push(
                    {NativeWatchAction::create, relative, 0, observed}, now);
                push_subtree_creates(relative, now);
                break;
            case FILE_ACTION_REMOVED:
                normalizer_->push(
                    {NativeWatchAction::remove, relative, 0, {}}, now);
                break;
            case FILE_ACTION_MODIFIED:
                normalizer_->push(
                    {NativeWatchAction::modify, relative, 0, observed}, now);
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:
                pending_rename_token_ = next_rename_token_++;
                normalizer_->push(
                    {NativeWatchAction::rename_from, relative,
                     *pending_rename_token_, {}}, now);
                break;
            case FILE_ACTION_RENAMED_NEW_NAME: {
                const auto token =
                    pending_rename_token_.value_or(next_rename_token_++);
                normalizer_->push(
                    {NativeWatchAction::rename_to, relative, token, observed},
                    now);
                pending_rename_token_.reset();
                push_subtree_creates(relative, now);
                break;
            }
            default:
                normalizer_->push(
                    {NativeWatchAction::overflow, {}, 0, {}}, now);
                break;
            }
            if (native->NextEntryOffset == 0) {
                break;
            }
            offset += native->NextEntryOffset;
        }
    }

    void push_subtree_creates(const std::filesystem::path& relative,
                              WatchTimePoint now) {
        std::error_code error;
        const auto status =
            std::filesystem::symlink_status(root_ / relative, error);
        if (error || !std::filesystem::is_directory(status)) {
            return;
        }
        const auto subtree =
            scan_workspace(root_ / relative, max_rescan_entries_);
        if (!subtree.complete) {
            normalizer_->push(
                {NativeWatchAction::overflow, {}, 0, {}}, now);
            return;
        }
        for (const auto& entry : subtree.entries) {
            normalizer_->push(
                {NativeWatchAction::create, relative / entry.path, 0,
                 entry.state},
                now);
        }
    }

    std::filesystem::path root_;
    HANDLE directory_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    std::vector<std::byte> buffer_;
    OVERLAPPED overlapped_{};
    bool pending_ = false;
    std::unique_ptr<WatchEventNormalizer> normalizer_;
    std::uint64_t next_rename_token_ = 1;
    std::optional<std::uint64_t> pending_rename_token_;
    std::size_t max_rescan_entries_;
};

} // namespace

std::unique_ptr<FilesystemWatcher> make_platform_filesystem_watcher(
    const std::filesystem::path& canonical_root, WatcherConfig config) {
    return std::make_unique<WindowsFilesystemWatcher>(canonical_root, config);
}

} // namespace ssg
