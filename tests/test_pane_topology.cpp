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
    auto root =
        std::filesystem::current_path() / ("pane_topology_" + std::string{name});
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

ssg::InvocationPrincipal principal(ssg::ClientId client) {
    return {client, ssg::InvocationOrigin::InProcess};
}

TEST(initialTopologyIsPublishedPerAttachment) {
    auto fixture = sessionFixture("initial");
    ASSERT_TRUE(fixture.session != nullptr);
    if (!fixture.session) return;
    const ssg::ClientId client{1};
    ASSERT_TRUE(
        fixture.session->attach(principal(client), ssg::ViewId{1}).accepted());

    auto snapshot = fixture.session->snapshot(client);
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
    const ssg::ClientId client{1};
    ASSERT_TRUE(session.attach(principal(client), ssg::ViewId{1}).accepted());

    auto revision = session.revision();
    auto split = session.dispatch(
        client, {"pane.split_horizontal", revision, {}});
    ASSERT_TRUE(split.completed());
    ASSERT_FALSE(split.viewAction.has_value());
    ASSERT_EQ(session.revision(), ssg::Revision{revision.value() + 1});

    auto snapshot = session.snapshot(client);
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
        session.dispatch(client, {"pane.next", revision, {}});
    ASSERT_TRUE(next.completed());
    ASSERT_EQ(session.revision(), ssg::Revision{revision.value() + 1});
    snapshot = session.snapshot(client);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->topology().panes.activePane(), ssg::PaneId{1});
    ASSERT_EQ(snapshot->sections().followEdits.mode, ssg::FollowMode::Paused);

    auto previous =
        session.dispatch(client, {"pane.previous", session.revision(), {}});
    ASSERT_TRUE(previous.completed());
    snapshot = session.snapshot(client);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->topology().panes.activePane(), ssg::PaneId{2});
}

TEST(closeUsesStableOrderAndRejectsTheOnlyPane) {
    auto fixture = sessionFixture("close");
    ASSERT_TRUE(fixture.session != nullptr);
    if (!fixture.session) return;
    auto& session = *fixture.session;
    const ssg::ClientId client{1};
    ASSERT_TRUE(session.attach(principal(client), ssg::ViewId{1}).accepted());
    ASSERT_TRUE(session
                    .dispatch(client,
                              {"pane.split_vertical", session.revision(), {}})
                    .completed());

    auto focus = session.input(
        client, ssg::ViewTransitionInput{
                    {session.revision()},
                    ssg::PaneFocusTransition{ssg::PaneId{1}}});
    ASSERT_EQ(focus.outcome, ssg::ClientInputOutcome::Dispatched);
    auto closed =
        session.dispatch(client, {"pane.close", session.revision(), {}});
    ASSERT_TRUE(closed.completed());
    auto snapshot = session.snapshot(client);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->topology().panes.panes(),
              std::vector<ssg::PaneId>{ssg::PaneId{2}});
    ASSERT_EQ(snapshot->topology().panes.activePane(), ssg::PaneId{2});

    const auto revision = session.revision();
    auto rejected =
        session.dispatch(client, {"pane.close", revision, {}});
    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error, ssg::CommandError::HandlerFailed);
    ASSERT_EQ(session.revision(), revision);
    snapshot = session.snapshot(client);
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) {
        ASSERT_EQ(snapshot->topology().panes.panes(),
                  std::vector<ssg::PaneId>{ssg::PaneId{2}});
    }
}

TEST(focusByIdentityValidatesTheCallingAttachment) {
    auto fixture = sessionFixture("focus");
    ASSERT_TRUE(fixture.session != nullptr);
    if (!fixture.session) return;
    auto& session = *fixture.session;
    const ssg::ClientId client{1};
    ASSERT_TRUE(session.attach(principal(client), ssg::ViewId{1}).accepted());
    ASSERT_TRUE(session
                    .dispatch(client,
                              {"pane.split_horizontal", session.revision(), {}})
                    .completed());

    const auto revision = session.revision();
    auto focused = session.input(
        client,
        ssg::ViewTransitionInput{
            {revision}, ssg::PaneFocusTransition{ssg::PaneId{1}}});
    ASSERT_EQ(focused.outcome, ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(session.revision(), ssg::Revision{revision.value() + 1});
    auto snapshot = session.snapshot(client);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->topology().panes.activePane(), ssg::PaneId{1});

    const auto focusRevision = session.revision();
    auto rejected = session.input(
        client,
        ssg::ViewTransitionInput{
            {focusRevision}, ssg::PaneFocusTransition{ssg::PaneId{999}}});
    ASSERT_EQ(rejected.outcome, ssg::ClientInputOutcome::Rejected);
    ASSERT_EQ(session.revision(), focusRevision);
    snapshot = session.snapshot(client);
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) {
        ASSERT_EQ(snapshot->topology().panes.activePane(), ssg::PaneId{1});
    }
}

TEST(attachmentsHaveIsolatedPaneTopologies) {
    auto fixture = sessionFixture("isolation");
    ASSERT_TRUE(fixture.session != nullptr);
    if (!fixture.session) return;
    auto& session = *fixture.session;
    const ssg::ClientId first{1};
    const ssg::ClientId second{2};
    ASSERT_TRUE(session.attach(principal(first), ssg::ViewId{1}).accepted());
    ASSERT_TRUE(session.attach(principal(second), ssg::ViewId{2}).accepted());
    ASSERT_TRUE(session
                    .dispatch(first,
                              {"pane.split_horizontal", session.revision(), {}})
                    .completed());

    auto firstSnapshot = session.snapshot(first);
    auto secondSnapshot = session.snapshot(second);
    ASSERT_TRUE(firstSnapshot.has_value());
    ASSERT_TRUE(secondSnapshot.has_value());
    if (!firstSnapshot || !secondSnapshot) return;
    ASSERT_EQ(firstSnapshot->topology().panes.panes().size(), std::size_t{2});
    ASSERT_EQ(secondSnapshot->topology().panes.panes().size(), std::size_t{1});
    ASSERT_EQ(firstSnapshot->revision(), secondSnapshot->revision());
}

}  // namespace

int main() {
    RUN(initialTopologyIsPublishedPerAttachment);
    RUN(paneCommandsMutatePublishedTopologyAndRevision);
    RUN(closeUsesStableOrderAndRejectsTheOnlyPane);
    RUN(focusByIdentityValidatesTheCallingAttachment);
    RUN(attachmentsHaveIsolatedPaneTopologies);
    return failed;
}
