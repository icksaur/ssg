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
    linux,
    windows,
};

enum class LongPathPolicy {
    legacy,
    extended,
};

enum class PathError {
    none,
    empty,
    absolute,
    traversal,
    invalid_utf8,
    invalid_character,
    reserved_name,
    trailing_dot_or_space,
    component_too_long,
    path_too_long,
};

struct PathValidation {
    PathError error = PathError::none;
    std::size_t component_index = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return error == PathError::none;
    }
};

[[nodiscard]] PathValidation validate_workspace_relative_path(
    std::string_view path,
    PathSyntax syntax,
    LongPathPolicy long_paths = LongPathPolicy::legacy) noexcept;

struct FileIdentity {
    std::uint64_t volume = 0;
    std::array<std::uint64_t, 2> file{};

    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

[[nodiscard]] FileIdentity file_identity(const std::filesystem::path& path);

class ExclusiveFileLock {
public:
    ~ExclusiveFileLock();
    ExclusiveFileLock(ExclusiveFileLock&& other) noexcept;
    ExclusiveFileLock& operator=(ExclusiveFileLock&& other) noexcept;

    ExclusiveFileLock(const ExclusiveFileLock&) = delete;
    ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;

private:
    explicit ExclusiveFileLock(std::intptr_t native_handle) noexcept;
    friend std::optional<ExclusiveFileLock> try_lock_file(
        const std::filesystem::path&);

    std::intptr_t native_handle_ = -1;
};

// Returns no value only when another live handle owns the lock. Other failures
// throw std::system_error so contention cannot hide an I/O or permission error.
[[nodiscard]] std::optional<ExclusiveFileLock> try_lock_file(
    const std::filesystem::path& path);

void set_owner_only_permissions(const std::filesystem::path& path);

[[nodiscard]] std::filesystem::path user_cache_root(
    std::string_view application_name);

void replace_file_atomically(
    const std::filesystem::path& target,
    std::span<const std::byte> contents);

} // namespace ssg
