#include <ssg/Theme.h>

#include <array>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ssg {
namespace {

inline constexpr std::array kSemanticNames{
    "text", "canvas", "caret", "selection", "tree_background", "tree_focus",
    "tab_active", "tab_inactive", "panel_active", "panel_inactive", "header",
    "footer", "status_info", "status_warning", "line_number", "search_match",
    "prompt", "scrollbar_track", "scrollbar_thumb", "diff_added", "diff_removed",
    "diff_modified", "tab_inactive_background", "header_background",
    "footer_background", "current_line_number", "current_line_number_background",
    "line_number_background",
};
inline constexpr std::array kSyntaxNames{
    "plain_text", "comment", "keyword", "string", "number", "type", "function",
    "variable", "operator", "punctuation", "invalid",
};

constexpr std::size_t position(SemanticRole role) noexcept {
    return static_cast<std::size_t>(role);
}

constexpr std::size_t position(SyntaxScope scope) noexcept {
    return static_cast<std::size_t>(scope);
}

constexpr bool valid(SemanticRole role) noexcept {
    return position(role) < kSemanticRoleCount;
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

std::optional<SemanticRole> semanticRoleFromName(std::string_view name) {
    for (std::size_t index = 0; index < kSemanticNames.size(); ++index) {
        if (kSemanticNames[index] == name) return kAllSemanticRoles[index];
    }
    return std::nullopt;
}

std::optional<SyntaxScope> syntaxScopeFromName(std::string_view name) {
    for (std::size_t index = 0; index < kSyntaxNames.size(); ++index) {
        if (kSyntaxNames[index] == name) return kAllSyntaxScopes[index];
    }
    return std::nullopt;
}

} // namespace ssg
