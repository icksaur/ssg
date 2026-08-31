#pragma once

// Composable chrome widgets.
//
// A widget is a self-contained UI element positioned by RELATIVE layout inside
// its container. This header is the geometry core of the model: the widget
// primitive kinds, and the pure fit + layout functions that turn a row of
// measured, collapsible items into placements. `paint`/`project` (the render and
// accessibility projections) land with their first callers as the header,
// footer, and prompt are ported (spec Plan steps 2-4); Step 1 is the geometry
// the rest is built on, kept free of any renderer or runtime dependency so its
// fit rule is a pure, hand-checkable integer computation.

#include <ssg/Geometry.h>    // Rect
#include <ssg/Style.h>       // ToggleGlyphs

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

// The closed set of chrome widget primitives. init.lua (eventually) COMPOSES
// trees of these; it does not add kinds. Kept as a fixed enum for exactly that
// reason (spec §Widget primitives).
enum class WidgetKind : std::uint8_t {
    Container,   // arranges children on an axis with a fit policy
    Label,       // static text in a role
    Field,       // an id'd, collapsible value with an optional command
    Checkbox,    // a boolean with checked/unchecked glyphs + a caption
    TextInput,   // a one-line editable region: sigil + scrolling tail + caret
    Spacer,      // a flexible gap
    View,        // an opaque client-rendered surface (its ViewSurface names which)
    StatusActions,  // the selected status item's actions, rendered from promptStatus
};

// The vocabulary made enumerable, mirroring SemanticRole's discipline: a count,
// a mirrored array, and a static_assert binding them. A client's UI profile
// (UiProfile.h) is a subset of this set, so both must enumerate the same kinds;
// keeping the enum, the count, and the array bound at compile time is what makes
// "the profile says X" and "the vocabulary has X" checkable against one source.
inline constexpr std::size_t kWidgetKindCount = 8;
inline constexpr std::array kAllWidgetKinds{
    WidgetKind::Container, WidgetKind::Label,     WidgetKind::Field,
    WidgetKind::Checkbox,  WidgetKind::TextInput, WidgetKind::Spacer,
    WidgetKind::View,      WidgetKind::StatusActions,
};
static_assert(kAllWidgetKinds.size() == kWidgetKindCount);

// The closed set of opaque client-rendered surfaces a View leaf may name. The
// library owns each surface's placement/size/presence in the tree and its
// authoritative data channel; a client owns the pixels. A client's UI profile
// declares which surfaces it implements, so this enum, its count, and its name
// array are bound like WidgetKind's.
enum class ViewSurface : std::uint8_t {
    TabBar = 0,       // the fixed tab strip
    FindResults = 3,  // the finder candidate universe (client filters locally)
    FooterPrompt = 5, // the footer-region prompt's controls (semantic PromptView)
    Notice = 6,       // the draft-conflict notice above the document (semantic NoticeView)
    ExternalModification = 7,  // the external-modification bar above the document
    Document = 8,     // the active document body, selection, caret, and syntax
    Tree = 9,         // the active tree provider named by TreeViewState
};

inline constexpr std::size_t kViewSurfaceCount = 7;
inline constexpr std::array kAllViewSurfaces{
    ViewSurface::TabBar, ViewSurface::FindResults, ViewSurface::FooterPrompt,
    ViewSurface::Notice, ViewSurface::ExternalModification,
    ViewSurface::Document, ViewSurface::Tree,
};
static_assert(kAllViewSurfaces.size() == kViewSurfaceCount);

// The stable wire/diagnostic name of a widget kind. Used to name the unsupported
// kind when a composition exceeds a client's UI profile. Throws
// std::invalid_argument on a corrupt/out-of-range enumerator, never a silent
// wrong slot.
[[nodiscard]] std::string_view widgetKindName(WidgetKind kind);

// The stable wire/diagnostic name of a view surface, mirroring widgetKindName.
// Throws std::invalid_argument on a corrupt/out-of-range enumerator.
[[nodiscard]] std::string_view viewSurfaceName(ViewSurface surface);

// Which end of the container the retained items pack toward. Start packs from
// the container's leading edge (header status fields); End packs flush to its
// trailing edge (footer actions + hint).
enum class Align : std::uint8_t { Start, End };

// One collapsible item's measurement, deliberately reduced to integers so the
// fit rule below is grapheme-agnostic and can be verified by hand. `desired` is
// the cells the item wants INCLUDING its own padding; `rank` orders collapse
// (lower rank = higher priority, dropped last).
struct FitItem {
    std::string id;
    int desired = 0;
    int rank = 0;
};

// One retained item's placement as an OFFSET within the container's main axis
// (relative, never absolute). `layoutRow` turns these into absolute rects at the
// single seam where the container's origin is known.
struct PlacedItem {
    std::string id;
    int offset = 0;
    int size = 0;
    // Position of this item in the input `items` vector, so a caller maps a
    // placement back to its source WITHOUT an id lookup -- ids need not be
    // unique and the caller never assumes they are.
    std::size_t index = 0;

    friend bool operator==(const PlacedItem&, const PlacedItem&) = default;
};

struct RowFit {
    // Retained items in ORIGINAL (pre-collapse) order, laid out left-to-right.
    std::vector<PlacedItem> placed;

    friend bool operator==(const RowFit&, const RowFit&) = default;
};

// The fit pass, and the collapse engine behind `WidgetStack`'s left group (spec
// §Relative layout): stable-sort the items by `rank` ascending, then a single
// forward scan that appends each item while it fits and STOPS at the first that
// does not -- later, lower-priority items are NOT reconsidered even if a smaller
// one would have fit. `separator` cells sit between adjacent retained items.
// Retained items are emitted in their original order; `align` decides which end
// they pack toward. Items with `desired <= 0` are SKIPPED entirely -- never
// retained and never ending the scan -- so empty-value fields (which callers
// give a non-positive width) cannot occupy a cell or a separator.
[[nodiscard]] RowFit fitRow(const std::vector<FitItem>& items, int extent,
                            int separator, Align align);

// The right-pack policy for the footer's actions and help hint: items fill flush
// to the trailing edge from the RIGHT (the last item is rightmost), each clamped
// to the space still remaining to its left, with NO separators. An item with no
// room left is dropped; a partially-fitting item is TRUNCATED to what remains
// (it is not dropped). This is a different rule from `fitRow` -- positional
// clamp-truncation, not rank-based collapse -- matching the footer's original
// reverse-iteration packing. Placements are returned in ORIGINAL order; items
// with `desired <= 0` are skipped. `rank` is unused here.
[[nodiscard]] RowFit packEnd(const std::vector<FitItem>& items, int extent);

// A `Field`'s desired width: its display cells plus the 2-cell padding, floored
// at 1. Provided so callers measure fields consistently; the fit rule itself
// takes the already-measured `desired`.
[[nodiscard]] int measureFieldCells(std::string_view value);

// --- Widget paint (glyph-owning text composition) ---------------------------
//
// These own "what text a widget shows", moved out of the renderer so the
// configurable glyphs live with the widget rather than being stitched in at
// paint time. Each is a pure
// string composition; the caller still blits the result and owns the row rect,
// role, and caret.

// A `Checkbox`'s cell text: the checked/unchecked glyph from `Style.toggle`
// followed by the caption. The find toggles (case/word/regex) draw through this.
[[nodiscard]] std::string checkboxText(bool checked, std::string_view caption,
                                       const ToggleGlyphs& toggle);

// A `TextInput`'s (or `Label`'s) cell text: a leading `prefix` (a prompt label
// or the picker input-line sigil), an optional `separator` (the prompt label
// separator; empty for the sigil), then the `value`. `textInputText(label, sep,
// value)` reproduces the prompt input composition; `textInputText(sigil, "",
// tail)` reproduces the picker input line; `textInputText(text, "", "")` is a
// bare `Label`.
[[nodiscard]] std::string textInputText(std::string_view prefix,
                                        std::string_view separator,
                                        std::string_view value);

// --- TextInput: a one-line editable text field (the reusable seam) ----------
//
// A `TextInput` shows a fixed leading `sigil` (the field's pinned identity, e.g.
// the picker's "> ") followed by a `value` that SCROLLS so its END -- where the
// next keystroke lands -- stays visible as it outgrows the field. This is the
// same reveal principle as the editor caret, and it is the seam a future
// non-prompt text field instantiates; today the picker input line is its only
// caller. The field reserves one trailing column for the caret so a terminal
// cursor always has a real cell to sit on when the value fills the width.
//
// The caret's screen column is NOT produced here: it is derived from the
// published node geometry at paint time (Renderer `inputLineCaret`) so the text
// and the caret are measured once and cannot drift. `layoutTextInput` owns only
// the text composition and the field width.
struct TextInputLayout {
    std::string text;  // sigil followed by the visible tail of value
    int width;         // cells the field occupies (<= available), caret-safe
};

// Lay out a TextInput in `available` cells: reserve one column for the caret,
// keep the `sigil` pinned, and scroll `value` to its visible tail. `width` is
// clamped so the caret column stays inside `available`.
[[nodiscard]] TextInputLayout layoutTextInput(std::string_view sigil,
                                              std::string_view value,
                                              int available);

// The tail of `value` that fits in `cells` display columns, grapheme-sliced (so
// a multi-byte or wide cluster is never cut in half) -- the END of a growing
// value stays visible. Exposed for reuse/testing; `layoutTextInput` uses it.
[[nodiscard]] std::string visibleTail(std::string_view value, int cells);

// The full picker input line: a scrolling `TextInput` (sigil + query tail) plus
// a trailing completion `ghost` that fills whatever room the query left, up to
// its own display width. This bundles the query field and the ghost into one
// widget computation so the reserve/grow + ghost geometry lives in the widget
// layer, not stitched inline by the shell. The caret column and the node rects/
// roles stay with the caller (Renderer `inputLineCaret`, ShellState emission);
// this owns only text + widths. `ghostWidth` is 0 when there is no ghost or the
// query consumed the row; `ghostText` is the whole ghost (the renderer clips it
// to `ghostWidth`).
struct InputLineLayout {
    std::string text;       // sigil + visible query tail
    int width;              // query field cells (caret-safe, <= available)
    std::string ghostText;  // the completion ghost (empty when none)
    int ghostWidth;         // ghost cells after the query (0 when none/no room)
};

// Lay out a picker input line in `available` cells: the query via
// `layoutTextInput` (caret reservation + tail scroll), then the ghost in the
// cells that remain, clamped to the ghost's display width.
[[nodiscard]] InputLineLayout layoutInputLine(std::string_view sigil,
                                              std::string_view query,
                                              std::string_view ghost,
                                              int available);

// --- WidgetStack: one composable row (packLeft/packRight/center) -------------
//
// A `WidgetStack` lays out ONE row of the header or footer as three groups
//: a LEFT group packed from the leading edge, a
// RIGHT group packed from the trailing edge, and an optional single CENTER slot
// between them. It is the one primitive that replaces `fitRow` + `packEnd` + the
// input-line arithmetic; it is a pure value type with no renderer/runtime
// dependency, and its builder methods return `*this` so calls chain:
// `stack.packLeft(a).packRight(z).packLeft(b)` yields `ab … z`.
//
// Two ORTHOGONAL fit levels:
//  - membership (which items survive a crowded group): the LEFT group collapses
//    by `rank` via the `fitRow` rule (stable-sort, forward scan, stop at first
//    non-fit), except items marked `keep` which are always retained; the RIGHT
//    group uses the `packEnd` rule (fill from the right, clamp-truncate the
//    leftmost item that still has room, drop zero-room items); the center slot is
//    never dropped.
//  - content fit (how a surviving item's text fills the cell it received):
//    `Overflow::None` (fits), `Truncate` (renderer clips to the rect; the node
//    keeps full content), or `ScrollTail` (pin `sigil`, show the value's tail via
//    `layoutTextInput`).
//
// Resolution order (reproduces both regions today): the RIGHT group claims its
// cells first, the LEFT group fills the remainder to the left of it, and the
// CENTER slot takes the gap between the left group's end and the right group's
// start (`Fixed` clamped to that gap, `Flex` = the whole gap).

enum class Overflow : std::uint8_t { None, Truncate, ScrollTail };

// One stack item. `desired` is the cells it wants (its content's display width,
// including any padding the caller already added). `rank` orders LEFT-group
// collapse (lower = higher priority, dropped last); `keep` exempts a LEFT item
// from collapse. `overflow` + `sigil` decide content fit (sigil is ScrollTail
// only). `content` is the full text the item wants to show.
struct StackItem {
    std::string id;
    std::string content;
    int desired = 0;
    int rank = 0;
    bool keep = false;
    Overflow overflow = Overflow::None;
    std::string sigil;
};

// One resolved placement: a relative `offset`+`size` on the row's main axis and
// the RESOLVED display `text` (the value tail for `ScrollTail`; the full content
// otherwise -- `Truncate` leaves the text whole and lets the renderer clip).
struct StackPlacement {
    std::string id;
    int offset = 0;
    int size = 0;
    std::string text;

    friend bool operator==(const StackPlacement&, const StackPlacement&) = default;
};

// Resolved stack: placements in EMISSION order -- left group (original order),
// then the center slot, then the right group (original order, left-to-right).
// Callers reorder for node emission as their region requires.
struct StackLayout {
    std::vector<StackPlacement> placed;

    friend bool operator==(const StackLayout&, const StackLayout&) = default;
};

// How the center slot claims the gap between the left and right groups.
enum class CenterWidth : std::uint8_t { Fixed, Flex };

class WidgetStack {
public:
    // `separator` cells sit between adjacent LEFT-group items (as `fitRow`); the
    // right group has none.
    explicit WidgetStack(int separator = 0) : separator_{separator} {}

    WidgetStack& packLeft(StackItem item);
    WidgetStack& packRight(StackItem item);
    // Sets the SINGLE center slot; a second call is a fail-loud error (`resolve`
    // returns nullopt), never a silent overwrite.
    WidgetStack& center(StackItem item, CenterWidth width, int fixed = 0);

    // Resolve over `extent` cells. Returns nullopt only on a structural authoring
    // error (a second center). Narrowness is handled gracefully (left collapses,
    // right truncates, a Fixed center clamps to the available gap).
    [[nodiscard]] std::optional<StackLayout> resolve(int extent) const;

private:
    int separator_;
    std::vector<StackItem> left_;
    std::vector<StackItem> right_;
    std::optional<StackItem> center_;
    CenterWidth centerWidth_ = CenterWidth::Flex;
    int centerFixed_ = 0;
    bool centerConflict_ = false;
};

}  // namespace ssg
