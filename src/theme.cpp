#include "ssg/theme.h"

#include <array>
#include <stdexcept>

namespace ssg {
namespace {

constexpr std::array semantic_names{
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
static_assert(semantic_names.size() == semantic_role_count);

constexpr std::array syntax_names{
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
static_assert(syntax_names.size() == syntax_scope_count);

constexpr std::size_t position(SemanticRole role) noexcept {
    return static_cast<std::size_t>(role);
}

constexpr std::size_t position(SyntaxScope scope) noexcept {
    return static_cast<std::size_t>(scope);
}

constexpr bool valid(SemanticRole role) noexcept {
    return position(role) < semantic_role_count;
}

constexpr bool valid(SyntaxScope scope) noexcept {
    return position(scope) < syntax_scope_count;
}

void validate_palette_index(std::uint8_t index) {
    if (index >= theme_palette_size) {
        throw std::invalid_argument("theme mapping palette index must be in [0, 15]");
    }
}

} // namespace

std::string_view semantic_role_name(SemanticRole role) {
    if (!valid(role)) throw std::invalid_argument("semantic role is not recognized");
    return semantic_names[position(role)];
}

std::optional<SemanticRole> semantic_role_from_name(std::string_view name) {
    for (std::size_t index = 0; index < semantic_names.size(); ++index) {
        if (semantic_names[index] == name) return all_semantic_roles[index];
    }
    return std::nullopt;
}

std::string_view syntax_scope_name(SyntaxScope scope) {
    if (!valid(scope)) throw std::invalid_argument("syntax scope is not recognized");
    return syntax_names[position(scope)];
}

std::optional<SyntaxScope> syntax_scope_from_name(std::string_view name) {
    for (std::size_t index = 0; index < syntax_names.size(); ++index) {
        if (syntax_names[index] == name) return all_syntax_scopes[index];
    }
    return std::nullopt;
}

Theme::Theme(std::string name,
             std::span<const IndexedColor> palette,
             std::span<const RoleMapping> semantic_mappings,
             std::span<const SyntaxMapping> syntax_mappings)
    : name_(std::move(name)) {
    if (name_.empty()) throw std::invalid_argument("theme name must not be empty");
    if (palette.size() != theme_palette_size) {
        throw std::invalid_argument("theme palette must contain exactly 16 indexed colors");
    }
    std::array<bool, theme_palette_size> seen_palette{};
    for (const auto& entry : palette) {
        if (entry.index >= theme_palette_size) {
            throw std::invalid_argument("theme palette index must be in [0, 15]");
        }
        if (seen_palette[entry.index]) {
            throw std::invalid_argument("theme palette contains a duplicate index");
        }
        seen_palette[entry.index] = true;
        palette_[entry.index] = entry.color;
    }

    if (semantic_mappings.size() != semantic_role_count) {
        throw std::invalid_argument("theme must map every semantic role exactly once");
    }
    std::array<bool, semantic_role_count> seen_roles{};
    for (const auto& mapping : semantic_mappings) {
        if (!valid(mapping.role)) {
            throw std::invalid_argument("theme contains an unknown semantic role");
        }
        validate_palette_index(mapping.palette_index);
        const auto index = position(mapping.role);
        if (seen_roles[index]) {
            throw std::invalid_argument("theme contains a duplicate semantic role");
        }
        seen_roles[index] = true;
        semantic_indices_[index] = mapping.palette_index;
    }

    if (syntax_mappings.size() != syntax_scope_count) {
        throw std::invalid_argument("theme must map every syntax scope exactly once");
    }
    std::array<bool, syntax_scope_count> seen_scopes{};
    for (const auto& mapping : syntax_mappings) {
        if (!valid(mapping.scope)) {
            throw std::invalid_argument("theme contains an unknown syntax scope");
        }
        validate_palette_index(mapping.palette_index);
        const auto index = position(mapping.scope);
        if (seen_scopes[index]) {
            throw std::invalid_argument("theme contains a duplicate syntax scope");
        }
        seen_scopes[index] = true;
        syntax_indices_[index] = mapping.palette_index;
    }

    for (const auto& pair : co_visible_role_pairs) {
        if (semantic_indices_[position(pair.first)] ==
            semantic_indices_[position(pair.second)]) {
            throw std::invalid_argument(
                "co-visible semantic roles must use distinct palette indices");
        }
    }
}

std::uint8_t Theme::index_for(SemanticRole role) const {
    if (!valid(role)) throw std::invalid_argument("semantic role is not recognized");
    return semantic_indices_[position(role)];
}

std::uint8_t Theme::index_for(SyntaxScope scope) const {
    if (!valid(scope)) throw std::invalid_argument("syntax scope is not recognized");
    return syntax_indices_[position(scope)];
}

std::uint8_t Theme::index_for_syntax(std::string_view scope) const noexcept {
    const auto recognized = syntax_scope_from_name(scope);
    return syntax_indices_[position(recognized.value_or(SyntaxScope::plain_text))];
}

ThemeSnapshot Theme::snapshot() const noexcept {
    return {palette_, semantic_indices_, syntax_indices_};
}

} // namespace ssg
