#pragma once

#include <ssg/Style.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

struct StatusField {
    std::string id;
    std::string accessibleLabel;
    std::string value;
    std::optional<std::string> commandId;
};

// The fixed header/footer status-field identities: a closed, in-process
// product vocabulary (FIXED-STATUS), not an extension point. `WholeScreenAssembly`
// builds the structural leaf for each id; `projectStatusFields` computes its
// value beside it, below, so the two never name a field differently.
inline constexpr std::string_view kPathStatusFieldId = "path";
inline constexpr std::string_view kBranchStatusFieldId = "branch";
inline constexpr std::string_view kStatusValueFieldId = "status";
inline constexpr std::string_view kFollowStatusFieldId = "follow";

struct StatusFieldContext {
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

struct StatusFieldProjection {
    std::vector<StatusField> header;
    std::vector<StatusField> footer;
};

// Compute the fixed header/footer status fields directly from `context`. Each
// field is dropped (absent from its region's vector) when its computed value
// is empty (EMPTY-DROP).
[[nodiscard]] StatusFieldProjection projectStatusFields(
    StatusFieldContext const& context);

[[nodiscard]] std::string statusFieldGridDisplay(
    std::string_view providerId, std::string_view semanticValue,
    const Style& style);

} // namespace ssg
