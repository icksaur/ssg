#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ssg {

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
    Text,
    Canvas,
    Caret,
    Selection,
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
    LineNumber,
    SearchMatch,
    Prompt,
    ScrollbarTrack,
    ScrollbarThumb,
    DiffAdded,
    DiffRemoved,
    DiffModified,
    // Chrome BACKGROUND roles (the roles above are foreground/text colors). The
    // active tab deliberately has no background role: it reuses Canvas so it
    // always matches the document by construction and cannot drift.
    TabInactiveBackground,
    HeaderBackground,
    FooterBackground,
};

inline constexpr std::size_t kSemanticRoleCount = 25;
inline constexpr std::array kAllSemanticRoles{
    SemanticRole::Text,
    SemanticRole::Canvas,
    SemanticRole::Caret,
    SemanticRole::Selection,
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
    SemanticRole::LineNumber,
    SemanticRole::SearchMatch,
    SemanticRole::Prompt,
    SemanticRole::ScrollbarTrack,
    SemanticRole::ScrollbarThumb,
    SemanticRole::DiffAdded,
    SemanticRole::DiffRemoved,
    SemanticRole::DiffModified,
    SemanticRole::TabInactiveBackground,
    SemanticRole::HeaderBackground,
    SemanticRole::FooterBackground,
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

// The flat render color table = every role color followed by every scope color.
// A cell stores a uint8 index into this table; the theme gives each role and
// scope its own slot, so nothing is shared and every color is set directly.
inline constexpr std::size_t kThemeColorSlotCount =
    kSemanticRoleCount + kSyntaxScopeCount;
// A rendered cell addresses this table with a uint8 foreground/background index
// (see CellGridCell in Renderer.h). Adding enough roles or scopes to exceed the
// uint8 range would make those indices wrap and silently render wrong colors, so
// pin the width contract here where the count is defined rather than trusting
// every call site to notice.
static_assert(kThemeColorSlotCount <= 256,
              "cell color indices are uint8; the color table cannot exceed 256 "
              "slots without widening CellGridCell's index type");

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
    // One color per semantic role and one per syntax scope, set directly. No
    // palette, no indirection: a role IS its color. The render color table
    // (`CellGrid.colors`) is these role colors followed by these scope colors.
    std::array<SrgbColor, kSemanticRoleCount> roleColors;
    std::array<SrgbColor, kSyntaxScopeCount> syntaxColors;

    friend bool operator==(const ThemeSnapshot&, const ThemeSnapshot&) = default;
};

[[nodiscard]] SrgbColor themeColor(ThemeSnapshot const& theme, SemanticRole role);
[[nodiscard]] SrgbColor themeColor(ThemeSnapshot const& theme, SyntaxScope scope);

// theme.set's argument: a table of semantic-role and syntax-scope snake_case
// names (the `semanticRoleName`/`syntaxScopeName` strings, e.g.
// "header_background", "tab_active", "comment"), each mapped to a "#rrggbb" hex
// string. An omitted name keeps its current color -- the table may be partial.
// Role and scope names share one namespace here (they are disjoint sets); an
// unknown name rejects the whole call.
struct ThemeSetArguments {
    std::unordered_map<std::string, std::string> colors;

    friend bool operator==(const ThemeSetArguments&, const ThemeSetArguments&) = default;
};

struct ThemeSetError {
    std::string message;

    friend bool operator==(const ThemeSetError&, const ThemeSetError&) = default;
};

struct ThemeSetResult {
    std::optional<ThemeSetError> error;
    // The replacement snapshot when accepted; left default-constructed (unused)
    // when rejected -- all-or-nothing, no partial apply on error.
    ThemeSnapshot snapshot;

    [[nodiscard]] bool accepted() const noexcept { return !error.has_value(); }
};

// Applies theme.set's table to `current`: replaces ONLY the named role/scope
// colors (an omitted name keeps its current color) with no derivation -- the
// set color is the final color. Validated whole before anything is replaced: an
// unknown name or a malformed "#rrggbb" string rejects the entire call, so a
// rejected call never partially mutates the result.
[[nodiscard]] ThemeSetResult applyThemeSet(
    ThemeSnapshot const& current,
    ThemeSetArguments const& arguments) noexcept;

// The compiled-in built-in theme: EditorRuntime::create()'s starting
// ThemeSnapshot, before any init.lua theme.set() call runs. This is the ONE
// source of the default theme's colors -- there is no data-file or other
// loadable-config path; a second, independently hand-maintained copy would
// silently drift with nothing to catch it. Defined in src/DefaultTheme.cpp, not
// Theme.cpp -- see that file's header comment.
[[nodiscard]] ThemeSnapshot defaultTheme() noexcept;

[[nodiscard]] std::string_view semanticRoleName(SemanticRole role);
[[nodiscard]] std::optional<SemanticRole> semanticRoleFromName(std::string_view name);
[[nodiscard]] std::string_view syntaxScopeName(SyntaxScope scope);
[[nodiscard]] std::optional<SyntaxScope> syntaxScopeFromName(std::string_view name);

} // namespace ssg
