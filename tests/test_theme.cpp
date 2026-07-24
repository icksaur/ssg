#include "ssg/Theme.h"
#include "ssg/color.h"
#include "test_helpers.h"

#include <algorithm>
#include <array>
#include <cmath>
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

ssg::Theme bundledTheme() {
    std::ifstream input(SSG_THEME_PATH);
    if (!input) throw std::runtime_error("failed to open bundled theme");
    std::string name;
    std::vector<IndexedColor> colors;
    std::vector<RoleMapping> roleMappings;
    std::vector<SyntaxMapping> syntaxMappings;
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
            std::string roleName;
            unsigned index = 0;
            input >> roleName >> index;
            const auto role = ssg::semanticRoleFromName(roleName);
            if (!role) throw std::runtime_error("unknown bundled theme role");
            roleMappings.push_back({*role, static_cast<std::uint8_t>(index)});
        } else if (kind == "syntax") {
            std::string scopeName;
            unsigned index = 0;
            input >> scopeName >> index;
            const auto scope = ssg::syntaxScopeFromName(scopeName);
            if (!scope) throw std::runtime_error("unknown bundled syntax scope");
            syntaxMappings.push_back({*scope, static_cast<std::uint8_t>(index)});
        } else {
            throw std::runtime_error("unknown bundled theme record");
        }
    }
    return {name, colors, roleMappings, syntaxMappings};
}

double linearChannel(std::uint8_t channel) {
    const double encoded = static_cast<double>(channel) / 255.0;
    return encoded <= 0.04045 ? encoded / 12.92
                             : std::pow((encoded + 0.055) / 1.055, 2.4);
}

double relativeLuminance(SrgbColor color) {
    return 0.2126 * linearChannel(color.red) +
           0.7152 * linearChannel(color.green) +
           0.0722 * linearChannel(color.blue);
}

double contrast(SrgbColor first, SrgbColor second) {
    const auto darker = std::min(relativeLuminance(first), relativeLuminance(second));
    const auto lighter = std::max(relativeLuminance(first), relativeLuminance(second));
    return (lighter + 0.05) / (darker + 0.05);
}

std::array<double, 3> lab(SrgbColor color) {
    const auto red = linearChannel(color.red);
    const auto green = linearChannel(color.green);
    const auto blue = linearChannel(color.blue);
    const std::array xyz{
        (0.4124564 * red + 0.3575761 * green + 0.1804375 * blue) / 0.95047,
        0.2126729 * red + 0.7151522 * green + 0.0721750 * blue,
        (0.0193339 * red + 0.1191920 * green + 0.9503041 * blue) / 1.08883,
    };
    std::array<double, 3> transformed{};
    std::transform(xyz.begin(), xyz.end(), transformed.begin(), [](double value) {
        return value > 0.008856 ? std::cbrt(value)
                               : 7.787 * value + 16.0 / 116.0;
    });
    return {116.0 * transformed[1] - 16.0,
            500.0 * (transformed[0] - transformed[1]),
            200.0 * (transformed[1] - transformed[2])};
}

double deltaE(SrgbColor first, SrgbColor second) {
    const auto a = lab(first);
    const auto b = lab(second);
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

std::optional<double> hslHueDegrees(SrgbColor color) {
    const auto red = static_cast<double>(color.red) / 255.0;
    const auto green = static_cast<double>(color.green) / 255.0;
    const auto blue = static_cast<double>(color.blue) / 255.0;
    const auto maxChannel = std::max({red, green, blue});
    const auto minChannel = std::min({red, green, blue});
    const auto chroma = maxChannel - minChannel;
    if (chroma <= 1e-9) return std::nullopt;
    double hue = 0.0;
    if (maxChannel == red) {
        hue = std::fmod((green - blue) / chroma, 6.0);
    } else if (maxChannel == green) {
        hue = (blue - red) / chroma + 2.0;
    } else {
        hue = (red - green) / chroma + 4.0;
    }
    hue *= 60.0;
    if (hue < 0.0) hue += 360.0;
    return hue;
}

double hueDistanceDegrees(double first, double second) {
    const auto distance = std::abs(first - second);
    return std::min(distance, 360.0 - distance);
}

std::array<SrgbColor, 6> colors(ssg::DiffTints const& tints);
SrgbColor resolved(SrgbColor color, ssg::ColorDepth depth);

void assertHueFidelityAtTruecolor(ssg::Theme const& theme, ssg::DiffTints const& tints) {
    constexpr double kHueToleranceDegrees = 40.0;
    constexpr auto kDepth = ssg::ColorDepth::Truecolor;
    const auto tintColors = colors(tints);
    const std::array anchorRoles{SemanticRole::GitAdded, SemanticRole::GitDeleted,
                                 SemanticRole::GitModified};
    for (std::size_t kind = 0; kind < anchorRoles.size(); ++kind) {
        const auto anchor = resolved(theme.palette()[theme.indexFor(anchorRoles[kind])],
                                     kDepth);
        const auto anchorHue = hslHueDegrees(anchor);
        ASSERT_TRUE(anchorHue.has_value());
        const auto rowHue = hslHueDegrees(resolved(tintColors[kind], kDepth));
        const auto wordHue = hslHueDegrees(resolved(tintColors[kind + 3], kDepth));
        ASSERT_TRUE(rowHue.has_value());
        ASSERT_TRUE(wordHue.has_value());
        ASSERT_TRUE(hueDistanceDegrees(*rowHue, *anchorHue) <= kHueToleranceDegrees);
        ASSERT_TRUE(hueDistanceDegrees(*wordHue, *anchorHue) <= kHueToleranceDegrees);
    }
}

void assertSemanticHueFamiliesAtTruecolor(ssg::DiffTints const& tints) {
    constexpr double kHueToleranceDegrees = 40.0;
    constexpr auto kDepth = ssg::ColorDepth::Truecolor;
    constexpr std::array kExpectedHues{120.0, 0.0, 34.0};
    const auto tintColors = colors(tints);
    for (std::size_t kind = 0; kind < kExpectedHues.size(); ++kind) {
        const auto rowHue = hslHueDegrees(resolved(tintColors[kind], kDepth));
        const auto wordHue = hslHueDegrees(resolved(tintColors[kind + 3], kDepth));
        ASSERT_TRUE(rowHue.has_value());
        ASSERT_TRUE(wordHue.has_value());
        ASSERT_TRUE(
            hueDistanceDegrees(*rowHue, kExpectedHues[kind]) <= kHueToleranceDegrees);
        ASSERT_TRUE(
            hueDistanceDegrees(*wordHue, kExpectedHues[kind]) <= kHueToleranceDegrees);
    }
}

std::array<SrgbColor, 6> colors(ssg::DiffTints const& tints) {
    return {tints.addedRow, tints.removedRow, tints.modifiedRow,
            tints.addedWord, tints.removedWord, tints.modifiedWord};
}

SrgbColor resolved(SrgbColor color, ssg::ColorDepth depth) {
    return ssg::resolveColor(color, depth).rgb;
}

// Readability is the load-bearing guarantee and is RELATIVE: a diff-row tint is a
// subtle wash over text whose foreground was chosen to read on the editor
// Background, so the tint must retain a fraction of each foreground's
// background-contrast (with an absolute floor), not independently reach a high
// absolute ratio (impossible for a theme with a mid-luminance accent — it would
// force every tint to near-black and erase the hue). Kind/word/row distinctness
// is asserted at Truecolor, where hue separation holds; Indexed256 quantization
// may soften it, and diff rows also read apart structurally (added vs phantom
// removed vs word-marked modified), so hue is not the only signal.
void assertDiffTintGates(ssg::ThemeSnapshot const& snapshot) {
    constexpr double kFloorContrast = 2.1;
    constexpr double kRetainContrast = 0.80;
    constexpr double kRowVisibleDeltaE = 2.0;
    std::array<SrgbColor, ssg::kSyntaxScopeCount + 1> foregrounds{};
    for (std::size_t index = 0; index < snapshot.syntaxIndices.size(); ++index) {
        foregrounds[index] = snapshot.palette[snapshot.syntaxIndices[index]];
    }
    foregrounds.back() =
        snapshot.palette[snapshot.semanticIndices[static_cast<std::size_t>(
            SemanticRole::Foreground)]];
    const auto background =
        snapshot.palette[snapshot.semanticIndices[static_cast<std::size_t>(
            SemanticRole::Background)]];

    const auto tintColors = colors(snapshot.diffTints);
    for (const auto depth :
         {ssg::ColorDepth::Truecolor, ssg::ColorDepth::Indexed256}) {
        for (const auto tint : tintColors) {
            for (const auto foreground : foregrounds) {
                const auto required = std::max(
                    kFloorContrast,
                    kRetainContrast * contrast(resolved(background, depth),
                                               resolved(foreground, depth)));
                ASSERT_TRUE(contrast(resolved(tint, depth),
                                     resolved(foreground, depth)) >=
                            required - 1e-9);
            }
        }
    }
    // Each row tint is a visible wash: distinguishable from the plain Background
    // at Truecolor (a diff row must read as diffed). Stronger kind/word hue
    // separation is best-effort (the shipped theme's derived washes achieve it;
    // a degenerate near-monochrome theme relies on structural cues), so it is not
    // asserted here — readability above is the load-bearing guarantee.
    const auto depth = ssg::ColorDepth::Truecolor;
    for (std::size_t first = 0; first < 3; ++first) {
        ASSERT_TRUE(deltaE(resolved(tintColors[first], depth),
                           resolved(background, depth)) >= kRowVisibleDeltaE);
    }
}

void assertSelectionFillGate(ssg::ThemeSnapshot const& snapshot) {
    constexpr double kFloorContrast = 2.1;
    constexpr double kRetainContrast = 0.80;
    std::array<SrgbColor, ssg::kSyntaxScopeCount + 1> foregrounds{};
    for (std::size_t index = 0; index < snapshot.syntaxIndices.size(); ++index) {
        foregrounds[index] = snapshot.palette[snapshot.syntaxIndices[index]];
    }

    foregrounds.back() =
        snapshot.palette[snapshot.semanticIndices[static_cast<std::size_t>(
            SemanticRole::Foreground)]];
    const auto background =
        snapshot.palette[snapshot.semanticIndices[static_cast<std::size_t>(
            SemanticRole::Background)]];
    const auto selectionAnchor =
        snapshot.palette[snapshot.semanticIndices[static_cast<std::size_t>(
            SemanticRole::Selection)]];

    bool anchorFails = false;
    for (const auto depth :
         {ssg::ColorDepth::Truecolor, ssg::ColorDepth::Indexed256}) {
        for (const auto foreground : foregrounds) {
            const auto required = std::max(
                kFloorContrast,
                kRetainContrast * contrast(resolved(background, depth),
                                           resolved(foreground, depth)));
            if (contrast(resolved(selectionAnchor, depth),
                         resolved(foreground, depth)) < required - 1e-9) {
                anchorFails = true;
            }
            ASSERT_TRUE(contrast(resolved(snapshot.selectionFill, depth),
                                 resolved(foreground, depth)) >=
                        required - 1e-9);
        }
    }
    ASSERT_TRUE(anchorFails);
}

void assertDistinctAtDepth(ssg::DiffTints const& tints, SrgbColor background,
                           ssg::ColorDepth depth) {
    const auto rowDeltaE =
        depth == ssg::ColorDepth::Truecolor ? 4.0 : 4.0;
    const auto tintColors = colors(tints);
    std::array<SrgbColor, 6> resolvedTints{};
    std::transform(tintColors.begin(), tintColors.end(), resolvedTints.begin(),
                   [depth](SrgbColor color) {
                       return resolved(color, depth);
                   });
    const auto resolvedBackground = resolved(background, depth);
    for (std::size_t first = 0; first < 3; ++first) {
        if (depth == ssg::ColorDepth::Truecolor) {
            for (std::size_t second = first + 1; second < 3; ++second) {
                ASSERT_TRUE(deltaE(resolvedTints[first], resolvedTints[second]) >=
                            4.0);
                ASSERT_TRUE(deltaE(resolvedTints[first + 3], resolvedTints[second + 3]) >=
                            4.0);
            }
            ASSERT_TRUE(deltaE(resolvedTints[first], resolvedTints[first + 3]) >=
                        1.0);
        }
        ASSERT_TRUE(deltaE(resolvedTints[first], resolvedBackground) >=
                    rowDeltaE);
        if (depth == ssg::ColorDepth::Indexed256) {
            ASSERT_TRUE(deltaE(resolvedTints[first + 3], resolvedBackground) >=
                        3.0);
        }
    }
}

bool passesDistinctAtDepth(ssg::DiffTints const& tints, SrgbColor background,
                           ssg::ColorDepth depth) {
    const auto rowDeltaE =
        depth == ssg::ColorDepth::Truecolor ? 4.0 : 4.0;
    const auto tintColors = colors(tints);
    std::array<SrgbColor, 6> resolvedTints{};
    std::transform(tintColors.begin(), tintColors.end(), resolvedTints.begin(),
                   [depth](SrgbColor color) {
                       return resolved(color, depth);
                   });
    const auto resolvedBackground = resolved(background, depth);
    for (std::size_t first = 0; first < 3; ++first) {
        if (depth == ssg::ColorDepth::Truecolor) {
            for (std::size_t second = first + 1; second < 3; ++second) {
                if (deltaE(resolvedTints[first], resolvedTints[second]) < 4.0 ||
                    deltaE(resolvedTints[first + 3], resolvedTints[second + 3]) <
                        4.0) {
                    return false;
                }
            }
            if (deltaE(resolvedTints[first], resolvedTints[first + 3]) < 1.0) {
                return false;
            }
        }
        if (deltaE(resolvedTints[first], resolvedBackground) < rowDeltaE) {
            return false;
        }
        if (depth == ssg::ColorDepth::Indexed256 &&
            deltaE(resolvedTints[first + 3], resolvedBackground) < 3.0) {
            return false;
        }
    }
    return true;
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

TEST(bundledThemeDataIsCompleteAndConstructible) {
    ASSERT_NO_THROW(bundledTheme());
}

TEST(diffTintsMeetResolvedReadabilityAndDistinctnessGates) {
    const auto theme = bundledTheme();
    const auto snapshot = theme.snapshot();
    bool naiveAnchorFails = false;
    for (const auto role : {SemanticRole::GitAdded, SemanticRole::GitDeleted,
                            SemanticRole::GitModified}) {
        const auto anchor = theme.palette()[theme.indexFor(role)];
        for (const auto scope : ssg::kAllSyntaxScopes) {
            if (contrast(anchor, theme.palette()[theme.indexFor(scope)]) < 3.0) {
                naiveAnchorFails = true;
            }
        }
    }
    ASSERT_TRUE(naiveAnchorFails);
    assertDiffTintGates(snapshot);
    assertSelectionFillGate(snapshot);
    for (const auto tint : colors(snapshot.diffTints)) {
        const auto indexed = ssg::resolveColor(tint, ssg::ColorDepth::Indexed256);
        ASSERT_TRUE(indexed.index >= 16);
        ASSERT_EQ(ssg::resolveColor(tint, ssg::ColorDepth::Truecolor).rgb, tint);
    }
}

void assertSnapshotDistinctAtBothDepths(const ssg::ThemeSnapshot& snapshot) {
    const auto background =
        snapshot.palette[snapshot.semanticIndices[static_cast<std::size_t>(
            SemanticRole::Background)]];
    assertDistinctAtDepth(snapshot.diffTints, background, ssg::ColorDepth::Truecolor);
    assertDistinctAtDepth(snapshot.diffTints, background, ssg::ColorDepth::Indexed256);
}

ssg::Theme nearMonochromeFixtureTheme() {
    auto nearMonochromeBase = bundledTheme();
    auto nearMonochromeColors = std::vector<IndexedColor>{};
    for (std::size_t index = 0; index < nearMonochromeBase.palette().size(); ++index) {
        nearMonochromeColors.push_back(
            {static_cast<std::uint8_t>(index), nearMonochromeBase.palette()[index]});
    }
    nearMonochromeColors[nearMonochromeBase.indexFor(SemanticRole::GitAdded)].color =
        {118, 119, 120};
    nearMonochromeColors[nearMonochromeBase.indexFor(SemanticRole::GitDeleted)].color =
        {120, 119, 118};
    nearMonochromeColors[nearMonochromeBase.indexFor(SemanticRole::GitModified)].color =
        {119, 120, 118};
    auto nearMonochromeRoles = roles();
    for (auto& mapping : nearMonochromeRoles) {
        mapping.paletteIndex = nearMonochromeBase.indexFor(mapping.role);
    }
    auto nearMonochromeSyntax = syntax();
    for (auto& mapping : nearMonochromeSyntax) {
        mapping.paletteIndex = nearMonochromeBase.indexFor(mapping.scope);
    }
    return ssg::Theme{"near-monochrome", nearMonochromeColors, nearMonochromeRoles,
                      nearMonochromeSyntax};
}

ssg::Theme lightFixtureTheme() {
    auto lightBase = bundledTheme();
    auto lightColors = std::vector<IndexedColor>{};
    for (std::size_t index = 0; index < lightBase.palette().size(); ++index) {
        lightColors.push_back(
            {static_cast<std::uint8_t>(index), lightBase.palette()[index]});
    }
    lightColors[lightBase.indexFor(SemanticRole::Background)].color = {225, 225, 225};
    lightColors[lightBase.indexFor(SemanticRole::Foreground)].color = {30, 30, 30};
    lightColors[lightBase.indexFor(SemanticRole::GitAdded)].color = {118, 119, 120};
    lightColors[lightBase.indexFor(SemanticRole::GitDeleted)].color = {120, 119, 118};
    lightColors[lightBase.indexFor(SemanticRole::GitModified)].color = {119, 120, 118};
    auto lightRoles = roles();
    for (auto& mapping : lightRoles) {
        mapping.paletteIndex = lightBase.indexFor(mapping.role);
    }
    auto lightSyntax = syntax();
    for (auto& mapping : lightSyntax) {
        mapping.paletteIndex = lightBase.indexFor(SemanticRole::Foreground);
    }
    return ssg::Theme{"light", lightColors, lightRoles, lightSyntax};
}

ssg::Theme rotatedAnchorFixtureTheme() {
    auto base = bundledTheme();
    auto colors = std::vector<IndexedColor>{};
    for (std::size_t index = 0; index < base.palette().size(); ++index) {
        colors.push_back(
            {static_cast<std::uint8_t>(index), base.palette()[index]});
    }
    colors[base.indexFor(SemanticRole::GitAdded)].color = {77, 129, 227};
    colors[base.indexFor(SemanticRole::GitDeleted)].color = {225, 96, 184};
    colors[base.indexFor(SemanticRole::GitModified)].color = {206, 176, 80};
    auto roleMappings = roles();
    for (auto& mapping : roleMappings) {
        mapping.paletteIndex = base.indexFor(mapping.role);
    }
    auto syntaxMappings = syntax();
    for (auto& mapping : syntaxMappings) {
        mapping.paletteIndex = base.indexFor(mapping.scope);
    }
    return ssg::Theme{"rotated-anchors", colors, roleMappings, syntaxMappings};
}

TEST(diffTintsRemainDistinctAtTruecolorAndIndexed256) {
    assertSnapshotDistinctAtBothDepths(bundledTheme().snapshot());
}

TEST(diffTintsTrackBundledGitAnchorHuesAtTruecolor) {
    const auto theme = bundledTheme();
    assertHueFidelityAtTruecolor(theme, theme.snapshot().diffTints);
}

TEST(bundledThemeDiffTintsUsePrimaryDerivationPath) {
    const auto snapshot = bundledTheme().snapshot();
    const auto result = ssg::deriveDiffTintsWithPath(
        snapshot.palette, snapshot.semanticIndices, snapshot.syntaxIndices);
    ASSERT_TRUE(result.path == ssg::DiffTintDerivationPath::Primary);
}

TEST(nearMonochromeDiffTintsRemainDistinctAtTruecolorAndIndexed256) {
    const auto theme = nearMonochromeFixtureTheme();
    assertSnapshotDistinctAtBothDepths(theme.snapshot());
    assertSemanticHueFamiliesAtTruecolor(theme.snapshot().diffTints);
}

TEST(lightFixtureDiffTintsRemainDistinctAtTruecolorAndIndexed256) {
    const auto theme = lightFixtureTheme();
    assertSnapshotDistinctAtBothDepths(theme.snapshot());
    assertSemanticHueFamiliesAtTruecolor(theme.snapshot().diffTints);
}

TEST(indexed256DistinctnessStillRejectsBackgroundCollapse) {
    const ssg::DiffTints collapsed{{0, 0, 0},
                                   {38, 38, 38},
                                   {88, 88, 88},
                                   {128, 128, 128},
                                   {138, 138, 138},
                                   {188, 188, 188}};
    ASSERT_FALSE(passesDistinctAtDepth(collapsed, {0, 0, 0},
                                       ssg::ColorDepth::Indexed256));
}

TEST(nearMonochromeAnchorsUseAReadableDistinctFallback) {
    const auto snapshot = nearMonochromeFixtureTheme().snapshot();
    assertDiffTintGates(snapshot);
    const auto result = ssg::deriveDiffTintsWithPath(
        snapshot.palette, snapshot.semanticIndices, snapshot.syntaxIndices);
    ASSERT_TRUE(result.path == ssg::DiffTintDerivationPath::Rescue);
}

TEST(lightThemeFallbackRemainsReadableAndDistinct) {
    const auto snapshot = lightFixtureTheme().snapshot();
    assertDiffTintGates(snapshot);
    const auto result = ssg::deriveDiffTintsWithPath(
        snapshot.palette, snapshot.semanticIndices, snapshot.syntaxIndices);
    ASSERT_TRUE(result.path == ssg::DiffTintDerivationPath::Rescue);
}

TEST(rotatedAnchorThemeUsesPrimaryDerivationPathAndTracksAnchorHues) {
    const auto theme = rotatedAnchorFixtureTheme();
    const auto snapshot = theme.snapshot();
    assertDiffTintGates(snapshot);
    assertSnapshotDistinctAtBothDepths(snapshot);
    assertHueFidelityAtTruecolor(theme, snapshot.diffTints);
    const auto result = ssg::deriveDiffTintsWithPath(
        snapshot.palette, snapshot.semanticIndices, snapshot.syntaxIndices);
    ASSERT_TRUE(result.path == ssg::DiffTintDerivationPath::Primary);
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
            relative.starts_with("data/themes/") ||
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
    RUN(bundledThemeDataIsCompleteAndConstructible);
    RUN(diffTintsMeetResolvedReadabilityAndDistinctnessGates);
    RUN(diffTintsRemainDistinctAtTruecolorAndIndexed256);
    RUN(diffTintsTrackBundledGitAnchorHuesAtTruecolor);
    RUN(bundledThemeDiffTintsUsePrimaryDerivationPath);
    RUN(nearMonochromeDiffTintsRemainDistinctAtTruecolorAndIndexed256);
    RUN(lightFixtureDiffTintsRemainDistinctAtTruecolorAndIndexed256);
    RUN(indexed256DistinctnessStillRejectsBackgroundCollapse);
    RUN(nearMonochromeAnchorsUseAReadableDistinctFallback);
    RUN(lightThemeFallbackRemainsReadableAndDistinct);
    RUN(rotatedAnchorThemeUsesPrimaryDerivationPathAndTracksAnchorHues);
    RUN(sourceAndConfigHaveNoIndependentColorSources);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
