#include "test_helpers.h"
#include "tui_fixture.h"

#include <ssg/Renderer.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

struct RuntimeFixture {
    RuntimeFixture() {
        root = std::filesystem::current_path() / "tui_runtime";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "workspace");
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

TEST(terminalEventsResolveThroughPublishedInputModels) {
    RuntimeFixture fixture;
    ssg::InvocationPrincipal const principal{
        ssg::ClientId{9}, ssg::InvocationOrigin::InProcess};
    ssg::tui::TuiClient client{
        *fixture.runtime, principal, ssg::ViewId{9},
        ssg::ViewportDimensions{80, 24}};
    ASSERT_TRUE(client.submit("file.new").accepted());

    auto const keymap = client.snapshot().sections().keymap;
    ssg::tui::TerminalInputCapture capture;
    auto text = ssg::CommittedText::fromUtf8("hello");
    ASSERT_TRUE(text.has_value());
    auto textCommand = capture.capture(*text, keymap, "editor");
    ASSERT_TRUE(textCommand.has_value());
    ASSERT_EQ(textCommand->commandId, std::string{"text.insert"});
    ASSERT_TRUE(client.submit(*textCommand).accepted());
    ASSERT_EQ(client.snapshot().sections().document.text,
              std::string{"hello"});

    auto close = capture.capture(
        ssg::KeyStroke{ssg::KeyCode::KeyW, false, true, false, false},
        keymap, "*");
    ASSERT_TRUE(close.has_value());
    ASSERT_EQ(close->commandId, std::string{"tab.close"});
    ASSERT_TRUE(client.submit(*close).accepted());
    ASSERT_TRUE(client.snapshot().sections().tabs.tabs.empty());

    ssg::SemanticHitTarget target{
        9, ssg::HitTargetKind::Tab, "Reopen",
        {"tab.reopen_closed", {}}};
    auto hit = capture.capture(target, keymap, "*");
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(hit->commandId, std::string{"tab.reopen_closed"});
    ASSERT_TRUE(client.submit(*hit).accepted());
    ASSERT_FALSE(client.snapshot().sections().tabs.tabs.empty());
}

TEST(realRuntimeSnapshotRendersDeterministicallyWithinTheme) {
    RuntimeFixture fixture;
    ssg::InvocationPrincipal const principal{
        ssg::ClientId{3}, ssg::InvocationOrigin::InProcess};
    ssg::tui::TuiClient client{
        *fixture.runtime, principal, ssg::ViewId{3},
        ssg::ViewportDimensions{80, 24}};
    ASSERT_TRUE(client.submit("file.new").accepted());
    ASSERT_TRUE(client
                    .submit("text.insert",
                            ssg::TextInputArguments{"rendered text"})
                    .accepted());

    auto screen = ssg::Renderer{}.render(client.snapshot());
    ASSERT_EQ(screen.colors.size(), ssg::kThemeColorSlotCount);
    for (auto const& cell : screen.cells) {
        ASSERT_TRUE(cell.foreground < ssg::kThemeColorSlotCount);
        ASSERT_TRUE(cell.background < ssg::kThemeColorSlotCount);
    }
    ASSERT_EQ(ssg::Renderer{}.render(client.snapshot()).canonical(),
              screen.canonical());
}

}  // namespace

int main() {
    RUN(terminalEventsResolveThroughPublishedInputModels);
    RUN(realRuntimeSnapshotRendersDeterministicallyWithinTheme);
    return failed == 0 ? 0 : 1;
}
