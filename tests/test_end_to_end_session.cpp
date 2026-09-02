#include "test_helpers.h"
#include "grid_test_frame.h"
#include "tui_fixture.h"

#include <ssg/EditorSession.h>
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
    ssg::Revision revision;
    std::string text;
    std::string label;
    std::size_t selectionCount;
    bool dirty;
    bool tabOpen;
    bool wordWrap;

    bool operator==(CanonicalState const&) const = default;
};

template <typename Snapshot>
CanonicalState canonical(Snapshot const& snapshot) {
    auto const& sections = snapshot.semantic().sections();
    auto const* tab =
        sections.tabs.tabs.empty() ? nullptr : &sections.tabs.tabs.front();
    bool wordWrap = false;
    for (auto const& node : sections.uiFrame.state().nodes) {
        wordWrap = wordWrap ||
                   (node.leaf && node.leaf->label == "Word wrap on");
    }
    return {snapshot.semantic().revision(),
            sections.document.text,
            tab ? tab->label : std::string{},
            sections.selection.items().size(),
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
        auto created = ssg::EditorSession::create(
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
    std::unique_ptr<ssg::EditorSession> runtime;
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
    ssg::InvocationPrincipal const directPrincipal{
        ssg::ClientId{1}, ssg::InvocationOrigin::InProcess};
    ssg::InvocationPrincipal const tuiPrincipal{
        ssg::ClientId{2}, ssg::InvocationOrigin::InProcess};
    ASSERT_TRUE(direct.attach(directPrincipal, ssg::ViewId{1}).accepted());
    ssg::tui::TuiClient client{
        tui, tuiPrincipal, ssg::ViewId{2}, ssg::ViewportDimensions{80, 24}};

    auto directSnapshot = ssg::test::projectGridFrame(
        direct, directPrincipal.clientId(), ssg::ViewId{1}, {80, 24});
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
            directPrincipal.clientId(),
            {step.command, direct.revision(), step.payload});
        auto tuiResult = client.submit(step.command, step.payload);
        ASSERT_TRUE(directResult.accepted());
        ASSERT_TRUE(tuiResult.accepted());
        directSnapshot = ssg::test::projectGridFrame(
            direct, directPrincipal.clientId(), ssg::ViewId{1}, {80, 24});
        ASSERT_TRUE(directSnapshot.has_value());
        ASSERT_EQ(canonical(*directSnapshot), canonical(client.snapshot()));
    }
}

}  // namespace

int main() {
    RUN(directAndTuiClientsMatchThroughRealRuntimeSnapshots);
    return failed == 0 ? 0 : 1;
}
