#include <ssg/layout.h>
#include <ssg/viewport.h>

#include "test_helpers.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef VIEWPORT_FIXTURE_DIR
#error "VIEWPORT_FIXTURE_DIR must name the hand-authored viewport fixtures"
#endif

namespace {

using ssg::CellRun;
using ssg::ViewportDimensions;
using ssg::ViewportViewState;

std::vector<CellRun> runs(std::initializer_list<std::string_view> lines) {
    std::vector<CellRun> result;
    result.reserve(lines.size());
    for (const auto line : lines) {
        result.push_back(ssg::compute_cell_run(line));
    }
    return result;
}

std::string serialize(const ViewportViewState& state) {
    std::ostringstream out;
    out << "offset=" << state.first_visual_row
        << " total=" << state.total_visual_rows << '\n';
    const auto& bar = state.scrollbar;
    out << "scroll=" << bar.total_rows << ',' << bar.viewport_rows << ','
        << bar.first_row << ',' << bar.maximum_first_row << ','
        << bar.thumb_start << ',' << bar.thumb_size << '\n';
    for (std::size_t i = 0; i < state.visible_rows.size(); ++i) {
        const auto& row = state.visible_rows[i];
        out << "row=" << i << ',' << row.logical_line << ','
            << row.first_span << ',' << row.span_count << ','
            << row.start_cell.value() << ',' << row.content_cells << ','
            << row.visible_cells << '\n';
    }
    for (const auto& hit : state.hit_targets) {
        out << "hit=" << hit.viewport_row << ',' << hit.viewport_column << ','
            << hit.logical_line << ',' << hit.cell.value() << ','
            << hit.byte_offset << ',' << hit.byte_len << '\n';
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

void assert_golden(std::string_view name,
                   const std::vector<CellRun>& lines,
                   ViewportDimensions dimensions,
                   uint32_t first_row) {
    ASSERT_EQ(serialize(ssg::compute_viewport(lines, dimensions, first_row)),
              fixture(name));
}

}  // namespace

TEST(empty_viewport_golden) {
    assert_golden("empty.txt", {}, ViewportDimensions{4, 2}, 0);
}

TEST(short_viewport_golden) {
    assert_golden("short.txt", runs({"abc"}), ViewportDimensions{5, 2}, 0);
}

TEST(wide_boundary_golden) {
    assert_golden("wide.txt", runs({"A\xE4\xB8\xAD" "B"}),
                  ViewportDimensions{3, 2}, 0);
}

TEST(wrapped_scrolled_golden) {
    assert_golden("wrapped.txt", runs({"abcdef", "xy", "12345"}),
                  ViewportDimensions{3, 2}, 2);
}

TEST(tiny_clipped_golden) {
    assert_golden("tiny.txt", runs({"\xE4\xB8\xAD" "a"}),
                  ViewportDimensions{1, 1}, 0);
}

TEST(scroll_saturates_and_delta_suppresses_equal_payload) {
    const auto lines = runs({"abcdef", "xy"});
    const auto initial =
        ssg::compute_viewport(lines, ViewportDimensions{2, 2}, 0);
    const auto bottom =
        ssg::scroll_viewport_by(lines, ViewportDimensions{2, 2}, 0, 99);
    ASSERT_EQ(bottom.first_visual_row, bottom.scrollbar.maximum_first_row);
    const auto top = ssg::scroll_viewport_by(
        lines, ViewportDimensions{2, 2}, bottom.first_visual_row, -99);
    ASSERT_EQ(top.first_visual_row, 0u);

    const auto same = ssg::derive_viewport_delta(initial, initial);
    ASSERT_FALSE(same.changed);
    ASSERT_FALSE(same.replacement.has_value());
    const auto changed = ssg::derive_viewport_delta(initial, bottom);
    ASSERT_TRUE(changed.changed);
    ASSERT_TRUE(changed.replacement.has_value());
    ASSERT_EQ(*changed.replacement, bottom);
}

TEST(viewport_bounds_properties) {
    const auto lines =
        runs({"", "abcdef", "A\xE4\xB8\xAD" "B", "\tZ",
              "\xCC\x81" "x"});
    for (uint32_t columns = 1; columns <= 8; ++columns) {
        for (uint32_t rows = 1; rows <= 5; ++rows) {
            for (uint32_t requested = 0; requested <= 40; ++requested) {
                const auto state = ssg::compute_viewport(
                    lines, ViewportDimensions{columns, rows}, requested);
                ASSERT_TRUE(state.first_visual_row <=
                            state.scrollbar.maximum_first_row);
                ASSERT_TRUE(state.visible_rows.size() <= rows);
                ASSERT_TRUE(state.scrollbar.thumb_size >= 1);
                ASSERT_TRUE(state.scrollbar.thumb_start +
                                state.scrollbar.thumb_size <=
                            rows);
                for (const auto& row : state.visible_rows) {
                    ASSERT_TRUE(row.logical_line < lines.size());
                    ASSERT_TRUE(row.visible_cells <= columns);
                }
                for (const auto& hit : state.hit_targets) {
                    ASSERT_TRUE(hit.viewport_row < rows);
                    ASSERT_TRUE(hit.viewport_column < columns);
                    ASSERT_TRUE(hit.logical_line < lines.size());
                    ASSERT_TRUE(hit.cell.value() <
                                lines[hit.logical_line].total_cells);
                }
            }
        }
    }
}

TEST(invalid_dimensions_are_actionable) {
    bool columns_threw = false;
    try {
        (void)ViewportDimensions{0, 1};
    } catch (const std::invalid_argument&) {
        columns_threw = true;
    }
    ASSERT_TRUE(columns_threw);

    bool rows_threw = false;
    try {
        (void)ViewportDimensions{1, 0};
    } catch (const std::invalid_argument&) {
        rows_threw = true;
    }
    ASSERT_TRUE(rows_threw);
}

TEST(list_scroll_view_clamps_and_hides_thumb_when_content_fits) {
    // Shorter than the viewport: first pinned to 0, all items visible, no thumb.
    auto view = ssg::compute_list_scroll_view(3, 10, 5, std::nullopt, false);
    ASSERT_EQ(view.first_visible, std::uint32_t{0});
    ASSERT_EQ(view.visible_count, std::uint32_t{3});
    ASSERT_EQ(view.scrollbar.maximum_first_row, std::uint32_t{0});
    ASSERT_EQ(view.scrollbar.thumb_start, std::uint32_t{0});
    ASSERT_EQ(view.scrollbar, ssg::scrollbar_metrics(3, 10, 0));

    // Exactly full: still no scrolling room.
    auto full = ssg::compute_list_scroll_view(10, 10, 4, std::nullopt, false);
    ASSERT_EQ(full.first_visible, std::uint32_t{0});
    ASSERT_EQ(full.visible_count, std::uint32_t{10});
    ASSERT_EQ(full.scrollbar.maximum_first_row, std::uint32_t{0});
}

TEST(list_scroll_view_empty_list) {
    auto view = ssg::compute_list_scroll_view(0, 8, 3, std::nullopt, false);
    ASSERT_EQ(view.first_visible, std::uint32_t{0});
    ASSERT_EQ(view.visible_count, std::uint32_t{0});
    ASSERT_EQ(view.scrollbar, ssg::scrollbar_metrics(0, 8, 0));
}

TEST(list_scroll_view_zero_viewport_is_inert) {
    auto view = ssg::compute_list_scroll_view(20, 0, 5, std::optional<std::uint32_t>{7}, true);
    ASSERT_EQ(view.first_visible, std::uint32_t{0});
    ASSERT_EQ(view.visible_count, std::uint32_t{0});
    ASSERT_EQ(view.scrollbar, ssg::scrollbar_metrics(20, 0, 0));
}

TEST(list_scroll_view_clamps_over_scroll_to_maximum) {
    // 100 items, 10-tall window: maximum first is 90.  A larger request clamps.
    auto view = ssg::compute_list_scroll_view(100, 10, 500, std::nullopt, false);
    ASSERT_EQ(view.scrollbar.maximum_first_row, std::uint32_t{90});
    ASSERT_EQ(view.first_visible, std::uint32_t{90});
    ASSERT_EQ(view.visible_count, std::uint32_t{10});
    ASSERT_EQ(view.scrollbar, ssg::scrollbar_metrics(100, 10, 90));
}

TEST(list_scroll_view_free_scroll_ignores_selection) {
    // keep=false: the clamped request is honored even though the selection (0)
    // is far above the window, and even though a selection is present.
    auto view = ssg::compute_list_scroll_view(100, 10, 40,
                                              std::optional<std::uint32_t>{0}, false);
    ASSERT_EQ(view.first_visible, std::uint32_t{40});
    ASSERT_EQ(view.visible_count, std::uint32_t{10});
    ASSERT_EQ(view.scrollbar, ssg::scrollbar_metrics(100, 10, 40));
}

TEST(list_scroll_view_keep_visible_scrolls_down_to_selection) {
    // Selection below the window forces the minimal downward shift so it lands
    // on the last visible row: first = selected - viewport + 1.
    auto view = ssg::compute_list_scroll_view(100, 10, 0,
                                              std::optional<std::uint32_t>{25}, true);
    ASSERT_EQ(view.first_visible, std::uint32_t{16});
    ASSERT_TRUE(25 >= view.first_visible &&
                25 < view.first_visible + view.visible_count);
    ASSERT_EQ(view.scrollbar, ssg::scrollbar_metrics(100, 10, 16));
}

TEST(list_scroll_view_keep_visible_scrolls_up_to_selection) {
    // Selection above the window forces first = selected.
    auto view = ssg::compute_list_scroll_view(100, 10, 50,
                                              std::optional<std::uint32_t>{12}, true);
    ASSERT_EQ(view.first_visible, std::uint32_t{12});
    ASSERT_TRUE(12 >= view.first_visible &&
                12 < view.first_visible + view.visible_count);
}

TEST(list_scroll_view_keep_visible_leaves_in_window_selection_untouched) {
    // Selection already inside the window: no shift.
    auto view = ssg::compute_list_scroll_view(100, 10, 20,
                                              std::optional<std::uint32_t>{25}, true);
    ASSERT_EQ(view.first_visible, std::uint32_t{20});
}

TEST(list_scroll_view_keep_visible_clamps_selection_to_last_item) {
    // An out-of-range selection is clamped to the last item, which still forces
    // a valid in-range window that never exceeds maximum_first_row.
    auto view = ssg::compute_list_scroll_view(100, 10, 0,
                                              std::optional<std::uint32_t>{999}, true);
    ASSERT_EQ(view.first_visible, std::uint32_t{90});
    ASSERT_EQ(view.scrollbar.maximum_first_row, std::uint32_t{90});
}

TEST(list_scroll_view_matches_compute_viewport_metrics) {
    // The generalized primitive must produce the same scrollbar metrics the
    // editor's compute_viewport already does for the same (total, viewport,
    // first) — no regression in the reused thumb math.
    std::vector<CellRun> lines;
    for (int i = 0; i < 40; ++i) lines.push_back(ssg::compute_cell_run("line", 4));
    auto viewport = ssg::compute_viewport(lines, ssg::ViewportDimensions{20, 10}, 7);
    auto list = ssg::compute_list_scroll_view(
        viewport.total_visual_rows, 10, 7, std::nullopt, false);
    ASSERT_EQ(list.scrollbar, viewport.scrollbar);
    ASSERT_EQ(list.first_visible, viewport.first_visual_row);
}

int main() {
    RUN(empty_viewport_golden);
    RUN(short_viewport_golden);
    RUN(wide_boundary_golden);
    RUN(wrapped_scrolled_golden);
    RUN(tiny_clipped_golden);
    RUN(scroll_saturates_and_delta_suppresses_equal_payload);
    RUN(viewport_bounds_properties);
    RUN(invalid_dimensions_are_actionable);
    RUN(list_scroll_view_clamps_and_hides_thumb_when_content_fits);
    RUN(list_scroll_view_empty_list);
    RUN(list_scroll_view_zero_viewport_is_inert);
    RUN(list_scroll_view_clamps_over_scroll_to_maximum);
    RUN(list_scroll_view_free_scroll_ignores_selection);
    RUN(list_scroll_view_keep_visible_scrolls_down_to_selection);
    RUN(list_scroll_view_keep_visible_scrolls_up_to_selection);
    RUN(list_scroll_view_keep_visible_leaves_in_window_selection_untouched);
    RUN(list_scroll_view_keep_visible_clamps_selection_to_last_item);
    RUN(list_scroll_view_matches_compute_viewport_metrics);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
