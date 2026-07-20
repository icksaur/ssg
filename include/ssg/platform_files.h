#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace ssg {

enum class PathSyntax {
    Linux,
    Windows,
};

enum class LongPathPolicy {
    Legacy,
    Extended,
};

enum class PathError {
    None,
    Empty,
    Absolute,
    Traversal,
    InvalidUtf8,
    InvalidCharacter,
    ReservedName,
    TrailingDotOrSpace,
    ComponentTooLong,
    PathTooLong,
};

struct PathValidation {
    PathError error = PathError::None;
    std::size_t componentIndex = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return error == PathError::None;
    }
};

[[nodiscard]] PathValidation validateWorkspaceRelativePath(
    std::string_view path,
    PathSyntax syntax,
    LongPathPolicy longPaths = LongPathPolicy::Legacy) noexcept;

struct FileIdentity {
    std::uint64_t volume = 0;
    std::array<std::uint64_t, 2> file{};

    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

[[nodiscard]] FileIdentity fileIdentity(const std::filesystem::path& path);

class ExclusiveFileLock {
public:
    ~ExclusiveFileLock();
    ExclusiveFileLock(ExclusiveFileLock&& other) noexcept;
    ExclusiveFileLock& operator=(ExclusiveFileLock&& other) noexcept;

    ExclusiveFileLock(const ExclusiveFileLock&) = delete;
    ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;

private:
    explicit ExclusiveFileLock(std::intptr_t nativeHandle) noexcept;
    friend std::optional<ExclusiveFileLock> tryLockFile(
        const std::filesystem::path&);

    std::intptr_t nativeHandle_ = -1;
};

// Returns no value only when another live handle owns the lock. Other failures
// throw std::system_error so contention cannot hide an I/O or permission error.
[[nodiscard]] std::optional<ExclusiveFileLock> tryLockFile(
    const std::filesystem::path& path);

void setOwnerOnlyPermissions(const std::filesystem::path& path);

[[nodiscard]] std::filesystem::path userCacheRoot(
    std::string_view applicationName);

void replaceFileAtomically(
    const std::filesystem::path& target,
    std::span<const std::byte> contents);

} // namespace ssg
