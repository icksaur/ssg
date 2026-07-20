#include "test_helpers.h"

#include <ssg/EditorRuntime.h>
#include <ssg/startup_audit.h>
#include <ssg/FilesystemWatcher.h>

#include <filesystem>
#include <fstream>
#include <cstdint>
#include <string>

// M10 fast-startup structural oracle (doc/spec-fast-startup.md).
//
// Deferred enrichment (syntax highlighting, workspace tree scan) must NOT run on
// the first-frame path when the runtime is created with defer_enrichment=true;
// prime_deferred() runs it, and it must actually arrive.  Default (eager)
// construction is unchanged.

namespace {

namespace fs = std::filesystem;

fs::path makeWorkspace(std::string const& name) {
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

ssg::EditorRuntimeConfig configFor(fs::path const& root, bool defer) {
    ssg::EditorRuntimeConfig config;
    config.cwd = root / "workspace";
    config.scratchRoot = root / "scratch";
    config.recoveryRoot = root / "recovery";
    config.deferEnrichment = defer;
    return config;
}

}  // namespace

TEST(deferredEnrichmentSkipsSyntaxAndTreeUntilPrimed) {
    auto root = makeWorkspace("deferred");
    auto created = ssg::EditorRuntime::create(configFor(root, /*defer=*/true));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(), std::string{"code.txt"}})
                    .accepted());

    // Producing the first frame (a snapshot) must not have run the deferrable
    // O(document) syntax pass or the O(workspace) tree scan.
    (void)runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    auto before = runtime.deferredWorkCounts();
    ASSERT_EQ(before.syntaxRuns, std::uint64_t{0});
    ASSERT_EQ(before.treeScans, std::uint64_t{0});

    // Priming runs the deferred work; it must actually arrive.
    auto const revisionBeforePrime = runtime.revision();
    runtime.primeDeferred();
    auto after = runtime.deferredWorkCounts();
    ASSERT_TRUE(after.syntaxRuns >= 1);
    ASSERT_TRUE(after.treeScans >= 1);
    // The session revision advances so delta-based clients observe the primed
    // enrichment (a same-revision snapshot pair yields no delta).
    ASSERT_TRUE(runtime.revision().value() > revisionBeforePrime.value());

    // Idempotent: a second prime does no additional deferred work and does not
    // advance the revision again.
    auto const revisionAfterPrime = runtime.revision();
    runtime.primeDeferred();
    auto again = runtime.deferredWorkCounts();
    ASSERT_EQ(again.syntaxRuns, after.syntaxRuns);
    ASSERT_EQ(again.treeScans, after.treeScans);
    ASSERT_EQ(runtime.revision().value(), revisionAfterPrime.value());

    fs::remove_all(root);
}

TEST(eagerConstructionRunsEnrichmentImmediately) {
    auto root = makeWorkspace("eager");
    auto created = ssg::EditorRuntime::create(configFor(root, /*defer=*/false));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(), std::string{"code.txt"}})
                    .accepted());

    // Default (eager) behavior: the tree scan ran at construction and syntax ran
    // at construction and again on open — all before any prime_deferred call.
    auto counts = runtime.deferredWorkCounts();
    ASSERT_TRUE(counts.treeScans >= 1);
    ASSERT_TRUE(counts.syntaxRuns >= 1);

    // prime_deferred is a harmless no-op when nothing was deferred.
    runtime.primeDeferred();
    auto after = runtime.deferredWorkCounts();
    ASSERT_EQ(after.treeScans, counts.treeScans);
    ASSERT_EQ(after.syntaxRuns, counts.syntaxRuns);

    fs::remove_all(root);
}

TEST(firstFrameConstructsNoOptionalSubsystem) {
    // M10-2 (doc/spec-fast-startup.md): producing the first frame must construct
    // no optional subsystem (Lua, LSP, a real Tree-sitter grammar, a filesystem
    // watcher, HTTP) — project invariant I12.
    ssg::resetOptionalConstructionAudit();
    auto root = makeWorkspace("no_optional");
    auto created = ssg::EditorRuntime::create(configFor(root, /*defer=*/true));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(), std::string{"code.txt"}})
                    .accepted());
    (void)runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});

    // Exhaustive over the enumerated subsystems (a missing enum entry fails the
    // static_assert in startup_audit.h, so the list cannot silently omit one).
    for (auto subsystem : ssg::kAllOptionalSubsystems) {
        ASSERT_EQ(ssg::optionalConstructionCount(subsystem), std::uint64_t{0});
    }
    ASSERT_EQ(ssg::optionalConstructionTotal(), std::uint64_t{0});

    // Priming (post-first-frame enrichment) also constructs nothing optional:
    // the plain-text syntax pass uses no Tree-sitter grammar.
    runtime.primeDeferred();
    ASSERT_EQ(ssg::optionalConstructionTotal(), std::uint64_t{0});

    fs::remove_all(root);
}

TEST(optionalConstructionAuditIsWiredPositiveControl) {
    // Guards against a false pass from broken instrumentation: constructing a
    // real optional subsystem (a filesystem watcher) MUST increment its counter.
    ssg::resetOptionalConstructionAudit();
    ASSERT_EQ(ssg::optionalConstructionCount(ssg::OptionalSubsystem::FilesystemWatcher),
              std::uint64_t{0});
    auto root = makeWorkspace("positive_control");
    {
        auto watcher = ssg::makePlatformFilesystemWatcher(
            std::filesystem::canonical(root / "workspace"));
        ASSERT_TRUE(watcher != nullptr);
    }
    ASSERT_TRUE(ssg::optionalConstructionCount(
                    ssg::OptionalSubsystem::FilesystemWatcher) >= 1);
    fs::remove_all(root);
}

int main() {
    // M10-2 static-init probe: nothing optional may construct before main (no
    // self-registering globals); the ledger must be empty at process entry.
    if (ssg::optionalConstructionTotal() != 0) {
        std::cerr << "  FAIL: an optional subsystem constructed before main "
                     "(static-init side effect)\n";
        ++failed;
    }
    RUN(deferredEnrichmentSkipsSyntaxAndTreeUntilPrimed);
    RUN(eagerConstructionRunsEnrichmentImmediately);
    RUN(firstFrameConstructsNoOptionalSubsystem);
    RUN(optionalConstructionAuditIsWiredPositiveControl);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
