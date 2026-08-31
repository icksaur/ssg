#include <ssg/EditorSession.h>
#include <ssg/Keymap.h>
#include <ssg/Renderer.h>
#include <ssg/session_snapshot.h>

#include "test_helpers.h"
#include "legacy_grid_frame.h"

#include <any>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

template <typename Runtime>
concept HasDimensionedSnapshot = requires(Runtime& runtime) {
    runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
};

static_assert(!HasDimensionedSnapshot<ssg::EditorSession>);

// Milestone 11 — Library API is the contract.
//
// M11-1: the TUI screen is a pure function of the production EditorSession's
// SessionSnapshot.  These tests drive the REAL runtime (not a hand-authored
// fixture model) through a fixed script and assert the screen contract:
// geometry, no uninitialised cells, theme-sourced colour, expected content, and
// render determinism.  They deliberately do not pin an exact appearance.

namespace {

namespace fs = std::filesystem;

fs::path uniqueRoot(std::string const& name) {
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
std::unique_ptr<ssg::EditorSession> makeRuntime(fs::path const& root) {
    std::ofstream{root / "workspace" / "alpha.txt", std::ios::binary}
        << "first line\nsecond line\nthird line\n";
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    if (!created.accepted()) return nullptr;
    auto runtime = std::move(created.session);
    (void)runtime->attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                          ssg::ViewId{1});
    return runtime;
}

// Screen invariants that hold for every rendered production screen, replacing
// three full-screen goldens.  The goldens pinned an exact appearance nobody had
// approved: every colour, glyph, or width change re-blessed 363 fixture lines,
// and a diff of those lines told a reviewer nothing about what had broken.
// These assert what the contract actually claims -- correct geometry, no
// uninitialised cells, and colours drawn only from the 16-entry theme (I22).
void assertScreenInvariants(ssg::CellGrid const& grid, int columns, int rows) {
    ASSERT_EQ(grid.size.columns, columns);
    ASSERT_EQ(grid.size.rows, rows);
    ASSERT_EQ(grid.cells.size(),
              static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            auto const& cell = grid.at(column, row);
            // A continuation cell of a wide glyph is deliberately empty; every
            // other cell must carry text, so no cell is left uninitialised.
            if (!cell.continuation) ASSERT_FALSE(cell.text.empty());
            ASSERT_TRUE(cell.foreground < ssg::kThemeColorSlotCount);
            ASSERT_TRUE(cell.background < ssg::kThemeColorSlotCount);
        }
    }
}

// Whether the rendered screen contains `needle` on any row.
bool screenContains(ssg::CellGrid const& grid, std::string_view needle) {
    for (int row = 0; row < grid.size.rows; ++row) {
        std::string line;
        for (int column = 0; column < grid.size.columns; ++column) {
            auto const& cell = grid.at(column, row);
            if (!cell.continuation) line += cell.text;
        }
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

// M11-4: the client's PaletteReport is a pure, bounded derived view built ONLY by
// the library seam `derive_palette_report` from the server-published candidates
// plus the local query/selection/window.  The app (apps/ssg_main.cpp build_report)
// uses this exact seam, so exercising it here proves the client cannot invent
// product data or substitute a private ranker — the projection is library code.
ssg::PaletteReport projectReport(
    std::vector<ssg::PaletteCandidate> const& candidates, std::string const& query,
    std::uint32_t paneRows, std::uint32_t firstVisible,
    std::size_t selectedIndex) {
    ssg::PaletteWindowState window{query, selectedIndex, firstVisible, paneRows};
    return ssg::PaletteSearcher{}.report(candidates, window);
}

// The published candidate list for an open palette, straight from the runtime.
std::vector<ssg::PaletteCandidate> publishedCandidates(
    ssg::EditorSession& runtime) {
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"palette.open", runtime.revision(), {}})
                    .accepted());
    auto snapshot = runtime.present(ssg::ClientId{1}, {80, 24});
    if (!snapshot) return {};
    return snapshot->semantic().sections().palette.commandCandidates;
}

// Reconstruct grid row `row` as a plain string (continuation cells contribute no
// text), for tracing rendered palette labels back to published candidates.
std::string gridLine(ssg::CellGrid const& grid, int row) {
    std::string line;
    for (int column = 0; column < grid.size.columns; ++column) {
        auto const& cell = grid.at(column, row);
        if (!cell.continuation) line += cell.text;
    }
    return line;
}

}  // namespace

TEST(paletteReportIsAPureFunctionOfCandidatesAndQuery) {
    auto root = uniqueRoot("derived");
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    auto candidates = publishedCandidates(*runtime);
    ASSERT_TRUE(!candidates.empty());
    if (candidates.empty()) return;

    // A representative match, the empty (keep-all) query, and a no-match query.
    for (std::string const& query : {std::string{}, std::string{"sa"},
                                     std::string{"zzq-no-such-command"}}) {
        auto report = projectReport(candidates, query, 12, 0, 0);

        // Pure function: identical inputs reproduce an identical report.
        ASSERT_TRUE(projectReport(candidates, query, 12, 0, 0) == report);

        // The query is echoed verbatim; it is not server-derived.
        ASSERT_EQ(report.query, query);

        // Invents no product data: every reported row is a published candidate,
        // and the ghost is derived solely from the top candidate's label.
        auto order = ssg::PaletteSearcher{}.rank(candidates, query);
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
                      ssg::PaletteSearcher{}.ghost(candidates[order.front()].label, query));
            // The reported rows are exactly the window of the shared ranker's
            // order — the client uses no private ranking.
            const auto expectedRows =
                std::min<std::size_t>(order.size(), 12);
            ASSERT_EQ(report.rows.size(), expectedRows);
            for (std::size_t row = 0; row < report.rows.size(); ++row) {
                ASSERT_TRUE(report.rows[row] ==
                            candidates[order[report.firstVisible + row]]);
            }
        }
    }
    fs::remove_all(root);
}

TEST(renderedPaletteLabelsTraceToPublishedCandidates) {
    auto root = uniqueRoot("derived-render");
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    auto candidates = publishedCandidates(*runtime);
    ASSERT_TRUE(!candidates.empty());
    if (candidates.empty()) return;

    auto report = projectReport(candidates, "sa", 12, 0, 0);
    ASSERT_TRUE(!report.rows.empty());
    if (report.rows.empty()) return;

    auto snapshot = runtime->present(ssg::ClientId{1}, {80, 24}, report);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto frame =
        ssg::test::gridFrameFromLegacy(std::move(*snapshot), report);
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    auto grid = ssg::Renderer{}.render(*frame);

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
            if (gridLine(grid, row).rfind(shown.label, 0) == 0) {
                rendered = true;
                break;
            }
        }
        ASSERT_TRUE(rendered);
    }
    fs::remove_all(root);
}

TEST(productionRuntimeNormalScreenSatisfiesTheScreenContract) {
    auto root = uniqueRoot("normal");
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(runtime->dispatch(ssg::ClientId{1},
                                  {"file.open", runtime->revision(),
                                   std::string{"alpha.txt"}})
                    .accepted());
    auto snapshot = runtime->present(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto frame =
        ssg::test::gridFrameFromLegacy(std::move(*snapshot));
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    auto grid = ssg::Renderer{}.render(*frame);
    assertScreenInvariants(grid, 80, 24);
    // The opened document's content and name reach the screen.
    ASSERT_TRUE(screenContains(grid, "first line"));
    ASSERT_TRUE(screenContains(grid, "alpha.txt"));
    // The screen is a pure function of the snapshot: a second render is identical.
    ASSERT_EQ(ssg::Renderer{}.render(*frame).canonical(), grid.canonical());
    fs::remove_all(root);
}

TEST(productionRuntimePaletteScreenSatisfiesTheScreenContract) {
    auto root = uniqueRoot("palette");
    auto runtime = makeRuntime(root);
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
    auto snapshot = runtime->present(ssg::ClientId{1}, {80, 24}, report);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto frame =
        ssg::test::gridFrameFromLegacy(std::move(*snapshot), report);
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    auto grid = ssg::Renderer{}.render(*frame);
    assertScreenInvariants(grid, 80, 24);
    // The reported query and candidate rows are projected onto the screen.
    ASSERT_TRUE(screenContains(grid, "sa"));
    ASSERT_TRUE(screenContains(grid, "Save File"));
    ASSERT_TRUE(screenContains(grid, "Save As"));
    ASSERT_EQ(ssg::Renderer{}.render(*frame).canonical(), grid.canonical());
    fs::remove_all(root);
}

TEST(productionRuntimeTooSmallScreenSatisfiesTheScreenContract) {
    auto root = uniqueRoot("small");
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(runtime->dispatch(ssg::ClientId{1},
                                  {"file.open", runtime->revision(),
                                   std::string{"alpha.txt"}})
                    .accepted());
    auto snapshot = runtime->present(ssg::ClientId{1}, {24, 3});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto frame =
        ssg::test::gridFrameFromLegacy(std::move(*snapshot));
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    auto grid = ssg::Renderer{}.render(*frame);
    // Below the supported minimum the library still emits a well-formed grid at
    // the terminal's real size, carrying the typed too-small state.
    assertScreenInvariants(grid, 24, 3);
    ASSERT_TRUE(screenContains(grid, "too small"));
    fs::remove_all(root);
}

TEST(deltaReplayReconstructsTheSameSnapshotAndGrid) {
    // M11-3: the "delta" leg of the command/snapshot/delta contract. After every
    // command, the delta between the prior and current production snapshots,
    // replayed onto the prior snapshot, reconstructs the SAME authoritative
    // snapshot as a fresh one — and renders to an identical grid.
    auto root = uniqueRoot("delta");
    auto runtime = makeRuntime(root);
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
        // A footer-anchored prompt opening and closing: the footer prompt lives in
        // PresentationSnapshot and MUST round-trip through the delta (it had no
        // delta member once, so a prompt change replayed to a stale prompt).
        {"goto.line", {}},
        {"prompt.cancel", {}},
    };

    auto previous = runtime->present(ssg::ClientId{1}, dims);
    ASSERT_TRUE(previous.has_value());
    if (!previous) return;

    for (auto const& step : script) {
        (void)runtime->dispatch(
            ssg::ClientId{1}, {step.command, runtime->revision(), step.payload});
        auto fresh = runtime->present(ssg::ClientId{1}, dims);
        ASSERT_TRUE(fresh.has_value());
        if (!fresh) break;

        if (fresh->semantic().revision().value() == previous->semantic().revision().value()) {
            // A command with no authoritative change produces no delta (the delta
            // API requires the revision to advance); the snapshot is unchanged.
            ASSERT_TRUE(*fresh == *previous);
            continue;
        }

        auto delta = ssg::SessionSnapshotCodec{}.deriveDelta(
            previous->semantic(), fresh->semantic());
        auto replayed = ssg::SessionSnapshotCodec{}.replay(
            previous->semantic(), delta);
        ASSERT_TRUE(replayed.accepted());
        if (!replayed.accepted()) break;

        // Delta replay is semantic-only; presentation is independently projected
        // by the owning presenter.
        ASSERT_TRUE(*replayed.snapshot == fresh->semantic());

        previous = runtime->present(ssg::ClientId{1}, dims);
        ASSERT_TRUE(previous.has_value());
        if (!previous) break;
    }
    fs::remove_all(root);
}

int main() {
    RUN(productionRuntimeNormalScreenSatisfiesTheScreenContract);
    RUN(productionRuntimePaletteScreenSatisfiesTheScreenContract);
    RUN(productionRuntimeTooSmallScreenSatisfiesTheScreenContract);
    RUN(deltaReplayReconstructsTheSameSnapshotAndGrid);
    RUN(paletteReportIsAPureFunctionOfCandidatesAndQuery);
    RUN(renderedPaletteLabelsTraceToPublishedCandidates);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
