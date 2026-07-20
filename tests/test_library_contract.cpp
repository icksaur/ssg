#include <ssg/editor_runtime.h>
#include <ssg/input.h>
#include <ssg/render.h>
#include <ssg/session_snapshot.h>

#include "test_helpers.h"

#include <any>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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
    (void)runtime->attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
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

// M11-4: the client's PaletteReport is a pure, bounded derived view built ONLY by
// the library seam `derive_palette_report` from the server-published candidates
// plus the local query/selection/window.  The app (apps/ssg_main.cpp build_report)
// uses this exact seam, so exercising it here proves the client cannot invent
// product data or substitute a private ranker — the projection is library code.
ssg::PaletteReport project_report(
    std::vector<ssg::PaletteCandidate> const& candidates, std::string const& query,
    std::uint32_t pane_rows, std::uint32_t first_visible,
    std::size_t selected_index) {
    ssg::PaletteWindowState window{query, selected_index, first_visible, pane_rows};
    return ssg::derive_palette_report(candidates, window);
}

// The published candidate list for an open palette, straight from the runtime.
std::vector<ssg::PaletteCandidate> published_candidates(
    ssg::EditorRuntime& runtime) {
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"palette.open", runtime.revision(), {}})
                    .accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, {80, 24});
    if (!snapshot) return {};
    return snapshot->sections().palette.candidates;
}

// Reconstruct grid row `row` as a plain string (continuation cells contribute no
// text), for tracing rendered palette labels back to published candidates.
std::string grid_line(ssg::CellGrid const& grid, int row) {
    std::string line;
    for (int column = 0; column < grid.size.columns; ++column) {
        auto const& cell = grid.at(column, row);
        if (!cell.continuation) line += cell.text;
    }
    return line;
}

}  // namespace

TEST(palette_report_is_a_pure_function_of_candidates_and_query) {
    auto root = unique_root("derived");
    auto runtime = make_runtime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    auto candidates = published_candidates(*runtime);
    ASSERT_TRUE(!candidates.empty());
    if (candidates.empty()) return;

    // A representative match, the empty (keep-all) query, and a no-match query.
    for (std::string const& query : {std::string{}, std::string{"sa"},
                                     std::string{"zzq-no-such-command"}}) {
        auto report = project_report(candidates, query, 12, 0, 0);

        // Pure function: identical inputs reproduce an identical report.
        ASSERT_TRUE(project_report(candidates, query, 12, 0, 0) == report);

        // The query is echoed verbatim; it is not server-derived.
        ASSERT_EQ(report.query, query);

        // Invents no product data: every reported row is a published candidate,
        // and the ghost is derived solely from the top candidate's label.
        auto order = ssg::palette_rank(candidates, query);
        for (auto const& shown : report.rows) {
            bool member = false;
            for (auto const& candidate : candidates) {
                if (candidate == shown) { member = true; break; }
            }
            ASSERT_TRUE(member);
        }
        if (order.empty()) {
            ASSERT_TRUE(report.rows.empty());
            ASSERT_EQ(report.ghost, std::string{});
        } else {
            ASSERT_EQ(report.ghost,
                      ssg::palette_ghost(candidates[order.front()].label, query));
            // The reported rows are exactly the window of the shared ranker's
            // order — the client uses no private ranking.
            ASSERT_EQ(report.rows.size(),
                      std::min<std::size_t>(order.size(), 12));
            for (std::size_t row = 0; row < report.rows.size(); ++row) {
                ASSERT_TRUE(report.rows[row] ==
                            candidates[order[report.first_visible + row]]);
            }
        }
    }
    fs::remove_all(root);
}

TEST(rendered_palette_labels_trace_to_published_candidates) {
    auto root = unique_root("derived-render");
    auto runtime = make_runtime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    auto candidates = published_candidates(*runtime);
    ASSERT_TRUE(!candidates.empty());
    if (candidates.empty()) return;

    auto report = project_report(candidates, "sa", 12, 0, 0);
    ASSERT_TRUE(!report.rows.empty());
    if (report.rows.empty()) return;

    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24}, {}, report);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::render(*snapshot);

    // Every rendered candidate row's label text traces to a published candidate:
    // for each reported row there is a grid line beginning with its label, and
    // that label belongs to the published set.
    for (auto const& shown : report.rows) {
        bool member = false;
        for (auto const& candidate : candidates) {
            if (candidate == shown) { member = true; break; }
        }
        ASSERT_TRUE(member);

        bool rendered = false;
        for (int row = 0; row < grid.size.rows; ++row) {
            if (grid_line(grid, row).rfind(shown.label, 0) == 0) {
                rendered = true;
                break;
            }
        }
        ASSERT_TRUE(rendered);
    }
    fs::remove_all(root);
}

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

TEST(delta_replay_reconstructs_the_same_snapshot_and_grid) {
    // M11-3: the "delta" leg of the command/snapshot/delta contract. After every
    // command, the delta between the prior and current production snapshots,
    // replayed onto the prior snapshot, reconstructs the SAME authoritative
    // snapshot as a fresh one — and renders to an identical grid.
    auto root = unique_root("delta");
    auto runtime = make_runtime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ssg::ViewportDimensions const dims{80, 24};

    struct Step {
        std::string command;
        std::any payload;
    };
    std::vector<Step> const script{
        {"file.open", std::string{"alpha.txt"}},
        {"cursor.right", {}},
        {"cursor.line_down", {}},
        {"select.line_down", {}},
        {"panel.toggle", {}},
        {"view.scroll_lines", ssg::ScrollLinesArguments{1}},
    };

    auto previous = runtime->snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(previous.has_value());
    if (!previous) return;

    for (auto const& step : script) {
        (void)runtime->dispatch(
            ssg::ClientId{1}, {step.command, runtime->revision(), step.payload});
        auto fresh = runtime->snapshot(ssg::ClientId{1}, dims);
        ASSERT_TRUE(fresh.has_value());
        if (!fresh) break;

        if (fresh->revision().value() == previous->revision().value()) {
            // A command with no authoritative change produces no delta (the delta
            // API requires the revision to advance); the snapshot is unchanged.
            ASSERT_TRUE(*fresh == *previous);
            continue;
        }

        auto delta = ssg::derive_session_delta(*previous, *fresh);
        auto replayed = ssg::replay_session_delta(*previous, delta);
        ASSERT_TRUE(replayed.accepted());
        if (!replayed.accepted()) break;

        // The delta-reconstructed snapshot equals a fresh production snapshot,
        // and renders to the identical screen.
        ASSERT_TRUE(*replayed.snapshot == *fresh);
        ASSERT_EQ(ssg::render(*replayed.snapshot).canonical(),
                  ssg::render(*fresh).canonical());

        previous = std::move(fresh);
    }
    fs::remove_all(root);
}

int main() {
    RUN(production_runtime_normal_screen_matches_golden);
    RUN(production_runtime_palette_screen_matches_golden);
    RUN(production_runtime_too_small_screen_matches_golden);
    RUN(delta_replay_reconstructs_the_same_snapshot_and_grid);
    RUN(palette_report_is_a_pure_function_of_candidates_and_query);
    RUN(rendered_palette_labels_trace_to_published_candidates);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
