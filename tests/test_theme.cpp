#include "ssg/EditorRuntime.h"
#include "ssg/Theme.h"
#include "test_helpers.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

using ssg::IndexedColor;
using ssg::RoleMapping;
using ssg::SemanticRole;
using ssg::SrgbColor;
using ssg::SyntaxMapping;
using ssg::SyntaxScope;

std::vector<IndexedColor> palette(std::size_t count = ssg::kThemePaletteSize) {
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
    for (std::size_t index = 0; index < ssg::kAllSemanticRoles.size(); ++index) {
        result.push_back(
            {ssg::kAllSemanticRoles[index], static_cast<std::uint8_t>(index % 16)});
    }
    for (const auto& pair : ssg::kCoVisibleRolePairs) {
        const auto left = static_cast<std::size_t>(pair.first);
        const auto right = static_cast<std::size_t>(pair.second);
        if (result[left].paletteIndex == result[right].paletteIndex) {
            result[right].paletteIndex =
                static_cast<std::uint8_t>((result[right].paletteIndex + 1) % 16);
        }
    }
    return result;
}

std::vector<SyntaxMapping> syntax() {
    std::vector<SyntaxMapping> result;
    for (std::size_t index = 0; index < ssg::kAllSyntaxScopes.size(); ++index) {
        result.push_back(
            {ssg::kAllSyntaxScopes[index], static_cast<std::uint8_t>(index % 16)});
    }
    return result;
}

ssg::Theme validTheme() {
    const auto colors = palette();
    const auto roleMappings = roles();
    const auto syntaxMappings = syntax();
    return {"test", colors, roleMappings, syntaxMappings};
}

// The 16 classic ANSI palette-slot names in the order documented publicly
// on ThemeDefineArguments/applyThemeDefine (Theme.h) -- this is the SPEC'd
// order (also the palette's own 0-15 index order), independent of Theme.cpp's
// private implementation array, so a test built from it is a genuine check
// that the implementation matches what was documented, not a tautology.
constexpr std::array<std::string_view, ssg::kThemePaletteSize> kAnsiSlotNamesInIndexOrder{
    "black",         "red",           "green",         "yellow",
    "blue",          "magenta",       "cyan",          "white",
    "brightBlack",   "brightRed",     "brightGreen",   "brightYellow",
    "brightBlue",    "brightMagenta", "brightCyan",    "brightWhite",
};

std::string hexOf(SrgbColor color) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", color.red,
                  color.green, color.blue);
    return buffer;
}

std::array<SrgbColor, 6> colors(ssg::DiffTints const& tints) {
    return {tints.addedRow, tints.removedRow, tints.modifiedRow,
            tints.addedWord, tints.removedWord, tints.modifiedWord};
}

// Selection fill is a FLAT anchor color: exactly the Selection role's own
// palette entry, matching how deriveDiffTints's Git-anchor colors work --
// no blend, no desaturation, no readability search. This keeps a text
// selection, a selected tab (TabActive), and a selected tree row
// (TreeFocus) visually identical, since all three read the same palette
// index the same way.
void assertSelectionFillGate(ssg::ThemeSnapshot const& snapshot) {
    const auto selectionAnchor =
        snapshot.palette[snapshot.semanticIndices[static_cast<std::size_t>(
            SemanticRole::Selection)]];
    ASSERT_EQ(snapshot.selectionFill, selectionAnchor);
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to open fixture: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

TEST(rejectsMissingDuplicateAndExtraPaletteIndices) {
    const auto roleMappings = roles();
    const auto syntaxMappings = syntax();

    auto missing = palette(15);
    ASSERT_THROWS(ssg::Theme("missing", missing, roleMappings, syntaxMappings),
                  std::invalid_argument);

    auto extra = palette(17);
    ASSERT_THROWS(ssg::Theme("extra", extra, roleMappings, syntaxMappings),
                  std::invalid_argument);

    auto duplicate = palette();
    duplicate.back().index = 0;
    ASSERT_THROWS(ssg::Theme("duplicate", duplicate, roleMappings, syntaxMappings),
                  std::invalid_argument);
}

TEST(requiresEverySemanticRoleAndSyntaxScopeExactlyOnce) {
    const auto colors = palette();
    auto roleMappings = roles();
    auto syntaxMappings = syntax();

    roleMappings.pop_back();
    ASSERT_THROWS(ssg::Theme("missing role", colors, roleMappings, syntaxMappings),
                  std::invalid_argument);
    roleMappings = roles();
    roleMappings.back().role = roleMappings.front().role;
    ASSERT_THROWS(ssg::Theme("duplicate role", colors, roleMappings, syntaxMappings),
                  std::invalid_argument);
    roleMappings = roles();
    roleMappings.front().role = static_cast<SemanticRole>(255);
    ASSERT_THROWS(ssg::Theme("unknown role", colors, roleMappings, syntaxMappings),
                  std::invalid_argument);

    roleMappings = roles();
    syntaxMappings.pop_back();
    ASSERT_THROWS(ssg::Theme("missing syntax", colors, roleMappings, syntaxMappings),
                  std::invalid_argument);
    syntaxMappings = syntax();
    syntaxMappings.back().scope = syntaxMappings.front().scope;
    ASSERT_THROWS(ssg::Theme("duplicate syntax", colors, roleMappings, syntaxMappings),
                  std::invalid_argument);
    syntaxMappings = syntax();
    syntaxMappings.front().scope = static_cast<SyntaxScope>(255);
    ASSERT_THROWS(ssg::Theme("unknown syntax", colors, roleMappings, syntaxMappings),
                  std::invalid_argument);
}

TEST(sharedFixtureRolesAreDistinctForEveryTheme) {
    const auto fixture = readFile(SSG_THEME_ROLES_PATH);
    const std::regex quotedName{"\"([a-z_]+)\""};
    std::vector<SemanticRole> fixtureRoles;
    for (std::sregex_iterator it(fixture.begin(), fixture.end(), quotedName), end;
         it != end; ++it) {
        const auto name = (*it)[1].str();
        if (name == "co_visible_role_classes") continue;
        const auto role = ssg::semanticRoleFromName(name);
        ASSERT_TRUE(role.has_value());
        fixtureRoles.push_back(*role);
    }
    std::set<std::pair<SemanticRole, SemanticRole>> expectedPairs;
    std::size_t cursor = 0;
    constexpr std::array classSizes{2u, 4u, 4u, 2u, 4u};
    for (const auto size : classSizes) {
        for (std::size_t left = 0; left < size; ++left) {
            for (std::size_t right = left + 1; right < size; ++right) {
                expectedPairs.emplace(fixtureRoles[cursor + left],
                                       fixtureRoles[cursor + right]);
            }
        }
        cursor += size;
    }
    ASSERT_EQ(cursor, fixtureRoles.size());
    ASSERT_EQ(expectedPairs.size(), ssg::kCoVisibleRolePairs.size());
    for (const auto& pair : ssg::kCoVisibleRolePairs) {
        ASSERT_TRUE(expectedPairs.contains({pair.first, pair.second}));
    }

    const auto theme = validTheme();
    for (const auto& pair : expectedPairs) {
        ASSERT_NE(theme.indexFor(pair.first), theme.indexFor(pair.second));
    }

    auto invalidRoles = roles();
    const auto firstPair = ssg::kCoVisibleRolePairs.front();
    invalidRoles[static_cast<std::size_t>(firstPair.second)].paletteIndex =
        invalidRoles[static_cast<std::size_t>(firstPair.first)].paletteIndex;
    const auto colors = palette();
    const auto syntaxMappings = syntax();
    ASSERT_THROWS(ssg::Theme("collision", colors, invalidRoles, syntaxMappings),
                  std::invalid_argument);
}

TEST(snapshotIsCompleteAndDeterministic) {
    const auto first = validTheme();
    const auto second = validTheme();
    const auto firstSnapshot = first.snapshot();
    const auto secondSnapshot = second.snapshot();

    ASSERT_EQ(firstSnapshot, secondSnapshot);
    ASSERT_EQ(firstSnapshot.palette.size(), ssg::kThemePaletteSize);
    ASSERT_EQ(firstSnapshot.semanticIndices.size(), ssg::kSemanticRoleCount);
    ASSERT_EQ(firstSnapshot.syntaxIndices.size(), ssg::kSyntaxScopeCount);
    for (std::size_t index = 0; index < firstSnapshot.palette.size(); ++index) {
        ASSERT_EQ(firstSnapshot.palette[index], first.palette()[index]);
    }
    for (std::size_t index = 0; index < ssg::kAllSemanticRoles.size(); ++index) {
        ASSERT_EQ(firstSnapshot.semanticIndices[index],
                  first.indexFor(ssg::kAllSemanticRoles[index]));
    }
    ASSERT_EQ(first.indexForSyntax("not.a.known.scope"),
              first.indexFor(SyntaxScope::PlainText));
}

TEST(diffTintsAreTheThemesOwnFlatGitAnchorColors) {
    // No desaturation, no dimming, no readability search: each diff kind is
    // exactly the theme's own Git anchor color, used directly as a
    // background. Row and word share that same color; a modified line's
    // word-level marks reuse Added/Removed directly (see Renderer.cpp),
    // so there is no separate fourth "modified word" shade to test either.
    const auto snapshot = ssg::defaultTheme();
    const auto added =
        snapshot.palette[snapshot.semanticIndices[static_cast<std::size_t>(
            SemanticRole::GitAdded)]];
    const auto deleted =
        snapshot.palette[snapshot.semanticIndices[static_cast<std::size_t>(
            SemanticRole::GitDeleted)]];
    const auto modified =
        snapshot.palette[snapshot.semanticIndices[static_cast<std::size_t>(
            SemanticRole::GitModified)]];

    ASSERT_EQ(snapshot.diffTints.addedRow, added);
    ASSERT_EQ(snapshot.diffTints.addedWord, added);
    ASSERT_EQ(snapshot.diffTints.removedRow, deleted);
    ASSERT_EQ(snapshot.diffTints.removedWord, deleted);
    ASSERT_EQ(snapshot.diffTints.modifiedRow, modified);
    ASSERT_EQ(snapshot.diffTints.modifiedWord, added);

    assertSelectionFillGate(snapshot);
}

TEST(themeDefineFullTableRoundTripsToAByteIdenticalSnapshot) {
    // Full round-trip is a no-op oracle (doc/spec-config.md's Acceptance):
    // re-specifying every slot with the theme's own current colors must
    // produce a snapshot byte-identical to the untouched original --
    // proves the whole apply path (name lookup, hex parse, palette
    // replacement, DiffTints/selectionFill recompute) with zero visual
    // ambiguity to eyeball.
    const auto current = ssg::defaultTheme();
    ssg::ThemeDefineArguments arguments;
    for (std::size_t index = 0; index < kAnsiSlotNamesInIndexOrder.size();
         ++index) {
        arguments.colors.emplace(std::string{kAnsiSlotNamesInIndexOrder[index]},
                                 hexOf(current.palette[index]));
    }
    const auto result = ssg::applyThemeDefine(current, arguments);
    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(result.snapshot, current);
}

TEST(themeDefinePartialTableChangesOnlyTheNamedSlotsExactly) {
    const auto current = ssg::defaultTheme();
    ssg::ThemeDefineArguments arguments;
    const auto replacement = SrgbColor{1, 2, 3};
    arguments.colors.emplace("red", hexOf(replacement));
    const auto result = ssg::applyThemeDefine(current, arguments);
    ASSERT_TRUE(result.accepted());
    for (std::size_t index = 0; index < ssg::kThemePaletteSize; ++index) {
        if (kAnsiSlotNamesInIndexOrder[index] == "red") {
            ASSERT_EQ(result.snapshot.palette[index], replacement);
        } else {
            ASSERT_EQ(result.snapshot.palette[index], current.palette[index]);
        }
    }
    ASSERT_EQ(result.snapshot.semanticIndices, current.semanticIndices);
    ASSERT_EQ(result.snapshot.syntaxIndices, current.syntaxIndices);
}

TEST(themeDefineUnknownSlotNameIsRejectedWithTheOriginalUntouched) {
    const auto current = ssg::defaultTheme();
    ssg::ThemeDefineArguments arguments;
    arguments.colors.emplace("not_a_real_slot", "#112233");
    const auto result = ssg::applyThemeDefine(current, arguments);
    ASSERT_FALSE(result.accepted());
    ASSERT_FALSE(result.error->message.empty());
}

TEST(themeDefineMalformedHexColorIsRejectedWithTheOriginalUntouched) {
    const auto current = ssg::defaultTheme();
    for (auto const* malformed :
         {"not-a-color", "#12345", "#gggggg", "112233", "#12345678"}) {
        ssg::ThemeDefineArguments arguments;
        arguments.colors.emplace("red", malformed);
        const auto result = ssg::applyThemeDefine(current, arguments);
        ASSERT_FALSE(result.accepted());
    }
}

TEST(themeDefineRejectionIsAllOrNothingNotPartial) {
    // A table with ONE valid entry and ONE invalid entry must reject the
    // whole call -- if it partially applied, this would silently mutate
    // the valid slot despite reporting failure.
    const auto current = ssg::defaultTheme();
    ssg::ThemeDefineArguments arguments;
    arguments.colors.emplace("red", "#112233");
    arguments.colors.emplace("green", "not-a-color");
    const auto result = ssg::applyThemeDefine(current, arguments);
    ASSERT_FALSE(result.accepted());
}

// EditorRuntime::create's initial theme must be exactly ssg::defaultTheme()
// -- the one compiled-in source (see its doc comment in Theme.h) -- and
// nothing else. This is a wiring regression test, not a drift guard: since
// there is now only one copy of the default theme's values, nothing can
// number-for-number drift out of sync with itself; what CAN regress is
// EditorRuntime::create() silently starting from some other theme.
TEST(editorRuntimeStartsFromTheDefaultTheme) {
    auto root = std::filesystem::temp_directory_path() /
                ("ssg_theme_wiring_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    ssg::EditorRuntimeConfig config;
    config.cwd = root / "workspace";
    config.scratchRoot = root / "scratch";
    config.recoveryRoot = root / "recovery";
    auto created = ssg::EditorRuntime::create(config);
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) { std::filesystem::remove_all(root); return; }
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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
    const std::regex themeBareRgbLiteral{
        R"(\{\s*(?:25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})\s*,\s*(?:25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})\s*,\s*(?:25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})\s*(?:,\s*(?:25[0-5]|2[0-4][0-9]|1?[0-9]{1,2}))?\s*\})"};
    const std::set<std::string> scannedExtensions{
        ".h",    ".hpp",  ".cpp", ".cc",   ".cxx", ".json", ".cmake",
        ".css",  ".scss", ".sass", ".html", ".js",  ".jsx",  ".ts",
        ".tsx",  ".lua"};
    std::vector<std::string> violations;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        const auto relative = std::filesystem::relative(entry.path(), root).generic_string();
        if (relative.starts_with(".git/") || relative.starts_with("build") ||
            relative.starts_with("doc/") || relative.starts_with("tasks/") ||
            relative.starts_with("vendor/") ||
            relative == "include/ssg/Theme.h" ||
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
        if (!scannedExtensions.contains(entry.path().extension().string())) continue;
        const auto contents = readFile(entry.path());
        for (const auto& pattern : forbidden) {
            if (std::regex_search(contents, pattern)) {
                violations.push_back(relative);
                break;
            }
        }
        if (relative == "src/Theme.cpp" &&
            std::regex_search(contents, themeBareRgbLiteral)) {
            violations.push_back(relative);
        }
    }
    ASSERT_TRUE(violations.empty());
}

} // namespace

int main() {
    RUN(rejectsMissingDuplicateAndExtraPaletteIndices);
    RUN(requiresEverySemanticRoleAndSyntaxScopeExactlyOnce);
    RUN(sharedFixtureRolesAreDistinctForEveryTheme);
    RUN(snapshotIsCompleteAndDeterministic);
    RUN(diffTintsAreTheThemesOwnFlatGitAnchorColors);
    RUN(themeDefineFullTableRoundTripsToAByteIdenticalSnapshot);
    RUN(themeDefinePartialTableChangesOnlyTheNamedSlotsExactly);
    RUN(themeDefineUnknownSlotNameIsRejectedWithTheOriginalUntouched);
    RUN(themeDefineMalformedHexColorIsRejectedWithTheOriginalUntouched);
    RUN(themeDefineRejectionIsAllOrNothingNotPartial);
    RUN(editorRuntimeStartsFromTheDefaultTheme);
    RUN(sourceAndConfigHaveNoIndependentColorSources);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
