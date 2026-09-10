#include <ssg/PieceTree.h>
#include "test_helpers.h"

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

TEST(originalBufferIsPreserved) {
    PieceTree tree("alpha\nbeta\n");

    ASSERT_EQ(tree.text(), std::string("alpha\nbeta\n"));
    ASSERT_EQ(tree.size(), std::size_t{11});
}

TEST(randomizedMutationsMatchStdString) {
    Random random(0x9e3779b97f4a7c15ULL);
    std::string oracle = "original\ntext";
    PieceTree tree(oracle);

    for (std::size_t operation = 0; operation < 20000; ++operation) {
        switch (random.below(2)) {
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
        }

        ASSERT_EQ(tree.size(), oracle.size());
        ASSERT_EQ(tree.text(), oracle);
    }
}

TEST(rejectsOutOfRangeOperationsWithoutMutation) {
    PieceTree tree("abc");
    const auto before = tree.text();
    bool insertFailed = false;
    bool eraseFailed = false;

    try { tree.insert(4, "x"); } catch (const std::out_of_range&) { insertFailed = true; }
    try { tree.erase(2, 2); } catch (const std::out_of_range&) { eraseFailed = true; }

    ASSERT_EQ(insertFailed, true);
    ASSERT_EQ(eraseFailed, true);
    ASSERT_EQ(tree.text(), before);
}

} // namespace

SSG_TEST_SUITE(test_piece_tree) {
    RUN(originalBufferIsPreserved);
    RUN(randomizedMutationsMatchStdString);
    RUN(rejectsOutOfRangeOperationsWithoutMutation);
    return failed == 0 ? 0 : 1;
}
