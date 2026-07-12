#include "command_cases.h"
#include "../test_helpers.h"

#include <ssg/editor_runtime.h>
#include <ssg/editor_session_assembly.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#ifndef SSG_SOURCE_SCAN_ROOT
#error "SSG_SOURCE_SCAN_ROOT must name the source tree"
#endif

namespace {

std::filesystem::path unique_root(std::string_view name) {
    auto root = std::filesystem::current_path() / ("runtime_snapshot_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    return root;
}

ssg::EditorRuntimeConfig config_for(const std::filesystem::path& root) {
    return {root / "workspace", root / "scratch", root / "recovery"};
}

TEST(construction_rejects_invalid_cwd) {
    auto root = unique_root("invalid_cwd");
    auto result = ssg::EditorRuntime::create(config_for(root / "missing"));
    ASSERT_FALSE(result.accepted());
    ASSERT_FALSE(result.message.empty());
}

TEST(command_case_table_exactly_matches_p0_catalog) {
    auto descriptors = ssg::p0_command_descriptors();
    std::set<std::string> actual;
    for (const auto& descriptor : descriptors) actual.insert(descriptor.id);

    std::set<std::string> expected;
    for (const auto& command : ssg::test::runtime_command_cases) {
        ASSERT_TRUE(expected.insert(std::string{command.id}).second);
        ASSERT_FALSE(command.owner.empty());
    }

    ASSERT_EQ(actual, expected);
}

TEST(runtime_constructs_attaches_and_produces_live_snapshot) {
    auto root = unique_root("snapshot");
    auto created = ssg::EditorRuntime::create(config_for(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;

    auto& runtime = *created.runtime;
    ssg::InvocationPrincipal principal{ssg::ClientId{7}, ssg::InvocationOrigin::in_process};
    ASSERT_TRUE(runtime.attach(std::move(principal), ssg::ViewId{9}).accepted());

    auto snapshot = runtime.snapshot(ssg::ClientId{7}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->revision(), runtime.revision());
    ASSERT_EQ(snapshot->client().client_id, ssg::ClientId{7});
    ASSERT_EQ(snapshot->client().view_id, ssg::ViewId{9});
}

TEST(runtime_sources_do_not_include_fixture_model) {
    auto root = std::filesystem::path{SSG_SOURCE_SCAN_ROOT};
    bool found = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root / "src"}) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".cpp" && entry.path().extension() != ".h") continue;
        const auto relative = std::filesystem::relative(entry.path(), root).generic_string();
        if (relative.rfind("src/runtime/", 0) != 0 && relative != "src/editor_runtime.cpp") continue;
        std::ifstream input{entry.path()};
        const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        found = found || text.find("FixtureModel") != std::string::npos;
    }
    ASSERT_FALSE(found);
}

} // namespace

int main() {
    RUN(construction_rejects_invalid_cwd);
    RUN(command_case_table_exactly_matches_p0_catalog);
    RUN(runtime_constructs_attaches_and_produces_live_snapshot);
    RUN(runtime_sources_do_not_include_fixture_model);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
