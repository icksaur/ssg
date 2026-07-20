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

    // Reconstructs channels already owned by an authoritative Theme snapshot.
    // This is not a second color-definition path: callers must not derive or
    // substitute channels outside theme data.
    [[nodiscard]] static constexpr SrgbColor from_serialized_channels(
        std::uint8_t red, std::uint8_t green, std::uint8_t blue) noexcept {
        return SrgbColor{red, green, blue};
    }

    friend bool operator==(const SrgbColor&, const SrgbColor&) = default;
};

struct IndexedColor {
    std::uint8_t index = 0;
    SrgbColor color;

    friend bool operator==(const IndexedColor&, const IndexedColor&) = default;
};

enum class SemanticRole : std::uint8_t {
    Foreground,
    Background,
    Caret,
    Selection,
    DiagnosticError,
    DiagnosticWarning,
    DiagnosticInfo,
    DiagnosticHint,
    GitAdded,
    GitModified,
    GitDeleted,
    GitConflict,
    TreeBackground,
    TreeFocus,
    TabActive,
    TabInactive,
    PanelActive,
    PanelInactive,
    Header,
    Footer,
    StatusInfo,
    StatusWarning,
    StatusError,
    LineNumber,
    ActiveLineNumber,
    SearchMatch,
    Prompt,
    ScrollbarTrack,
    ScrollbarThumb,
    DiffAdded,
    DiffRemoved,
    DiffModified,
};

inline constexpr std::size_t semantic_role_count = 32;
inline constexpr std::array all_semantic_roles{
    SemanticRole::Foreground,
    SemanticRole::Background,
    SemanticRole::Caret,
    SemanticRole::Selection,
    SemanticRole::DiagnosticError,
    SemanticRole::DiagnosticWarning,
    SemanticRole::DiagnosticInfo,
    SemanticRole::DiagnosticHint,
    SemanticRole::GitAdded,
    SemanticRole::GitModified,
    SemanticRole::GitDeleted,
    SemanticRole::GitConflict,
    SemanticRole::TreeBackground,
    SemanticRole::TreeFocus,
    SemanticRole::TabActive,
    SemanticRole::TabInactive,
    SemanticRole::PanelActive,
    SemanticRole::PanelInactive,
    SemanticRole::Header,
    SemanticRole::Footer,
    SemanticRole::StatusInfo,
    SemanticRole::StatusWarning,
    SemanticRole::StatusError,
    SemanticRole::LineNumber,
    SemanticRole::ActiveLineNumber,
    SemanticRole::SearchMatch,
    SemanticRole::Prompt,
    SemanticRole::ScrollbarTrack,
    SemanticRole::ScrollbarThumb,
    SemanticRole::DiffAdded,
    SemanticRole::DiffRemoved,
    SemanticRole::DiffModified,
};
static_assert(all_semantic_roles.size() == semantic_role_count);

enum class SyntaxScope : std::uint8_t {
    PlainText,
    Comment,
    Keyword,
    String,
    Number,
    Type,
    Function,
    Variable,
    OperatorToken,
    Punctuation,
    Invalid,
};

inline constexpr std::size_t syntax_scope_count = 11;
inline constexpr std::array all_syntax_scopes{
    SyntaxScope::PlainText,
    SyntaxScope::Comment,
    SyntaxScope::Keyword,
    SyntaxScope::String,
    SyntaxScope::Number,
    SyntaxScope::Type,
    SyntaxScope::Function,
    SyntaxScope::Variable,
    SyntaxScope::OperatorToken,
    SyntaxScope::Punctuation,
    SyntaxScope::Invalid,
};
static_assert(all_syntax_scopes.size() == syntax_scope_count);

struct RolePair {
    SemanticRole first;
    SemanticRole second;

    friend bool operator==(const RolePair&, const RolePair&) = default;
};

inline constexpr std::array co_visible_role_pairs{
    RolePair{SemanticRole::Caret, SemanticRole::Selection},
    RolePair{SemanticRole::DiagnosticError, SemanticRole::DiagnosticWarning},
    RolePair{SemanticRole::DiagnosticError, SemanticRole::DiagnosticInfo},
    RolePair{SemanticRole::DiagnosticError, SemanticRole::DiagnosticHint},
    RolePair{SemanticRole::DiagnosticWarning, SemanticRole::DiagnosticInfo},
    RolePair{SemanticRole::DiagnosticWarning, SemanticRole::DiagnosticHint},
    RolePair{SemanticRole::DiagnosticInfo, SemanticRole::DiagnosticHint},
    RolePair{SemanticRole::GitAdded, SemanticRole::GitModified},
    RolePair{SemanticRole::GitAdded, SemanticRole::GitDeleted},
    RolePair{SemanticRole::GitAdded, SemanticRole::GitConflict},
    RolePair{SemanticRole::GitModified, SemanticRole::GitDeleted},
    RolePair{SemanticRole::GitModified, SemanticRole::GitConflict},
    RolePair{SemanticRole::GitDeleted, SemanticRole::GitConflict},
    RolePair{SemanticRole::TreeBackground, SemanticRole::TreeFocus},
    RolePair{SemanticRole::TabActive, SemanticRole::TabInactive},
    RolePair{SemanticRole::TabActive, SemanticRole::PanelActive},
    RolePair{SemanticRole::TabActive, SemanticRole::PanelInactive},
    RolePair{SemanticRole::TabInactive, SemanticRole::PanelActive},
    RolePair{SemanticRole::TabInactive, SemanticRole::PanelInactive},
    RolePair{SemanticRole::PanelActive, SemanticRole::PanelInactive},
};

struct RoleMapping {
    SemanticRole role = SemanticRole::Foreground;
    std::uint8_t palette_index = 0;

    friend bool operator==(const RoleMapping&, const RoleMapping&) = default;
};

struct SyntaxMapping {
    SyntaxScope scope = SyntaxScope::PlainText;
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
