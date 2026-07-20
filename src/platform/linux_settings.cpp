#include "ssg/settings.h"

#include <iomanip>
#include <sstream>

namespace ssg {
namespace {

std::string workspaceKey(std::string_view workspace) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : workspace) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream key;
    key << std::hex << std::setfill('0') << std::setw(16) << hash;
    return key.str();
}

} // namespace

SettingsPaths linuxSettingsPaths(
    const std::filesystem::path& user_configuration_root,
    const std::filesystem::path& workspace_storage_root,
    std::string_view canonical_workspace) {
    if (user_configuration_root.empty() || workspace_storage_root.empty() ||
        canonical_workspace.empty()) {
        throw std::invalid_argument("Linux settings roots and workspace must not be empty");
    }
    return {user_configuration_root / "settings.v1",
            workspace_storage_root / workspaceKey(canonical_workspace) / "settings.v1"};
}

} // namespace ssg
