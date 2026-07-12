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

int main() {
    RUN(empty_viewport_golden);
    RUN(short_viewport_golden);
    RUN(wide_boundary_golden);
    RUN(wrapped_scrolled_golden);
    RUN(tiny_clipped_golden);
    RUN(scroll_saturates_and_delta_suppresses_equal_payload);
    RUN(viewport_bounds_properties);
    RUN(invalid_dimensions_are_actionable);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
