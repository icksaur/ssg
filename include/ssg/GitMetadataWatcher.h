#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <vector>

namespace ssg {

class GitMetadataWatcher {
public:
    virtual ~GitMetadataWatcher() = default;

    // Native events may coalesce or repeat; true means relevant metadata may
    // have changed and callers must refresh authoritative repository state.
    [[nodiscard]] virtual bool poll(std::chrono::milliseconds timeout) = 0;
    [[nodiscard]] virtual bool healthy() const noexcept = 0;
    virtual void replaceDirectories(
        std::vector<std::filesystem::path> directories) = 0;
};

[[nodiscard]] std::unique_ptr<GitMetadataWatcher> makePlatformGitMetadataWatcher(
    std::vector<std::filesystem::path> directories);

}  // namespace ssg
