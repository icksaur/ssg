#include "test_helpers.h"
#include "grid_test_frame.h"
#include "tui_fixture.h"

#include <ssg/Editor.h>
#include <ssg/TextInputCommands.h>

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

    const auto runCommand = [&](std::string command) {
        auto directResult = direct.dispatch(command);
        auto tuiResult = client.submit(std::move(command));
        ASSERT_TRUE(directResult.accepted());
        ASSERT_TRUE(tuiResult.accepted());
        directSnapshot = ssg::test::projectGridFrame(direct, {80, 24});
        ASSERT_TRUE(directSnapshot.has_value());
        ASSERT_EQ(canonical(*directSnapshot), canonical(client.snapshot()));
    };
    const auto runText = [&](std::string text) {
        auto directResult = direct.input(ssg::ClientKeyInput{{}, text});
        auto tuiResult = client.input(ssg::ClientKeyInput{{}, std::move(text)});
        ASSERT_TRUE(directResult.command && directResult.command->accepted());
        ASSERT_TRUE(tuiResult.command && tuiResult.command->accepted());
        directSnapshot = ssg::test::projectGridFrame(direct, {80, 24});
        ASSERT_TRUE(directSnapshot.has_value());
        ASSERT_EQ(canonical(*directSnapshot), canonical(client.snapshot()));
    };

    const auto openFile = [&] {
        auto directResult = ssg::test::openFile(direct, "doc.txt");
        auto tuiResult = ssg::test::openFile(tui, "doc.txt");
        ASSERT_TRUE(directResult.accepted());
        ASSERT_TRUE(tuiResult.accepted());
    };

    openFile();
    runText("!");
    runCommand("cursor.left");
    runText("?");
    runCommand("edit.undo");
    runCommand("edit.redo");
    runCommand("view.toggle_word_wrap");
    runCommand("tab.close");
    runCommand("tab.reopen_closed");
}

}  // namespace

SSG_TEST_SUITE(test_end_to_end) {
    RUN(directAndTuiClientsMatchThroughRealRuntimeSnapshots);
    return failed == 0 ? 0 : 1;
}
