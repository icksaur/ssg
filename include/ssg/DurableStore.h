#pragma once

#include <ssg/platform_files.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

struct DurableStoreEntry {
    std::filesystem::path path;
    std::optional<std::chrono::system_clock::time_point> created;
};

struct DurableStoreListResult {
    FileIoStatus status = FileIoStatus::IoError;
    std::vector<DurableStoreEntry> entries;
    bool complete = false;
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return status == FileIoStatus::Ok;
    }
};

struct DurableStoreClaimResult {
    FileIoStatus status = FileIoStatus::IoError;
    std::filesystem::path path;
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return status == FileIoStatus::Ok;
    }
};

struct DurableStoreEvictionResult {
    std::vector<DurableStoreEntry> removed;
    std::vector<DurableStoreEntry> remaining;
    bool policySatisfied = false;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return message.empty(); }
};

class DurableStore {
public:
    explicit DurableStore(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

    // Ownership is established by exclusive directory creation. The random
    // suffix only avoids retries; it is not the ownership decision.
    [[nodiscard]] DurableStoreClaimResult claimEntry(
        std::chrono::system_clock::time_point created);

    // Symlinked directories are not entries. Missing roots are reported as
    // NotFound; other listing failures never become successful empty results.
    [[nodiscard]] DurableStoreListResult entries() const;

    [[nodiscard]] static std::string entryName(
        std::chrono::system_clock::time_point created,
        std::string_view suffix);

    // Accepts canonical names and the prior decimal-nanosecond prefix used by
    // scratch and recovery so retained state survives upgrades.
    [[nodiscard]] static std::optional<
        std::chrono::system_clock::time_point>
    entryTimestamp(std::string_view name);

    // The caller selects candidates and defines satisfaction; this function
    // owns only oldest-first ordering, removal, and termination.
    [[nodiscard]] static DurableStoreEvictionResult evictOldestWhile(
        std::vector<DurableStoreEntry> candidates,
        const std::function<bool(std::span<const DurableStoreEntry>)>&
            policySatisfied);

private:
    std::filesystem::path root_;
};

} // namespace ssg
