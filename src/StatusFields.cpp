#include <ssg/StatusFields.h>

#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <regex>
#include <utility>

namespace ssg {
namespace {

std::vector<StatusFieldCatalogEntry> fallbackStatusFieldCatalog() {
    return {
        {"cwd", "Workspace", StatusFieldRegion::Header, 0},
        {"file", "File", StatusFieldRegion::Header, 1},
        {"status", "Status", StatusFieldRegion::Footer, 0},
        {"follow", "Follow edits", StatusFieldRegion::Footer, 1},
    };
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

std::optional<std::vector<StatusFieldCatalogEntry>>
loadStatusFieldCatalog(std::filesystem::path path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return std::nullopt;
    }
    const std::string json((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    const std::regex objectRegex{R"(\{[^}]*\})"};
    std::vector<StatusFieldCatalogEntry> entries;
    for (auto it = std::sregex_iterator(json.begin(), json.end(), objectRegex);
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
            entries.push_back(
                {*id, *accessibleLabel, StatusFieldRegion::Header, *collapseRank});
            continue;
        }
        if (*region == "footer") {
            entries.push_back(
                {*id, *accessibleLabel, StatusFieldRegion::Footer, *collapseRank});
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
#ifdef SSG_STATUS_FIELDS_PATH
    if (auto loaded = loadStatusFieldCatalog(SSG_STATUS_FIELDS_PATH)) {
        return *loaded;
    }
#endif
    return fallbackStatusFieldCatalog();
}

std::vector<StatusFieldProviderBinding> defaultStatusFieldProviders() {
    return {
        {"cwd",
         [](StatusFieldProviderContext const& context)
             -> std::optional<std::string> {
             return context.workspaceRoot.string();
         }},
        {"file",
         [](StatusFieldProviderContext const& context)
             -> std::optional<std::string> {
             return context.currentPathLabel;
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
