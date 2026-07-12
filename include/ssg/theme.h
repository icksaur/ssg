#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ssg {

inline constexpr std::size_t theme_palette_size = 16;

struct SrgbColor {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    friend bool operator==(const SrgbColor&, const SrgbColor&) = default;
};

struct IndexedColor {
    std::uint8_t index = 0;
    SrgbColor color;

    friend bool operator==(const IndexedColor&, const IndexedColor&) = default;
};

enum class SemanticRole : std::uint8_t {
    foreground,
    background,
    caret,
    selection,
    diagnostic_error,
    diagnostic_warning,
    diagnostic_info,
    diagnostic_hint,
    git_added,
    git_modified,
    git_deleted,
    git_conflict,
    tree_background,
    tree_focus,
    tab_active,
    tab_inactive,
    panel_active,
    panel_inactive,
    header,
    footer,
    status_info,
    status_warning,
    status_error,
    line_number,
    active_line_number,
    search_match,
    prompt,
    scrollbar_track,
    scrollbar_thumb,
    diff_added,
    diff_removed,
    diff_modified,
};

inline constexpr std::size_t semantic_role_count = 32;
inline constexpr std::array all_semantic_roles{
    SemanticRole::foreground,
    SemanticRole::background,
    SemanticRole::caret,
    SemanticRole::selection,
    SemanticRole::diagnostic_error,
    SemanticRole::diagnostic_warning,
    SemanticRole::diagnostic_info,
    SemanticRole::diagnostic_hint,
    SemanticRole::git_added,
    SemanticRole::git_modified,
    SemanticRole::git_deleted,
    SemanticRole::git_conflict,
    SemanticRole::tree_background,
    SemanticRole::tree_focus,
    SemanticRole::tab_active,
    SemanticRole::tab_inactive,
    SemanticRole::panel_active,
    SemanticRole::panel_inactive,
    SemanticRole::header,
    SemanticRole::footer,
    SemanticRole::status_info,
    SemanticRole::status_warning,
    SemanticRole::status_error,
    SemanticRole::line_number,
    SemanticRole::active_line_number,
    SemanticRole::search_match,
    SemanticRole::prompt,
    SemanticRole::scrollbar_track,
    SemanticRole::scrollbar_thumb,
    SemanticRole::diff_added,
    SemanticRole::diff_removed,
    SemanticRole::diff_modified,
};
static_assert(all_semantic_roles.size() == semantic_role_count);

enum class SyntaxScope : std::uint8_t {
    plain_text,
    comment,
    keyword,
    string,
    number,
    type,
    function,
    variable,
    operator_token,
    punctuation,
    invalid,
};

inline constexpr std::size_t syntax_scope_count = 11;
inline constexpr std::array all_syntax_scopes{
    SyntaxScope::plain_text,
    SyntaxScope::comment,
    SyntaxScope::keyword,
    SyntaxScope::string,
    SyntaxScope::number,
    SyntaxScope::type,
    SyntaxScope::function,
    SyntaxScope::variable,
    SyntaxScope::operator_token,
    SyntaxScope::punctuation,
    SyntaxScope::invalid,
};
static_assert(all_syntax_scopes.size() == syntax_scope_count);

struct RolePair {
    SemanticRole first;
    SemanticRole second;

    friend bool operator==(const RolePair&, const RolePair&) = default;
};

inline constexpr std::array co_visible_role_pairs{
    RolePair{SemanticRole::caret, SemanticRole::selection},
    RolePair{SemanticRole::diagnostic_error, SemanticRole::diagnostic_warning},
    RolePair{SemanticRole::diagnostic_error, SemanticRole::diagnostic_info},
    RolePair{SemanticRole::diagnostic_error, SemanticRole::diagnostic_hint},
    RolePair{SemanticRole::diagnostic_warning, SemanticRole::diagnostic_info},
    RolePair{SemanticRole::diagnostic_warning, SemanticRole::diagnostic_hint},
    RolePair{SemanticRole::diagnostic_info, SemanticRole::diagnostic_hint},
    RolePair{SemanticRole::git_added, SemanticRole::git_modified},
    RolePair{SemanticRole::git_added, SemanticRole::git_deleted},
    RolePair{SemanticRole::git_added, SemanticRole::git_conflict},
    RolePair{SemanticRole::git_modified, SemanticRole::git_deleted},
    RolePair{SemanticRole::git_modified, SemanticRole::git_conflict},
    RolePair{SemanticRole::git_deleted, SemanticRole::git_conflict},
    RolePair{SemanticRole::tree_background, SemanticRole::tree_focus},
    RolePair{SemanticRole::tab_active, SemanticRole::tab_inactive},
    RolePair{SemanticRole::tab_active, SemanticRole::panel_active},
    RolePair{SemanticRole::tab_active, SemanticRole::panel_inactive},
    RolePair{SemanticRole::tab_inactive, SemanticRole::panel_active},
    RolePair{SemanticRole::tab_inactive, SemanticRole::panel_inactive},
    RolePair{SemanticRole::panel_active, SemanticRole::panel_inactive},
};

struct RoleMapping {
    SemanticRole role = SemanticRole::foreground;
    std::uint8_t palette_index = 0;

    friend bool operator==(const RoleMapping&, const RoleMapping&) = default;
};

struct SyntaxMapping {
    SyntaxScope scope = SyntaxScope::plain_text;
    std::uint8_t palette_index = 0;

    friend bool operator==(const SyntaxMapping&, const SyntaxMapping&) = default;
};

struct ThemeSnapshot {
    std::array<SrgbColor, theme_palette_size> palette;
    std::array<std::uint8_t, semantic_role_count> semantic_indices;
    std::array<std::uint8_t, syntax_scope_count> syntax_indices;

    friend bool operator==(const ThemeSnapshot&, const ThemeSnapshot&) = default;
};

[[nodiscard]] std::string_view semantic_role_name(SemanticRole role);
[[nodiscard]] std::optional<SemanticRole> semantic_role_from_name(std::string_view name);
[[nodiscard]] std::string_view syntax_scope_name(SyntaxScope scope);
[[nodiscard]] std::optional<SyntaxScope> syntax_scope_from_name(std::string_view name);

class Theme {
public:
    Theme(std::string name,
          std::span<const IndexedColor> palette,
          std::span<const RoleMapping> semantic_mappings,
          std::span<const SyntaxMapping> syntax_mappings);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::array<SrgbColor, theme_palette_size>& palette() const noexcept {
        return palette_;
    }
    [[nodiscard]] std::uint8_t index_for(SemanticRole role) const;
    [[nodiscard]] std::uint8_t index_for(SyntaxScope scope) const;
    [[nodiscard]] std::uint8_t index_for_syntax(std::string_view scope) const noexcept;
    [[nodiscard]] ThemeSnapshot snapshot() const noexcept;

private:
    std::string name_;
    std::array<SrgbColor, theme_palette_size> palette_{};
    std::array<std::uint8_t, semantic_role_count> semantic_indices_{};
    std::array<std::uint8_t, syntax_scope_count> syntax_indices_{};
};

} // namespace ssg
