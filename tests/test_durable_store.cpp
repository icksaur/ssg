#include "test_helpers.h"

#include <ssg/DurableStore.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = testSystemRuntimePath(
            "durable_store_" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::chrono::system_clock::time_point moment(std::int64_t nanoseconds) {
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds{nanoseconds})};
}

TEST(entryNamesRoundTripCanonicalAndLegacyTimestamps) {
    const auto created = moment(1770000000123456789);
    const auto canonical =
        ssg::DurableStore::entryName(created, "0123456789abcdef");
    ASSERT_EQ(ssg::DurableStore::entryTimestamp(canonical), created);
    ASSERT_EQ(ssg::DurableStore::entryTimestamp(
                  "01770000000123456789-0123456789abcdef"),
              created);
    ASSERT_TRUE(ssg::DurableStore::entryTimestamp(
                    "20260201T000000-0000")
                    .has_value());
    ASSERT_FALSE(
        ssg::DurableStore::entryTimestamp("not-an-entry").has_value());
}

TEST(entriesSortMixedFormatsByDecodedAge) {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "entries";
    std::filesystem::create_directories(
        root / ssg::DurableStore::entryName(moment(3000000000), "new"));
    std::filesystem::create_directories(root / "00000000002000000000-old");
    std::filesystem::create_directories(root / "unknown");

    const auto listed = ssg::DurableStore{root}.entries();
    ASSERT_TRUE(listed.ok());
    ASSERT_TRUE(listed.complete);
    ASSERT_EQ(listed.entries.size(), std::size_t{3});
    ASSERT_EQ(listed.entries[0].created, moment(2000000000));
    ASSERT_EQ(listed.entries[1].created, moment(3000000000));
    ASSERT_FALSE(listed.entries[2].created.has_value());
}

TEST(independentStoresClaimDistinctDirectories) {
    TemporaryDirectory temporary;
    ssg::DurableStore first{temporary.path()};
    ssg::DurableStore second{temporary.path()};

    const auto a = first.claimEntry(moment(4000000000));
    const auto b = second.claimEntry(moment(4000000000));
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    ASSERT_NE(a.path, b.path);
    ASSERT_TRUE(std::filesystem::is_directory(a.path));
    ASSERT_TRUE(std::filesystem::is_directory(b.path));
}

TEST(evictionRemovesOldestUntilCallerPolicyIsSatisfied) {
    TemporaryDirectory temporary;
    std::vector<ssg::DurableStoreEntry> entries;
    for (std::int64_t age : {3, 1, 2}) {
        const auto path = temporary.path() / std::to_string(age);
        std::filesystem::create_directory(path);
        entries.push_back({path, moment(age)});
    }

    const auto evicted = ssg::DurableStore::evictOldestWhile(
        std::move(entries),
        [](std::span<const ssg::DurableStoreEntry> remaining) {
            return remaining.size() <= 1;
        });
    ASSERT_TRUE(evicted.ok());
    ASSERT_TRUE(evicted.policySatisfied);
    ASSERT_EQ(evicted.removed.size(), std::size_t{2});
    ASSERT_EQ(evicted.removed[0].created, moment(1));
    ASSERT_EQ(evicted.removed[1].created, moment(2));
    ASSERT_EQ(evicted.remaining[0].created, moment(3));
    ASSERT_TRUE(std::filesystem::is_directory(evicted.remaining[0].path));
}

} // namespace

SSG_TEST_SUITE(test_durable_store) {
    RUN(entryNamesRoundTripCanonicalAndLegacyTimestamps);
    RUN(entriesSortMixedFormatsByDecodedAge);
    RUN(independentStoresClaimDistinctDirectories);
    RUN(evictionRemovesOldestUntilCallerPolicyIsSatisfied);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
