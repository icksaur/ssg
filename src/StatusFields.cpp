#include <ssg/StatusFields.h>

#include "status_fields_catalog_json.h"

#include <charconv>
#include <limits>
#include <optional>
#include <regex>
#include <string_view>
#include <stdexcept>
#include <utility>

namespace ssg {
namespace {

std::string composeBranchField(std::string_view branchName) {
    return std::string{"\xE2\x8E\x87 "} + std::string{branchName};
}

// Replaces a leading home directory with "~" so a home-rooted workspace path
// shows compactly (e.g. "~/repo/ssg"), leaving header room for the branch. A
// path outside home, or an unknown home, is returned unchanged.
std::string abbreviateHome(const std::filesystem::path& path,
                           std::string_view home) {
    auto text = path.generic_string();
    if (home.empty() || text.size() < home.size() ||
        text.compare(0, home.size(), home) != 0) {
        return text;
    }
    if (text.size() == home.size()) return "~";
    if (text[home.size()] == '/') return "~" + text.substr(home.size());
    return text;
}

std::optional<std::string> objectStringField(const std::string& object,
                                             const std::string& fieldName) {
    const std::regex expression{
        "\"" + fieldName + R"regex("\s*:\s*"([^"]*)")regex"};
    std::smatch match;
    if (!std::regex_search(object, match, expression)) {
        return std::nullopt;
    }
    return match[1].str();
}

std::optional<std::uint8_t> objectUnsignedByteField(const std::string& object,
                                                    const std::string& fieldName) {
    const std::regex expression{
        "\"" + fieldName + R"regex("\s*:\s*([0-9]+))regex"};
    std::smatch match;
    if (!std::regex_search(object, match, expression)) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto first = match[1].first.base();
    const auto last = match[1].second.base();
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last ||
        value > std::numeric_limits<std::uint8_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(value);
}

std::optional<std::vector<StatusFieldCatalogEntry>> parseStatusFieldCatalog(
    std::string_view json) {
    std::string catalogText{json};
    const std::regex objectRegex{R"(\{[^}]*\})"};
    std::vector<StatusFieldCatalogEntry> entries;
    for (auto it = std::sregex_iterator(catalogText.begin(), catalogText.end(),
                                        objectRegex);
         it != std::sregex_iterator(); ++it) {
        const auto object = it->str();
        auto id = objectStringField(object, "id");
        auto region = objectStringField(object, "region");
        auto accessibleLabel = objectStringField(object, "accessible_label");
        auto collapseRank = objectUnsignedByteField(object, "collapse_rank");
        if (!id || !region || !accessibleLabel || !collapseRank) {
            return std::nullopt;
        }
        if (*region == "header") {
            entries.push_back(StatusFieldCatalogEntry{*id, *accessibleLabel,
                                                      StatusFieldRegion::Header,
                                                      *collapseRank});
            continue;
        }
        if (*region == "footer") {
            entries.push_back(StatusFieldCatalogEntry{*id, *accessibleLabel,
                                                      StatusFieldRegion::Footer,
                                                      *collapseRank});
            continue;
        }
        return std::nullopt;
    }
    return entries.empty() ? std::nullopt
                           : std::optional<std::vector<StatusFieldCatalogEntry>>{
                                 std::move(entries)};
}

} // namespace

std::vector<StatusFieldCatalogEntry> p0StatusFieldCatalog() {
    if (auto parsed = parseStatusFieldCatalog(kStatusFieldsCatalogJson)) {
        return *parsed;
    }
    throw std::logic_error{
        "status fields catalog is invalid: data/ui/status_fields.json"};
}

std::vector<StatusFieldProviderBinding> defaultStatusFieldProviders() {
    return {
        {"path",
         [](StatusFieldProviderContext const& context)
             -> std::optional<std::string> {
             return abbreviateHome(context.workspaceRoot,
                                   context.homeDirectory);
         }},
        {"branch",
         [](StatusFieldProviderContext const& context)
             -> std::optional<std::string> {
             if (!context.currentBranch || context.currentBranch->empty()) {
                 return std::nullopt;
             }
             return composeBranchField(*context.currentBranch);
         }},
        {"status",
         [](StatusFieldProviderContext const& context)
             -> std::optional<std::string> {
             return context.statusValue;
         }},
        {"follow",
         [](StatusFieldProviderContext const& context)
             -> std::optional<std::string> {
             return context.followMode;
         }},
    };
}

StatusFieldProjection projectStatusFields(
    const std::vector<StatusFieldCatalogEntry>& catalog,
    const std::unordered_map<std::string, StatusFieldProvider>& providers,
    StatusFieldProviderContext const& context) {
    StatusFieldProjection projection;
    for (const auto& entry : catalog) {
        const auto found = providers.find(entry.id);
        if (found == providers.end() || !found->second) {
            continue;
        }
        auto value = found->second(context);
        if (!value.has_value() || value->empty()) {
            continue;
        }
        StatusField field{
            .id = entry.id,
            .accessibleLabel = entry.accessibleLabel,
            .value = std::move(*value),
            .collapseRank = entry.collapseRank,
        };
        if (entry.region == StatusFieldRegion::Header) {
            projection.headerFields.push_back(std::move(field));
        } else {
            projection.footerFields.push_back(std::move(field));
        }
    }
    return projection;
}

} // namespace ssg
