#include "test_helpers.h"

#include <ssg/EditorSession.h>

#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace {

struct SessionFixture {
    std::filesystem::path root;
    std::unique_ptr<ssg::EditorSession> session;

    ~SessionFixture() {
        session.reset();
        std::filesystem::remove_all(root);
    }
};

SessionFixture sessionFixture(std::string_view name) {
    auto root = testRuntimePath("pane_topology_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    auto created = ssg::EditorSession::create(
        {.cwd = root / "workspace",
         .scratchRoot = root / "scratch",
         .recoveryRoot = root / "recovery",
         .enableGitDiffWorker = false,
         .enableFilesystemWatcher = false});
    return {std::move(root), std::move(created.session)};
}

TEST(initialTopologyIsPublished) {
    auto fixture = sessionFixture("initial");
    ASSERT_TRUE(fixture.session != nullptr);
    if (!fixture.session) return;

    auto snapshot = fixture.session->snapshot();
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto& panes = snapshot->topology().panes;
    ASSERT_TRUE(panes.root().isLeaf());
    ASSERT_EQ(panes.root().id, ssg::PaneId{1});
    ASSERT_EQ(panes.activePane(), ssg::PaneId{1});
    ASSERT_EQ(panes.panes(), std::vector<ssg::PaneId>{ssg::PaneId{1}});
}

TEST(paneCommandsMutatePublishedTopologyAndRevision) {
    auto fixture = sessionFixture("commands");
    ASSERT_TRUE(fixture.session != nullptr);
    if (!fixture.session) return;
    auto& session = *fixture.session;

    auto revision = session.revision();
    auto split = session.dispatch({"pane.split_horizontal", revision, {}});
    ASSERT_TRUE(split.completed());
    ASSERT_FALSE(split.viewAction.has_value());
    ASSERT_EQ(session.revision(), ssg::Revision{revision.value() + 1});

    auto snapshot = session.snapshot();
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto& panes = snapshot->topology().panes;
    ASSERT_FALSE(panes.root().isLeaf());
    ASSERT_EQ(panes.root().axis, ssg::SplitAxis::Horizontal);
    ASSERT_EQ(panes.root().children.size(), std::size_t{2});
    ASSERT_EQ(panes.panes(),
              (std::vector<ssg::PaneId>{ssg::PaneId{1}, ssg::PaneId{2}}));
    ASSERT_EQ(panes.activePane(), ssg::PaneId{2});

    revision = session.revision();
    auto next =
        session.dispatch({"pane.next", revision, {}});
    ASSERT_TRUE(next.completed());
    ASSERT_EQ(session.revision(), ssg::Revision{revision.value() + 1});
    snapshot = session.snapshot();
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->topology().panes.activePane(), ssg::PaneId{1});
    ASSERT_EQ(snapshot->sections().followEdits.mode, ssg::FollowMode::Paused);

    auto previous =
        session.dispatch({"pane.previous", session.revision(), {}});
    ASSERT_TRUE(previous.completed());
    snapshot = session.snapshot();
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->topology().panes.activePane(), ssg::PaneId{2});
}

TEST(closeUsesStableOrderAndRejectsTheOnlyPane) {
    auto fixture = sessionFixture("close");
    ASSERT_TRUE(fixture.session != nullptr);
    if (!fixture.session) return;
    auto& session = *fixture.session;
    ASSERT_TRUE(session
                    .dispatch({"pane.split_vertical", session.revision(), {}})
                    .completed());

    auto focus = session.input(ssg::ViewTransitionInput{
                    {session.revision()},
                    ssg::PaneFocusTransition{ssg::PaneId{1}}});
    ASSERT_EQ(focus.outcome, ssg::ClientInputOutcome::Dispatched);
    auto closed =
        session.dispatch({"pane.close", session.revision(), {}});
    ASSERT_TRUE(closed.completed());
    auto snapshot = session.snapshot();
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->topology().panes.panes(),
              std::vector<ssg::PaneId>{ssg::PaneId{2}});
    ASSERT_EQ(snapshot->topology().panes.activePane(), ssg::PaneId{2});

    const auto revision = session.revision();
    auto rejected =
        session.dispatch({"pane.close", revision, {}});
    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error, ssg::CommandError::HandlerFailed);
    ASSERT_EQ(session.revision(), revision);
    snapshot = session.snapshot();
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) {
        ASSERT_EQ(snapshot->topology().panes.panes(),
                  std::vector<ssg::PaneId>{ssg::PaneId{2}});
    }
}

TEST(focusRejectsAnUnknownPaneIdentity) {
    auto fixture = sessionFixture("focus");
    ASSERT_TRUE(fixture.session != nullptr);
    if (!fixture.session) return;
    auto& session = *fixture.session;
    ASSERT_TRUE(session
                    .dispatch({"pane.split_horizontal", session.revision(), {}})
                    .completed());

    const auto revision = session.revision();
    auto focused = session.input(ssg::ViewTransitionInput{
            {revision}, ssg::PaneFocusTransition{ssg::PaneId{1}}});
    ASSERT_EQ(focused.outcome, ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(session.revision(), ssg::Revision{revision.value() + 1});
    auto snapshot = session.snapshot();
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->topology().panes.activePane(), ssg::PaneId{1});

    const auto focusRevision = session.revision();
    auto rejected = session.input(ssg::ViewTransitionInput{
            {focusRevision}, ssg::PaneFocusTransition{ssg::PaneId{999}}});
    ASSERT_EQ(rejected.outcome, ssg::ClientInputOutcome::Rejected);
    ASSERT_EQ(session.revision(), focusRevision);
    snapshot = session.snapshot();
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) {
        ASSERT_EQ(snapshot->topology().panes.activePane(), ssg::PaneId{1});
    }
}

}  // namespace

SSG_TEST_SUITE(test_pane_topology) {
    RUN(initialTopologyIsPublished);
    RUN(paneCommandsMutatePublishedTopologyAndRevision);
    RUN(closeUsesStableOrderAndRejectsTheOnlyPane);
    RUN(focusRejectsAnUnknownPaneIdentity);
    return failed;
}
