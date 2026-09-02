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
    std::int64_t modificationTime = 0;

    [[nodiscard]] static std::optional<WatchFileState> observe(
        const std::filesystem::path& path);

    friend bool operator==(const WatchFileState&, const WatchFileState&) = default;
};

struct WatchEvent {
    WatchEventKind kind = WatchEventKind::Overflow;
    std::filesystem::path path;
    std::optional<std::filesystem::path> previousPath;
    std::optional<FileIdentity> identity;
    std::uint64_t sequence = 0;
    std::optional<std::uint64_t> size;
    std::optional<std::int64_t> modificationTime;
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
    std::chrono::milliseconds rescanRetry{1000};
    std::size_t maxQueuedEvents = 1024;
    std::size_t maxPendingRenames = 256;
    std::size_t maxSaveExpectations = 256;
    std::size_t maxRescanEntries = 100'000;
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
    std::uint64_t renameToken = 0;
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

using WorkspaceScanner = std::function<WorkspaceScan(std::size_t maxEntries)>;

class WatchEventNormalizer {
public:
    WatchEventNormalizer(WatcherConfig config,
                         std::vector<WorkspaceEntry> initialEntries,
                         WorkspaceScanner scanner);
    ~WatchEventNormalizer();
    WatchEventNormalizer(WatchEventNormalizer&&) noexcept;
    WatchEventNormalizer& operator=(WatchEventNormalizer&&) noexcept;

    WatchEventNormalizer(const WatchEventNormalizer&) = delete;
    WatchEventNormalizer& operator=(const WatchEventNormalizer&) = delete;

    void registerSave(SaveExpectation expectation);
    void push(NativeWatchEvent event, WatchTimePoint observedAt);
    [[nodiscard]] std::vector<WatchEvent> takeReady(WatchTimePoint now);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class FilesystemWatcher {
public:
    virtual ~FilesystemWatcher() = default;

    virtual void registerSave(SaveExpectation expectation) = 0;
    [[nodiscard]] virtual std::vector<WatchEvent> poll(
        std::chrono::milliseconds timeout) = 0;
};

[[nodiscard]] std::unique_ptr<FilesystemWatcher>
makePlatformFilesystemWatcher(const std::filesystem::path& canonicalRoot,
                                 WatcherConfig config = {});

} // namespace ssg
