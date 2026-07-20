#include "../test_helpers.h"

#include <ssg/editor_runtime.h>
#include <ssg/render.h>
#include <ssg/ui_layout.h>
#include <ssg/viewport.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// M9-T layout totality (doc/spec-terminal-robustness.md).
//
// The library layout is dimension-parametric and must be TOTAL: for any viewport
// it produces either a well-formed snapshot (>= the 20x4 minimum) or the typed
// "too small" outcome (snapshot() == nullopt), and render() never throws on a
// snapshot it hands back.  We sweep sizes crossed with UI states and documents so
// a resize storm (which sweeps through 1-row / 1-col transients) can never crash
// the app or produce out-of-bounds geometry.

namespace {

namespace fs = std::filesystem;

constexpr int kMinimumColumns = 20;
constexpr int kMinimumRows = 4;

fs::path makeRoot(const std::string& name) {
    auto root = fs::current_path() / ("runtime_totality_" + name);
    fs::remove_all(root);
    fs::create_directories(root / "workspace");
    fs::create_directories(root / "scratch");
    fs::create_directories(root / "recovery");
    std::string text;
    for (int i = 0; i < 200; ++i) text += "the quick brown fox jumps\n";
    std::ofstream{root / "workspace" / "doc.txt", std::ios::binary} << text;
    return root;
}

// Every dimension in the sweep, including degenerate 1-cell axes, the boundary
// rows/cols around 20x4, and realistic sizes.
const std::vector<ssg::ViewportDimensions>& sweep() {
    static const std::vector<ssg::ViewportDimensions> dims{
        {1, 1},   {2, 1},   {1, 2},   {19, 4},  {20, 3},  {4, 20},
        {20, 4},  {21, 5},  {8, 3},   {80, 24}, {200, 60}, {132, 43},
    };
    return dims;
}

bool within(const ssg::Rect& rect, int cols, int rows) {
    return rect.x >= 0 && rect.y >= 0 && rect.width >= 0 && rect.height >= 0 &&
           rect.right() <= cols && rect.bottom() <= rows;
}

// Assert every published region rectangle lies inside the viewport.
void assertRegionsInBounds(const ssg::ShellViewState& shell) {
    int const cols = shell.viewport.columns;
    int const rows = shell.viewport.rows;
    if (shell.header) ASSERT_TRUE(within(*shell.header, cols, rows));
    if (shell.footer) ASSERT_TRUE(within(*shell.footer, cols, rows));
    if (shell.tabBar) ASSERT_TRUE(within(*shell.tabBar, cols, rows));
    if (shell.panel) ASSERT_TRUE(within(*shell.panel, cols, rows));
    if (shell.panelScrollbar) ASSERT_TRUE(within(*shell.panelScrollbar, cols, rows));
    if (shell.prompt) ASSERT_TRUE(within(*shell.prompt, cols, rows));
    for (auto const& pane : shell.panes) {
        ASSERT_TRUE(within(pane.frame, cols, rows));
        ASSERT_TRUE(within(pane.content, cols, rows));
        ASSERT_TRUE(within(pane.scrollbar, cols, rows));
    }
    for (auto const& hit : shell.tabHits) ASSERT_TRUE(within(hit.rect, cols, rows));
    for (auto const& node : shell.accessibilityNodes) {
        ASSERT_TRUE(within(node.rect, cols, rows));
    }
}

// One UI-state configuration, applied to a fresh runtime by dispatching a command
// sequence.  Named so a failure points at the state.
struct UiState {
    const char* name;
    std::vector<std::string> commands;
    bool openDocument;
};

const std::vector<UiState>& uiStates() {
    static const std::vector<UiState> states{
        {"default_empty", {}, false},
        {"default_doc", {}, true},
        {"panel_toggled", {"panel.toggle"}, true},
        {"palette_open", {"palette.open"}, true},
        {"find_open", {"find.open"}, true},
        {"distraction_free", {"view.toggle_distraction_free"}, true},
        {"split_horizontal", {"pane.split_horizontal"}, true},
        {"split_vertical", {"pane.split_vertical"}, true},
        {"panel_and_split", {"panel.toggle", "pane.split_vertical"}, true},
    };
    return states;
}

void runState(const UiState& state) {
    auto root = makeRoot(state.name);
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) { fs::remove_all(root); return; }
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    if (state.openDocument) {
        (void)runtime.dispatch(ssg::ClientId{1},
                               {"file.open", runtime.revision(), std::string{"doc.txt"}});
    }
    for (auto const& command : state.commands) {
        (void)runtime.dispatch(ssg::ClientId{1}, {command, runtime.revision(), {}});
    }

    for (auto const dims : sweep()) {
        // snapshot() is total: it always returns a value.  The typed "too small"
        // outcome is a zeroed shell viewport ({0,0}); a laid-out shell carries the
        // requested dimensions.
        auto snapshot = runtime.snapshot(ssg::ClientId{1}, dims);
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot.has_value()) continue;
        auto const& shell = snapshot->sections().shell;

        bool const belowMinimum =
            static_cast<int>(dims.columns) < kMinimumColumns ||
            static_cast<int>(dims.rows) < kMinimumRows;
        bool const laidOut =
            shell.viewport.columns > 0 && shell.viewport.rows > 0;

        // Hard boundary: any viewport below 20x4 is ALWAYS too small, regardless
        // of UI state.
        if (belowMinimum) {
            ASSERT_FALSE(laidOut);
            ASSERT_EQ(shell.viewport.columns, 0);
            ASSERT_EQ(shell.viewport.rows, 0);
        }

        // Positive side of the oracle: a roomy viewport (>= 80x24) must lay out in
        // every UI state — otherwise an "always too small" regression would pass
        // the negative checks alone.
        if (static_cast<int>(dims.columns) >= 80 &&
            static_cast<int>(dims.rows) >= 24) {
            ASSERT_TRUE(laidOut);
        }

        // A too-small snapshot (e.g. a prompt-open state at a height that leaves
        // no content row) is a valid typed outcome: the app shows a placeholder
        // and must NOT call render(), which requires a positive viewport.
        if (!laidOut) {
            ASSERT_EQ(shell.viewport.columns, 0);
            ASSERT_EQ(shell.viewport.rows, 0);
            continue;
        }

        // Laid out: geometry is well-formed and render() is total.
        ASSERT_EQ(shell.viewport.columns, static_cast<int>(dims.columns));
        ASSERT_EQ(shell.viewport.rows, static_cast<int>(dims.rows));
        assertRegionsInBounds(shell);

        ASSERT_NO_THROW(ssg::render(*snapshot));
        auto grid = ssg::render(*snapshot);
        ASSERT_EQ(grid.size.columns, static_cast<int>(dims.columns));
        ASSERT_EQ(grid.size.rows, static_cast<int>(dims.rows));
        ASSERT_EQ(grid.cells.size(),
                  static_cast<std::size_t>(dims.columns) * dims.rows);
        if (grid.caret) {
            ASSERT_TRUE(grid.caret->column >= 0 &&
                        grid.caret->column < static_cast<int>(dims.columns));
            ASSERT_TRUE(grid.caret->row >= 0 &&
                        grid.caret->row < static_cast<int>(dims.rows));
        }
    }
    fs::remove_all(root);
}

}  // namespace

TEST(layoutIsTotalAcrossSizesAndUiStates) {
    for (auto const& state : uiStates()) {
        runState(state);
    }
}

int main() {
    RUN(layoutIsTotalAcrossSizesAndUiStates);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
