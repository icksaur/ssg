#include "ssg/Theme.h"

#include <array>
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

// A flat anchor color, matching deriveDiffTints's model exactly: the
// Selection role's own palette color, used directly with no blend, no
// desaturation, and no readability search against it. This is deliberate:
// a selected tab (TabActive), a selected tree row (TreeFocus), and a text
// selection all read the SAME palette entry the same way, so all three
// render as visually identical highlights -- there is exactly one
// "selected" color in a theme, not a separate muted derivative for text.
// (An earlier revision computed a desaturated, readability-guarded blend
// toward Background here instead; that made the text-selection wash
// visibly dimmer than the flat TabActive/TreeFocus highlight even though
// both point at the same Selection-adjacent palette index, which is the
// mismatch this flat model corrects.)
SrgbColor deriveSelectionFill(
    std::array<SrgbColor, kThemePaletteSize> const& palette,
    std::array<std::uint8_t, kSemanticRoleCount> const& semanticIndices,
    std::array<std::uint8_t, kSyntaxScopeCount> const&) noexcept {
    return palette[semanticIndices[position(SemanticRole::Selection)]];
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
