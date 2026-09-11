#pragma once

#include <ssg/Style.h>   // ToggleGlyphs
#include <ssg/Widget.h>  // Overflow

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

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
// published node geometry at paint time by renderFrame so the text
// and the caret are measured once and cannot drift. `layoutTextInput` owns only
// the text composition and the field width.
struct TextInputLayout {
    std::string text;  // sigil followed by the visible tail of value
    int width;         // cells the field occupies (<= available), caret-safe
    // The caret's column, counted from the field's own start (sigil
    // included). Tracks `cursor` when given; defaults to the field's width
    // (the caret trails the text), matching every caller that has no cursor.
    int cursorColumn = 0;
};

// Lay out a TextInput in `available` cells: reserve one column for the caret,
// keep the `sigil` pinned, and scroll `value` to keep `cursor` (a byte offset
// into `value`, defaulting to its end) visible. `width` is clamped so the
// caret column stays inside `available`.
[[nodiscard]] TextInputLayout layoutTextInput(
    std::string_view sigil, std::string_view value, int available,
    std::optional<std::size_t> cursor = std::nullopt);

// The tail of `value` that fits in `cells` display columns, grapheme-sliced (so
// a multi-byte or wide cluster is never cut in half) -- the END of a growing
// value stays visible. Exposed for reuse/testing; `layoutTextInput` uses it.
[[nodiscard]] std::string visibleTail(std::string_view value, int cells);

struct VisibleWindow {
    std::string text;
    int cursorColumn = 0;  // cells before the cursor, within `text`
};

// The minimal window of `value` (grapheme-sliced) that fits `cells` columns
// while keeping the grapheme boundary at byte offset `cursor` visible: the
// tail of `value[0:cursor]` that fits, followed by as much of `value[cursor:]`
// as the remaining cells allow. Degenerates to `visibleTail(value, cells)`
// (cursor trailing the shown text) when `cursor >= value.size()`.
[[nodiscard]] VisibleWindow visibleWindow(std::string_view value,
                                          std::size_t cursor, int cells);

// The full picker input line: a scrolling `TextInput` (sigil + query tail) plus
// a trailing completion `ghost` that fills whatever room the query left, up to
// its own display width. This bundles the query field and the ghost into one
// widget computation so the reserve/grow + ghost geometry lives in the widget
// layer, not stitched inline by the shell. The caret column and the node rects/
// roles stay with the renderFrame caller;
// this owns only text + widths. `ghostWidth` is 0 when there is no ghost or the
// query consumed the row; `ghostText` is the whole ghost (the renderer clips it
// to `ghostWidth`).
struct InputLineLayout {
    std::string text;       // sigil + visible query tail
    int width;              // query field cells (caret-safe, <= available)
    std::string ghostText;  // the completion ghost (empty when none)
    int ghostWidth;         // ghost cells after the query (0 when none/no room)
    int cursorColumn = 0;   // the query caret's column, field start included
};

// Lay out a picker input line in `available` cells: the query via
// `layoutTextInput` (caret reservation + cursor-visible scroll), then the
// ghost in the cells that remain, clamped to the ghost's display width.
[[nodiscard]] InputLineLayout layoutInputLine(
    std::string_view sigil, std::string_view query, std::string_view ghost,
    int available, std::optional<std::size_t> cursor = std::nullopt);

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
