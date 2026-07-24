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

// theme.define's 16 classic ANSI palette-slot names, in the SAME order as
// the palette's own 0-15 indices (see doc/spec-config.md's Considerations:
// this vocabulary was chosen over SemanticRole names because it matches
// how every terminal color-scheme config already names things, and it
// fits the palette's own 16-slot shape exactly).
constexpr std::array kAnsiSlotNames{
    std::string_view{"black"},
    std::string_view{"red"},
    std::string_view{"green"},
    std::string_view{"yellow"},
    std::string_view{"blue"},
    std::string_view{"magenta"},
    std::string_view{"cyan"},
    std::string_view{"white"},
    std::string_view{"brightBlack"},
    std::string_view{"brightRed"},
    std::string_view{"brightGreen"},
    std::string_view{"brightYellow"},
    std::string_view{"brightBlue"},
    std::string_view{"brightMagenta"},
    std::string_view{"brightCyan"},
    std::string_view{"brightWhite"},
};
static_assert(kAnsiSlotNames.size() == kThemePaletteSize);

std::optional<std::size_t> ansiSlotIndex(std::string_view name) noexcept {
    for (std::size_t index = 0; index < kAnsiSlotNames.size(); ++index) {
        if (kAnsiSlotNames[index] == name) return index;
    }
    return std::nullopt;
}

// Parses a "#rrggbb" literal (exactly '#' followed by 6 hex digits; no
// short form, no alpha channel -- the one shape theme.define accepts).
std::optional<SrgbColor> parseHexColor(std::string_view text) noexcept {
    if (text.size() != 7 || text[0] != '#') return std::nullopt;
    std::array<std::uint8_t, 3> channels{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        std::uint8_t value = 0;
        for (std::size_t digit = 0; digit < 2; ++digit) {
            const char byte = text[1 + channel * 2 + digit];
            std::uint8_t nibble = 0;
            if (byte >= '0' && byte <= '9') {
                nibble = static_cast<std::uint8_t>(byte - '0');
            } else if (byte >= 'a' && byte <= 'f') {
                nibble = static_cast<std::uint8_t>(byte - 'a' + 10);
            } else if (byte >= 'A' && byte <= 'F') {
                nibble = static_cast<std::uint8_t>(byte - 'A' + 10);
            } else {
                return std::nullopt;
            }
            value = static_cast<std::uint8_t>((value << 4) | nibble);
        }
        channels[channel] = value;
    }
    return SrgbColor::fromSerializedChannels(channels[0], channels[1],
                                             channels[2]);
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

// Selection fill is a subtle background wash laid over text whose foreground
// was already chosen to read on the editor Background. Readability is
// therefore relative: a tint must not drop any foreground's contrast below a
// fraction of what it had on the plain Background, subject to a hard
// absolute floor. Requiring the tint to independently reach a high absolute
// ratio against every syntax foreground is impossible for a theme with a
// mid-luminance accent (it forces the tint to near-black and erases the
// hue); this relative rule matches how editors actually tint a selection.
constexpr double kFloorContrast = 2.1;
constexpr double kRetainContrast = 0.80;

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

// Selection fill is the one remaining derived (not flat-anchor) tint: walk
// from Background toward the Selection role's own color, keeping the
// boldest weight that stays readable against every syntax foreground.
SrgbColor strongestReadableTint(
    SrgbColor background, SrgbColor anchor, double desiredWeight,
    double retainedSaturation,
    std::array<SrgbColor, kSyntaxScopeCount + 1> const& foregrounds) noexcept {
    const auto mutedAnchor = desaturate(anchor, retainedSaturation);
    const auto steps = static_cast<int>(std::lround(desiredWeight * 100.0));
    for (int step = steps; step >= 1; --step) {
        const auto candidate =
            interpolate(background, mutedAnchor, static_cast<double>(step) / 100.0);
        if (readable(candidate, background, foregrounds)) {
            return candidate;
        }
    }
    return background;
}

} // namespace

// Each diff kind uses ONE flat color straight from the theme's own Git
// anchor role -- no blending, no desaturation, no readability search. Row
// and word share that same color for a given kind; a word mark's job is
// only to say "here specifically" within an already-tinted row.
DiffTints deriveDiffTints(
    std::array<SrgbColor, kThemePaletteSize> const& palette,
    std::array<std::uint8_t, kSemanticRoleCount> const& semanticIndices,
    std::array<std::uint8_t, kSyntaxScopeCount> const&) noexcept {
    const auto added = palette[semanticIndices[position(SemanticRole::GitAdded)]];
    const auto deleted = palette[semanticIndices[position(SemanticRole::GitDeleted)]];
    const auto modified = palette[semanticIndices[position(SemanticRole::GitModified)]];
    return {.addedRow = added,
            .removedRow = deleted,
            .modifiedRow = modified,
            .addedWord = added,
            .removedWord = deleted,
            // Word-level marks inside a modified line reuse the Added/
            // Removed colors directly (the added/inserted span reads as
            // "added", the phantom row's removed span reads as "removed");
            // there is no separate fourth shade for "modified word".
            .modifiedWord = added};
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

ThemeDefineResult applyThemeDefine(ThemeSnapshot const& current,
                                   ThemeDefineArguments const& arguments) noexcept {
    // Validate the WHOLE table before replacing anything -- an unknown slot
    // name or a malformed hex string rejects the whole call, never a
    // partial apply (see doc/spec-config.md's Acceptance: all-or-nothing).
    auto palette = current.palette;
    for (auto const& [name, hex] : arguments.colors) {
        const auto index = ansiSlotIndex(name);
        if (!index) {
            return {ThemeDefineError{"unknown palette slot name: " + name}, {}};
        }
        const auto color = parseHexColor(hex);
        if (!color) {
            return {ThemeDefineError{"invalid \"#rrggbb\" color for " + name +
                                     ": " + hex},
                    {}};
        }
        palette[*index] = *color;
    }

    ThemeSnapshot next{palette, current.semanticIndices, current.syntaxIndices,
                       {}, {}};
    next.diffTints =
        deriveDiffTints(next.palette, next.semanticIndices, next.syntaxIndices);
    next.selectionFill = deriveSelectionFill(next.palette, next.semanticIndices,
                                             next.syntaxIndices);
    return {std::nullopt, next};
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
