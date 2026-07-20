#pragma once

// Uniform pointer hit-testing over the scrollable regions a snapshot publishes
// (see doc/spec-scroll.md R4). This is pure, data-only classification: it maps a
// terminal cell to the region and item under it, or to a scrollbar position. It
// performs NO input handling — milestone 8 (mouse) is the caller that turns a
// RegionHit into commands.

#include <ssg/session_snapshot.h>
#include <ssg/tree.h>

#include <cstdint>
#include <optional>

namespace ssg {

enum class HitRegion : std::uint8_t {
    None,               // out of bounds, chrome, or a reserved-but-empty cell
    Editor,             // a document cell: byte_offset / byte_len are set.  A click
                        // past a row's content, on a blank row, or below the last
                        // line resolves to a zero-width END-OF-LINE position
                        // (byte_offset = the row's end, byte_len = 0), so clicking
                        // anywhere on an editor row places the caret at the line end.
    Panel,              // a tree row: node_id is set
    Palette,            // a palette row: item_index is the absolute rank index
    Tab,                // a tab-bar tab: tab_index selects sections().tabs.tabs
    EditorScrollbar,   // the editor pane gutter: scroll_* are set
    PanelScrollbar,    // the side-panel gutter: scroll_* are set
    PaletteScrollbar,  // the palette gutter: scroll_* are set
};

struct RegionHit {
    HitRegion region = HitRegion::None;
    // Editor content: the document byte span of the hit cell.
    std::uint32_t byteOffset = 0;
    std::uint32_t byteLen = 0;
    // Panel content: the tree node under the cell.
    std::optional<TreeNodeId> nodeId;
    // Palette content: the ABSOLUTE index into the full ranked order
    // (first_visible + on-screen row).
    std::uint32_t itemIndex = 0;
    // Tab content: the index into sections().tabs.tabs of the clicked tab.
    std::uint32_t tabIndex = 0;
    // Scrollbar regions: the position as a numerator/denominator pair ready to
    // feed view.scroll_to_fraction (first_row = maximum_first_row * numerator /
    // denominator), plus the equivalent [0, 1] fraction for display.
    std::uint32_t scrollNumerator = 0;
    std::uint32_t scrollDenominator = 1;
    double scrollbarFraction = 0.0;

    [[nodiscard]] bool hit() const noexcept { return region != HitRegion::None; }
    bool operator==(const RegionHit&) const = default;
};

// Classify the terminal cell at (column, row) against the snapshot's regions.
// Precedence: when the palette is open it overlays the editor pane, so a cell in
// the pane area resolves to the palette (or its gutter), never the editor. The
// side panel, editor pane, and their gutters occupy disjoint columns, so their
// order does not matter. A cell outside every region, on the tree provider-label
// row, or in a reserved-but-empty gutter/list area returns HitRegion::none.
[[nodiscard]] RegionHit hitTest(SessionSnapshot const& snapshot, int column,
                                 int row);

}  // namespace ssg
