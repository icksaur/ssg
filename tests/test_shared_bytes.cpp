#include "test_helpers.h"

#include <ssg/SharedBytes.h>

#include <array>
#include <cstddef>
#include <string>

// LF-2 (doc/spec-large-files-loading.md): SharedBytes is an immutable ref-counted
// handle whose backing is swappable behind data()/size().  These tests prove the
// two properties the plan relies on: copies SHARE one buffer (no double-allocate)
// and an alternate backing works without touching consumers.

namespace {

TEST(owningHoldsBytes) {
    auto bytes = ssg::SharedBytes::owning(std::string{"hello world"});
    ASSERT_EQ(bytes.size(), std::size_t{11});
    ASSERT_FALSE(bytes.empty());
    ASSERT_EQ(bytes.view(), std::string_view{"hello world"});
}

TEST(defaultIsEmpty) {
    ssg::SharedBytes bytes;
    ASSERT_TRUE(bytes.empty());
    ASSERT_EQ(bytes.size(), std::size_t{0});
    ASSERT_TRUE(bytes.data() == nullptr);
}

TEST(copiesShareOneBufferNotASecondAllocation) {
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
    const char* data() const noexcept override { return kStorage.data(); }
    std::size_t size() const noexcept override { return kStorage.size(); }
    static constexpr std::array<char, 3> kStorage{'a', 'b', 'c'};
};
constexpr std::array<char, 3> ArrayBacking::kStorage;

TEST(alternateBackingWorksThroughTheSameInterface) {
    ssg::SharedBytes bytes{std::make_shared<const ArrayBacking>()};
    ASSERT_EQ(bytes.size(), std::size_t{3});
    ASSERT_EQ(bytes.view(), std::string_view{"abc"});
}

}  // namespace

int main() {
    RUN(owningHoldsBytes);
    RUN(defaultIsEmpty);
    RUN(copiesShareOneBufferNotASecondAllocation);
    RUN(alternateBackingWorksThroughTheSameInterface);
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
