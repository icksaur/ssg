#include "test_helpers.h"

#include <ssg/TextCodec.h>
#include <ssg/Workspace.h>
#include <ssg/RecoveryManager.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

fs::path uniqueRoot() {
    auto base = fs::temp_directory_path() /
                ("ssg-open-metrics-" +
                 std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

TEST(directUtf8OpenValidatesOnce) {
    auto root = uniqueRoot();
    { std::ofstream{root / "a.txt", std::ios::binary} << "hello\nworld\n"; }
    auto recovery = ssg::RecoveryManager::create(root / ".recovery");
    auto workspace = ssg::Workspace::create(root, recovery);

    ssg::resetUtf8ValidationCalls();
    auto opened = workspace.openFile("a.txt");
    ASSERT_TRUE(opened.accepted());
    ASSERT_EQ(ssg::utf8ValidationCalls(), std::uint64_t{1});

    fs::remove_all(root);
}

}  // namespace

SSG_TEST_SUITE(test_open_metrics) {
    RUN(directUtf8OpenValidatesOnce);
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
