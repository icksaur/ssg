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

// Multipliers applied to a background wash before it is painted.  1.0 means
// "unchanged"; above 1.0 strengthens, below weakens.  Not colors -- tuning
// parameters, so holding them does not make a type a source of color (I22).
struct TintAdjustment {
    float brightness = 1.0f;
    float saturation = 1.0f;

    // Whether this is the identity, and therefore skippable.
    [[nodiscard]] constexpr bool neutral() const noexcept {
        return brightness == 1.0f && saturation == 1.0f;
    }

    friend bool operator==(TintAdjustment const&, TintAdjustment const&) = default;
};

// Which background wash an adjustment applies to.  Named for MEANING, not
// color: a theme may make "added" cyan, and DiffAdded still identifies it.
enum class BackgroundTintTarget : std::uint8_t {
    DiffAdded,
    DiffRemoved,
    DiffModified,
    Selection,
};

inline constexpr std::size_t kBackgroundTintTargetCount = 4;
inline constexpr std::array kAllBackgroundTintTargets{
    BackgroundTintTarget::DiffAdded,
    BackgroundTintTarget::DiffRemoved,
    BackgroundTintTarget::DiffModified,
    BackgroundTintTarget::Selection,
};
static_assert(kAllBackgroundTintTargets.size() == kBackgroundTintTargetCount);

// Per-target brightness/saturation multipliers, applied when the washes are
// derived.  Default-constructed is the identity, which is what ships.
struct BackgroundTintAdjustments {
    std::array<TintAdjustment, kBackgroundTintTargetCount> byTarget{};

    [[nodiscard]] TintAdjustment const& operator[](
        BackgroundTintTarget target) const noexcept {
        return byTarget[static_cast<std::size_t>(target)];
    }
    [[nodiscard]] TintAdjustment& operator[](
        BackgroundTintTarget target) noexcept {
        return byTarget[static_cast<std::size_t>(target)];
    }

    friend bool operator==(BackgroundTintAdjustments const&,
                           BackgroundTintAdjustments const&) = default;
};

[[nodiscard]] std::string_view backgroundTintTargetName(BackgroundTintTarget target);
[[nodiscard]] std::optional<BackgroundTintTarget> backgroundTintTargetFromName(
    std::string_view name);

struct ThemeSnapshot {
    std::array<SrgbColor, kThemePaletteSize> palette;
    std::array<std::uint8_t, kSemanticRoleCount> semanticIndices;
    std::array<std::uint8_t, kSyntaxScopeCount> syntaxIndices;
    DiffTints diffTints;
    SrgbColor selectionFill;
    // The multipliers the tints above were derived WITH.  Carried on the
    // snapshot because they are theme state: a later theme.define re-derives
    // over the current palette and must re-apply them.
    BackgroundTintAdjustments backgroundTints;

    friend bool operator==(const ThemeSnapshot&, const ThemeSnapshot&) = default;
};

// theme.define's argument: a table of the 16 classic ANSI palette-slot
// names (see kAnsiSlotNames in Theme.cpp), each optionally mapped to a
// "#rrggbb" hex string. A name absent from `colors` keeps the CURRENT
// active theme's color for that slot -- the table may be partial. This is
// the ONLY argument shape theme.define accepts (see doc/spec-config.md);
// role/syntax mappings are never touched by this command.
struct ThemeDefineArguments {
    std::unordered_map<std::string, std::string> colors;

    friend bool operator==(const ThemeDefineArguments&, const ThemeDefineArguments&) = default;
};

// theme.background's argument: the FLAT string->string table the Lua seam
// permits (LuaCommandHost rejects nested tables and non-string values before
// dispatch).  Keys are "brightness"/"saturation" for every target, and
// "<target>_brightness"/"<target>_saturation" to override one; values are
// numeric strings. Any key may be omitted.
struct ThemeBackgroundArguments {
    std::unordered_map<std::string, std::string> values;

    friend bool operator==(const ThemeBackgroundArguments&,
                           const ThemeBackgroundArguments&) = default;
};

struct ThemeDefineError {
    std::string message;

    friend bool operator==(const ThemeDefineError&, const ThemeDefineError&) = default;
};

struct ThemeDefineResult {
    std::optional<ThemeDefineError> error;
    // The replacement snapshot when accepted; left default-constructed
    // (unused) when rejected -- all-or-nothing, no partial apply on error.
    ThemeSnapshot snapshot;

    [[nodiscard]] bool accepted() const noexcept { return !error.has_value(); }
};

// Applies theme.define's color table to `current`: replaces ONLY the named
// palette slots (an omitted name keeps its current value) and recomputes
// DiffTints/selectionFill (pure functions of palette + role/syntax indices)
// over the new palette. Role/syntax mappings are copied from `current`
// unchanged. Rejects (leaving `current` conceptually untouched -- the
// caller simply does not apply `.snapshot`) on an unknown slot name or a
// malformed "#rrggbb" string; the whole table is validated before any
// slot is replaced, so a rejected call never partially mutates the result.
[[nodiscard]] ThemeDefineResult applyThemeDefine(
    ThemeSnapshot const& current,
    ThemeDefineArguments const& arguments) noexcept;

// Applies theme.background's table to `current`, re-deriving the washes.
// Validated whole before anything is replaced: an unknown key, an unparseable
// number, or a negative or non-finite multiplier rejects the entire call, so a
// rejected call never leaves a partially-adjusted theme.
[[nodiscard]] ThemeDefineResult applyThemeBackground(
    ThemeSnapshot const& current,
    ThemeBackgroundArguments const& arguments) noexcept;

// The compiled-in built-in theme: EditorRuntime::create()'s starting
// ThemeSnapshot, before any init.lua theme.define() call runs. This is
// the ONE source of the default theme's colors -- there is no data-file
// or other loadable-config path (init.lua's theme.define overrides it
// entirely in memory, per doc/spec-config.md); a second, independently
// hand-maintained copy of these values would silently drift with nothing
// to catch it, so tests requiring a realistic full-16-color theme call
// this function directly rather than parsing a duplicate. Defined in
// src/DefaultTheme.cpp, not Theme.cpp -- see that file's header comment.
[[nodiscard]] ThemeSnapshot defaultTheme() noexcept;

[[nodiscard]] std::string_view semanticRoleName(SemanticRole role);
[[nodiscard]] std::optional<SemanticRole> semanticRoleFromName(std::string_view name);
[[nodiscard]] std::string_view syntaxScopeName(SyntaxScope scope);
[[nodiscard]] std::optional<SyntaxScope> syntaxScopeFromName(std::string_view name);

// Each of a diff's three kinds (added/removed/modified) uses ONE flat color
// straight from the theme's own Git anchor role -- no blending toward
// Background, no desaturation, no per-theme readability search. Row and
// word share that same color; a word mark exists only to say "here
// specifically" within an already-tinted row, not to be a different shade.
[[nodiscard]] DiffTints deriveDiffTints(
    std::array<SrgbColor, kThemePaletteSize> const& palette,
    std::array<std::uint8_t, kSemanticRoleCount> const& semanticIndices,
    std::array<std::uint8_t, kSyntaxScopeCount> const& syntaxIndices,
    BackgroundTintAdjustments const& adjustments = {}) noexcept;
[[nodiscard]] SrgbColor deriveSelectionFill(
    std::array<SrgbColor, kThemePaletteSize> const& palette,
    std::array<std::uint8_t, kSemanticRoleCount> const& semanticIndices,
    std::array<std::uint8_t, kSyntaxScopeCount> const& syntaxIndices,
    BackgroundTintAdjustments const& adjustments = {}) noexcept;

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
