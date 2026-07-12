#include "piece_tree.h"
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

std::string random_text(Random& random) {
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

std::size_t oracle_line_count(std::string_view text) {
    return 1 + static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
}

std::size_t oracle_line_start(std::string_view text, std::size_t line) {
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

std::size_t oracle_line_of_offset(std::string_view text, std::size_t offset) {
    return static_cast<std::size_t>(
        std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
}

TEST(original_buffer_and_boundary_queries) {
    PieceTree tree("alpha\nbeta\n");

    ASSERT_EQ(tree.text(), std::string("alpha\nbeta\n"));
    ASSERT_EQ(tree.size(), std::size_t{11});
    ASSERT_EQ(tree.line_count(), std::size_t{3});
    ASSERT_EQ(tree.line_start(0), std::size_t{0});
    ASSERT_EQ(tree.line_start(1), std::size_t{6});
    ASSERT_EQ(tree.line_start(2), std::size_t{11});
    ASSERT_EQ(tree.line_of_offset(5), std::size_t{0});
    ASSERT_EQ(tree.line_of_offset(6), std::size_t{1});
    ASSERT_EQ(tree.substr(2, 7), std::string("pha\nbet"));
    ASSERT_EQ(tree.validate(), true);
}

TEST(randomized_std_string_oracle_and_invariants) {
    Random random(0x9e3779b97f4a7c15ULL);
    std::string oracle = "original\ntext";
    PieceTree tree(oracle);

    for (std::size_t operation = 0; operation < 20000; ++operation) {
        switch (random.below(5)) {
        case 0: {
            const auto offset = random.below(oracle.size() + 1);
            const auto inserted = random_text(random);
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
            const auto line = random.below(oracle_line_count(oracle));
            ASSERT_EQ(tree.line_start(line), oracle_line_start(oracle, line));
            break;
        }
        default: {
            const auto offset = random.below(oracle.size() + 1);
            ASSERT_EQ(tree.line_of_offset(offset), oracle_line_of_offset(oracle, offset));
            break;
        }
        }

        ASSERT_EQ(tree.size(), oracle.size());
        ASSERT_EQ(tree.line_count(), oracle_line_count(oracle));
        ASSERT_EQ(tree.text(), oracle);
        ASSERT_EQ(tree.validate(), true);
    }
}

TEST(rejects_out_of_range_operations_without_mutation) {
    PieceTree tree("abc");
    const auto before = tree.text();
    bool insert_failed = false;
    bool erase_failed = false;
    bool read_failed = false;
    bool line_failed = false;
    bool offset_failed = false;

    try { tree.insert(4, "x"); } catch (const std::out_of_range&) { insert_failed = true; }
    try { tree.erase(2, 2); } catch (const std::out_of_range&) { erase_failed = true; }
    try { static_cast<void>(tree.substr(3, 1)); } catch (const std::out_of_range&) { read_failed = true; }
    try { static_cast<void>(tree.line_start(1)); } catch (const std::out_of_range&) { line_failed = true; }
    try { static_cast<void>(tree.line_of_offset(4)); } catch (const std::out_of_range&) { offset_failed = true; }

    ASSERT_EQ(insert_failed, true);
    ASSERT_EQ(erase_failed, true);
    ASSERT_EQ(read_failed, true);
    ASSERT_EQ(line_failed, true);
    ASSERT_EQ(offset_failed, true);
    ASSERT_EQ(tree.text(), before);
    ASSERT_EQ(tree.validate(), true);
}

} // namespace

int main() {
    RUN(original_buffer_and_boundary_queries);
    RUN(randomized_std_string_oracle_and_invariants);
    RUN(rejects_out_of_range_operations_without_mutation);
    return failed == 0 ? 0 : 1;
}
