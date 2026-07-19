#include <ssg/editor_runtime.h>
#include <ssg/render.h>
#include <ssg/session_snapshot.h>

#include "test_helpers.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <unistd.h>

// Milestone 11 — Library API is the contract (doc/spec-library-contract.md).
//
// M11-1: the TUI screen is a pure function of the production EditorRuntime's
// SessionSnapshot.  These tests drive the REAL runtime (not a hand-authored
// fixture model) through a fixed script and compare render(snapshot).canonical()
// to a checked-in golden, for the normal, prompt, and too-small screens.  Set
// SSG_REGEN_GOLDEN=1 to (re)write the goldens after an intentional change.

namespace {

namespace fs = std::filesystem;

fs::path unique_root(std::string const& name) {
    // A FIXED path (not pid-based): the rendered screen includes the workspace
    // CWD in the header, so the golden must be produced against a deterministic
    // path.  Linux-first (temp_directory_path() is /tmp on CI and dev); the
    // golden encodes that fixture path.
    auto base = fs::temp_directory_path() / "ssg-contract-fixtures" / name;
    fs::remove_all(base);
    fs::create_directories(base / "workspace");
    fs::create_directories(base / "scratch");
    fs::create_directories(base / "recovery");
    return base;
}

// A production runtime over a workspace with exactly one known file, so the
// rendered screen (including any filesystem tree) is deterministic.
std::unique_ptr<ssg::EditorRuntime> make_runtime(fs::path const& root) {
    std::ofstream{root / "workspace" / "alpha.txt", std::ios::binary}
        << "first line\nsecond line\nthird line\n";
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    if (!created.accepted()) return nullptr;
    auto runtime = std::move(created.runtime);
    (void)runtime->attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process},
                          ssg::ViewId{1});
    return runtime;
}

std::string read_file(char const* path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

// Compare `actual` to the golden at `path`, or rewrite the golden when
// SSG_REGEN_GOLDEN is set.  Returns true on match.
bool matches_golden(std::string const& actual, char const* path) {
    if (std::getenv("SSG_REGEN_GOLDEN") != nullptr) {
        std::ofstream{path, std::ios::binary} << actual;
        return true;
    }
    auto expected = read_file(path);
    if (actual != expected) {
        std::cerr << "  golden mismatch for " << path << "\n--- actual ---\n"
                  << actual << "--- end ---\n";
        return false;
    }
    return true;
}

}  // namespace

TEST(production_runtime_normal_screen_matches_golden) {
    auto root = unique_root("normal");
    auto runtime = make_runtime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(runtime->dispatch(ssg::ClientId{1},
                                  {"file.open", runtime->revision(),
                                   std::string{"alpha.txt"}})
                    .accepted());
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::render(*snapshot);
    ASSERT_TRUE(matches_golden(grid.canonical(), SSG_CONTRACT_NORMAL_GOLDEN));
    // The screen is a pure function of the snapshot: a second render is identical.
    ASSERT_EQ(ssg::render(*snapshot).canonical(), grid.canonical());
    fs::remove_all(root);
}

TEST(production_runtime_palette_screen_matches_golden) {
    auto root = unique_root("palette");
    auto runtime = make_runtime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(runtime->dispatch(ssg::ClientId{1},
                                  {"file.open", runtime->revision(),
                                   std::string{"alpha.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime->dispatch(ssg::ClientId{1},
                                  {"palette.open", runtime->revision(), {}})
                    .accepted());
    // The client derived view (query/ghost/rows/selection) is reported as input;
    // the library builds the rendered projection.
    ssg::PaletteReport report;
    report.query = "sa";
    report.ghost = "ve File";
    report.rows = {{"file.save", "Save File", ""},
                   {"file.save_as", "Save As", ""}};
    report.selected = std::uint32_t{0};
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24}, {}, report);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::render(*snapshot);
    ASSERT_TRUE(matches_golden(grid.canonical(), SSG_CONTRACT_PALETTE_GOLDEN));
    ASSERT_EQ(ssg::render(*snapshot).canonical(), grid.canonical());
    fs::remove_all(root);
}

TEST(production_runtime_too_small_screen_matches_golden) {
    auto root = unique_root("small");
    auto runtime = make_runtime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(runtime->dispatch(ssg::ClientId{1},
                                  {"file.open", runtime->revision(),
                                   std::string{"alpha.txt"}})
                    .accepted());
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {24, 3});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::render(*snapshot);
    ASSERT_TRUE(matches_golden(grid.canonical(), SSG_CONTRACT_TOO_SMALL_GOLDEN));
    fs::remove_all(root);
}

int main() {
    RUN(production_runtime_normal_screen_matches_golden);
    RUN(production_runtime_palette_screen_matches_golden);
    RUN(production_runtime_too_small_screen_matches_golden);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
