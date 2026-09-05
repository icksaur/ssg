#include "test_helpers.h"
#include "grid_test_frame.h"
#include "tui_fixture.h"

#include <ssg/Editor.h>
#include <ssg/TextInputCommands.h>

#include <any>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

struct CanonicalState {
    std::string text;
    std::string label;
    std::size_t selectionCount;
    bool dirty;
    bool tabOpen;
    bool wordWrap;

    bool operator==(CanonicalState const&) const = default;
};

CanonicalState canonical(ssg::GridPresentation const& snapshot) {
    auto const* tab =
        snapshot.tabs.tabs.empty() ? nullptr : &snapshot.tabs.tabs.front();
    bool wordWrap = snapshot.wordWrap;
    const auto walk = [&](const auto& self, const ssg::UiNode& node) -> void {
        wordWrap = wordWrap ||
                   (node.resolved && node.resolved->label == "Word wrap on");
        if (const auto* container = std::get_if<ssg::UiContainer>(&node.content)) {
            for (auto const& child : container->children) self(self, child);
        }
    };
    walk(walk, snapshot.uiTree.root);
    return {snapshot.documentText,
            tab ? tab->label : std::string{},
            snapshot.selections.items().size(),
            tab ? tab->dirty : false,
            tab != nullptr,
            wordWrap};
}

struct RuntimeFixture {
    explicit RuntimeFixture(std::string name) {
        root = testRuntimePath(name);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "workspace");
        std::ofstream{root / "workspace" / "doc.txt"} << "alpha";
        auto created = ssg::createEditor(
            {.cwd = root / "workspace",
             .scratchRoot = root / "scratch",
             .recoveryRoot = root / "recovery",
             .enableGitDiffWorker = false,
             .enableFilesystemWatcher = false});
        if (!created.accepted()) {
            throw std::runtime_error{created.message};
        }
        runtime = std::move(created.session);
    }

    ~RuntimeFixture() { std::filesystem::remove_all(root); }

    std::filesystem::path root;
    std::unique_ptr<ssg::Editor> runtime;
};

struct Step {
    std::string command;
    std::any payload;
};

TEST(directAndTuiClientsMatchThroughRealRuntimeSnapshots) {
    RuntimeFixture directFixture{"e2e_runtime_direct"};
    RuntimeFixture tuiFixture{"e2e_runtime_tui"};
    auto& direct = *directFixture.runtime;
    auto& tui = *tuiFixture.runtime;
    ssg::tui::TuiClient client{
        tui, ssg::ViewportDimensions{80, 24}};

    auto directSnapshot = ssg::test::projectGridFrame(direct, {80, 24});
    ASSERT_TRUE(directSnapshot.has_value());
    ASSERT_EQ(canonical(*directSnapshot), canonical(client.snapshot()));

    std::vector<Step> steps;
    steps.push_back({"file.open", std::string{"doc.txt"}});
    steps.push_back({"text.insert", ssg::TextInputArguments{"!"}});
    steps.push_back({"cursor.left", {}});
    steps.push_back({"text.insert", ssg::TextInputArguments{"?"}});
    steps.push_back({"edit.undo", {}});
    steps.push_back({"edit.redo", {}});
    steps.push_back({"view.toggle_word_wrap", {}});
    steps.push_back({"tab.close", {}});
    steps.push_back({"tab.reopen_closed", {}});

    for (auto& step : steps) {
        auto directResult = direct.dispatch(
            {step.command,  step.payload});
        auto tuiResult = client.submit(step.command, step.payload);
        ASSERT_TRUE(directResult.accepted());
        ASSERT_TRUE(tuiResult.accepted());
        directSnapshot = ssg::test::projectGridFrame(direct, {80, 24});
        ASSERT_TRUE(directSnapshot.has_value());
        ASSERT_EQ(canonical(*directSnapshot), canonical(client.snapshot()));
    }
}

}  // namespace

SSG_TEST_SUITE(test_end_to_end) {
    RUN(directAndTuiClientsMatchThroughRealRuntimeSnapshots);
    return failed == 0 ? 0 : 1;
}
