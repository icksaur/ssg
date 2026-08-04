#pragma once

#include <cstdint>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

namespace ssg {

// Style is the UI's dimensions and chrome glyphs, separated from color (which
// is the theme's job, see doc/spec-color.md) and from layout arithmetic (which
// is ShellState's).  See doc/spec-style.md.
//
// Everything here is DATA plus the small amount of logic needed to resolve that
// data into a glyph.  Style deliberately knows nothing about a grid, a snapshot
// or a renderer, so it can be constructed and interrogated directly.

// The glyphs a scrollbar is drawn from.
//
// `single`, `top`, `body` and `bottom` let a thumb have end caps.  A style that
// wants a uniform thumb sets all four to the same glyph -- that is the default,
// and is what makes the caps a capability rather than a cost.
struct ScrollbarGlyphs {
    // Painted for the whole column when there is nothing to scroll.  Its own
    // entry rather than a hardcoded blank, so an empty gutter can be styled.
    std::string gutter = " ";
    std::string track = "|";
    // Thumb of exactly one row: no room for a top and a bottom, so neither is
    // used.
    std::string single = "#";
    std::string top = "#";
    std::string body = "#";
    std::string bottom = "#";
    friend bool operator==(ScrollbarGlyphs const&, ScrollbarGlyphs const&) = default;
};

struct TreeGlyphs {
    std::string expanded = "\xe2\x96\xbe ";    // U+25BE
    std::string collapsed = "\xe2\x96\xb8 ";   // U+25B8
    int indentPerDepth = 2;
    friend bool operator==(TreeGlyphs const&, TreeGlyphs const&) = default;
};

struct TabGlyphs {
    std::string dirtySuffix = " *";
    std::string liveDiffPrefix = "D ";
    std::string readOnlySuffix = " (readonly)";
    // Layout-affecting (variable-width) glyphs: the tab row recomputes its rects
    // from their actual width, so unlike the fixed-slot glyphs above these may be
    // reconfigured to any width.  A tab draws as leftEdge + title + rightEdge and
    // tabs are parted by `separator`.  Defaults reproduce a tight row with one
    // space between tabs.
    std::string leftEdge = "";
    std::string rightEdge = "";
    std::string separator = " ";
    friend bool operator==(TabGlyphs const&, TabGlyphs const&) = default;
};

// Checkbox markers for prompt toggle controls (find's case/word/regex).
struct ToggleGlyphs {
    std::string checked = "[x] ";
    std::string unchecked = "[ ] ";
    friend bool operator==(ToggleGlyphs const&, ToggleGlyphs const&) = default;
};

// Sizes in terminal cells.  There is no unit system and no scaling factor: the
// unit is a cell.
struct StyleDimensions {
    int minimumColumns = 20;
    int minimumRows = 4;
    int panelTargetWidth = 24;
    int panelMinimumWidth = 12;
    int editorMinimumWidth = 20;
    int scrollbarGutterWidth = 1;
    int headerHeight = 1;
    int tabBarHeight = 1;
    int footerHeight = 1;
    // Horizontal padding around a tab's and a footer action's label.  One value
    // shared by both, because they were the same literal written twice.
    int labelPadding = 2;
    int inputLineSeparator = 1;
    int inputLineQueryBudget = 8;
    friend bool operator==(StyleDimensions const&, StyleDimensions const&) = default;
};

enum class ScrollbarCellKind : std::uint8_t { Gutter, Track, Thumb };

// A resolved scrollbar cell: what to draw, and what it IS.  The kind is
// returned alongside the glyph because the caller colors track and thumb
// differently, and re-deriving "is this a thumb row?" at the call site would
// put the size rule in two places.
struct ScrollbarCell {
    std::string_view glyph;
    ScrollbarCellKind kind = ScrollbarCellKind::Gutter;
};

class Style {
public:
    Style() = default;

    ScrollbarGlyphs scrollbar{};
    TreeGlyphs tree{};
    TabGlyphs tab{};
    ToggleGlyphs toggle{};
    // Marks a label that did not fit.  U+2026.
    std::string truncation = "\xe2\x80\xa6";
    // The input line's prefix.  Its WIDTH is derived from this string (see
    // sigilWidth); the two must never be configured independently.
    std::string inputLineSigil = "> ";
    // Substitute drawn for a byte that has no glyph -- a control character or
    // invalid UTF-8.  U+FFFD.  A substitution glyph like `truncation`, and the
    // reason both live here: "what do we show when we cannot show the real
    // thing?" is a style decision, not a rendering detail.
    std::string unrenderable = "\xef\xbf\xbd";
    // Separates a prompt control's label from its value ("Find: text").
    std::string promptLabelSeparator = ": ";
    StyleDimensions dimensions{};

    // Display width of the sigil, measured -- not declared.  A one-cell sigil
    // reserves one column, a wide glyph reserves two, with no second constant
    // to keep in step.
    int sigilWidth() const;

    // What the input line reserves from the status fields while a picker is
    // open.  Independent of the typed query by design (doc/spec-input-line.md).
    int inputLineReservation() const;

    // The cell at `row` of a scrollbar column `trackHeight` rows tall.
    //
    // Encodes the whole size rule in one place: size 0 means nothing is a thumb
    // and the column is all gutter, size 1 uses `single`, size 2 is top+bottom
    // with no body, and larger sizes are top, body..., bottom.  A thumb longer
    // than the track is clamped rather than trusted.
    ScrollbarCell scrollbarCell(int row, int thumbStart, int thumbSize,
                                 int trackHeight) const;

    // Style is a published snapshot section (doc/spec-style.md Y4), so it
    // participates in equality and wire round-trips like any other section.
    bool operator==(Style const&) const = default;
};

// style.define's argument: a flat name->value table.  Keys are the same field
// names the wire codec uses (scrollbar_track, tree_expanded, dim_header_height,
// ...); a glyph value is the literal string, a `dim_` value is a decimal
// integer.  The table may be PARTIAL -- a name absent from `values` keeps the
// current style's value for that field (doc/spec-style.md).  This mirrors
// theme.define's partial-table shape and is the one shape the Lua seam permits.
struct StyleDefineArguments {
    std::unordered_map<std::string, std::string> values;

    friend bool operator==(StyleDefineArguments const&,
                           StyleDefineArguments const&) = default;
};

struct StyleDefineError {
    std::string message;

    friend bool operator==(StyleDefineError const&,
                           StyleDefineError const&) = default;
};

struct StyleDefineResult {
    std::optional<StyleDefineError> error;
    // The replacement style when accepted; default-constructed and unused when
    // rejected -- all-or-nothing, no partial apply on error.
    Style style;

    [[nodiscard]] bool accepted() const noexcept { return !error.has_value(); }
};

// Applies style.define's table to `current`: replaces ONLY the named fields (an
// omitted name keeps its current value).  The whole table is validated before
// anything is applied, so a rejected call (unknown key, or a non-integer or
// negative value for a `dim_` key) never partially mutates the result -- the
// caller simply does not use `.style`.
[[nodiscard]] StyleDefineResult applyStyleDefine(
    Style const& current, StyleDefineArguments const& arguments);

// Every key style.define accepts (glyph + dimension names).  Exposed so a test
// can assert this set exactly matches the wire codec's Style field names --
// the two lists are hand-maintained in different files (this and Protocol.cpp),
// and this is the guard against them drifting apart.
[[nodiscard]] std::vector<std::string> styleDefineKeys();

// Every glyph style.define key paired with its current value in `style`, sorted
// by key.  Drives the help system's chrome-glyph listing so a newly added glyph
// (like a tab edge) documents itself with no second list to maintain.  Dimension
// keys are excluded -- this is glyphs only.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> styleGlyphValues(
    Style const& style);

}  // namespace ssg
