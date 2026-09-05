// Oracle: tests/test_types.cpp
//
// Compile-only consumer via the public include path verifies that
// include/ssg/types.h and include/ssg/Settings.h are self-contained and
// well-formed without requiring any internal headers.
//
// Tests: valid construction/accessor round trips, invalid-construction
// failures (std::invalid_argument), equality and ordering, and scoped-enum
// values.

#include <ssg/Settings.h>
#include <ssg/types.h>

#include "test_helpers.h"

#include <stdexcept>

// ---------------------------------------------------------------------------
// ByteOffset

TEST(byteOffsetDefaultIsZero) {
    ssg::ByteOffset o;
    ASSERT_EQ(o.value(), uint64_t{0});
}

TEST(byteOffsetRoundTrip) {
    ssg::ByteOffset o{100};
    ASSERT_EQ(o.value(), uint64_t{100});
}

TEST(byteOffsetOrdering) {
    ASSERT_TRUE(ssg::ByteOffset{5} < ssg::ByteOffset{10});
    ASSERT_TRUE(ssg::ByteOffset{10} > ssg::ByteOffset{5});
}

// ---------------------------------------------------------------------------
// LineIndex

TEST(lineIndexRoundTrip) {
    ssg::LineIndex l{5};
    ASSERT_EQ(l.value(), uint64_t{5});
}

TEST(lineIndexZeroBased) {
    ssg::LineIndex l{0};
    ASSERT_EQ(l.value(), uint64_t{0});
}

// ---------------------------------------------------------------------------
// CellIndex

TEST(cellIndexRoundTrip) {
    ssg::CellIndex c{3};
    ASSERT_EQ(c.value(), uint64_t{3});
}

// ---------------------------------------------------------------------------
// DocumentPosition

TEST(documentPositionRoundTrip) {
    ssg::DocumentPosition pos{
        .byteOffset = ssg::ByteOffset{10},
        .line        = ssg::LineIndex{1},
        .cell        = ssg::CellIndex{2},
    };
    ASSERT_EQ(pos.byteOffset.value(), uint64_t{10});
    ASSERT_EQ(pos.line.value(), uint64_t{1});
    ASSERT_EQ(pos.cell.value(), uint64_t{2});
}

TEST(documentPositionEquality) {
    ssg::DocumentPosition a{ssg::ByteOffset{0}, ssg::LineIndex{0}, ssg::CellIndex{0}};
    ssg::DocumentPosition b{ssg::ByteOffset{0}, ssg::LineIndex{0}, ssg::CellIndex{0}};
    ASSERT_TRUE(a == b);
    ssg::DocumentPosition c{ssg::ByteOffset{1}, ssg::LineIndex{0}, ssg::CellIndex{0}};
    ASSERT_TRUE(a != c);
}

// ---------------------------------------------------------------------------
// DocumentMode (scoped enum — no operator<<; ASSERT_EQ must not stream values)

TEST(documentModeDistinctValues) {
    ASSERT_TRUE(ssg::DocumentMode::Edit      != ssg::DocumentMode::ReadOnly);
    ASSERT_TRUE(ssg::DocumentMode::ReadOnly != ssg::DocumentMode::Diff);
    ASSERT_TRUE(ssg::DocumentMode::Edit      != ssg::DocumentMode::Diff);
}

TEST(documentModeEquality) {
    ASSERT_EQ(ssg::DocumentMode::Edit, ssg::DocumentMode::Edit);
}

// ---------------------------------------------------------------------------
// TabWidth

TEST(tabWidthValidBoundaryLow) {
    ASSERT_NO_THROW(ssg::TabWidth{1});
}

TEST(tabWidthValidBoundaryHigh) {
    ASSERT_NO_THROW(ssg::TabWidth{16});
}

TEST(tabWidthValidTypical) {
    ASSERT_NO_THROW(ssg::TabWidth{4});
    ASSERT_NO_THROW(ssg::TabWidth{8});
}

TEST(tabWidthRoundTrip) {
    ssg::TabWidth w{4};
    ASSERT_EQ(w.value(), 4);
}

TEST(tabWidthInvalidZero) {
    ASSERT_THROWS(ssg::TabWidth{0}, std::invalid_argument);
}

TEST(tabWidthInvalidTooLarge) {
    ASSERT_THROWS(ssg::TabWidth{17}, std::invalid_argument);
}

TEST(tabWidthInvalidNegative) {
    ASSERT_THROWS(ssg::TabWidth{-1}, std::invalid_argument);
}

TEST(tabWidthEquality) {
    ssg::TabWidth a{4};
    ssg::TabWidth b{4};
    ssg::TabWidth c{2};
    ASSERT_TRUE(a == b);
    ASSERT_TRUE(a != c);
}

TEST(indentStyleDistinctValues) {
    ASSERT_TRUE(ssg::IndentStyle::Spaces != ssg::IndentStyle::Tabs);
}

// ---------------------------------------------------------------------------
// LineEnding (scoped enum)

TEST(lineEndingDistinctValues) {
    ASSERT_TRUE(ssg::LineEnding::Lf   != ssg::LineEnding::Crlf);
    ASSERT_TRUE(ssg::LineEnding::Crlf != ssg::LineEnding::Cr);
    ASSERT_TRUE(ssg::LineEnding::Cr   != ssg::LineEnding::Mixed);
}

// ---------------------------------------------------------------------------

SSG_TEST_SUITE(test_types) {
    std::cout << "=== SSG types tests ===" << "\n";


    RUN(byteOffsetDefaultIsZero);
    RUN(byteOffsetRoundTrip);
    RUN(byteOffsetOrdering);

    RUN(lineIndexRoundTrip);
    RUN(lineIndexZeroBased);

    RUN(cellIndexRoundTrip);

    RUN(documentPositionRoundTrip);
    RUN(documentPositionEquality);

    RUN(documentModeDistinctValues);
    RUN(documentModeEquality);

    RUN(tabWidthValidBoundaryLow);
    RUN(tabWidthValidBoundaryHigh);
    RUN(tabWidthValidTypical);
    RUN(tabWidthRoundTrip);
    RUN(tabWidthInvalidZero);
    RUN(tabWidthInvalidTooLarge);
    RUN(tabWidthInvalidNegative);
    RUN(tabWidthEquality);


    RUN(indentStyleDistinctValues);

    RUN(lineEndingDistinctValues);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
