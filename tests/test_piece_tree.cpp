#include "PieceTree.h"
#include "test_helpers.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using ssg::detail::PieceTree;

class Random {
public:
    explicit Random(std::uint64_t state) : state_(state) {}

    std::size_t below(std::size_t limit) {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 7U;
        state_ ^= state_ << 17U;
        return limit == 0 ? 0 : static_cast<std::size_t>(state_ % limit);
    }

private:
    std::uint64_t state_;
};

std::string randomText(Random& random) {
    static constexpr std::string_view bytes =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 \n"
        "\t\xc3\xa9\xe4\xb8\xad";
    std::string result;
    const auto length = 1 + random.below(24);
    result.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        result.push_back(bytes[random.below(bytes.size())]);
    }
    return result;
}

std::size_t oracleLineCount(std::string_view text) {
    return 1 + static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
}

std::size_t oracleLineStart(std::string_view text, std::size_t line) {
    if (line == 0) {
        return 0;
    }
    std::size_t current = 0;
    for (std::size_t offset = 0; offset < text.size(); ++offset) {
        if (text[offset] == '\n' && ++current == line) {
            return offset + 1;
        }
    }
    throw std::out_of_range("line");
}

std::size_t oracleLineOfOffset(std::string_view text, std::size_t offset) {
    return static_cast<std::size_t>(
        std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
}

TEST(originalBufferAndBoundaryQueries) {
    PieceTree tree("alpha\nbeta\n");

    ASSERT_EQ(tree.text(), std::string("alpha\nbeta\n"));
    ASSERT_EQ(tree.size(), std::size_t{11});
    ASSERT_EQ(tree.lineCount(), std::size_t{3});
    ASSERT_EQ(tree.lineStart(0), std::size_t{0});
    ASSERT_EQ(tree.lineStart(1), std::size_t{6});
    ASSERT_EQ(tree.lineStart(2), std::size_t{11});
    ASSERT_EQ(tree.lineOfOffset(5), std::size_t{0});
    ASSERT_EQ(tree.lineOfOffset(6), std::size_t{1});
    ASSERT_EQ(tree.substr(2, 7), std::string("pha\nbet"));
    ASSERT_EQ(tree.validate(), true);
}

TEST(randomizedStdStringOracleAndInvariants) {
    Random random(0x9e3779b97f4a7c15ULL);
    std::string oracle = "original\ntext";
    PieceTree tree(oracle);

    for (std::size_t operation = 0; operation < 20000; ++operation) {
        switch (random.below(5)) {
        case 0: {
            const auto offset = random.below(oracle.size() + 1);
            const auto inserted = randomText(random);
            oracle.insert(offset, inserted);
            tree.insert(offset, inserted);
            break;
        }
        case 1: {
            const auto offset = random.below(oracle.size() + 1);
            const auto count = random.below(oracle.size() - offset + 1);
            oracle.erase(offset, count);
            tree.erase(offset, count);
            break;
        }
        case 2: {
            const auto offset = random.below(oracle.size() + 1);
            const auto count = random.below(oracle.size() - offset + 1);
            ASSERT_EQ(tree.substr(offset, count), oracle.substr(offset, count));
            break;
        }
        case 3: {
            const auto line = random.below(oracleLineCount(oracle));
            ASSERT_EQ(tree.lineStart(line), oracleLineStart(oracle, line));
            break;
        }
        default: {
            const auto offset = random.below(oracle.size() + 1);
            ASSERT_EQ(tree.lineOfOffset(offset), oracleLineOfOffset(oracle, offset));
            break;
        }
        }

        ASSERT_EQ(tree.size(), oracle.size());
        ASSERT_EQ(tree.lineCount(), oracleLineCount(oracle));
        ASSERT_EQ(tree.text(), oracle);
        ASSERT_EQ(tree.validate(), true);
    }
}

TEST(rejectsOutOfRangeOperationsWithoutMutation) {
    PieceTree tree("abc");
    const auto before = tree.text();
    bool insertFailed = false;
    bool eraseFailed = false;
    bool readFailed = false;
    bool lineFailed = false;
    bool offsetFailed = false;

    try { tree.insert(4, "x"); } catch (const std::out_of_range&) { insertFailed = true; }
    try { tree.erase(2, 2); } catch (const std::out_of_range&) { eraseFailed = true; }
    try { static_cast<void>(tree.substr(3, 1)); } catch (const std::out_of_range&) { readFailed = true; }
    try { static_cast<void>(tree.lineStart(1)); } catch (const std::out_of_range&) { lineFailed = true; }
    try { static_cast<void>(tree.lineOfOffset(4)); } catch (const std::out_of_range&) { offsetFailed = true; }

    ASSERT_EQ(insertFailed, true);
    ASSERT_EQ(eraseFailed, true);
    ASSERT_EQ(readFailed, true);
    ASSERT_EQ(lineFailed, true);
    ASSERT_EQ(offsetFailed, true);
    ASSERT_EQ(tree.text(), before);
    ASSERT_EQ(tree.validate(), true);
}

} // namespace

SSG_TEST_SUITE(test_piece_tree) {
    RUN(originalBufferAndBoundaryQueries);
    RUN(randomizedStdStringOracleAndInvariants);
    RUN(rejectsOutOfRangeOperationsWithoutMutation);
    return failed == 0 ? 0 : 1;
}
