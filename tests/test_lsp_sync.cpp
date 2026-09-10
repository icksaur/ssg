#include <ssg/LspState.h>

#include "test_helpers.h"

#include <fstream>
#include <iterator>
#include <string>

namespace {

using namespace ssg;

template <typename Result>
auto errorOf(Result result) {
    return result.error;
}

LspPosition positionOf(LspPositionResult result) {
    return result.position;
}

ByteOffset offsetOf(LspByteOffsetResult result) {
    return result.offset;
}

std::string fixture() {
    std::ifstream input(std::string{SSG_TEST_SOURCE_DIR} +
                        "/tests/fixtures/lsp/sync/utf_positions.txt",
                        std::ios::binary);
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

TEST(utf8Utf16PositionsMatchHandComputedFixture) {
    const auto text = fixture();
    ASSERT_EQ(positionOf(byteOffsetToLspPosition(text, ByteOffset{0})),
              (LspPosition{0, 0}));
    ASSERT_EQ(positionOf(byteOffsetToLspPosition(text, ByteOffset{1})),
              (LspPosition{0, 1}));
    ASSERT_EQ(positionOf(byteOffsetToLspPosition(text, ByteOffset{3})),
              (LspPosition{0, 2}));
    ASSERT_EQ(positionOf(byteOffsetToLspPosition(text, ByteOffset{7})),
              (LspPosition{0, 4}));
    ASSERT_EQ(offsetOf(lspPositionToByteOffset(text, {0, 4})), ByteOffset{7});
    ASSERT_EQ(errorOf(lspPositionToByteOffset(text, {0, 3})),
              LspPositionError::SplitSurrogate);
    ASSERT_EQ(offsetOf(lspPositionToByteOffset(text, {1, 0})), ByteOffset{9});
    ASSERT_EQ(offsetOf(lspPositionToByteOffset("a\r\nb", {1, 0})),
              ByteOffset{3});
    ASSERT_EQ(errorOf(byteOffsetToLspPosition(text, ByteOffset{2})),
              LspPositionError::InvalidUtf8Boundary);
    ASSERT_EQ(errorOf(byteOffsetToLspPosition(std::string{"\xff", 1},
                                             ByteOffset{0})),
              LspPositionError::InvalidUtf8);
}

TEST(diagnosticSeverityMatchesLspProtocolValues) {
    ASSERT_EQ(static_cast<std::uint8_t>(LspDiagnosticSeverity::Error),
              std::uint8_t{1});
    ASSERT_EQ(static_cast<std::uint8_t>(LspDiagnosticSeverity::Warning),
              std::uint8_t{2});
    ASSERT_EQ(static_cast<std::uint8_t>(LspDiagnosticSeverity::Information),
              std::uint8_t{3});
    ASSERT_EQ(static_cast<std::uint8_t>(LspDiagnosticSeverity::Hint),
              std::uint8_t{4});
}

} // namespace

SSG_TEST_SUITE(test_lsp_sync) {
    RUN(utf8Utf16PositionsMatchHandComputedFixture);
    RUN(diagnosticSeverityMatchesLspProtocolValues);
    return failed == 0 ? 0 : 1;
}
