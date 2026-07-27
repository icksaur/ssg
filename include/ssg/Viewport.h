#pragma once

#include <ssg/DiffModel.h>
#include <ssg/GraphemeLayout.h>
#include <ssg/types.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
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

struct RealRow {
    uint32_t bufferLine;
    uint32_t bufferVisualRow;
    uint32_t startByteOffset = 0;
    uint32_t endByteOffset = 0;
    uint32_t startCell = 0;
    uint32_t endCell = 0;
    // Present only for an UNWRAPPED Modified line rendered as ONE merged
    // inline row (git --word-diff style: baseline-removed and target-added
    // words shown inline on one row) instead of the usual phantom-row-above
    // plus target-row-below split. Empty for every other row, including a
    // Modified line under word wrap (ghost spans are unwrapped-only; a
    // wrapped Modified line always keeps the two-row split). Viewport is the
    // sole computer of this data (from DiffLineChange::inlineWordSegments);
    // Renderer paints it verbatim.
    std::vector<InlineWordSegment> mergedSegments;

    bool operator==(const RealRow&) const noexcept = default;
};

struct PhantomRow {
    uint32_t baselineLine;
    std::string text;
    uint32_t followingByteOffset;
    // Byte ranges within `text` that were the specific words removed by a
    // Modified pair's edit (empty for a phantom row from a pure Removed
    // line, where the whole line is gone and there is nothing more specific
    // to mark). Lets the renderer apply a stronger RemovedWord tint over
    // just those ranges, mirroring how a real row's AddedWord/ModifiedWord
    // marks work -- the phantom row stays tint-only (no syntax fg, per this
    // project's REMOVED-ROW SYNTAX decision), but still gets word-level
    // diff marks, which are an orthogonal concept to syntax coloring.
    std::vector<DiffWordRange> removedWordRanges;

    bool operator==(const PhantomRow&) const noexcept = default;
};

using ProjectedRow = std::variant<RealRow, PhantomRow>;

class RowProjection {
public:
    explicit RowProjection(std::vector<ProjectedRow> rows);

    [[nodiscard]] std::span<const ProjectedRow> rows() const noexcept;
    [[nodiscard]] const ProjectedRow& row(uint32_t visualRow) const;
    [[nodiscard]] uint32_t totalRows() const noexcept;
    [[nodiscard]] uint32_t visualRowForReal(uint32_t bufferVisualRow) const;
    [[nodiscard]] uint32_t visualRowForBufferLine(uint32_t bufferLine) const;
    [[nodiscard]] uint32_t visualRowForPosition(
        const DocumentPosition& position) const;
    [[nodiscard]] uint32_t movedRealRow(uint32_t visualRow,
                                        int64_t visualDistance) const;

private:
    std::vector<ProjectedRow> rows_;
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

struct ScrollbarMetrics {
    uint32_t totalRows;
    uint32_t viewportRows;
    uint32_t firstRow;
    uint32_t maximumFirstRow;
    uint32_t thumbStart;
    uint32_t thumbSize;

    bool operator==(const ScrollbarMetrics&) const noexcept = default;
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
    // an operation that clamps. Prefer shiftUnbounded below for a relative
    // move, which cannot overflow.
    void setFirstVisible(uint32_t value) noexcept { firstVisible_ = value; }

    // Shift with an UNBOUNDED top, so the top clamp stays deferred to the
    // viewport (see below), but saturating rather than overflowing: `rows` is
    // wire-decoded and may be any int64, and signed overflow is UB.
    void shiftUnbounded(std::int64_t delta) noexcept {
        constexpr auto kMax = std::numeric_limits<std::uint32_t>::max();
        if (delta >= 0) {
            const auto room = static_cast<std::uint64_t>(kMax - firstVisible_);
            firstVisible_ = static_cast<std::uint64_t>(delta) > room
                                ? kMax
                                : firstVisible_ + static_cast<std::uint32_t>(delta);
            return;
        }
        // Negating INT64_MIN is UB, so take the magnitude without negating.
        const std::uint64_t magnitude =
            static_cast<std::uint64_t>(-(delta + 1)) + 1;
        firstVisible_ = magnitude > firstVisible_
                            ? 0
                            : firstVisible_ - static_cast<std::uint32_t>(magnitude);
    }

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
public:    // The scrollbar thumb geometry for a list of `total_rows` items shown in a
    // `viewport_rows`-tall window scrolled to `first_row`.  When the content fits
    // (`total_rows <= viewport_rows`) the thumb is hidden: `maximum_first_row`,
    // `thumb_start`, and `thumb_size` collapse to a no-thumb sentinel.  Shared by
    // every scrollable region (editor, tree, palette) so thumb math lives in one
    // place (see doc/spec-scroll.md).
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

    [[nodiscard]] ViewportViewState compute(
        std::span<const CellRun> logicalLines,
        ViewportDimensions dimensions,
        uint32_t requestedFirstVisualRow = 0,
        const DiffFileView* diff = nullptr) const;

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
        ViewportDimensions dimensions,
        uint32_t requestedFirstVisualRow,
        uint32_t requestedFirstVisualColumn,
        int tabWidth,
        const DiffFileView* diff = nullptr) const;

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
};

}  // namespace ssg
