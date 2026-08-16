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
    // The user's home directory, if known. The header path field abbreviates a
    // leading home directory to "~" so a home-rooted workspace path stays short
    // enough to leave room for the branch field beside it. Empty disables the
    // abbreviation (the full path is shown).
    std::string homeDirectory;
    std::optional<std::string> currentBranch;
    std::string statusValue;
    std::string followMode;
    // Drawn immediately left of the (abbreviated) workspace path in the header.
    // Empty by default; supplied from Style::cwdPrefix.
    std::string cwdPrefix;
};

using StatusFieldProvider =
    std::function<std::optional<std::string>(StatusFieldProviderContext const&)>;

struct StatusFieldProviderBinding {
    std::string id;
    StatusFieldProvider provider;
};

struct StatusFieldProjection {
    std::vector<StatusField> header;
    std::vector<StatusField> footer;
};

[[nodiscard]] std::vector<StatusFieldCatalogEntry> p0StatusFieldCatalog();
[[nodiscard]] std::vector<StatusFieldProviderBinding>
defaultStatusFieldProviders();
[[nodiscard]] StatusFieldProjection projectStatusFields(
    const std::vector<StatusFieldCatalogEntry>& catalog,
    const std::unordered_map<std::string, StatusFieldProvider>& providers,
    StatusFieldProviderContext const& context);

} // namespace ssg
