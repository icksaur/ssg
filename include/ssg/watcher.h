#pragma once

#include "ssg/platform_files.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ssg {

using WatchClock = std::chrono::steady_clock;
using WatchTimePoint = WatchClock::time_point;

enum class WatchEventKind {
    Create,
    Modify,
    Rename,
    Remove,
    Overflow,
};

enum class WatchEventOrigin {
    External,
    SsgSave,
};

struct WatchFileState {
    FileIdentity identity;
    std::uint64_t size = 0;
    std::int64_t modification_time = 0;

    friend bool operator==(const WatchFileState&, const WatchFileState&) = default;
};

struct WatchEvent {
    WatchEventKind kind = WatchEventKind::Overflow;
    std::filesystem::path path;
    std::optional<std::filesystem::path> previous_path;
    std::optional<FileIdentity> identity;
    std::uint64_t sequence = 0;
    std::optional<std::uint64_t> size;
    std::optional<std::int64_t> modification_time;
    WatchEventOrigin origin = WatchEventOrigin::External;

    friend bool operator==(const WatchEvent&, const WatchEvent&) = default;
};

struct SaveExpectation {
    std::filesystem::path path;
    WatchFileState state;

    friend bool operator==(const SaveExpectation&, const SaveExpectation&) = default;
};

struct WatcherConfig {
    std::chrono::milliseconds debounce{50};
    std::chrono::milliseconds rescan_retry{1000};
    std::size_t max_queued_events = 1024;
    std::size_t max_pending_renames = 256;
    std::size_t max_save_expectations = 256;
    std::size_t max_rescan_entries = 100'000;
};

enum class NativeWatchAction {
    Create,
    Modify,
    Remove,
    RenameFrom,
    RenameTo,
    Overflow,
};

struct NativeWatchEvent {
    NativeWatchAction action = NativeWatchAction::Overflow;
    std::filesystem::path path;
    std::uint64_t rename_token = 0;
    std::optional<WatchFileState> observed;
};

struct WorkspaceEntry {
    std::filesystem::path path;
    WatchFileState state;

    friend bool operator==(const WorkspaceEntry&, const WorkspaceEntry&) = default;
};

struct WorkspaceScan {
    std::vector<WorkspaceEntry> entries;
    bool complete = true;
};

using WorkspaceScanner = std::function<WorkspaceScan(std::size_t max_entries)>;

class WatchEventNormalizer {
public:
    WatchEventNormalizer(WatcherConfig config,
                         std::vector<WorkspaceEntry> initial_entries,
                         WorkspaceScanner scanner);
    ~WatchEventNormalizer();
    WatchEventNormalizer(WatchEventNormalizer&&) noexcept;
    WatchEventNormalizer& operator=(WatchEventNormalizer&&) noexcept;

    WatchEventNormalizer(const WatchEventNormalizer&) = delete;
    WatchEventNormalizer& operator=(const WatchEventNormalizer&) = delete;

    void register_save(SaveExpectation expectation);
    void push(NativeWatchEvent event, WatchTimePoint observed_at);
    [[nodiscard]] std::vector<WatchEvent> take_ready(WatchTimePoint now);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class FilesystemWatcher {
public:
    virtual ~FilesystemWatcher() = default;

    virtual void register_save(SaveExpectation expectation) = 0;
    [[nodiscard]] virtual std::vector<WatchEvent> poll(
        std::chrono::milliseconds timeout) = 0;
};

[[nodiscard]] std::unique_ptr<FilesystemWatcher>
make_platform_filesystem_watcher(const std::filesystem::path& canonical_root,
                                 WatcherConfig config = {});

} // namespace ssg
