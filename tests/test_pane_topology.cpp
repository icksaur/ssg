#include "test_helpers.h"
#include "grid_test_frame.h"

#include <ssg/Editor.h>

#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace {

struct SessionFixture {
    std::filesystem::path root;
    std::unique_ptr<ssg::Editor> session;

    ~SessionFixture() {
        session.reset();
        std::filesystem::remove_all(root);
    }
};

SessionFixture sessionFixture(std::string_view name) {
    auto root = testRuntimePath("pane_topology_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    auto created = ssg::createEditor(
        {.cwd = root / "workspace",
         .scratchRoot = root / "scratch",
         .recoveryRoot = root / "recovery",
         .enableGitDiffWorker = false,
         .enableFilesystemWatcher = false});
    return {std::move(root), std::move(created.session)};
}

std::vector<ssg::PaneId> paneIds(const ssg::GridPresentation& presentation) {
    std::vector<ssg::PaneId> ids;
    if (!presentation.document) return ids;
    for (const auto& pane : presentation.document->panes) {
        ids.push_back(pane.id);
    }
    return ids;
}

std::optional<ssg::PaneId> activePane(
    const ssg::GridPresentation& presentation) {
    if (!presentation.document || presentation.document->panes.empty()) {
        return std::nullopt;
    }
    return presentation.document
        ->panes[presentation.document->activePaneIndex]
        .id;
}

TEST(initialTopologyIsPublished) {
    auto fixture = sessionFixture("initial");
    ASSERT_TRUE(fixture.session != nullptr);
    if (!fixture.session) return;

    auto snapshot = ssg::test::projectGridFrame(*fixture.session);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(activePane(*snapshot), std::optional{ssg::PaneId{1}});
    ASSERT_EQ(paneIds(*snapshot),
              std::vector<ssg::PaneId>{ssg::PaneId{1}});
}

TEST(paneCommandsMutatePresentedTopology) {
    auto fixture = sessionFixture("commands");
    ASSERT_TRUE(fixture.session != nullptr);
    if (!fixture.session) return;
    auto& session = *fixture.session;

    auto split = session.dispatch({"pane.split_horizontal", {}});
    ASSERT_TRUE(split.completed());
    ASSERT_FALSE(split.viewAction.has_value());

    auto snapshot = ssg::test::projectGridFrame(session);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(paneIds(*snapshot),
              (std::vector<ssg::PaneId>{ssg::PaneId{1}, ssg::PaneId{2}}));
    ASSERT_EQ(activePane(*snapshot), std::optional{ssg::PaneId{2}});

    auto next = session.dispatch({"pane.next", {}});
    ASSERT_TRUE(next.completed());
    snapshot = ssg::test::projectGridFrame(session);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(activePane(*snapshot), std::optional{ssg::PaneId{1}});
    ASSERT_EQ(snapshot->followMode, ssg::FollowMode::Paused);

    auto previous =
        session.dispatch({"pane.previous",  {}});
    ASSERT_TRUE(previous.completed());
    snapshot = ssg::test::projectGridFrame(session);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(activePane(*snapshot), std::optional{ssg::PaneId{2}});
}

TEST(closeUsesStableOrderAndRejectsTheOnlyPane) {
    auto fixture = sessionFixture("close");
    ASSERT_TRUE(fixture.session != nullptr);
    if (!fixture.session) return;
    auto& session = *fixture.session;
    ASSERT_TRUE(session
                    .dispatch({"pane.split_vertical",  {}})
                    .completed());

    auto focus = session.input(ssg::ViewTransitionInput{
                    ssg::PaneFocusTransition{ssg::PaneId{1}}});
    ASSERT_EQ(focus.outcome, ssg::ClientInputOutcome::Dispatched);
    auto closed =
        session.dispatch({"pane.close",  {}});
    ASSERT_TRUE(closed.completed());
    auto snapshot = ssg::test::projectGridFrame(session);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(paneIds(*snapshot),
              std::vector<ssg::PaneId>{ssg::PaneId{2}});
    ASSERT_EQ(activePane(*snapshot), std::optional{ssg::PaneId{2}});

    auto rejected = session.dispatch({"pane.close", {}});
    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error, ssg::CommandError::HandlerFailed);
    snapshot = ssg::test::projectGridFrame(session);
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) {
        ASSERT_EQ(paneIds(*snapshot),
                  std::vector<ssg::PaneId>{ssg::PaneId{2}});
    }
}

TEST(focusRejectsAnUnknownPaneIdentity) {
    auto fixture = sessionFixture("focus");
    ASSERT_TRUE(fixture.session != nullptr);
    if (!fixture.session) return;
    auto& session = *fixture.session;
    ASSERT_TRUE(session
                    .dispatch({"pane.split_horizontal",  {}})
                    .completed());

    auto focused = session.input(ssg::ViewTransitionInput{
            ssg::PaneFocusTransition{ssg::PaneId{1}}});
    ASSERT_EQ(focused.outcome, ssg::ClientInputOutcome::Dispatched);
    auto snapshot = ssg::test::projectGridFrame(session);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(activePane(*snapshot), std::optional{ssg::PaneId{1}});

    auto rejected = session.input(ssg::ViewTransitionInput{
            ssg::PaneFocusTransition{ssg::PaneId{999}}});
    ASSERT_EQ(rejected.outcome, ssg::ClientInputOutcome::Rejected);
    snapshot = ssg::test::projectGridFrame(session);
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) {
        ASSERT_EQ(activePane(*snapshot), std::optional{ssg::PaneId{1}});
    }
}

}  // namespace

SSG_TEST_SUITE(test_pane_topology) {
    RUN(initialTopologyIsPublished);
    RUN(paneCommandsMutatePresentedTopology);
    RUN(closeUsesStableOrderAndRejectsTheOnlyPane);
    RUN(focusRejectsAnUnknownPaneIdentity);
    return failed;
}
