#include "../test_helpers.h"
#include "../grid_test_frame.h"

#include <ssg/Editor.h>
#include <ssg/Renderer.h>
#include <ssg/Viewport.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// M9-T layout totality.
//
// The library layout is dimension-parametric and must be TOTAL: for any viewport
// it produces a well-formed frame, including the typed too-small presentation,
// and render() never throws. We sweep sizes crossed with UI states and documents
// so a resize storm can never crash the app or produce out-of-bounds geometry.

namespace {

namespace fs = std::filesystem;

fs::path makeRoot(const std::string& name) {
    auto root = testRuntimePath("runtime_totality_" + name);
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
    ssg::LineLayoutCache lineCache;
    auto root = makeRoot(state.name);
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) { fs::remove_all(root); return; }
    auto& runtime = *created.session;
    if (state.openDocument) {
        (void)ssg::test::openFile(runtime, std::string{"doc.txt"});
    }
    for (auto const& command : state.commands) {
        (void)runtime.dispatch({command,  {}});
    }

    for (auto const dims : sweep()) {
        auto frame = ssg::test::projectGridFrame(runtime, dims);
        ASSERT_TRUE(frame.has_value());
        if (!frame) continue;
        ASSERT_NO_THROW(ssg::renderFrame(*frame, lineCache));
        auto grid = ssg::renderFrame(*frame, lineCache);
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

SSG_TEST_SUITE(test_session_totality) {
    RUN(layoutIsTotalAcrossSizesAndUiStates);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
