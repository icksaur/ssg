#pragma once

// Uniform pointer hit-testing over the scrollable regions a snapshot publishes
//. HitTester maps a terminal cell to the region and
// item under it, or to a scrollbar position. It performs NO input handling —
// milestone 8 (mouse) is the caller that turns a RegionHit into commands.

#include <ssg/GridPresenter.h>
#include <ssg/TreeModel.h>

#include <cstdint>
#include <optional>
#include <string>

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
    HeaderField,        // a shell header status field
    FooterField,
    ExternalAction,      // an external-modification action: externalFileId +
                         // commandId are set (7A-5b)
    NoticeAction,        // a notice action: fieldId names the semantic action
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
    // feed view.scroll_to_fraction. The server maps it with round-half-up
    // (first_row = round(maximum_first_row * numerator / denominator), then
    // clamped), so it is the exact inverse of the rounded thumb render and no
    // gutter row is skipped.
    std::uint32_t scrollNumerator = 0;
    std::uint32_t scrollDenominator = 1;
    // Header/footer status-field hits: the published field id and optional command.
    std::optional<std::string> fieldId;
    std::optional<std::string> commandId;
    // An external-modification action hit: the runtime-minted file id to select
    // before dispatching `commandId` (select-then-act). 7A-5b.
    std::optional<std::string> externalFileId;

    [[nodiscard]] bool hit() const noexcept { return region != HitRegion::None; }
    bool operator==(const RegionHit&) const = default;
};

// Classifies the terminal cell at (column, row) against a snapshot's regions.
// Precedence: when the palette is open it overlays the editor pane, so a cell in
// the pane area resolves to the palette (or its gutter), never the editor. The
// side panel, editor pane, and their gutters occupy disjoint columns, so their
// order does not matter. A cell outside every region, on the tree provider-label
// row, or in a reserved-but-empty gutter/list area is HitRegion::None.
class HitTester {
public:
    explicit HitTester(GridFrame const& snapshot) noexcept
        : snapshot_(snapshot) {}

    [[nodiscard]] RegionHit at(int column, int row) const;

    // The thumb geometry for a scrollbar region, for grab-offset dragging: the
    // gutter's top row, and the region's ScrollbarMetrics viewportRows /
    // thumbStart / thumbSize (the same basis the server scrolls against).  A
    // scrollbar drag follows the row alone -- once the button is down the user is
    // manipulating THAT thumb and the pointer may wander off the one-column
    // gutter horizontally without dropping the drag -- so the app holds this
    // geometry from press and never re-classifies by column.  Empty when `region`
    // is not a scrollbar the snapshot currently lays out.
    struct GutterThumb {
        int gutterY;
        std::uint32_t viewportRows;
        std::uint32_t thumbStart;
        std::uint32_t thumbSize;
    };
    [[nodiscard]] std::optional<GutterThumb> gutterThumb(HitRegion region) const;

private:
    GridFrame const& snapshot_;
};

}  // namespace ssg
