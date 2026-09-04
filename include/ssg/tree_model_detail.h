#pragma once

#include <ssg/TreeModel.h>

#include <filesystem>
#include <optional>
#include <system_error>

namespace ssg::detail {

// Internal inspection seam used to reproduce an enumeration/stat race
// deterministically without exposing test hooks in the public tree API.
[[nodiscard]] bool filesystemTreeEntryDisappeared(
    const std::error_code& error) noexcept;

[[nodiscard]] std::optional<TreeNode> inspectFilesystemTreeEntry(
    const TreeProviderId& providerId, const std::filesystem::path& root,
    const std::filesystem::directory_entry& entry);

}  // namespace ssg::detail
