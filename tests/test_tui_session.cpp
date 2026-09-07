#include "test_helpers.h"
#include "grid_test_frame.h"
#include "tui_fixture.h"

#include <ssg/Renderer.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

struct RuntimeFixture {
    RuntimeFixture() {
        root = testRuntimePath("tui_runtime");
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "workspace");
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

TEST(terminalEventsResolveThroughRuntimeInput) {
    RuntimeFixture fixture;
    ASSERT_TRUE(fixture.runtime->dispatch({"file.new", {}}).accepted());

    auto text = fixture.runtime->input(
        ssg::ClientKeyInput{{}, "hello"});
    ASSERT_EQ(text.outcome, ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(fixture.runtime->activeDocumentText(), std::string{"hello"});

    auto close = fixture.runtime->input(ssg::ClientKeyInput{
        ssg::KeyStroke{ssg::KeyCode::KeyW, true, false, false}, {}});
    ASSERT_EQ(close.outcome, ssg::ClientInputOutcome::Dispatched);
    auto presentation = ssg::test::projectGridFrame(*fixture.runtime);
    ASSERT_TRUE(presentation && presentation->tabs.tabs.empty());
}

TEST(realRuntimeSnapshotRendersDeterministicallyWithinTheme) {
    ssg::LineLayoutCache lineCache;
    RuntimeFixture fixture;
    ssg::tui::TuiClient client{
        *fixture.runtime,
        ssg::ViewportDimensions{80, 24}};
    ASSERT_TRUE(client.submit("file.new").accepted());
    ASSERT_TRUE(client
                    .submit("text.insert",
                            ssg::TextInputArguments{"rendered text"})
                    .accepted());

    auto screen = ssg::renderFrame(client.snapshot(), lineCache);
    ASSERT_EQ(screen.colors.size(), ssg::kThemeColorSlotCount);
    for (auto const& cell : screen.cells) {
        ASSERT_TRUE(cell.foreground < ssg::kThemeColorSlotCount);
        ASSERT_TRUE(cell.background < ssg::kThemeColorSlotCount);
    }
    ASSERT_EQ(ssg::renderFrame(client.snapshot(), lineCache).canonical(),
              screen.canonical());
}

}  // namespace

SSG_TEST_SUITE(test_tui_fixture) {
    RUN(terminalEventsResolveThroughRuntimeInput);
    RUN(realRuntimeSnapshotRendersDeterministicallyWithinTheme);
    return failed == 0 ? 0 : 1;
}
