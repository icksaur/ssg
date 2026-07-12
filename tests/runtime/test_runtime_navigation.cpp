#include "../test_helpers.h"

#include <ssg/editor_runtime.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path unique_root() {
    auto root = std::filesystem::current_path() / "runtime_navigation";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "needle.txt"} << "alpha needle omega";
    return root;
}

TEST(search_tree_diff_and_follow_sections_use_runtime_state) {
    auto root = unique_root();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"search.workspace", runtime.revision(), std::string{"needle"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"follow_edits.pause", runtime.revision(), {}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_FALSE(snapshot->sections().search.results.empty());
    ASSERT_FALSE(snapshot->sections().tree.providers.empty());
    ASSERT_EQ(snapshot->sections().follow_edits.mode, ssg::FollowMode::paused);
}

} // namespace

int main() {
    RUN(search_tree_diff_and_follow_sections_use_runtime_state);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
