#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ssg {

inline constexpr std::size_t kThemePaletteSize = 16;

struct SrgbColor {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    // Reconstructs channels already owned by an authoritative Theme snapshot.
    // Theme-internal DiffTints derivation is also authoritative theme color;
    // renderers and clients must not derive or substitute channels.
    [[nodiscard]] static constexpr SrgbColor fromSerializedChannels(
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

inline constexpr std::size_t kSemanticRoleCount = 32;
inline constexpr std::array kAllSemanticRoles{
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
static_assert(kAllSemanticRoles.size() == kSemanticRoleCount);

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

inline constexpr std::size_t kSyntaxScopeCount = 11;
inline constexpr std::array kAllSyntaxScopes{
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
static_assert(kAllSyntaxScopes.size() == kSyntaxScopeCount);

struct RolePair {
    SemanticRole first;
    SemanticRole second;

    friend bool operator==(const RolePair&, const RolePair&) = default;
};

inline constexpr std::array kCoVisibleRolePairs{
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
    std::uint8_t paletteIndex = 0;

    friend bool operator==(const RoleMapping&, const RoleMapping&) = default;
};

struct SyntaxMapping {
    SyntaxScope scope = SyntaxScope::PlainText;
    std::uint8_t paletteIndex = 0;

    friend bool operator==(const SyntaxMapping&, const SyntaxMapping&) = default;
};

struct DiffTints {
    SrgbColor addedRow;
    SrgbColor removedRow;
    SrgbColor modifiedRow;
    SrgbColor addedWord;
    SrgbColor removedWord;
    SrgbColor modifiedWord;

    friend bool operator==(const DiffTints&, const DiffTints&) = default;
};

struct ThemeSnapshot {
    std::array<SrgbColor, kThemePaletteSize> palette;
    std::array<std::uint8_t, kSemanticRoleCount> semanticIndices;
    std::array<std::uint8_t, kSyntaxScopeCount> syntaxIndices;
    DiffTints diffTints;
    SrgbColor selectionFill;

    friend bool operator==(const ThemeSnapshot&, const ThemeSnapshot&) = default;
};

[[nodiscard]] std::string_view semanticRoleName(SemanticRole role);
[[nodiscard]] std::optional<SemanticRole> semanticRoleFromName(std::string_view name);
[[nodiscard]] std::string_view syntaxScopeName(SyntaxScope scope);
[[nodiscard]] std::optional<SyntaxScope> syntaxScopeFromName(std::string_view name);

// Derive the six diff tint colors from a theme's own palette + role/scope
// mappings (GitAdded/Deleted/Modified hue anchors blended toward Background,
// contrast- and distinctness-gated). Exposed so a hand-built ThemeSnapshot (e.g.
// the runtime's defaultTheme) populates diffTints/selectionFill the same way
// Theme::snapshot() does; without it those fields default to black.
[[nodiscard]] DiffTints deriveDiffTints(
    std::array<SrgbColor, kThemePaletteSize> const& palette,
    std::array<std::uint8_t, kSemanticRoleCount> const& semanticIndices,
    std::array<std::uint8_t, kSyntaxScopeCount> const& syntaxIndices) noexcept;
[[nodiscard]] SrgbColor deriveSelectionFill(
    std::array<SrgbColor, kThemePaletteSize> const& palette,
    std::array<std::uint8_t, kSemanticRoleCount> const& semanticIndices,
    std::array<std::uint8_t, kSyntaxScopeCount> const& syntaxIndices) noexcept;

class Theme {
public:
    Theme(std::string name,
          std::span<const IndexedColor> palette,
          std::span<const RoleMapping> semanticMappings,
          std::span<const SyntaxMapping> syntaxMappings);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::array<SrgbColor, kThemePaletteSize>& palette() const noexcept {
        return palette_;
    }
    [[nodiscard]] std::uint8_t indexFor(SemanticRole role) const;
    [[nodiscard]] std::uint8_t indexFor(SyntaxScope scope) const;
    [[nodiscard]] std::uint8_t indexForSyntax(std::string_view scope) const noexcept;
    [[nodiscard]] ThemeSnapshot snapshot() const noexcept;

private:
    std::string name_;
    std::array<SrgbColor, kThemePaletteSize> palette_{};
    std::array<std::uint8_t, kSemanticRoleCount> semanticIndices_{};
    std::array<std::uint8_t, kSyntaxScopeCount> syntaxIndices_{};
};

} // namespace ssg

// Testing-only observability seam: reports WHICH internal path
// deriveDiffTints() took (Primary derivation vs. the degenerate-theme
// Rescue fallback). Not part of the render contract — production code
// only ever needs deriveDiffTints()'s final colors; this exists so tests
// can assert a non-degenerate theme never silently depends on rescue.
namespace ssg::testing {

enum class DiffTintDerivationPath : std::uint8_t { Primary, Rescue };

struct DiffTintDerivationResult {
    DiffTints tints;
    DiffTintDerivationPath path = DiffTintDerivationPath::Rescue;

    friend bool operator==(const DiffTintDerivationResult&,
                           const DiffTintDerivationResult&) = default;
};

[[nodiscard]] DiffTintDerivationResult deriveDiffTintsWithPath(
    std::array<SrgbColor, kThemePaletteSize> const& palette,
    std::array<std::uint8_t, kSemanticRoleCount> const& semanticIndices,
    std::array<std::uint8_t, kSyntaxScopeCount> const& syntaxIndices) noexcept;

} // namespace ssg::testing
