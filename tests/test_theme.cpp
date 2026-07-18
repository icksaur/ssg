#include "ssg/theme.h"
#include "test_helpers.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ssg::IndexedColor;
using ssg::RoleMapping;
using ssg::SemanticRole;
using ssg::SrgbColor;
using ssg::SyntaxMapping;
using ssg::SyntaxScope;

std::vector<IndexedColor> palette(std::size_t count = ssg::theme_palette_size) {
    std::vector<IndexedColor> result;
    for (std::size_t index = 0; index < count; ++index) {
        result.push_back({static_cast<std::uint8_t>(index),
                          SrgbColor{static_cast<std::uint8_t>(index * 11),
                                    static_cast<std::uint8_t>(index * 7),
                                    static_cast<std::uint8_t>(index * 3)}});
    }
    return result;
}

std::vector<RoleMapping> roles() {
    std::vector<RoleMapping> result;
    for (std::size_t index = 0; index < ssg::all_semantic_roles.size(); ++index) {
        result.push_back(
            {ssg::all_semantic_roles[index], static_cast<std::uint8_t>(index % 16)});
    }
    for (const auto& pair : ssg::co_visible_role_pairs) {
        const auto left = static_cast<std::size_t>(pair.first);
        const auto right = static_cast<std::size_t>(pair.second);
        if (result[left].palette_index == result[right].palette_index) {
            result[right].palette_index =
                static_cast<std::uint8_t>((result[right].palette_index + 1) % 16);
        }
    }
    return result;
}

std::vector<SyntaxMapping> syntax() {
    std::vector<SyntaxMapping> result;
    for (std::size_t index = 0; index < ssg::all_syntax_scopes.size(); ++index) {
        result.push_back(
            {ssg::all_syntax_scopes[index], static_cast<std::uint8_t>(index % 16)});
    }
    return result;
}

ssg::Theme valid_theme() {
    const auto colors = palette();
    const auto role_mappings = roles();
    const auto syntax_mappings = syntax();
    return {"test", colors, role_mappings, syntax_mappings};
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to open fixture: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

TEST(rejects_missing_duplicate_and_extra_palette_indices) {
    const auto role_mappings = roles();
    const auto syntax_mappings = syntax();

    auto missing = palette(15);
    ASSERT_THROWS(ssg::Theme("missing", missing, role_mappings, syntax_mappings),
                  std::invalid_argument);

    auto extra = palette(17);
    ASSERT_THROWS(ssg::Theme("extra", extra, role_mappings, syntax_mappings),
                  std::invalid_argument);

    auto duplicate = palette();
    duplicate.back().index = 0;
    ASSERT_THROWS(ssg::Theme("duplicate", duplicate, role_mappings, syntax_mappings),
                  std::invalid_argument);
}

TEST(requires_every_semantic_role_and_syntax_scope_exactly_once) {
    const auto colors = palette();
    auto role_mappings = roles();
    auto syntax_mappings = syntax();

    role_mappings.pop_back();
    ASSERT_THROWS(ssg::Theme("missing role", colors, role_mappings, syntax_mappings),
                  std::invalid_argument);
    role_mappings = roles();
    role_mappings.back().role = role_mappings.front().role;
    ASSERT_THROWS(ssg::Theme("duplicate role", colors, role_mappings, syntax_mappings),
                  std::invalid_argument);
    role_mappings = roles();
    role_mappings.front().role = static_cast<SemanticRole>(255);
    ASSERT_THROWS(ssg::Theme("unknown role", colors, role_mappings, syntax_mappings),
                  std::invalid_argument);

    role_mappings = roles();
    syntax_mappings.pop_back();
    ASSERT_THROWS(ssg::Theme("missing syntax", colors, role_mappings, syntax_mappings),
                  std::invalid_argument);
    syntax_mappings = syntax();
    syntax_mappings.back().scope = syntax_mappings.front().scope;
    ASSERT_THROWS(ssg::Theme("duplicate syntax", colors, role_mappings, syntax_mappings),
                  std::invalid_argument);
    syntax_mappings = syntax();
    syntax_mappings.front().scope = static_cast<SyntaxScope>(255);
    ASSERT_THROWS(ssg::Theme("unknown syntax", colors, role_mappings, syntax_mappings),
                  std::invalid_argument);
}

TEST(shared_fixture_roles_are_distinct_for_every_theme) {
    const auto fixture = read_file(SSG_THEME_ROLES_PATH);
    const std::regex quoted_name{"\"([a-z_]+)\""};
    std::vector<SemanticRole> fixture_roles;
    for (std::sregex_iterator it(fixture.begin(), fixture.end(), quoted_name), end;
         it != end; ++it) {
        const auto name = (*it)[1].str();
        if (name == "co_visible_role_classes") continue;
        const auto role = ssg::semantic_role_from_name(name);
        ASSERT_TRUE(role.has_value());
        fixture_roles.push_back(*role);
    }
    std::set<std::pair<SemanticRole, SemanticRole>> expected_pairs;
    std::size_t cursor = 0;
    constexpr std::array class_sizes{2u, 4u, 4u, 2u, 4u};
    for (const auto size : class_sizes) {
        for (std::size_t left = 0; left < size; ++left) {
            for (std::size_t right = left + 1; right < size; ++right) {
                expected_pairs.emplace(fixture_roles[cursor + left],
                                       fixture_roles[cursor + right]);
            }
        }
        cursor += size;
    }
    ASSERT_EQ(cursor, fixture_roles.size());
    ASSERT_EQ(expected_pairs.size(), ssg::co_visible_role_pairs.size());
    for (const auto& pair : ssg::co_visible_role_pairs) {
        ASSERT_TRUE(expected_pairs.contains({pair.first, pair.second}));
    }

    const auto theme = valid_theme();
    for (const auto& pair : expected_pairs) {
        ASSERT_NE(theme.index_for(pair.first), theme.index_for(pair.second));
    }

    auto invalid_roles = roles();
    const auto first_pair = ssg::co_visible_role_pairs.front();
    invalid_roles[static_cast<std::size_t>(first_pair.second)].palette_index =
        invalid_roles[static_cast<std::size_t>(first_pair.first)].palette_index;
    const auto colors = palette();
    const auto syntax_mappings = syntax();
    ASSERT_THROWS(ssg::Theme("collision", colors, invalid_roles, syntax_mappings),
                  std::invalid_argument);
}

TEST(snapshot_is_complete_and_deterministic) {
    const auto first = valid_theme();
    const auto second = valid_theme();
    const auto first_snapshot = first.snapshot();
    const auto second_snapshot = second.snapshot();

    ASSERT_EQ(first_snapshot, second_snapshot);
    ASSERT_EQ(first_snapshot.palette.size(), ssg::theme_palette_size);
    ASSERT_EQ(first_snapshot.semantic_indices.size(), ssg::semantic_role_count);
    ASSERT_EQ(first_snapshot.syntax_indices.size(), ssg::syntax_scope_count);
    for (std::size_t index = 0; index < first_snapshot.palette.size(); ++index) {
        ASSERT_EQ(first_snapshot.palette[index], first.palette()[index]);
    }
    for (std::size_t index = 0; index < ssg::all_semantic_roles.size(); ++index) {
        ASSERT_EQ(first_snapshot.semantic_indices[index],
                  first.index_for(ssg::all_semantic_roles[index]));
    }
    ASSERT_EQ(first.index_for_syntax("not.a.known.scope"),
              first.index_for(SyntaxScope::plain_text));
}

TEST(bundled_theme_data_is_complete_and_constructible) {
    std::ifstream input(SSG_THEME_PATH);
    ASSERT_TRUE(input.good());
    std::string name;
    std::vector<IndexedColor> colors;
    std::vector<RoleMapping> role_mappings;
    std::vector<SyntaxMapping> syntax_mappings;
    std::string kind;
    while (input >> kind) {
        if (kind == "name") {
            input >> name;
        } else if (kind == "palette") {
            unsigned index = 0;
            unsigned red = 0;
            unsigned green = 0;
            unsigned blue = 0;
            input >> index >> red >> green >> blue;
            colors.push_back({static_cast<std::uint8_t>(index),
                              {static_cast<std::uint8_t>(red),
                               static_cast<std::uint8_t>(green),
                               static_cast<std::uint8_t>(blue)}});
        } else if (kind == "role") {
            std::string role_name;
            unsigned index = 0;
            input >> role_name >> index;
            const auto role = ssg::semantic_role_from_name(role_name);
            ASSERT_TRUE(role.has_value());
            role_mappings.push_back({*role, static_cast<std::uint8_t>(index)});
        } else if (kind == "syntax") {
            std::string scope_name;
            unsigned index = 0;
            input >> scope_name >> index;
            const auto scope = ssg::syntax_scope_from_name(scope_name);
            ASSERT_TRUE(scope.has_value());
            syntax_mappings.push_back({*scope, static_cast<std::uint8_t>(index)});
        } else {
            throw std::runtime_error("unknown bundled theme record");
        }
    }
    ASSERT_NO_THROW(ssg::Theme(name, colors, role_mappings, syntax_mappings));
}

TEST(source_and_config_have_no_independent_color_sources) {
    const std::filesystem::path root = SSG_SOURCE_ROOT;
    const std::array forbidden{
        std::regex{R"(#([[:xdigit:]]{8}|[[:xdigit:]]{6}|[[:xdigit:]]{4}|[[:xdigit:]]{3})\b)"},
        std::regex{R"(\b(rgb|rgba|hsl|hsla)\s*\()"},
        std::regex{R"(\b(lighten|darken|shade|tint|blend|gradient)\s*\()"},
        std::regex{R"(\bSrgbColor\s*[\{\(])"},
    };
    const std::set<std::string> scanned_extensions{
        ".h",    ".hpp",  ".cpp", ".cc",   ".cxx", ".json", ".cmake",
        ".css",  ".scss", ".sass", ".html", ".js",  ".jsx",  ".ts",
        ".tsx",  ".lua"};
    std::vector<std::string> violations;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        const auto relative = std::filesystem::relative(entry.path(), root).generic_string();
        if (relative.starts_with(".git/") || relative.starts_with("build") ||
            relative.starts_with("doc/") || relative.starts_with("tasks/") ||
            relative.starts_with("data/themes/") ||
            relative == "include/ssg/theme.h" ||
            // Terminal color-depth adaptation (M9-C): these define the xterm-256
            // and ANSI-16 TERMINAL palettes — hardware swatches a reduced-depth
            // terminal can display — not editor theme colors.  See
            // doc/spec-terminal-robustness.md (M9-C1) for the I22 reconciliation:
            // resolve_color adds no color to the theme/snapshot/API surface.
            relative == "include/ssg/color.h" ||
            relative == "src/color.cpp" ||
            relative == "tests/test_color.cpp" ||
            relative == "tests/test_theme.cpp") {
            continue;
        }
        if (!scanned_extensions.contains(entry.path().extension().string())) continue;
        const auto contents = read_file(entry.path());
        for (const auto& pattern : forbidden) {
            if (std::regex_search(contents, pattern)) {
                violations.push_back(relative);
                break;
            }
        }
    }
    ASSERT_TRUE(violations.empty());
}

} // namespace

int main() {
    RUN(rejects_missing_duplicate_and_extra_palette_indices);
    RUN(requires_every_semantic_role_and_syntax_scope_exactly_once);
    RUN(shared_fixture_roles_are_distinct_for_every_theme);
    RUN(snapshot_is_complete_and_deterministic);
    RUN(bundled_theme_data_is_complete_and_constructible);
    RUN(source_and_config_have_no_independent_color_sources);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
