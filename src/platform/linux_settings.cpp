#include "ssg/Settings.h"

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
    const std::filesystem::path& userConfigurationRoot,
    const std::filesystem::path& workspaceStorageRoot,
    std::string_view canonicalWorkspace) {
    if (userConfigurationRoot.empty() || workspaceStorageRoot.empty() ||
        canonicalWorkspace.empty()) {
        throw std::invalid_argument("Linux settings roots and workspace must not be empty");
    }
    return {userConfigurationRoot / "settings.v1",
            workspaceStorageRoot / workspaceKey(canonicalWorkspace) / "settings.v1"};
}

} // namespace ssg
