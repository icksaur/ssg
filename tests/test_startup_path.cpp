#include "test_helpers.h"

#include <ssg/editor_runtime.h>

#include <filesystem>
#include <fstream>
#include <string>

// M10 fast-startup structural oracle (doc/spec-fast-startup.md).
//
// Deferred enrichment (syntax highlighting, workspace tree scan) must NOT run on
// the first-frame path when the runtime is created with defer_enrichment=true;
// prime_deferred() runs it, and it must actually arrive.  Default (eager)
// construction is unchanged.

namespace {

namespace fs = std::filesystem;

fs::path make_workspace(std::string const& name) {
    auto root = fs::current_path() / ("startup_path_" + name);
    fs::remove_all(root);
    fs::create_directories(root / "workspace");
    fs::create_directories(root / "scratch");
    fs::create_directories(root / "recovery");
    std::string text;
    for (int i = 0; i < 200; ++i) text += "int value = 1; // a line of code\n";
    std::ofstream{root / "workspace" / "code.txt", std::ios::binary} << text;
    // A few extra files so the tree scan has something to find.
    for (int i = 0; i < 5; ++i) {
        std::ofstream{root / "workspace" / ("extra_" + std::to_string(i) + ".txt")}
            << "x\n";
    }
    return root;
}

ssg::EditorRuntimeConfig config_for(fs::path const& root, bool defer) {
    ssg::EditorRuntimeConfig config;
    config.cwd = root / "workspace";
    config.scratch_root = root / "scratch";
    config.recovery_root = root / "recovery";
    config.defer_enrichment = defer;
    return config;
}

}  // namespace

TEST(deferred_enrichment_skips_syntax_and_tree_until_primed) {
    auto root = make_workspace("deferred");
    auto created = ssg::EditorRuntime::create(config_for(root, /*defer=*/true));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process},
                               ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(), std::string{"code.txt"}})
                    .accepted());

    // Producing the first frame (a snapshot) must not have run the deferrable
    // O(document) syntax pass or the O(workspace) tree scan.
    (void)runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    auto before = runtime.deferred_work_counts();
    ASSERT_EQ(before.syntax_runs, std::uint64_t{0});
    ASSERT_EQ(before.tree_scans, std::uint64_t{0});

    // Priming runs the deferred work; it must actually arrive.
    auto const revision_before_prime = runtime.revision();
    runtime.prime_deferred();
    auto after = runtime.deferred_work_counts();
    ASSERT_TRUE(after.syntax_runs >= 1);
    ASSERT_TRUE(after.tree_scans >= 1);
    // The session revision advances so delta-based clients observe the primed
    // enrichment (a same-revision snapshot pair yields no delta).
    ASSERT_TRUE(runtime.revision().value() > revision_before_prime.value());

    // Idempotent: a second prime does no additional deferred work and does not
    // advance the revision again.
    auto const revision_after_prime = runtime.revision();
    runtime.prime_deferred();
    auto again = runtime.deferred_work_counts();
    ASSERT_EQ(again.syntax_runs, after.syntax_runs);
    ASSERT_EQ(again.tree_scans, after.tree_scans);
    ASSERT_EQ(runtime.revision().value(), revision_after_prime.value());

    fs::remove_all(root);
}

TEST(eager_construction_runs_enrichment_immediately) {
    auto root = make_workspace("eager");
    auto created = ssg::EditorRuntime::create(config_for(root, /*defer=*/false));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process},
                               ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(), std::string{"code.txt"}})
                    .accepted());

    // Default (eager) behavior: the tree scan ran at construction and syntax ran
    // at construction and again on open — all before any prime_deferred call.
    auto counts = runtime.deferred_work_counts();
    ASSERT_TRUE(counts.tree_scans >= 1);
    ASSERT_TRUE(counts.syntax_runs >= 1);

    // prime_deferred is a harmless no-op when nothing was deferred.
    runtime.prime_deferred();
    auto after = runtime.deferred_work_counts();
    ASSERT_EQ(after.tree_scans, counts.tree_scans);
    ASSERT_EQ(after.syntax_runs, counts.syntax_runs);

    fs::remove_all(root);
}

int main() {
    RUN(deferred_enrichment_skips_syntax_and_tree_until_primed);
    RUN(eager_construction_runs_enrichment_immediately);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
