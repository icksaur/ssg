#pragma once

// Composable chrome widgets (doc/spec-widget-chrome.md).
//
// A widget is a self-contained UI element positioned by RELATIVE layout inside
// its container. This header is the geometry core of the model: the widget
// primitive kinds, and the pure fit + layout functions that turn a row of
// measured, collapsible items into placements. `paint`/`project` (the render and
// accessibility projections) land with their first callers as the header,
// footer, and prompt are ported (spec Plan steps 2-4); Step 1 is the geometry
// the rest is built on, kept free of any renderer or runtime dependency so its
// fit rule is a pure, hand-checkable integer computation.

#include <ssg/ShellState.h>  // Rect

#include <cstdint>
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
};

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

// The fit pass. Reproduces `ShellState::addFields` EXACTLY (spec §Relative
// layout): stable-sort the items by `rank` ascending, then a single forward
// scan that appends each item while it fits and STOPS at the first that does not
// -- later, lower-priority items are NOT reconsidered even if a smaller one
// would have fit. `separator` cells sit between adjacent retained items.
// Retained items are emitted in their original order; `align` decides which end
// they pack toward. Items with `desired <= 0` are SKIPPED entirely -- never
// retained and never ending the scan -- matching addFields, which drops
// empty-value fields before the fit loop (callers omit empties upstream too).
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

// Map a `RowFit`'s relative offsets onto `container`, producing one absolute,
// single-row `Rect` per retained item (y and height come from the container's
// leading row). This is the one place a relative offset becomes an absolute
// coordinate.
[[nodiscard]] std::vector<Rect> layoutRow(const RowFit& fit,
                                          const Rect& container);

// A `Field`'s desired width: its display cells plus the 2-cell padding, floored
// at 1, matching `addFields`. Provided so callers measure fields consistently;
// the fit rule itself takes the already-measured `desired`.
[[nodiscard]] int measureFieldCells(std::string_view value);

}  // namespace ssg
