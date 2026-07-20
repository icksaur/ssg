#include "test_helpers.h"

#include <ssg/shared_bytes.h>

#include <array>
#include <cstddef>
#include <string>

// LF-2 (doc/spec-large-files-loading.md): SharedBytes is an immutable ref-counted
// handle whose backing is swappable behind data()/size().  These tests prove the
// two properties the plan relies on: copies SHARE one buffer (no double-allocate)
// and an alternate backing works without touching consumers.

namespace {

TEST(owning_holds_bytes) {
    auto bytes = ssg::SharedBytes::owning(std::string{"hello world"});
    ASSERT_EQ(bytes.size(), std::size_t{11});
    ASSERT_FALSE(bytes.empty());
    ASSERT_EQ(bytes.view(), std::string_view{"hello world"});
}

TEST(default_is_empty) {
    ssg::SharedBytes bytes;
    ASSERT_TRUE(bytes.empty());
    ASSERT_EQ(bytes.size(), std::size_t{0});
    ASSERT_TRUE(bytes.data() == nullptr);
}

TEST(copies_share_one_buffer_not_a_second_allocation) {
    auto a = ssg::SharedBytes::owning(std::string(1024, 'x'));
    auto b = a;
    // Same backing pointer => the bytes are shared, not copied.
    ASSERT_TRUE(a.data() == b.data());
    ASSERT_EQ(a.size(), b.size());
}

// A backing that owns nothing on the heap — a fixed static array — proving the
// interface admits an alternate backing behind the same data()/size() contract.
class ArrayBacking final : public ssg::SharedBytes::Backing {
public:
    const char* data() const noexcept override { return storage.data(); }
    std::size_t size() const noexcept override { return storage.size(); }
    static constexpr std::array<char, 3> storage{'a', 'b', 'c'};
};
constexpr std::array<char, 3> ArrayBacking::storage;

TEST(alternate_backing_works_through_the_same_interface) {
    ssg::SharedBytes bytes{std::make_shared<const ArrayBacking>()};
    ASSERT_EQ(bytes.size(), std::size_t{3});
    ASSERT_EQ(bytes.view(), std::string_view{"abc"});
}

}  // namespace

int main() {
    RUN(owning_holds_bytes);
    RUN(default_is_empty);
    RUN(copies_share_one_buffer_not_a_second_allocation);
    RUN(alternate_backing_works_through_the_same_interface);
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
