#include "ssg/Theme.h"

#include <array>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ssg {
namespace {

constexpr std::array kSemanticNames{
    std::string_view{"text"},
    std::string_view{"canvas"},
    std::string_view{"caret"},
    std::string_view{"selection"},
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
    std::string_view{"line_number"},
    std::string_view{"search_match"},
    std::string_view{"prompt"},
    std::string_view{"scrollbar_track"},
    std::string_view{"scrollbar_thumb"},
    std::string_view{"diff_added"},
    std::string_view{"diff_removed"},
    std::string_view{"diff_modified"},
    std::string_view{"tab_inactive_background"},
    std::string_view{"header_background"},
    std::string_view{"footer_background"},
    std::string_view{"current_line_number"},
    std::string_view{"current_line_number_background"},
    std::string_view{"line_number_background"},
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

// Parses a "#rrggbb" literal (exactly '#' followed by 6 hex digits; no short
// form, no alpha channel -- the one shape theme.set accepts).
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

} // namespace

SrgbColor ThemeSnapshot::color(SemanticRole role) const {
    if (!valid(role)) throw std::invalid_argument("semantic role is not recognized");
    return roleColors[position(role)];
}
SrgbColor ThemeSnapshot::color(SyntaxScope scope) const {
    if (!valid(scope)) throw std::invalid_argument("syntax scope is not recognized");
    return syntaxColors[position(scope)];
}

ThemeSetResult ThemeSnapshot::withOverrides(
    ThemeSetArguments const& arguments) const noexcept {
    // Validate the WHOLE table before replacing anything: an unknown name or a
    // malformed hex string rejects the whole call, never a partial apply. Role
    // and scope names share one namespace (disjoint sets).
    auto next = *this;
    for (auto const& [name, hex] : arguments.colors) {
        const auto color = parseHexColor(hex);
        if (!color) {
            return {ThemeSetError{"invalid \"#rrggbb\" color for " + name + ": " +
                                  hex},
                    {}};
        }
        if (const auto role = semanticRoleFromName(name)) {
            next.roleColors[position(*role)] = *color;
        } else if (const auto scope = syntaxScopeFromName(name)) {
            next.syntaxColors[position(*scope)] = *color;
        } else {
            return {ThemeSetError{"unknown theme color name: " + name}, {}};
        }
    }
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

} // namespace ssg
