#pragma once

#include <ssg/DiffRows.h>
#include <ssg/GraphemeLayout.h>
#include <ssg/LineLayoutCache.h>
#include <ssg/Scrollbar.h>
#include <ssg/types.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

struct DiffFileView;

struct ViewportDimensions {
    uint32_t columns;
    uint32_t rows;

    ViewportDimensions(uint32_t columns, uint32_t rows);
    bool operator==(const ViewportDimensions&) const noexcept = default;
};

struct VisualRow {
    uint32_t logicalLine;
    uint32_t firstSpan;
    uint32_t spanCount;
    CellIndex startCell;
    uint32_t contentCells;
    uint32_t visibleCells;
    // Document-absolute byte offset of this VISUAL row's end — the position a
    // click at/past the row's last content cell takes (M8 click-past-EOL).  For a
    // final/unwrapped visual row this is the logical end-of-line (the newline byte,
    // or text.size() for the last line without a trailing newline); for an interior
    // wrapped row it is the wrap boundary; for an empty line it is the line start.
    // Always a valid document position.
    uint32_t endByteOffset;

    bool operator==(const VisualRow&) const noexcept = default;
};

struct CellHitTarget {
    uint32_t viewportRow;
    uint32_t viewportColumn;
    uint32_t logicalLine;
    CellIndex cell;
    uint32_t byteOffset;
    uint32_t byteLen;

    bool operator==(const CellHitTarget&) const noexcept = default;
};

// A resolved scroll view for a simple list region: the clamped first visible
// item, how many items are visible, and the scrollbar geometry.  This is the
// generalized primitive the tree and palette use, mirroring what
// `Viewport::compute` produces for the editor.
struct ListScrollView {
    uint32_t firstVisible;
    uint32_t visibleCount;
    ScrollbarMetrics scrollbar;

    bool operator==(const ListScrollView&) const noexcept = default;
};

struct ViewportViewState {
    // The CLIENT'S full surface, which is what a client allocates its grid from.
    // Deliberately NOT the scrollable area: the shell spends rows on the header,
    // tab bar, footer and any reserved prompt, so the editor paints fewer rows
    // than this.  Everything below -- visibleRows, totalVisualRows, scrollbar --
    // describes that smaller CONTENT region.  Sizing scroll math from this field
    // instead leaves the last few lines of a document permanently unreachable.
    ViewportDimensions dimensions;
    uint32_t firstVisualRow;
    // The horizontal scroll offset in cells (word wrap OFF only; always 0 when
    // word wrap is on, since wrapped lines never scroll horizontally).  All
    // visible rows share this single per-pane offset.  It snaps to a grapheme
    // boundary: the leftmost visible cell is the first span start >= the requested
    // offset, so a wide cluster is never split (M12 VP-H / Decision A / H0).
    uint32_t firstVisualColumn;
    uint32_t totalVisualRows;
    std::vector<VisualRow> visibleRows;
    std::vector<ProjectedRow> rowProjection;
    std::vector<CellHitTarget> hitTargets;
    ScrollbarMetrics scrollbar;

    [[nodiscard]] ProjectedRow projectedRow(uint32_t viewportRow) const;
    [[nodiscard]] uint32_t editableOffset(uint32_t viewportRow) const;

    bool operator==(const ViewportViewState&) const noexcept = default;
};

struct ViewportDelta {
    bool changed;
    std::optional<ViewportViewState> replacement;

    bool operator==(const ViewportDelta&) const noexcept = default;
};

struct SelectionNavigation {
    std::uint32_t firstVisualRow = 0;
    std::uint32_t firstVisualColumn = 0;
    std::optional<CellIndex> desiredCell;

    bool operator==(const SelectionNavigation&) const noexcept = default;
};

// A scrollable surface's vertical position, and the operations on it.
//
// Owns exactly one durable field: which item is at the top. Geometry
// (`totalItems`, `viewportRows`, the selection) is passed at each call rather
// than stored, because those change every frame -- a cached copy would be a
// stale-cache bug, and every caller already threads them per-frame.
//
// This exists because a shared *function* could not stop the three scrollable
// surfaces diverging: a pure function cannot own state, so each surface grew
// its own offset field and its own clamp-and-shift arithmetic. Every operation
// here is expressed through `Viewport::listScrollView`, so the math still lives
// in one place; what is new is that the state and its mutators do too.
//
// A value type rather than a base class the surfaces inherit: the editor and
// tree offsets are server state while the picker's is client-owned by
// deliberate latency design, so a common base would have to drag one across
// that boundary. Both sides can hold one of these instead.
class ScrollOffset {
public:
    ScrollOffset() = default;
    explicit ScrollOffset(uint32_t firstVisible) noexcept
        : firstVisible_(firstVisible) {}

    [[nodiscard]] uint32_t firstVisible() const noexcept {
        return firstVisible_;
    }

    // Assign a caller-supplied position. Unclamped on purpose: the surface may
    // set a position before it knows its geometry, and every read goes through
    // an operation that clamps. Prefer byLines/byPages below for a relative
    // move, which cannot overflow and cannot drift outside the scroll range.
    void setFirstVisible(uint32_t value) noexcept { firstVisible_ = value; }

    // Explicit scroll gestures. These deliberately do NOT keep the selection
    // visible -- scrolling is the one interaction that decouples the view from
    // the selection, and must never snap back.
    //
    // `delta`/`pages` are wire-decoded and may be enormous, so the shift
    // saturates rather than overflowing.
    void byLines(std::int64_t delta, uint32_t totalItems, uint32_t viewportRows);
    void byPages(std::int64_t pages, uint32_t totalItems, uint32_t viewportRows);

    // Scroll to a position along the track, as from a scrollbar drag.
    // A zero denominator scrolls to the top rather than dividing by zero.
    void toFraction(uint32_t numerator, uint32_t denominator,
                    uint32_t totalItems, uint32_t viewportRows);

    // Shift minimally so `selected` lies inside the window. The counterpart to
    // the explicit gestures above.
    void revealSelection(uint32_t selected, uint32_t totalItems,
                         uint32_t viewportRows);

    // The window this offset currently denotes. Const: resolving describes the
    // surface, and must not scroll the thing it is describing.
    [[nodiscard]] ListScrollView resolve(uint32_t totalItems,
                                         uint32_t viewportRows) const;

    bool operator==(const ScrollOffset&) const noexcept = default;

private:
    uint32_t firstVisible_ = 0;
};

class Viewport {
public:
    // The scrollbar thumb geometry for a list of `total_rows` items shown in a
    // `viewport_rows`-tall window scrolled to `first_row`.  When the content fits
    // (`total_rows <= viewport_rows`) the thumb is hidden: `maximum_first_row`,
    // `thumb_start`, and `thumb_size` collapse to a no-thumb sentinel.  Shared by
    // every scrollable region (editor, tree, palette) so thumb math lives in one
    // place.
    [[nodiscard]] ScrollbarMetrics scrollbarMetrics(
        uint32_t totalRows,
        uint32_t viewportRows,
        uint32_t firstRow) const;

    // Resolve a list scroll view.  `first_visible` is always clamped to
    // `[0, maximum_first_row]`.  Keep-visible is an explicit input, never inferred:
    // only when `keep_selection_visible` is true AND `selected` holds an item index
    // does the window shift minimally so `selected` lies within
    // `[first_visible, first_visible + visible_count)`.  With
    // `keep_selection_visible == false` the (clamped) `first_visible` is honored
    // verbatim and the selection may fall outside the window, exactly as the editor
    // caret can.  `selected` is an absolute item index.
    [[nodiscard]] ListScrollView listScrollView(
        uint32_t totalItems,
        uint32_t viewportRows,
        uint32_t firstVisible,
        std::optional<uint32_t> selected,
        bool keepSelectionVisible) const;

    // `contentArea` is the region the editor actually PAINTS -- the client
    // surface minus whatever the shell spends on header, tab bar, footer and any
    // reserved prompt.  All scroll math derives from it, so a caller that passes
    // the whole surface here leaves the last rows of a document unreachable.
    // `clientSurface` is republished as `ViewportViewState::dimensions` for a
    // client to size its grid from; when omitted the two are the same, which is
    // what a caller with no surrounding chrome wants.
    [[nodiscard]] ViewportViewState compute(
        std::span<const CellRun> logicalLines,
        ViewportDimensions contentArea,
        uint32_t requestedFirstVisualRow = 0,
        const DiffFileView* diff = nullptr,
        std::optional<ViewportDimensions> clientSurface = std::nullopt) const;

    // Word-wrap-OFF viewport projection.  Builds the SAME ViewportViewState shape as
    // `compute` for a NON-wrapping document, but in O(visible rows) grapheme
    // segmentation instead of O(document): the total visual row count is the logical
    // line count (a byte scan for '\n'), and compute_cell_run runs only for the
    // visible lines.  Long lines are clipped at `dimensions.columns` (cells beyond
    // the width are not emitted).  For documents whose lines all fit the width, the
    // result is field-for-field equal to
    // `compute(active_cell_runs(document_text), dimensions, first_row)` — the
    // reference oracle (INV-projection-equivalence).  `tab_width` must match the full
    // path's (4 today).  Hit-target byte offsets are document-absolute.
    [[nodiscard]] ViewportViewState computeUnwrapped(
        std::string_view documentText,
        ViewportDimensions contentArea,
        uint32_t requestedFirstVisualRow,
        uint32_t requestedFirstVisualColumn,
        int tabWidth,
        const DiffFileView* diff = nullptr,
        std::optional<ViewportDimensions> clientSurface = std::nullopt,
        LineLayoutCache* lineCache = nullptr) const;

    [[nodiscard]] RowProjection rowProjection(
        std::span<const CellRun> logicalLines,
        uint32_t columns,
        const DiffFileView& diff) const;

    [[nodiscard]] RowProjection rowProjectionUnwrapped(
        std::string_view documentText,
        const DiffFileView& diff) const;

    [[nodiscard]] ViewportViewState scrollBy(
        std::span<const CellRun> logicalLines,
        ViewportDimensions dimensions,
        uint32_t currentFirstVisualRow,
        int64_t rowDelta,
        const DiffFileView* diff = nullptr) const;

    [[nodiscard]] ViewportDelta deriveDelta(
        const ViewportViewState& previous,
        const ViewportViewState& current) const;

private:
    LineLayoutCache lineCache_;
    std::optional<std::uint64_t> cellRunsRevision_;
    std::optional<std::uint64_t> cellRunsDocument_;
    std::vector<CellRun> cellRuns_;

    friend class EditorSession;
};

}  // namespace ssg
