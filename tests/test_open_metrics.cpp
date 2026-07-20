#include "test_helpers.h"

#include <ssg/open_metrics.h>
#include <ssg/workspace.h>
#include <ssg/recovery.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

// LF-1 (doc/spec-large-files-loading.md): the EVOLVING counter oracle.
//
// These expected values quantify the redundancy the milestone removes and MOVE
// as later steps land:
//   - utf8_validation_calls for a direct-UTF-8 open is 2 today (decode scan +
//     Document-ctor re-scan); LF-3a drives it to 1.
//   - a fresh open's Workspace::state() materializes the whole piece tree once
//     (piece_tree_text_calls == 1); LF-4b drives it to 0.
// A test that still reads 2 / 1 after those steps proves the redundancy is gone.

namespace {

namespace fs = std::filesystem;

fs::path unique_root() {
    auto base = fs::temp_directory_path() /
                ("ssg-open-metrics-" +
                 std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

TEST(direct_utf8_open_validates_twice_today) {
    auto root = unique_root();
    { std::ofstream{root / "a.txt", std::ios::binary} << "hello\nworld\n"; }
    auto recovery = ssg::RecoveryActions::create(root / ".recovery");
    auto workspace = ssg::Workspace::create(root, recovery);

    ssg::reset_utf8_validation_calls();
    auto opened = workspace.open_file("a.txt");
    ASSERT_TRUE(opened.accepted());
    // Decode's validating scan + the Document ctor's redundant re-validation.
    ASSERT_EQ(ssg::utf8_validation_calls(), std::uint64_t{2});

    fs::remove_all(root);
}

TEST(fresh_open_state_materializes_tree_once_today) {
    auto root = unique_root();
    { std::ofstream{root / "a.txt", std::ios::binary} << "hello\nworld\n"; }
    auto recovery = ssg::RecoveryActions::create(root / ".recovery");
    auto workspace = ssg::Workspace::create(root, recovery);
    auto opened = workspace.open_file("a.txt");
    ASSERT_TRUE(opened.accepted());

    ssg::reset_piece_tree_text_calls();
    auto state = workspace.state(*opened.document);
    ASSERT_TRUE(state.has_value());
    // Pass I: state() walks the just-built tree back into a std::string to
    // dirty-check.  LF-4b removes this for a fresh open.
    ASSERT_EQ(ssg::piece_tree_text_calls(), std::uint64_t{1});

    fs::remove_all(root);
}

}  // namespace

int main() {
    RUN(direct_utf8_open_validates_twice_today);
    RUN(fresh_open_state_materializes_tree_once_today);
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
