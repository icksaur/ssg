#include <ssg/GraphemeLayout.h>
#include <ssg/DiffModel.h>
#include <ssg/LineLayoutCache.h>
#include <ssg/Selection.h>
#include <ssg/Viewport.h>

#include "test_helpers.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#ifndef VIEWPORT_FIXTURE_DIR
#error "VIEWPORT_FIXTURE_DIR must name the hand-authored viewport fixtures"
#endif

namespace {

using ssg::CellRun;
using ssg::DiffFileId;
using ssg::DiffFileView;
using ssg::PhantomRow;
using ssg::RealRow;
using ssg::ViewportDimensions;
using ssg::ViewportViewState;

std::vector<CellRun> runs(std::initializer_list<std::string_view> lines) {
    std::vector<CellRun> result;
    result.reserve(lines.size());
    for (const auto line : lines) {
        result.push_back(ssg::computeCellRun(line));
    }
    return result;
}

// The reference line splitter: mirrors EditorSession::Impl::active_cell_runs
// (split on '\n', one CellRun per logical line, an empty document -> one empty
// run).  compute_viewport over these runs is the oracle compute_viewport_unwrapped
// must match for fitting lines.
std::vector<CellRun> cellRunsFromText(std::string_view text, int tab) {
    std::vector<CellRun> result;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        auto line = text.substr(
            start, end == std::string_view::npos ? end : end - start);
        result.push_back(ssg::computeCellRun(line, tab));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    if (result.empty()) result.push_back(ssg::computeCellRun("", tab));
    return result;
}

std::string serialize(const ViewportViewState& state) {
    std::ostringstream out;
    out << "offset=" << state.firstVisualRow
        << " total=" << state.totalVisualRows << '\n';
    const auto& bar = state.scrollbar;
    out << "scroll=" << bar.totalRows << ',' << bar.viewportRows << ','
        << bar.firstRow << ',' << bar.maximumFirstRow << ','
        << bar.thumbStart << ',' << bar.thumbSize << '\n';
    for (std::size_t i = 0; i < state.visibleRows.size(); ++i) {
        const auto& row = state.visibleRows[i];
        out << "row=" << i << ',' << row.logicalLine << ','
            << row.firstSpan << ',' << row.spanCount << ','
            << row.startCell.value() << ',' << row.contentCells << ','
            << row.visibleCells << '\n';
    }
    for (const auto& hit : state.hitTargets) {
        out << "hit=" << hit.viewportRow << ',' << hit.viewportColumn << ','
            << hit.logicalLine << ',' << hit.cell.value() << ','
            << hit.byteOffset << ',' << hit.byteLen << '\n';
    }
    return out.str();
}

std::string fixture(std::string_view name) {
    std::ifstream input(std::string(VIEWPORT_FIXTURE_DIR) + "/" +
                        std::string(name));
    std::ostringstream contents;
    contents << input.rdbuf();
    ASSERT_TRUE(input.good() || input.eof());
    return contents.str();
}

DiffFileView removedLines(std::size_t baselineStart,
                          std::size_t targetStart,
                          std::initializer_list<std::string> lines) {
    DiffFileView file{DiffFileId{"doc.txt"}};
    file.currentContent = "one\ntwo\nthree";
    file.hunks.push_back({.baselineStart = baselineStart,
                          .targetStart = targetStart,
                          .baselineLines = lines,
                          .targetLines = {}});
    return file;
}

// A clean single-line 1:1 Modified pair ("gamma original line two" ->
// "gamma modified line two") with inline segments already isolating the one
// changed word, mirroring what DiffModel.cpp's computeWordDiff actually
// produces: Unchanged "gamma ", Removed "original", Separator " ", Added
// "modified", Unchanged " line two". Only Unchanged/Added segments are REAL
// (they reconstruct the target line "gamma modified line two" exactly);
// Removed/Separator are GHOST (display-only, no real document byte).
DiffFileView modifiedLineWithInlineSegments() {
    using ssg::InlineWordSegment;
    DiffFileView file{DiffFileId{"doc.txt"}};
    file.currentContent = "gamma modified line two";
    file.hunks.push_back({.baselineStart = 0,
                          .targetStart = 0,
                          .baselineLines = {"gamma original line two"},
                          .targetLines = {"gamma modified line two"}});
    file.changedLines.push_back(
        {.kind = ssg::DiffLineKind::Modified,
         .baselineLine = std::size_t{0},
         .targetLine = std::size_t{0},
         .inlineWordSegments = {
             {InlineWordSegment::Kind::Unchanged, "gamma "},
             {InlineWordSegment::Kind::Removed, "original"},
             {InlineWordSegment::Kind::Separator, " "},
             {InlineWordSegment::Kind::Added, "modified"},
             {InlineWordSegment::Kind::Unchanged, " line two"},
         }});
    return file;
}

void assertGolden(std::string_view name,
                   const std::vector<CellRun>& lines,
                   ViewportDimensions dimensions,
                   uint32_t firstRow) {
    ASSERT_EQ(serialize(ssg::computeViewport(lines, dimensions, firstRow)),
              fixture(name));
}

}  // namespace

TEST(emptyViewportGolden) {
    assertGolden("empty.txt", {}, ViewportDimensions{4, 2}, 0);
}

TEST(shortViewportGolden) {
    assertGolden("short.txt", runs({"abc"}), ViewportDimensions{5, 2}, 0);
}

TEST(wideBoundaryGolden) {
    assertGolden("wide.txt", runs({"A\xE4\xB8\xAD" "B"}),
                  ViewportDimensions{3, 2}, 0);
}

TEST(wrappedScrolledGolden) {
    assertGolden("wrapped.txt", runs({"abcdef", "xy", "12345"}),
                  ViewportDimensions{3, 2}, 2);
}

TEST(tinyClippedGolden) {
    assertGolden("tiny.txt", runs({"\xE4\xB8\xAD" "a"}),
                  ViewportDimensions{1, 1}, 0);
}

TEST(noDiffProjectionIsIdentity) {
    auto state = ssg::computeViewport(
        runs({"one", "two", "three"}), ViewportDimensions{20, 4});
    ASSERT_TRUE(state.rowProjection.empty());
    ASSERT_EQ(state.totalVisualRows, std::uint32_t{3});
    for (std::uint32_t row = 0; row < state.visibleRows.size(); ++row) {
        auto projected = state.projectedRow(row);
        ASSERT_TRUE(std::holds_alternative<RealRow>(projected));
        if (auto real = std::get_if<RealRow>(&projected)) {
            ASSERT_EQ(real->bufferLine, row);
            ASSERT_EQ(real->bufferVisualRow, row);
        }
    }
}

TEST(removedRowsAreProjectedAtTargetAndCountedByScrollbar) {
    auto diff = removedLines(1, 1, {"gone\n", "also gone\n"});
    auto state = ssg::computeViewport(
        runs({"one", "two", "three"}), ViewportDimensions{20, 4}, 0, &diff);

    ASSERT_EQ(state.totalVisualRows, std::uint32_t{5});
    ASSERT_EQ(state.scrollbar.totalRows, std::uint32_t{5});
    ASSERT_EQ(state.rowProjection.size(), std::size_t{4});
    ASSERT_TRUE(std::holds_alternative<RealRow>(state.projectedRow(0)));
    ASSERT_TRUE(std::holds_alternative<PhantomRow>(state.projectedRow(1)));
    ASSERT_TRUE(std::holds_alternative<PhantomRow>(state.projectedRow(2)));
    ASSERT_TRUE(std::holds_alternative<RealRow>(state.projectedRow(3)));
    auto first = std::get<PhantomRow>(state.projectedRow(1));
    auto second = std::get<PhantomRow>(state.projectedRow(2));
    ASSERT_EQ(first.baselineLine, std::uint32_t{1});
    ASSERT_EQ(first.text, "gone");
    ASSERT_EQ(first.followingByteOffset, std::uint32_t{4});
    ASSERT_EQ(second.baselineLine, std::uint32_t{2});
    ASSERT_EQ(second.text, "also gone");
    ASSERT_EQ(second.followingByteOffset, std::uint32_t{4});
}

TEST(leadingTrailingAndWrappedPhantomBlocksUseTheSameProjection) {
    auto leading = removedLines(0, 0, {"before\n"});
    auto leadingState = ssg::computeViewport(
        runs({"one", "two", "three"}), ViewportDimensions{20, 8}, 0,
        &leading);
    ASSERT_TRUE(
        std::holds_alternative<PhantomRow>(leadingState.projectedRow(0)));
    ASSERT_EQ(std::get<PhantomRow>(leadingState.projectedRow(0))
                  .followingByteOffset,
              std::uint32_t{0});
    ASSERT_EQ(leadingState.editableOffset(0), std::uint32_t{0});

    auto trailing = removedLines(3, 3, {"abcdef\n"});
    auto trailingState = ssg::computeViewport(
        runs({"one", "two", "three"}), ViewportDimensions{3, 8}, 0,
        &trailing);
    ASSERT_EQ(trailingState.totalVisualRows, std::uint32_t{6});
    ASSERT_TRUE(
        std::holds_alternative<PhantomRow>(trailingState.projectedRow(4)));
    ASSERT_TRUE(
        std::holds_alternative<PhantomRow>(trailingState.projectedRow(5)));
    ASSERT_EQ(std::get<PhantomRow>(trailingState.projectedRow(4)).text,
              "abc");
    ASSERT_EQ(std::get<PhantomRow>(trailingState.projectedRow(5)).text,
              "def");
    ASSERT_EQ(std::get<PhantomRow>(trailingState.projectedRow(5))
                  .followingByteOffset,
              std::uint32_t{13});
    ASSERT_EQ(trailingState.editableOffset(5), std::uint32_t{13});
}

TEST(unwrappedProjectionStaysAlignedAcrossEmptyAndScrolledOffRows) {
    auto diff = removedLines(0, 0, {});
    const std::string text = "a\n\nb";
    auto state = ssg::computeUnwrappedViewport(
        text, ViewportDimensions{2, 3}, 0, 5, 4, &diff);
    ASSERT_EQ(state.visibleRows.size(), std::size_t{3});
    ASSERT_EQ(state.rowProjection.size(), state.visibleRows.size());
    for (std::uint32_t row = 0; row < state.visibleRows.size(); ++row) {
        ASSERT_TRUE(std::holds_alternative<RealRow>(state.projectedRow(row)));
        ASSERT_EQ(state.editableOffset(row),
                  state.visibleRows[row].endByteOffset);
    }
}

TEST(unwrappedMergedInlineRowGhostSpansResolveHitTestsToRealBytesAroundThem) {
    auto diff = modifiedLineWithInlineSegments();
    auto state = ssg::computeUnwrappedViewport(
        diff.currentContent, ViewportDimensions{40, 3}, 0, 0, 4, &diff);
    // Merged text: "gamma original modified line two" (ghosts "original"/" "
    // spliced between the real "gamma " prefix and the real "modified line
    // two" suffix). Real text is "gamma modified line two" -- the real
    // document only ever has ONE row here (the merged inline row replaces
    // both the baseline phantom row and the plain target row), never a
    // separate phantom row for this line.
    ASSERT_EQ(state.rowProjection.size(), std::size_t{1});
    ASSERT_TRUE(std::holds_alternative<RealRow>(state.projectedRow(0)));
    const auto ghostStart = std::string_view{"gamma "}.size();
    const auto ghostLen = std::string_view{"original "}.size();
    const auto realAfterGhost =
        std::string_view{"gamma "}.size();  // "modified..." starts right
                                             // after "gamma " in the real doc
    for (std::size_t column = ghostStart; column < ghostStart + ghostLen;
         ++column) {
        const ssg::CellHitTarget* hit = nullptr;
        for (const auto& candidate : state.hitTargets) {
            if (candidate.viewportRow == 0 &&
                candidate.viewportColumn == column) {
                hit = &candidate;
                break;
            }
        }
        ASSERT_TRUE(hit != nullptr);
        ASSERT_EQ(hit->byteOffset, std::uint32_t{6});  // "gamma " is 6 bytes
        ASSERT_EQ(hit->byteLen, std::uint32_t{0});
    }
    const ssg::CellHitTarget* afterGhost = nullptr;
    for (const auto& candidate : state.hitTargets) {
        if (candidate.viewportRow == 0 &&
            candidate.viewportColumn == ghostStart + ghostLen) {
            afterGhost = &candidate;
            break;
        }
    }
    ASSERT_TRUE(afterGhost != nullptr);
    ASSERT_EQ(afterGhost->byteOffset,
              static_cast<std::uint32_t>(realAfterGhost));
    ASSERT_TRUE(afterGhost->byteLen > 0);
}

TEST(wrappedModifiedLineNeverMergesEvenWhenTheLineFitsOnOneRow) {
    // Ghost spans are unwrapped-only: the
    // WRAPPED projection must always keep the two-row baseline-phantom /
    // target-row split for a Modified line, even one short enough to have
    // fit as a single visual row.
    auto diff = modifiedLineWithInlineSegments();
    auto lines = runs({"gamma modified line two"});
    auto state = ssg::computeViewport(lines, ViewportDimensions{80, 4}, 0,
                                          &diff);
    ASSERT_EQ(state.totalVisualRows, std::uint32_t{2});
    ASSERT_TRUE(std::holds_alternative<PhantomRow>(state.projectedRow(0)));
    ASSERT_TRUE(std::holds_alternative<RealRow>(state.projectedRow(1)));
    ASSERT_EQ(std::get<PhantomRow>(state.projectedRow(0)).text,
              "gamma original line two");
}

TEST(scrollSaturatesAndDeltaSuppressesEqualPayload) {
    const auto lines = runs({"abcdef", "xy"});
    const auto initial =
        ssg::computeViewport(lines, ViewportDimensions{2, 2}, 0);
    const auto bottom =
        ssg::scrollViewportBy(lines, ViewportDimensions{2, 2}, 0, 99);
    ASSERT_EQ(bottom.firstVisualRow, bottom.scrollbar.maximumFirstRow);
    const auto top = ssg::scrollViewportBy(
        lines, ViewportDimensions{2, 2}, bottom.firstVisualRow, -99);
    ASSERT_EQ(top.firstVisualRow, 0u);

    const auto same = ssg::deriveViewportDelta(initial, initial);
    ASSERT_FALSE(same.changed);
    ASSERT_FALSE(same.replacement.has_value());
    const auto changed = ssg::deriveViewportDelta(initial, bottom);
    ASSERT_TRUE(changed.changed);
    ASSERT_TRUE(changed.replacement.has_value());
    ASSERT_EQ(*changed.replacement, bottom);
}

TEST(viewportBoundsProperties) {
    const auto lines =
        runs({"", "abcdef", "A\xE4\xB8\xAD" "B", "\tZ",
              "\xCC\x81" "x"});
    for (uint32_t columns = 1; columns <= 8; ++columns) {
        for (uint32_t rows = 1; rows <= 5; ++rows) {
            for (uint32_t requested = 0; requested <= 40; ++requested) {
                const auto state = ssg::computeViewport(
                    lines, ViewportDimensions{columns, rows}, requested);
                ASSERT_TRUE(state.firstVisualRow <=
                            state.scrollbar.maximumFirstRow);
                ASSERT_TRUE(state.visibleRows.size() <= rows);
                ASSERT_TRUE(state.scrollbar.thumbSize >= 1);
                ASSERT_TRUE(state.scrollbar.thumbStart +
                                state.scrollbar.thumbSize <=
                            rows);
                for (const auto& row : state.visibleRows) {
                    ASSERT_TRUE(row.logicalLine < lines.size());
                    ASSERT_TRUE(row.visibleCells <= columns);
                }
                for (const auto& hit : state.hitTargets) {
                    ASSERT_TRUE(hit.viewportRow < rows);
                    ASSERT_TRUE(hit.viewportColumn < columns);
                    ASSERT_TRUE(hit.logicalLine < lines.size());
                    ASSERT_TRUE(hit.cell.value() <
                                lines[hit.logicalLine].totalCells);
                }
            }
        }
    }
}

TEST(invalidDimensionsAreActionable) {
    bool columnsThrew = false;
    try {
        (void)ViewportDimensions{0, 1};
    } catch (const std::invalid_argument&) {
        columnsThrew = true;
    }
    ASSERT_TRUE(columnsThrew);

    bool rowsThrew = false;
    try {
        (void)ViewportDimensions{1, 0};
    } catch (const std::invalid_argument&) {
        rowsThrew = true;
    }
    ASSERT_TRUE(rowsThrew);
}

TEST(listScrollViewClampsAndHidesThumbWhenContentFits) {
    // Shorter than the viewport: first pinned to 0, all items visible, no thumb.
    auto view = ssg::listScrollView(3, 10, 5, std::nullopt, false);
    ASSERT_EQ(view.firstVisible, std::uint32_t{0});
    ASSERT_EQ(view.visibleCount, std::uint32_t{3});
    ASSERT_EQ(view.scrollbar.maximumFirstRow, std::uint32_t{0});
    ASSERT_EQ(view.scrollbar.thumbStart, std::uint32_t{0});
    ASSERT_EQ(view.scrollbar, ssg::scrollbarMetrics(3, 10, 0));

    // Exactly full: still no scrolling room.
    auto full = ssg::listScrollView(10, 10, 4, std::nullopt, false);
    ASSERT_EQ(full.firstVisible, std::uint32_t{0});
    ASSERT_EQ(full.visibleCount, std::uint32_t{10});
    ASSERT_EQ(full.scrollbar.maximumFirstRow, std::uint32_t{0});
}

TEST(listScrollViewEmptyList) {
    auto view = ssg::listScrollView(0, 8, 3, std::nullopt, false);
    ASSERT_EQ(view.firstVisible, std::uint32_t{0});
    ASSERT_EQ(view.visibleCount, std::uint32_t{0});
    ASSERT_EQ(view.scrollbar, ssg::scrollbarMetrics(0, 8, 0));
}

TEST(listScrollViewZeroViewportIsInert) {
    auto view = ssg::listScrollView(20, 0, 5, std::optional<std::uint32_t>{7}, true);
    ASSERT_EQ(view.firstVisible, std::uint32_t{0});
    ASSERT_EQ(view.visibleCount, std::uint32_t{0});
    ASSERT_EQ(view.scrollbar, ssg::scrollbarMetrics(20, 0, 0));
}

TEST(listScrollViewClampsOverScrollToMaximum) {
    // 100 items, 10-tall window: maximum first is 90.  A larger request clamps.
    auto view = ssg::listScrollView(100, 10, 500, std::nullopt, false);
    ASSERT_EQ(view.scrollbar.maximumFirstRow, std::uint32_t{90});
    ASSERT_EQ(view.firstVisible, std::uint32_t{90});
    ASSERT_EQ(view.visibleCount, std::uint32_t{10});
    ASSERT_EQ(view.scrollbar, ssg::scrollbarMetrics(100, 10, 90));
}

TEST(listScrollViewFreeScrollIgnoresSelection) {
    // keep=false: the clamped request is honored even though the selection (0)
    // is far above the window, and even though a selection is present.
    auto view = ssg::listScrollView(100, 10, 40,
                                              std::optional<std::uint32_t>{0}, false);
    ASSERT_EQ(view.firstVisible, std::uint32_t{40});
    ASSERT_EQ(view.visibleCount, std::uint32_t{10});
    ASSERT_EQ(view.scrollbar, ssg::scrollbarMetrics(100, 10, 40));
}

TEST(listScrollViewKeepVisibleScrollsDownToSelection) {
    // Selection below the window forces the minimal downward shift so it lands
    // on the last visible row: first = selected - viewport + 1.
    auto view = ssg::listScrollView(100, 10, 0,
                                              std::optional<std::uint32_t>{25}, true);
    ASSERT_EQ(view.firstVisible, std::uint32_t{16});
    ASSERT_TRUE(25 >= view.firstVisible &&
                25 < view.firstVisible + view.visibleCount);
    ASSERT_EQ(view.scrollbar, ssg::scrollbarMetrics(100, 10, 16));
}

TEST(listScrollViewKeepVisibleScrollsUpToSelection) {
    // Selection above the window forces first = selected.
    auto view = ssg::listScrollView(100, 10, 50,
                                              std::optional<std::uint32_t>{12}, true);
    ASSERT_EQ(view.firstVisible, std::uint32_t{12});
    ASSERT_TRUE(12 >= view.firstVisible &&
                12 < view.firstVisible + view.visibleCount);
}

TEST(listScrollViewKeepVisibleLeavesInWindowSelectionUntouched) {
    // Selection already inside the window: no shift.
    auto view = ssg::listScrollView(100, 10, 20,
                                              std::optional<std::uint32_t>{25}, true);
    ASSERT_EQ(view.firstVisible, std::uint32_t{20});
}

TEST(listScrollViewKeepVisibleClampsSelectionToLastItem) {
    // An out-of-range selection is clamped to the last item, which still forces
    // a valid in-range window that never exceeds maximum_first_row.
    auto view = ssg::listScrollView(100, 10, 0,
                                              std::optional<std::uint32_t>{999}, true);
    ASSERT_EQ(view.firstVisible, std::uint32_t{90});
    ASSERT_EQ(view.scrollbar.maximumFirstRow, std::uint32_t{90});
}

TEST(listScrollViewMatchesComputeViewportMetrics) {
    // The generalized primitive must produce the same scrollbar metrics the
    // editor's compute_viewport already does for the same (total, viewport,
    // first) — no regression in the reused thumb math.
    std::vector<CellRun> lines;
    for (int i = 0; i < 40; ++i) lines.push_back(ssg::computeCellRun("line", 4));
    auto viewport = ssg::computeViewport(lines, ssg::ViewportDimensions{20, 10}, 7);
    auto list = ssg::listScrollView(
        viewport.totalVisualRows, 10, 7, std::nullopt, false);
    ASSERT_EQ(list.scrollbar, viewport.scrollbar);
    ASSERT_EQ(list.firstVisible, viewport.firstVisualRow);
}

// VP-1 (INV-projection-equivalence): for documents whose lines all fit the pane
// width, compute_viewport_unwrapped is field-for-field equal to the full path
// compute_viewport(active_cell_runs(text), ...).  Reference oracle over a
// generated corpus crossed with dimensions, first_row, and tab width.
TEST(unwrappedMatchesFullPathForFittingLines) {
    const std::vector<std::string> corpus = {
        "",                                        // empty document
        "abc",                                     // single line, no newline
        "abc\n",                                   // trailing newline
        "a\nbb\nccc\n\ndd",                        // short lines incl. an empty one
        std::string{"A\xE4\xB8\xAD" "B"},          // wide char (fits)
        std::string{"x\xCC\x81y"},                 // combining mark
        "ab\tcd",                                  // tab
        "l1\nl2\nl3\nl4\nl5\nl6\nl7",              // more lines than any tested rows
        "\n\n\n",                                  // only newlines
    };
    for (int tab : {2, 4, 8}) {
        for (const auto& doc : corpus) {
            const auto lines = cellRunsFromText(doc, tab);
            uint32_t maxwidth = 0;
            for (const auto& r : lines) {
                maxwidth = std::max(maxwidth, r.totalCells);
            }
            for (uint32_t columns = 1; columns <= 24; ++columns) {
                if (columns < maxwidth) continue;  // fitting-lines precondition
                for (uint32_t rows = 1; rows <= 6; ++rows) {
                    for (uint32_t first = 0; first <= 12; ++first) {
                        ViewportDimensions dims{columns, rows};
                        const auto full = ssg::computeViewport(lines, dims, first);
                        const auto proj = ssg::computeUnwrappedViewport(
                            doc, dims, first, 0, tab);
                        ASSERT_EQ(serialize(proj), serialize(full));
                        ASSERT_TRUE(proj == full);
                    }
                }
            }
        }
    }
}

// VP-1 (no-wrap clipping): a line wider than the pane is ONE visual row clipped
// to the width, not multiple wrapped rows; hit offsets are document-absolute.
TEST(unwrappedClipsLongLinesToOneRow) {
    const std::string doc = "abcdef\nxy";  // line 0 is 6 cells wide
    const ViewportDimensions dims{3, 2};
    const auto proj = ssg::computeUnwrappedViewport(doc, dims, 0, 0, 4);

    ASSERT_EQ(proj.totalVisualRows, 2u);  // two logical lines, NOT wrapped
    ASSERT_EQ(proj.visibleRows.size(), 2u);
    ASSERT_EQ(proj.visibleRows[0].logicalLine, 0u);
    // span_count is the VISIBLE (clipped) span count: 3 of the 6 fit the width.
    ASSERT_EQ(proj.visibleRows[0].spanCount, 3u);
    ASSERT_EQ(proj.visibleRows[0].contentCells, 6u);  // full line width
    ASSERT_EQ(proj.visibleRows[0].visibleCells, 3u);  // clipped to the width

    int row0Hits = 0;
    for (const auto& hit : proj.hitTargets) {
        if (hit.viewportRow == 0) ++row0Hits;
    }
    ASSERT_EQ(row0Hits, 3);  // only the visible cols 0,1,2 are hit targets

    // "xy" begins at document byte 7 (6 content bytes + one '\n').
    for (const auto& hit : proj.hitTargets) {
        if (hit.viewportRow == 1 && hit.viewportColumn == 0) {
            ASSERT_EQ(hit.byteOffset, 7u);
        }
    }
}

// VP-1: the same long-line document wraps into MORE rows on the full path and
// clips to fewer on the unwrapped path — the intended wrap-on vs wrap-off split.
TEST(unwrappedVsWrappedRowCountDiffersForLongLines) {
    const std::string doc = "abcdef\nxy";
    const ViewportDimensions dims{3, 8};
    const auto wrapped = ssg::computeViewport(cellRunsFromText(doc, 4), dims, 0);
    const auto proj = ssg::computeUnwrappedViewport(doc, dims, 0, 0, 4);
    ASSERT_EQ(proj.totalVisualRows, 2u);
    ASSERT_EQ(wrapped.totalVisualRows, 3u);  // "abcdef" -> 2 rows, "xy" -> 1
    ASSERT_TRUE(wrapped.totalVisualRows > proj.totalVisualRows);
}

// VP-1 (INV-scrollbar-total): total rows is the line count, and first_row clamps
// against it exactly as the full path clamps against its total.
TEST(unwrappedClampsFirstRowToLineCount) {
    const std::string doc = "a\nb\nc\nd\ne";  // 5 logical lines
    const ViewportDimensions dims{4, 2};
    const auto proj = ssg::computeUnwrappedViewport(doc, dims, 99, 0, 4);
    ASSERT_EQ(proj.totalVisualRows, 5u);
    ASSERT_EQ(proj.scrollbar.maximumFirstRow, 3u);  // 5 - 2
    ASSERT_EQ(proj.firstVisualRow, 3u);
    ASSERT_EQ(proj.visibleRows.size(), 2u);
    ASSERT_EQ(proj.visibleRows[0].logicalLine, 3u);
}

// VP-H (H0): a horizontal offset windows each row from that cell, snapping to a
// grapheme boundary, with document-absolute hit offsets.
TEST(unwrappedHorizontalOffsetWindowsEachRow) {
    const std::string doc = "abcdefghij\nkl";  // line 0 is 10 cells wide
    const ViewportDimensions dims{4, 2};
    // Scroll right by 3 cells: the row shows cells [3, 7) = "defg".
    const auto proj = ssg::computeUnwrappedViewport(doc, dims, 0, 3, 4);

    ASSERT_EQ(proj.firstVisualColumn, 3u);
    ASSERT_EQ(proj.totalVisualRows, 2u);
    ASSERT_EQ(proj.visibleRows[0].startCell.value(), 3u);
    ASSERT_EQ(proj.visibleRows[0].spanCount, 4u);   // d e f g
    ASSERT_EQ(proj.visibleRows[0].visibleCells, 4u);

    // Row 0 hit targets map viewport columns 0..3 to document bytes 3..6 ("defg").
    int checked = 0;
    for (const auto& hit : proj.hitTargets) {
        if (hit.viewportRow != 0) continue;
        ASSERT_EQ(hit.byteOffset, 3u + hit.viewportColumn);
        ASSERT_EQ(hit.cell.value(), 3u + hit.viewportColumn);
        ++checked;
    }
    ASSERT_EQ(checked, 4);

    // A short line (row 1 = "kl", 2 cells) scrolled past its end shows nothing.
    for (const auto& row : proj.visibleRows) {
        if (row.logicalLine == 1) ASSERT_EQ(row.visibleCells, 0u);
    }
}

// VP-H (H0): the offset snaps to a grapheme boundary — a wide cluster straddling
// the requested column scrolls fully off rather than splitting.
TEST(unwrappedHorizontalOffsetSnapsToGraphemeBoundary) {
    // "A" + U+4E2D (wide, 2 cells) + "B" -> cells: A@0, 中@1-2, B@3.
    const std::string doc = "A\xE4\xB8\xAD" "B";
    const ViewportDimensions dims{4, 1};
    // Requesting offset 2 lands inside the wide cluster (cells 1-2); the pane
    // offset is the requested value (2), while the row's own origin snaps to the
    // first span at/after cell 2 — "B" at cell 3 — recorded in start_cell.
    const auto proj = ssg::computeUnwrappedViewport(doc, dims, 0, 2, 4);
    ASSERT_EQ(proj.firstVisualColumn, 2u);
    ASSERT_EQ(proj.visibleRows[0].startCell.value(), 3u);
    ASSERT_EQ(proj.visibleRows[0].spanCount, 1u);  // just "B"
    ASSERT_EQ(proj.hitTargets.size(), std::size_t{1});
    ASSERT_EQ(proj.hitTargets[0].viewportColumn, 0u);
}

// CE-1 (M8 click-past-EOL): VisualRow.end_byte_offset is the document-absolute
// end of each visual row, over BOTH viewport builders. For the UNWRAPPED builder
// every visual row is a full logical line, so its end is the logical EOL (the
// newline byte, or text.size() for the last line) — and is INDEPENDENT of the
// horizontal offset. Verified against a hand-computed line-end table and
// resolve_document_position.
TEST(unwrappedEndByteOffsetIsTheTrueLineEnd) {
    // Lines: "ab"(0..2) | ""(3) blank | "cde"(4..7) | ""(8) trailing.
    const std::string doc = "ab\n\ncde\n";
    // Expected end offset per logical line: newline byte, or text.size() for the
    // final (trailing-newline) empty line.
    const std::array<std::uint32_t, 4> wantEnd{2, 3, 7, 8};
    for (uint32_t columns : {1u, 3u, 80u}) {          // incl. a clipping width
        for (uint32_t firstCol : {0u, 1u, 5u}) {      // incl. horizontal offset
            const auto proj = ssg::computeUnwrappedViewport(
                doc, ViewportDimensions{columns, 8}, 0, firstCol, 4);
            for (const auto& row : proj.visibleRows) {
                ASSERT_EQ(row.endByteOffset, wantEnd[row.logicalLine]);
                auto pos = ssg::resolveSelectionPosition(
                    doc, ssg::ByteOffset{row.endByteOffset});
                ASSERT_TRUE(pos.has_value());
                if (pos) {
                    ASSERT_EQ(pos->line.value(),
                              static_cast<std::uint64_t>(row.logicalLine));
                }
            }
        }
    }
}

// CE-1: for the WRAPPED builder, end_byte_offset is the VISUAL row's end — the
// wrap boundary for an interior row, the logical EOL for the final row of a line.
TEST(wrappedEndByteOffsetIsTheVisualRowEnd) {
    // "abcdef"(0..6) wraps at width 3 into "abc"(rel 0..3) + "def"(rel 3..6);
    // then "xy"(7..9). Document ends after "xy" with no trailing newline.
    const std::string doc = "abcdef\nxy";
    const auto lines = cellRunsFromText(doc, 4);
    const auto proj = ssg::computeViewport(lines, ViewportDimensions{3, 8}, 0);
    // Rows: 0="abc" end=3 (wrap boundary), 1="def" end=6 (logical EOL / newline),
    // 2="xy" end=9 (text.size()).
    ASSERT_EQ(proj.visibleRows.size(), std::size_t{3});
    ASSERT_EQ(proj.visibleRows[0].logicalLine, 0u);
    ASSERT_EQ(proj.visibleRows[0].endByteOffset, 3u);   // interior wrap boundary
    ASSERT_EQ(proj.visibleRows[1].logicalLine, 0u);
    ASSERT_EQ(proj.visibleRows[1].endByteOffset, 6u);   // logical EOL
    ASSERT_EQ(proj.visibleRows[2].logicalLine, 1u);
    ASSERT_EQ(proj.visibleRows[2].endByteOffset, 9u);   // last line, no newline
}

// The scrollbar mapping in isolation. The
// render (firstRow -> thumbStart) and drag-inverse (thumbTop -> firstRow) are one
// matched pair through scrollScaleRounded, so no gutter row is skipped and the
// ends map to the ends. Driven purely by the pure functions -- no runtime, no app.
TEST(scrollbarThumbAndFirstRowAreAnExactMatchedPair) {
    struct Regime { std::uint32_t total, viewport; };
    for (auto const& r : {Regime{200, 10}, Regime{1000, 30}, Regime{100, 20},
                          Regime{40, 20}, Regime{25, 20}, Regime{57, 13},
                          Regime{500, 50}, Regime{80, 79}}) {
        auto const metrics = ssg::scrollbarMetrics(r.total, r.viewport, 0);
        std::uint32_t const maximum = metrics.maximumFirstRow;
        std::uint32_t const travel = metrics.viewportRows - metrics.thumbSize;
        ASSERT_TRUE(maximum > 0);
        ASSERT_TRUE(travel > 0);

        // Endpoints: top cell shows the first line, bottom cell the last.
        ASSERT_EQ(ssg::scrollFirstRow(0, travel, maximum), std::uint32_t{0});
        ASSERT_EQ(ssg::scrollFirstRow(travel, travel, maximum), maximum);
        ASSERT_EQ(ssg::scrollThumbStart(0, maximum, travel), std::uint32_t{0});
        ASSERT_EQ(ssg::scrollThumbStart(maximum, maximum, travel), travel);

        // Surjective + monotone: every gutter row is reachable and renders back
        // exactly where it was dragged; firstRow never jumps backward.
        std::uint32_t previousFirstRow = 0;
        for (std::uint32_t thumbTop = 0; thumbTop <= travel; ++thumbTop) {
            std::uint32_t const firstRow =
                ssg::scrollFirstRow(thumbTop, travel, maximum);
            ASSERT_EQ(ssg::scrollThumbStart(firstRow, maximum, travel), thumbTop);
            ASSERT_TRUE(firstRow >= previousFirstRow);
            previousFirstRow = firstRow;
        }

        // The thumb always fits and never jumps backward as firstRow advances.
        std::uint32_t previousThumb = 0;
        for (std::uint32_t firstRow = 0; firstRow <= maximum; ++firstRow) {
            auto const m = ssg::scrollbarMetrics(r.total, r.viewport,
                                                            firstRow);
            ASSERT_TRUE(m.thumbStart + m.thumbSize <= r.viewport);
            ASSERT_TRUE(m.thumbStart >= previousThumb);
            previousThumb = m.thumbStart;
        }
    }
}

// The rounded fraction mapping is shared by every surface, not editor-only: a
// list scrollbar (tree/palette shape) resolves the same way through toFraction.
TEST(scrollToFractionRoundsUniformlyForListSurfaces) {
    // total=200 items, viewport=10 -> maximumFirstRow=190. A gutter drag that
    // grabs thumb row 1 of travel 9 sends numerator=1, denominator=9; rounded,
    // that is round(190*1/9)=21, and the top row (0/9) pins to 0.
    ssg::ScrollOffset top;
    top.toFraction(0, 9, 200, 10);
    ASSERT_EQ(top.firstVisible(), std::uint32_t{0});

    ssg::ScrollOffset bottom;
    bottom.toFraction(9, 9, 200, 10);
    ASSERT_EQ(bottom.firstVisible(), std::uint32_t{190});

    ssg::ScrollOffset one;
    one.toFraction(1, 9, 200, 10);
    ASSERT_EQ(one.firstVisible(), ssg::scrollFirstRow(1, 9, 190));
    ASSERT_EQ(one.firstVisible(), std::uint32_t{21});
}

// Degenerate: a thumb that fills the gutter (travel 0) or a document that fits
// never scrolls and never divides by zero.
TEST(scrollMappingIsInertWhenNothingScrolls) {
    ASSERT_EQ(ssg::scrollFirstRow(3, 0, 0), std::uint32_t{0});
    ASSERT_EQ(ssg::scrollThumbStart(0, 0, 0), std::uint32_t{0});
    ASSERT_EQ(ssg::scrollScaleRounded(5, 3, 0), std::uint32_t{0});
    ssg::ScrollOffset still;
    still.toFraction(0, 1, 5, 20);  // total 5 < viewport 20: nothing scrolls
    ASSERT_EQ(still.firstVisible(), std::uint32_t{0});
}

// Lever 2 (visible-line cache). A LineLayoutCache hit must be byte-identical to a
// fresh computeCellRun -- the cache key is the exact (text, tabWidth)
// so it needs no semantic invalidation. Covers tabs, wide graphemes, and
// combining sequences, and the (text, tabWidth) key discrimination.
TEST(cachedLineLayoutEqualsFreshComputeRun) {
    auto sameRun = [](const ssg::CellRun& a, const ssg::CellRun& b) {
        if (a.totalCells != b.totalCells) return false;
        if (a.spans.size() != b.spans.size()) return false;
        for (std::size_t i = 0; i < a.spans.size(); ++i) {
            if (a.spans[i].byteOffset != b.spans[i].byteOffset ||
                a.spans[i].byteLen != b.spans[i].byteLen ||
                a.spans[i].cellWidth != b.spans[i].cellWidth ||
                a.spans[i].kind != b.spans[i].kind) {
                return false;
            }
        }
        return true;
    };
    ssg::LineLayoutCache cache;
    const std::vector<std::string> lines = {
        "plain ascii",
        "with\ttab\tstops",
        "wide \xE6\x97\xA5\xE6\x9C\xAC chars",       // CJK (2-cell)
        "emoji \xF0\x9F\x98\x80 here",               // U+1F600
        "combining a\xCC\x81 e\xCC\x80",             // a + acute, e + grave
        "",
    };
    for (const auto& line : lines) {
        const auto fresh = ssg::computeCellRun(line, 4);
        // First call (miss) and a second (hit) must both equal the fresh run.
        ASSERT_TRUE(sameRun(cache.run(line, 4), fresh));
        ASSERT_TRUE(sameRun(cache.run(line, 4), fresh));
    }
    // The tab width is part of the key: the same text at a different width is a
    // distinct entry equal to its own fresh run, and the two coexist.
    ssg::LineLayoutCache widthCache;
    ASSERT_TRUE(sameRun(widthCache.run("a\tb", 8),
                        ssg::computeCellRun("a\tb", 8)));
    ASSERT_TRUE(sameRun(widthCache.run("a\tb", 2),
                        ssg::computeCellRun("a\tb", 2)));
    ASSERT_EQ(widthCache.size(), std::size_t{2});

    // Bounded: a capacity-2 cache never holds more than 2 entries.
    ssg::LineLayoutCache tiny{2};
    (void)tiny.run("one", 4);
    (void)tiny.run("two", 4);
    (void)tiny.run("three", 4);
    ASSERT_EQ(tiny.size(), std::size_t{2});
}

// Lever 2: the word-wrap-OFF viewport path re-shapes the same visible lines every
// frame; with a borrowed cache, an identical projection re-segments nothing.
TEST(unwrappedProjectionReusesCachedVisibleLines) {
    std::string doc;
    for (int i = 0; i < 40; ++i) doc += "line " + std::to_string(i) + " text\n";
    ssg::ViewportDimensions const dims{80, 24};
    ssg::LineLayoutCache cache;
    (void)ssg::computeUnwrappedViewport(doc, dims, 0, 0, 4, nullptr,
                                           std::nullopt, &cache);  // warm
    ssg::resetCellRunCalls();
    (void)ssg::computeUnwrappedViewport(doc, dims, 0, 0, 4, nullptr,
                                           std::nullopt, &cache);
    ASSERT_EQ(ssg::cellRunCalls(), std::uint64_t{0});
    // Without a cache the same projection re-segments the visible lines.
    ssg::resetCellRunCalls();
    (void)ssg::computeUnwrappedViewport(doc, dims, 0, 0, 4);
    ASSERT_TRUE(ssg::cellRunCalls() > 0);
}

SSG_TEST_SUITE(test_viewport) {
    RUN(emptyViewportGolden);
    RUN(shortViewportGolden);
    RUN(wideBoundaryGolden);
    RUN(wrappedScrolledGolden);
    RUN(tinyClippedGolden);
    RUN(noDiffProjectionIsIdentity);
    RUN(removedRowsAreProjectedAtTargetAndCountedByScrollbar);
    RUN(leadingTrailingAndWrappedPhantomBlocksUseTheSameProjection);
    RUN(unwrappedProjectionStaysAlignedAcrossEmptyAndScrolledOffRows);
    RUN(scrollSaturatesAndDeltaSuppressesEqualPayload);
    RUN(unwrappedMatchesFullPathForFittingLines);
    RUN(unwrappedClipsLongLinesToOneRow);
    RUN(unwrappedVsWrappedRowCountDiffersForLongLines);
    RUN(unwrappedClampsFirstRowToLineCount);
    RUN(unwrappedEndByteOffsetIsTheTrueLineEnd);
    RUN(wrappedEndByteOffsetIsTheVisualRowEnd);
    RUN(unwrappedHorizontalOffsetWindowsEachRow);
    RUN(unwrappedHorizontalOffsetSnapsToGraphemeBoundary);
    RUN(viewportBoundsProperties);
    RUN(invalidDimensionsAreActionable);
    RUN(listScrollViewClampsAndHidesThumbWhenContentFits);
    RUN(listScrollViewEmptyList);
    RUN(listScrollViewZeroViewportIsInert);
    RUN(listScrollViewClampsOverScrollToMaximum);
    RUN(listScrollViewFreeScrollIgnoresSelection);
    RUN(listScrollViewKeepVisibleScrollsDownToSelection);
    RUN(listScrollViewKeepVisibleScrollsUpToSelection);
    RUN(listScrollViewKeepVisibleLeavesInWindowSelectionUntouched);
    RUN(listScrollViewKeepVisibleClampsSelectionToLastItem);
    RUN(listScrollViewMatchesComputeViewportMetrics);
    RUN(scrollbarThumbAndFirstRowAreAnExactMatchedPair);
    RUN(scrollToFractionRoundsUniformlyForListSurfaces);
    RUN(scrollMappingIsInertWhenNothingScrolls);
    RUN(cachedLineLayoutEqualsFreshComputeRun);
    RUN(unwrappedProjectionReusesCachedVisibleLines);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
