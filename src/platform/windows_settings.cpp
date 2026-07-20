#include "ssg/settings.h"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace ssg {
namespace {

std::string workspaceKey(std::string_view workspace) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : workspace) {
        if (byte == '\\') byte = '/';
        byte = static_cast<unsigned char>(std::tolower(byte));
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream key;
    key << std::hex << std::setfill('0') << std::setw(16) << hash;
    return key.str();
}

} // namespace

SettingsPaths windowsSettingsPaths(
    const std::filesystem::path& userConfigurationRoot,
    const std::filesystem::path& workspaceStorageRoot,
    std::string_view canonicalWorkspace) {
    if (userConfigurationRoot.empty() || workspaceStorageRoot.empty() ||
        canonicalWorkspace.empty()) {
        throw std::invalid_argument("Windows settings roots and workspace must not be empty");
    }
    return {userConfigurationRoot / "settings.v1",
            workspaceStorageRoot / workspaceKey(canonicalWorkspace) / "settings.v1"};
}

} // namespace ssg
