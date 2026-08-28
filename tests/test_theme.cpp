#include "ssg/EditorSession.h"
#include "ssg/Theme.h"
#include "test_helpers.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using ssg::SemanticRole;
using ssg::SrgbColor;
using ssg::SyntaxScope;

std::string hexOf(SrgbColor color) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", color.red,
                  color.green, color.blue);
    return buffer;
}

// A theme.set table naming EVERY role and scope with the theme's own current
// colors -- re-specifying every slot with what it already holds.
ssg::ThemeSetArguments fullIdentityTable(ssg::ThemeSnapshot const& theme) {
    ssg::ThemeSetArguments arguments;
    for (const auto role : ssg::kAllSemanticRoles) {
        arguments.colors.emplace(std::string{ssg::semanticRoleName(role)},
                                 hexOf(theme.color(role)));
    }
    for (const auto scope : ssg::kAllSyntaxScopes) {
        arguments.colors.emplace(std::string{ssg::syntaxScopeName(scope)},
                                 hexOf(theme.color(scope)));
    }
    return arguments;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to open fixture: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

TEST(themeColorAccessorsReturnTheDirectRoleAndScopeColors) {
    const auto theme = ssg::defaultTheme();
    for (const auto role : ssg::kAllSemanticRoles) {
        ASSERT_EQ(theme.color(role),
                  theme.roleColors[static_cast<std::size_t>(role)]);
    }
    for (const auto scope : ssg::kAllSyntaxScopes) {
        ASSERT_EQ(theme.color(scope),
                  theme.syntaxColors[static_cast<std::size_t>(scope)]);
    }
    ASSERT_EQ(theme.roleColors.size(), ssg::kSemanticRoleCount);
    ASSERT_EQ(theme.syntaxColors.size(), ssg::kSyntaxScopeCount);
}

TEST(themeSetFullTableRoundTripsToAByteIdenticalSnapshot) {
    // Full round-trip is a no-op oracle: re-specifying every role and scope
    // with the theme's own current colors must produce a snapshot
    // byte-identical to the untouched original -- proves the whole apply path
    // (name lookup, hex parse, direct color replacement) with zero visual
    // ambiguity to eyeball.
    const auto current = ssg::defaultTheme();
    const auto result = current.withOverrides(fullIdentityTable(current));
    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(result.snapshot, current);
}

TEST(themeSetPartialTableChangesOnlyTheNamedColorsExactly) {
    const auto current = ssg::defaultTheme();
    ssg::ThemeSetArguments arguments;
    const auto replacement = SrgbColor{1, 2, 3};
    arguments.colors.emplace("selection", hexOf(replacement));
    arguments.colors.emplace("comment", hexOf(replacement));
    const auto result = current.withOverrides(arguments);
    ASSERT_TRUE(result.accepted());
    for (const auto role : ssg::kAllSemanticRoles) {
        const auto expected = role == SemanticRole::Selection
                                  ? replacement
                                  : current.color(role);
        ASSERT_EQ(result.snapshot.color(role), expected);
    }
    for (const auto scope : ssg::kAllSyntaxScopes) {
        const auto expected = scope == SyntaxScope::Comment
                                  ? replacement
                                  : current.color(scope);
        ASSERT_EQ(result.snapshot.color(scope), expected);
    }
}

TEST(themeSetUnknownNameIsRejectedWithTheOriginalUntouched) {
    const auto current = ssg::defaultTheme();
    ssg::ThemeSetArguments arguments;
    arguments.colors.emplace("not_a_real_role", "#112233");
    const auto result = current.withOverrides(arguments);
    ASSERT_FALSE(result.accepted());
    ASSERT_FALSE(result.error->message.empty());
}

TEST(themeSetMalformedHexColorIsRejected) {
    const auto current = ssg::defaultTheme();
    for (auto const* malformed :
         {"not-a-color", "#12345", "#gggggg", "112233", "#12345678"}) {
        ssg::ThemeSetArguments arguments;
        arguments.colors.emplace("selection", malformed);
        const auto result = current.withOverrides(arguments);
        ASSERT_FALSE(result.accepted());
    }
}

TEST(themeSetRejectionIsAllOrNothingNotPartial) {
    // A table with ONE valid entry and ONE invalid entry must reject the whole
    // call -- if it partially applied, this would silently mutate the valid
    // color despite reporting failure.
    const auto current = ssg::defaultTheme();
    ssg::ThemeSetArguments arguments;
    arguments.colors.emplace("selection", "#112233");
    arguments.colors.emplace("comment", "not-a-color");
    const auto result = current.withOverrides(arguments);
    ASSERT_FALSE(result.accepted());
}

TEST(anEmptyThemeSetTableIsAcceptedAndChangesNothing) {
    const auto before = ssg::defaultTheme();
    const auto result = before.withOverrides(ssg::ThemeSetArguments{});
    ASSERT_TRUE(result.accepted());
    if (result.accepted()) ASSERT_EQ(result.snapshot, before);
}

// EditorSession::create's initial theme must be exactly ssg::defaultTheme()
// -- the one compiled-in source (see its doc comment in Theme.h) -- and
// nothing else. This is a wiring regression test: what CAN regress is
// EditorSession::create() silently starting from some other theme.
TEST(editorRuntimeStartsFromTheDefaultTheme) {
    auto root = std::filesystem::temp_directory_path() /
                ("ssg_theme_wiring_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    ssg::EditorSessionConfig config;
    config.cwd = root / "workspace";
    config.scratchRoot = root / "scratch";
    config.recoveryRoot = root / "recovery";
    auto created = ssg::EditorSession::create(config);
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) { std::filesystem::remove_all(root); return; }
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());
    auto snapshot = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) {
        ASSERT_EQ(snapshot->sections().theme, ssg::defaultTheme());
    }
    std::filesystem::remove_all(root);
}

TEST(sourceAndConfigHaveNoIndependentColorSources) {
    const std::filesystem::path root = SSG_SOURCE_ROOT;
    const std::array forbidden{
        std::regex{R"(#([[:xdigit:]]{8}|[[:xdigit:]]{6}|[[:xdigit:]]{4}|[[:xdigit:]]{3})\b)"},
        std::regex{R"(\b(rgb|rgba|hsl|hsla)\s*\()"},
        std::regex{R"(\b(lighten|darken|shade|tint|blend|gradient)\s*\()"},
        std::regex{R"(\bSrgbColor\s*[\{\(])"},
    };
    const std::regex bareRgbLiteral{
        R"(\{\s*(?:25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})\s*,\s*(?:25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})\s*,\s*(?:25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})\s*(?:,\s*(?:25[0-5]|2[0-4][0-9]|1?[0-9]{1,2}))?\s*\})"};
    const std::set<std::string> scannedExtensions{
        ".h",    ".hpp",  ".cpp", ".cc",   ".cxx", ".json", ".cmake",
        ".css",  ".scss", ".sass", ".html", ".js",  ".jsx",  ".ts",
        ".tsx",  ".mjs",  ".lua"};
    std::vector<std::string> violations;
    const auto prunedDirectory = [](std::string_view relative) {
        return relative == ".git" || relative.starts_with("build") ||
               relative == "doc" || relative == "tasks" || relative == "vendor";
    };
    std::error_code walkError;
    std::filesystem::recursive_directory_iterator entry{
        root, std::filesystem::directory_options::skip_permission_denied,
        walkError};
    ASSERT_FALSE(static_cast<bool>(walkError));
    const std::filesystem::recursive_directory_iterator last;
    for (; entry != last; entry.increment(walkError)) {
        if (walkError) break;
        const auto relative =
            std::filesystem::relative(entry->path(), root).generic_string();
        std::error_code kindError;
        if (entry->is_directory(kindError) && !kindError) {
            if (entry.depth() == 0 && prunedDirectory(relative)) {
                entry.disable_recursion_pending();
            }
            continue;
        }
        if (!entry->is_regular_file(kindError) || kindError) continue;
        if (relative == "include/ssg/Theme.h" ||
            // Terminal color-depth adaptation (M9-C): these define the xterm-256
            // and ANSI-16 TERMINAL palettes -- hardware swatches a reduced-depth
            // terminal can display -- not editor theme colors.
            relative == "include/ssg/color.h" ||
            relative == "src/color.cpp" ||
            relative == "tests/test_color.cpp" ||
            relative == "tests/test_theme.cpp" ||
            // The one narrow, reviewed home for the built-in theme's literal
            // starting colors (see its file header). Every other file must
            // receive colors through a theme.set table.
            relative == "src/DefaultTheme.cpp") {
            continue;
        }
        if (!scannedExtensions.contains(entry->path().extension().string())) continue;
        const auto contents = readFile(entry->path());
        for (const auto& pattern : forbidden) {
            if (std::regex_search(contents, pattern)) {
                violations.push_back(relative);
                break;
            }
        }
        if (relative == "src/Theme.cpp" &&
            std::regex_search(contents, bareRgbLiteral)) {
            violations.push_back(relative);
        }
    }
    ASSERT_FALSE(static_cast<bool>(walkError));
    ASSERT_TRUE(violations.empty());
}

TEST(theWebRendererRoleOrdinalsMatchTheSemanticRoleEnum) {
    // The served web client indexes theme.role_colors by these ordinals to build
    // its CSS custom properties (const ROLE in http_serve.cpp's page). A reorder
    // of SemanticRole without updating the client would silently mis-color it.
    ASSERT_EQ(static_cast<int>(ssg::SemanticRole::Text), 0);
    ASSERT_EQ(static_cast<int>(ssg::SemanticRole::Canvas), 1);
    ASSERT_EQ(static_cast<int>(ssg::SemanticRole::Caret), 2);
    ASSERT_EQ(static_cast<int>(ssg::SemanticRole::Selection), 3);
    ASSERT_EQ(static_cast<int>(ssg::SemanticRole::TabActive), 6);
    ASSERT_EQ(static_cast<int>(ssg::SemanticRole::TabInactive), 7);
    // The web chrome interpreter colors a composed widget by the effective role the
    // SERVER resolves and publishes as a SemanticRole ordinal (reconcile.mjs indexes
    // theme.role_colors by it); these ordinals pin that wire contract.
    ASSERT_EQ(static_cast<int>(ssg::SemanticRole::Header), 10);
    ASSERT_EQ(static_cast<int>(ssg::SemanticRole::Footer), 11);
    ASSERT_EQ(static_cast<int>(ssg::SemanticRole::StatusInfo), 12);
    ASSERT_EQ(static_cast<int>(ssg::SemanticRole::StatusWarning), 13);
    ASSERT_EQ(static_cast<int>(ssg::SemanticRole::ScrollbarTrack), 17);
    ASSERT_EQ(static_cast<int>(ssg::SemanticRole::ScrollbarThumb), 18);
}

} // namespace

int main() {
    RUN(themeColorAccessorsReturnTheDirectRoleAndScopeColors);
    RUN(themeSetFullTableRoundTripsToAByteIdenticalSnapshot);
    RUN(themeSetPartialTableChangesOnlyTheNamedColorsExactly);
    RUN(themeSetUnknownNameIsRejectedWithTheOriginalUntouched);
    RUN(themeSetMalformedHexColorIsRejected);
    RUN(themeSetRejectionIsAllOrNothingNotPartial);
    RUN(anEmptyThemeSetTableIsAcceptedAndChangesNothing);
    RUN(editorRuntimeStartsFromTheDefaultTheme);
    RUN(sourceAndConfigHaveNoIndependentColorSources);
    RUN(theWebRendererRoleOrdinalsMatchTheSemanticRoleEnum);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
