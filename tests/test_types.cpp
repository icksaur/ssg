// Oracle: tests/test_types.cpp
//
// Compile-only consumer via the public include path verifies that
// include/ssg/types.h and include/ssg/config.h are self-contained and
// well-formed without requiring any internal headers.
//
// Tests: valid construction/accessor round trips, invalid-construction
// failures (std::invalid_argument), equality and ordering, and scoped-enum
// values.  The test executable itself is the CMake fixture that proves the
// foundation-types component manifest is discovered, built, and registered
// without editing CMakeLists.txt.

#include <ssg/config.h>
#include <ssg/types.h>

#include "test_helpers.h"

#include <stdexcept>

// ---------------------------------------------------------------------------
// Revision

TEST(revision_default_is_zero) {
    ssg::Revision r;
    ASSERT_EQ(r.value(), uint64_t{0});
}

TEST(revision_round_trip) {
    ssg::Revision r{42};
    ASSERT_EQ(r.value(), uint64_t{42});
}

TEST(revision_equality) {
    ASSERT_TRUE(ssg::Revision{1} == ssg::Revision{1});
    ASSERT_TRUE(ssg::Revision{1} != ssg::Revision{2});
}

TEST(revision_ordering) {
    ASSERT_TRUE(ssg::Revision{1} < ssg::Revision{2});
    ASSERT_TRUE(ssg::Revision{2} > ssg::Revision{1});
    ASSERT_TRUE(ssg::Revision{1} <= ssg::Revision{1});
}

// ---------------------------------------------------------------------------
// ByteOffset

TEST(byte_offset_default_is_zero) {
    ssg::ByteOffset o;
    ASSERT_EQ(o.value(), uint64_t{0});
}

TEST(byte_offset_round_trip) {
    ssg::ByteOffset o{100};
    ASSERT_EQ(o.value(), uint64_t{100});
}

TEST(byte_offset_ordering) {
    ASSERT_TRUE(ssg::ByteOffset{5} < ssg::ByteOffset{10});
    ASSERT_TRUE(ssg::ByteOffset{10} > ssg::ByteOffset{5});
}

// ---------------------------------------------------------------------------
// LineIndex

TEST(line_index_round_trip) {
    ssg::LineIndex l{5};
    ASSERT_EQ(l.value(), uint64_t{5});
}

TEST(line_index_zero_based) {
    ssg::LineIndex l{0};
    ASSERT_EQ(l.value(), uint64_t{0});
}

// ---------------------------------------------------------------------------
// CellIndex

TEST(cell_index_round_trip) {
    ssg::CellIndex c{3};
    ASSERT_EQ(c.value(), uint64_t{3});
}

// ---------------------------------------------------------------------------
// DocumentPosition

TEST(document_position_round_trip) {
    ssg::DocumentPosition pos{
        .byte_offset = ssg::ByteOffset{10},
        .line        = ssg::LineIndex{1},
        .cell        = ssg::CellIndex{2},
    };
    ASSERT_EQ(pos.byte_offset.value(), uint64_t{10});
    ASSERT_EQ(pos.line.value(), uint64_t{1});
    ASSERT_EQ(pos.cell.value(), uint64_t{2});
}

TEST(document_position_equality) {
    ssg::DocumentPosition a{ssg::ByteOffset{0}, ssg::LineIndex{0}, ssg::CellIndex{0}};
    ssg::DocumentPosition b{ssg::ByteOffset{0}, ssg::LineIndex{0}, ssg::CellIndex{0}};
    ASSERT_TRUE(a == b);
    ssg::DocumentPosition c{ssg::ByteOffset{1}, ssg::LineIndex{0}, ssg::CellIndex{0}};
    ASSERT_TRUE(a != c);
}

// ---------------------------------------------------------------------------
// DocumentMode (scoped enum — no operator<<; ASSERT_EQ must not stream values)

TEST(document_mode_distinct_values) {
    ASSERT_TRUE(ssg::DocumentMode::Edit      != ssg::DocumentMode::ReadOnly);
    ASSERT_TRUE(ssg::DocumentMode::ReadOnly != ssg::DocumentMode::Diff);
    ASSERT_TRUE(ssg::DocumentMode::Edit      != ssg::DocumentMode::Diff);
}

TEST(document_mode_equality) {
    ASSERT_EQ(ssg::DocumentMode::Edit, ssg::DocumentMode::Edit);
}

// ---------------------------------------------------------------------------
// TabWidth

TEST(tab_width_valid_boundary_low) {
    ASSERT_NO_THROW(ssg::TabWidth{1});
}

TEST(tab_width_valid_boundary_high) {
    ASSERT_NO_THROW(ssg::TabWidth{16});
}

TEST(tab_width_valid_typical) {
    ASSERT_NO_THROW(ssg::TabWidth{4});
    ASSERT_NO_THROW(ssg::TabWidth{8});
}

TEST(tab_width_round_trip) {
    ssg::TabWidth w{4};
    ASSERT_EQ(w.value(), 4);
}

TEST(tab_width_invalid_zero) {
    ASSERT_THROWS(ssg::TabWidth{0}, std::invalid_argument);
}

TEST(tab_width_invalid_too_large) {
    ASSERT_THROWS(ssg::TabWidth{17}, std::invalid_argument);
}

TEST(tab_width_invalid_negative) {
    ASSERT_THROWS(ssg::TabWidth{-1}, std::invalid_argument);
}

TEST(tab_width_equality) {
    ssg::TabWidth a{4};
    ssg::TabWidth b{4};
    ssg::TabWidth c{2};
    ASSERT_TRUE(a == b);
    ASSERT_TRUE(a != c);
}

// ---------------------------------------------------------------------------
// HistoryConfig

TEST(history_config_defaults) {
    auto cfg = ssg::HistoryConfig::defaults();
    ASSERT_EQ(cfg.byte_budget, uint64_t{16u * 1024u * 1024u});
    ASSERT_EQ(cfg.coalesce_ms, uint32_t{750});
}

TEST(history_config_round_trip) {
    ssg::HistoryConfig cfg;
    cfg.byte_budget = 1024;
    cfg.coalesce_ms = 500;
    ASSERT_EQ(cfg.byte_budget, uint64_t{1024});
    ASSERT_EQ(cfg.coalesce_ms, uint32_t{500});
}

TEST(history_config_equality) {
    ssg::HistoryConfig a;
    ssg::HistoryConfig b;
    ASSERT_TRUE(a == b);
    b.byte_budget = 1;
    ASSERT_TRUE(a != b);
}

// ---------------------------------------------------------------------------
// IndentConfig

TEST(indent_config_defaults) {
    ssg::IndentConfig cfg;
    ASSERT_EQ(cfg.style, ssg::IndentStyle::Spaces);
    ASSERT_EQ(cfg.width.value(), 4);
    ASSERT_TRUE(cfg.auto_detect);
}

TEST(indent_config_custom_tabs) {
    ssg::IndentConfig cfg{
        .style       = ssg::IndentStyle::Tabs,
        .width       = ssg::TabWidth{2},
        .auto_detect = false,
    };
    ASSERT_EQ(cfg.style, ssg::IndentStyle::Tabs);
    ASSERT_EQ(cfg.width.value(), 2);
    ASSERT_FALSE(cfg.auto_detect);
}

TEST(indent_style_distinct_values) {
    ASSERT_TRUE(ssg::IndentStyle::Spaces != ssg::IndentStyle::Tabs);
}

// ---------------------------------------------------------------------------
// LineEnding (scoped enum)

TEST(line_ending_distinct_values) {
    ASSERT_TRUE(ssg::LineEnding::Lf   != ssg::LineEnding::Crlf);
    ASSERT_TRUE(ssg::LineEnding::Crlf != ssg::LineEnding::Cr);
    ASSERT_TRUE(ssg::LineEnding::Cr   != ssg::LineEnding::Mixed);
}

// ---------------------------------------------------------------------------

int main() {
    std::cout << "=== SSG types tests ===" << "\n";

    RUN(revision_default_is_zero);
    RUN(revision_round_trip);
    RUN(revision_equality);
    RUN(revision_ordering);

    RUN(byte_offset_default_is_zero);
    RUN(byte_offset_round_trip);
    RUN(byte_offset_ordering);

    RUN(line_index_round_trip);
    RUN(line_index_zero_based);

    RUN(cell_index_round_trip);

    RUN(document_position_round_trip);
    RUN(document_position_equality);

    RUN(document_mode_distinct_values);
    RUN(document_mode_equality);

    RUN(tab_width_valid_boundary_low);
    RUN(tab_width_valid_boundary_high);
    RUN(tab_width_valid_typical);
    RUN(tab_width_round_trip);
    RUN(tab_width_invalid_zero);
    RUN(tab_width_invalid_too_large);
    RUN(tab_width_invalid_negative);
    RUN(tab_width_equality);

    RUN(history_config_defaults);
    RUN(history_config_round_trip);
    RUN(history_config_equality);

    RUN(indent_config_defaults);
    RUN(indent_config_custom_tabs);
    RUN(indent_style_distinct_values);

    RUN(line_ending_distinct_values);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
