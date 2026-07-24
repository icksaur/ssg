#include "ssg/Theme.h"

#include "ssg/color.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace ssg {
namespace {

constexpr std::array kSemanticNames{
    std::string_view{"foreground"},
    std::string_view{"background"},
    std::string_view{"caret"},
    std::string_view{"selection"},
    std::string_view{"diagnostic_error"},
    std::string_view{"diagnostic_warning"},
    std::string_view{"diagnostic_info"},
    std::string_view{"diagnostic_hint"},
    std::string_view{"git_added"},
    std::string_view{"git_modified"},
    std::string_view{"git_deleted"},
    std::string_view{"git_conflict"},
    std::string_view{"tree_background"},
    std::string_view{"tree_focus"},
    std::string_view{"tab_active"},
    std::string_view{"tab_inactive"},
    std::string_view{"panel_active"},
    std::string_view{"panel_inactive"},
    std::string_view{"header"},
    std::string_view{"footer"},
    std::string_view{"status_info"},
    std::string_view{"status_warning"},
    std::string_view{"status_error"},
    std::string_view{"line_number"},
    std::string_view{"active_line_number"},
    std::string_view{"search_match"},
    std::string_view{"prompt"},
    std::string_view{"scrollbar_track"},
    std::string_view{"scrollbar_thumb"},
    std::string_view{"diff_added"},
    std::string_view{"diff_removed"},
    std::string_view{"diff_modified"},
};
static_assert(kSemanticNames.size() == kSemanticRoleCount);

constexpr std::array kSyntaxNames{
    std::string_view{"plain_text"},
    std::string_view{"comment"},
    std::string_view{"keyword"},
    std::string_view{"string"},
    std::string_view{"number"},
    std::string_view{"type"},
    std::string_view{"function"},
    std::string_view{"variable"},
    std::string_view{"operator"},
    std::string_view{"punctuation"},
    std::string_view{"invalid"},
};
static_assert(kSyntaxNames.size() == kSyntaxScopeCount);

constexpr std::size_t position(SemanticRole role) noexcept {
    return static_cast<std::size_t>(role);
}

constexpr std::size_t position(SyntaxScope scope) noexcept {
    return static_cast<std::size_t>(scope);
}

constexpr bool valid(SemanticRole role) noexcept {
    return position(role) < kSemanticRoleCount;
}

constexpr bool valid(SyntaxScope scope) noexcept {
    return position(scope) < kSyntaxScopeCount;
}

void validatePaletteIndex(std::uint8_t index) {
    if (index >= kThemePaletteSize) {
        throw std::invalid_argument("theme mapping palette index must be in [0, 15]");
    }
}

// A diff-row tint is a subtle background wash laid over text whose foreground was
// already chosen to read on the editor Background. Readability is therefore
// relative: a tint must not drop any foreground's contrast below a fraction of
// what it had on the plain Background, subject to a hard absolute floor. Requiring
// each tint to independently reach a high absolute ratio against every syntax
// foreground is impossible for a theme with a mid-luminance accent (it forces the
// tint to near-black and erases the hue); this relative rule matches how editors
// actually tint diff rows.
constexpr double kFloorContrast = 2.1;
constexpr double kRetainContrast = 0.80;
constexpr double kKindDeltaETruecolor = 4.0;
constexpr double kWordDeltaETruecolor = 1.0;
constexpr double kRowDeltaETruecolor = 4.0;
// Indexed 256-color quantization collapses close washes; keep a smaller
// non-equality floor so themes avoid unnecessary fallback while still requiring
// observable separation after quantization.
constexpr double kKindDeltaEIndexed256 = 0.01;
constexpr double kWordDeltaEIndexed256 = 0.01;
constexpr double kRowDeltaEIndexed256 = 0.01;

double linearChannel(std::uint8_t channel) noexcept {
    const double encoded = static_cast<double>(channel) / 255.0;
    return encoded <= 0.04045 ? encoded / 12.92
                             : std::pow((encoded + 0.055) / 1.055, 2.4);
}

double luminance(SrgbColor color) noexcept {
    return 0.2126 * linearChannel(color.red) +
           0.7152 * linearChannel(color.green) +
           0.0722 * linearChannel(color.blue);
}

double contrast(SrgbColor first, SrgbColor second) noexcept {
    const auto darker = std::min(luminance(first), luminance(second));
    const auto lighter = std::max(luminance(first), luminance(second));
    return (lighter + 0.05) / (darker + 0.05);
}

std::array<double, 3> lab(SrgbColor color) noexcept {
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

double deltaE(SrgbColor first, SrgbColor second) noexcept {
    const auto a = lab(first);
    const auto b = lab(second);
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

SrgbColor interpolate(SrgbColor first, SrgbColor second, double weight) noexcept {
    const auto channel = [weight](std::uint8_t from, std::uint8_t to) {
        return static_cast<std::uint8_t>(std::clamp(
            std::lround(static_cast<double>(from) +
                        (static_cast<double>(to) - from) * weight),
            0L, 255L));
    };
    return {channel(first.red, second.red), channel(first.green, second.green),
            channel(first.blue, second.blue)};
}

SrgbColor desaturate(SrgbColor color, double retainedSaturation) noexcept {
    const auto linearGray = luminance(color);
    const auto encodedGray =
        linearGray <= 0.0031308
            ? 12.92 * linearGray
            : 1.055 * std::pow(linearGray, 1.0 / 2.4) - 0.055;
    const auto gray = static_cast<std::uint8_t>(
        std::clamp(std::lround(255.0 * encodedGray), 0L, 255L));
    return interpolate({gray, gray, gray}, color, retainedSaturation);
}

std::array<SrgbColor, 6> colors(DiffTints const& tints) noexcept {
    return {tints.addedRow, tints.removedRow, tints.modifiedRow,
            tints.addedWord, tints.removedWord, tints.modifiedWord};
}

std::array<SrgbColor, kSyntaxScopeCount + 1> foregrounds(
    std::array<SrgbColor, kThemePaletteSize> const& palette,
    std::array<std::uint8_t, kSemanticRoleCount> const& semanticIndices,
    std::array<std::uint8_t, kSyntaxScopeCount> const& syntaxIndices) noexcept {
    std::array<SrgbColor, kSyntaxScopeCount + 1> colors{};
    for (std::size_t index = 0; index < syntaxIndices.size(); ++index) {
        colors[index] = palette[syntaxIndices[index]];
    }
    colors.back() = palette[semanticIndices[position(SemanticRole::Foreground)]];
    return colors;
}

SrgbColor background(
    std::array<SrgbColor, kThemePaletteSize> const& palette,
    std::array<std::uint8_t, kSemanticRoleCount> const& semanticIndices) noexcept {
    return palette[semanticIndices[position(SemanticRole::Background)]];
}

bool readable(SrgbColor tint, SrgbColor background,
              std::array<SrgbColor, kSyntaxScopeCount + 1> const& foregrounds)
    noexcept {
    for (const auto depth : {ColorDepth::Truecolor, ColorDepth::Indexed256}) {
        const auto resolvedTint = resolveColor(tint, depth).rgb;
        const auto resolvedBackground = resolveColor(background, depth).rgb;
        for (const auto foreground : foregrounds) {
            const auto resolvedForeground = resolveColor(foreground, depth).rgb;
            const auto required = std::max(
                kFloorContrast,
                kRetainContrast * contrast(resolvedBackground, resolvedForeground));
            if (contrast(resolvedTint, resolvedForeground) < required) {
                return false;
            }
        }
    }
    return true;
}

bool readable(
    DiffTints const& tints, SrgbColor background,
    std::array<SrgbColor, kSyntaxScopeCount + 1> const& foregrounds) noexcept {
    return std::ranges::all_of(colors(tints), [&](SrgbColor tint) {
        return readable(tint, background, foregrounds);
    });
}

SrgbColor strongestReadableTint(
    SrgbColor background, SrgbColor anchor, double desiredWeight,
    double retainedSaturation,
    std::array<SrgbColor, kSyntaxScopeCount + 1> const& foregrounds) noexcept {
    const auto mutedAnchor = desaturate(anchor, retainedSaturation);
    const auto steps = static_cast<int>(std::lround(desiredWeight * 100.0));
    for (int step = steps; step >= 1; --step) {
        const auto candidate =
            interpolate(background, mutedAnchor, static_cast<double>(step) / 100.0);
        if (readable(candidate, background, foregrounds)) return candidate;
    }
    return background;
}

bool distinct(DiffTints const& tints, SrgbColor background) noexcept {
    const auto tintColors = colors(tints);
    for (const auto depth : {ColorDepth::Truecolor, ColorDepth::Indexed256}) {
        const auto kindDeltaE = depth == ColorDepth::Truecolor
                                    ? kKindDeltaETruecolor
                                    : kKindDeltaEIndexed256;
        const auto wordDeltaE = depth == ColorDepth::Truecolor
                                    ? kWordDeltaETruecolor
                                    : kWordDeltaEIndexed256;
        const auto rowDeltaE = depth == ColorDepth::Truecolor
                                   ? kRowDeltaETruecolor
                                   : kRowDeltaEIndexed256;
        std::array<SrgbColor, 6> resolved{};
        std::transform(tintColors.begin(), tintColors.end(), resolved.begin(),
                       [depth](SrgbColor color) {
                           return resolveColor(color, depth).rgb;
                       });
        const auto resolvedBackground = resolveColor(background, depth).rgb;
        for (std::size_t first = 0; first < 3; ++first) {
            for (std::size_t second = first + 1; second < 3; ++second) {
                if (deltaE(resolved[first], resolved[second]) < kindDeltaE ||
                    deltaE(resolved[first + 3], resolved[second + 3]) <
                        kindDeltaE) {
                    return false;
                }
            }
            if (deltaE(resolved[first], resolved[first + 3]) < wordDeltaE ||
                deltaE(resolved[first], resolvedBackground) < rowDeltaE) {
                return false;
            }
        }
    }
    return true;
}

DiffTints fixedFallback(bool lightBackground) noexcept {
    if (lightBackground) {
        return {{215, 255, 215}, {255, 215, 215}, {215, 215, 255},
                {175, 255, 175}, {255, 215, 175}, {175, 215, 255}};
    }
    return {{0, 0, 0}, {0, 0, 95}, {0, 0, 135},
            {0, 0, 95}, {0, 0, 135}, {95, 0, 0}};
}

} // namespace

DiffTints deriveDiffTints(
    std::array<SrgbColor, kThemePaletteSize> const& palette,
    std::array<std::uint8_t, kSemanticRoleCount> const& semanticIndices,
    std::array<std::uint8_t, kSyntaxScopeCount> const& syntaxIndices) noexcept {
    const auto allForegrounds = foregrounds(palette, semanticIndices, syntaxIndices);
    const auto themeBackground = background(palette, semanticIndices);
    const std::array anchors{
        palette[semanticIndices[position(SemanticRole::GitAdded)]],
        palette[semanticIndices[position(SemanticRole::GitDeleted)]],
        palette[semanticIndices[position(SemanticRole::GitModified)]],
    };

    DiffTints derived;
    auto derivedColors = std::array<SrgbColor*, 6>{
        &derived.addedRow, &derived.removedRow, &derived.modifiedRow,
        &derived.addedWord, &derived.removedWord, &derived.modifiedWord};
    for (std::size_t kind = 0; kind < anchors.size(); ++kind) {
        *derivedColors[kind] = strongestReadableTint(
            themeBackground, anchors[kind], 0.40, 0.60, allForegrounds);
        *derivedColors[kind + 3] = strongestReadableTint(
            themeBackground, anchors[kind], 0.72, 0.85, allForegrounds);
    }

    // Prefer the derived washes when they are readable and stay distinct at both
    // Truecolor and Indexed256 depths; their hue comes from the theme's own Git
    // anchors. A fixed set rescues only a degenerate theme whose near-monochrome
    // anchors make the derived tints indistinguishable. If nothing is both
    // readable and distinct, keep the readable derived washes (readability is
    // the primary guarantee).
    if (readable(derived, themeBackground, allForegrounds) &&
        distinct(derived, themeBackground)) {
        return derived;
    }
    const bool lightBackground = luminance(themeBackground) > 0.5;
    const auto preferredFallback = fixedFallback(lightBackground);
    if (readable(preferredFallback, themeBackground, allForegrounds) &&
        distinct(preferredFallback, themeBackground)) {
        return preferredFallback;
    }
    const auto alternateFallback = fixedFallback(!lightBackground);
    if (readable(alternateFallback, themeBackground, allForegrounds) &&
        distinct(alternateFallback, themeBackground)) {
        return alternateFallback;
    }
    return derived;
}

SrgbColor deriveSelectionFill(
    std::array<SrgbColor, kThemePaletteSize> const& palette,
    std::array<std::uint8_t, kSemanticRoleCount> const& semanticIndices,
    std::array<std::uint8_t, kSyntaxScopeCount> const& syntaxIndices) noexcept {
    const auto allForegrounds = foregrounds(palette, semanticIndices, syntaxIndices);
    const auto themeBackground = background(palette, semanticIndices);
    const auto selectionAnchor =
        palette[semanticIndices[position(SemanticRole::Selection)]];
    return strongestReadableTint(themeBackground, selectionAnchor, 0.40, 0.60,
                                 allForegrounds);
}

std::string_view semanticRoleName(SemanticRole role) {
    if (!valid(role)) throw std::invalid_argument("semantic role is not recognized");
    return kSemanticNames[position(role)];
}

std::optional<SemanticRole> semanticRoleFromName(std::string_view name) {
    for (std::size_t index = 0; index < kSemanticNames.size(); ++index) {
        if (kSemanticNames[index] == name) return kAllSemanticRoles[index];
    }
    return std::nullopt;
}

std::string_view syntaxScopeName(SyntaxScope scope) {
    if (!valid(scope)) throw std::invalid_argument("syntax scope is not recognized");
    return kSyntaxNames[position(scope)];
}

std::optional<SyntaxScope> syntaxScopeFromName(std::string_view name) {
    for (std::size_t index = 0; index < kSyntaxNames.size(); ++index) {
        if (kSyntaxNames[index] == name) return kAllSyntaxScopes[index];
    }
    return std::nullopt;
}

Theme::Theme(std::string name,
             std::span<const IndexedColor> palette,
             std::span<const RoleMapping> semanticMappings,
             std::span<const SyntaxMapping> syntaxMappings)
    : name_(std::move(name)) {
    if (name_.empty()) throw std::invalid_argument("theme name must not be empty");
    if (palette.size() != kThemePaletteSize) {
        throw std::invalid_argument("theme palette must contain exactly 16 indexed colors");
    }
    std::array<bool, kThemePaletteSize> seenPalette{};
    for (const auto& entry : palette) {
        if (entry.index >= kThemePaletteSize) {
            throw std::invalid_argument("theme palette index must be in [0, 15]");
        }
        if (seenPalette[entry.index]) {
            throw std::invalid_argument("theme palette contains a duplicate index");
        }
        seenPalette[entry.index] = true;
        palette_[entry.index] = entry.color;
    }

    if (semanticMappings.size() != kSemanticRoleCount) {
        throw std::invalid_argument("theme must map every semantic role exactly once");
    }
    std::array<bool, kSemanticRoleCount> seenRoles{};
    for (const auto& mapping : semanticMappings) {
        if (!valid(mapping.role)) {
            throw std::invalid_argument("theme contains an unknown semantic role");
        }
        validatePaletteIndex(mapping.paletteIndex);
        const auto index = position(mapping.role);
        if (seenRoles[index]) {
            throw std::invalid_argument("theme contains a duplicate semantic role");
        }
        seenRoles[index] = true;
        semanticIndices_[index] = mapping.paletteIndex;
    }

    if (syntaxMappings.size() != kSyntaxScopeCount) {
        throw std::invalid_argument("theme must map every syntax scope exactly once");
    }
    std::array<bool, kSyntaxScopeCount> seenScopes{};
    for (const auto& mapping : syntaxMappings) {
        if (!valid(mapping.scope)) {
            throw std::invalid_argument("theme contains an unknown syntax scope");
        }
        validatePaletteIndex(mapping.paletteIndex);
        const auto index = position(mapping.scope);
        if (seenScopes[index]) {
            throw std::invalid_argument("theme contains a duplicate syntax scope");
        }
        seenScopes[index] = true;
        syntaxIndices_[index] = mapping.paletteIndex;
    }

    for (const auto& pair : kCoVisibleRolePairs) {
        if (semanticIndices_[position(pair.first)] ==
            semanticIndices_[position(pair.second)]) {
            throw std::invalid_argument(
                "co-visible semantic roles must use distinct palette indices");
        }
    }
}

std::uint8_t Theme::indexFor(SemanticRole role) const {
    if (!valid(role)) throw std::invalid_argument("semantic role is not recognized");
    return semanticIndices_[position(role)];
}

std::uint8_t Theme::indexFor(SyntaxScope scope) const {
    if (!valid(scope)) throw std::invalid_argument("syntax scope is not recognized");
    return syntaxIndices_[position(scope)];
}

std::uint8_t Theme::indexForSyntax(std::string_view scope) const noexcept {
    const auto recognized = syntaxScopeFromName(scope);
    return syntaxIndices_[position(recognized.value_or(SyntaxScope::PlainText))];
}

ThemeSnapshot Theme::snapshot() const noexcept {
    return {palette_, semanticIndices_, syntaxIndices_,
            deriveDiffTints(palette_, semanticIndices_, syntaxIndices_),
            deriveSelectionFill(palette_, semanticIndices_, syntaxIndices_)};
}

} // namespace ssg
