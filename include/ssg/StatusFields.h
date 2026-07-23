#pragma once

#include <ssg/ShellState.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ssg {

enum class StatusFieldRegion : std::uint8_t { Header, Footer };

struct StatusFieldCatalogEntry {
    std::string id;
    std::string accessibleLabel;
    StatusFieldRegion region = StatusFieldRegion::Footer;
    std::uint8_t collapseRank = 0;
};

struct StatusFieldProviderContext {
    std::filesystem::path workspaceRoot;
    std::string currentPathLabel;
    std::optional<std::string> currentBranch;
    std::string statusValue;
    std::string followMode;
};

using StatusFieldProvider =
    std::function<std::optional<std::string>(StatusFieldProviderContext const&)>;

struct StatusFieldProviderBinding {
    std::string id;
    StatusFieldProvider provider;
};

struct StatusFieldProjection {
    std::vector<StatusField> headerFields;
    std::vector<StatusField> footerFields;
};

[[nodiscard]] std::vector<StatusFieldCatalogEntry> p0StatusFieldCatalog();
[[nodiscard]] std::vector<StatusFieldProviderBinding>
defaultStatusFieldProviders();
[[nodiscard]] StatusFieldProjection projectStatusFields(
    const std::vector<StatusFieldCatalogEntry>& catalog,
    const std::unordered_map<std::string, StatusFieldProvider>& providers,
    StatusFieldProviderContext const& context);

} // namespace ssg
