#include "test_helpers.h"

#include <ssg/EditorSessionBuilder.h>
#include <ssg/session_snapshot.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {



ssg::SelectionSet selection(std::uint64_t byte,
                            std::uint32_t) {
    ssg::DocumentPosition const position{
        ssg::ByteOffset{byte}, ssg::LineIndex{0}, ssg::CellIndex{byte}};
    return ssg::SelectionSet{{ssg::Selection{position, position}}};
}

ssg::SessionSnapshotSections sections(ssg::Revision revision,
                                      std::string marker) {
    ssg::SettingsViewState settings;
    settings.entries[0].effective = {
        static_cast<std::uint32_t>(marker.size()), ssg::SettingScope::User};
    ssg::ThemeSnapshot theme;
    theme.roleColors[0].red = static_cast<std::uint8_t>(marker.size());

    ssg::ShellViewState shell;
    shell.viewport = {static_cast<int>(20 + marker.size()), 8};

    return {
        {revision, marker, ssg::ByteOffset{marker.size()}},
        selection(marker.size(), static_cast<std::uint32_t>(marker.size())),
        {true, false, marker.size()},
        {{marker}, marker, std::nullopt},
        {{{}, marker.size()}},
        {revision, true, marker, ssg::SearchMode::File, {}, std::nullopt,
         marker.size(), false},
        {marker.size(), true, false, revision, marker, {}, {}, {}, std::nullopt,
         ssg::FindReplaceError::None, {}},
        settings,
        {marker, {}},
        {{marker.size() > 1 ? ssg::TextEncoding::Utf16le
                           : ssg::TextEncoding::Utf8,
          ssg::LineEnding::Lf, false,
          !marker.empty()}},
        {{{ssg::TabId{1}, ssg::TabKind::ReadOnlyOutput, std::nullopt,
           std::nullopt, "output", marker, ssg::DocumentMode::ReadOnly,
           false, ssg::TabRecoveryBadge::None}},
         ssg::TabId{1}},
        {revision, {}},
        {revision, {}},
        {marker.size(), ssg::FollowMode::Following, ssg::PaneId{},
         std::nullopt, {}, {}},
        {ssg::TreeRevision{marker.size()}, {}},
        ssg::SyntaxViewState::plainText(revision, ssg::LanguageId{"plain"},
                                          marker, 4),
        {revision, {}},
        {revision, {}, std::nullopt, {}, marker},
        theme,
        ssg::FocusTarget::Editor,
    };
}

ssg::ViewportViewState clientView(std::uint32_t firstRow) {
    return {ssg::ViewportDimensions{20, 8},
            firstRow,
            0,
            firstRow + 8,
            {},
            {},
            {},
            {firstRow + 8, 8, firstRow, firstRow, 0, 8}};
}


// requiredCatalogEqualsAssembledRegistryExactly and
// bindingAnUndeclaredCommandIsRefusedAtTheBinding are deleted with the static
// table.  Both existed to prove a declaration and its handler agreed, which a
// single registration expression now makes true by construction

class TestServices final : public ssg::CommandServices {
public:
    ssg::CommandHandlerResult runTransaction(
        std::function<ssg::CommandHandlerResult()> operation) override {
        return operation();
    }

private:
    std::any state_{std::uint32_t{0}};

    std::any& featureStateValue(std::type_index) override { return state_; }
    void publishStatusValue(std::type_index, std::any) override {}
    void publishDeltaValue(std::type_index, std::any) override {}
};

// Registers its own command rather than binding one from the static table: the
// path under test is service threading, not which commands happen to exist, and
// a test naming a real command breaks every time that command migrates.
TEST(builderThreadsServicesThroughTheCommonDispatchPath) {
    TestServices services;
    ssg::EditorSessionBuilder builder;
    builder.add(ssg::CommandSpecBuilder{"probe.services"}
                    .owner("test-owner")
                    .summary("Observes the services it was dispatched with")
                    .observes()
                    .handler([&services](ssg::CommandContext& context) {
                        ASSERT_TRUE(context.services() == &services);
                        return ssg::CommandHandlerResult::success();
                    }));
    auto session = builder.services(services).build();
    ASSERT_TRUE(session->attach(
                    ssg::InvocationPrincipal{
                        ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                    ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(session
                    ->dispatch(ssg::ClientId{1},
                               {"probe.services", ssg::Revision{1}, {}})
                    .accepted());
}

TEST(fullSnapshotMatchesReplayedAggregateDelta) {
    auto before = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {}, ssg::InvocationPrincipal{
                                  ssg::ClientId{7},
                                  ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), sections(ssg::Revision{4}, "a"));
    auto after = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{5}, {ssg::WorkspaceId{2}, ssg::ViewId{9}},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(5), sections(ssg::Revision{5}, "changed"));

    auto delta = ssg::SessionSnapshotCodec{}.deriveDelta(before, after);
    auto replayed = ssg::SessionSnapshotCodec{}.replay(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_TRUE(replayed.snapshot.has_value());
    ASSERT_EQ(*replayed.snapshot, after);
}

TEST(nonDocumentTransitionReplaysAndRejectsADifferentClient) {
    auto oldSections = sections(ssg::Revision{4}, "same");
    auto newSections = oldSections;
    newSections.tabs.tabs.front().label = "new label";

    auto before = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), std::move(oldSections));
    auto after = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{5}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), std::move(newSections));
    auto delta = ssg::SessionSnapshotCodec{}.deriveDelta(before, after);
    ASSERT_FALSE(delta.document().has_value());
    auto replayed = ssg::SessionSnapshotCodec{}.replay(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(*replayed.snapshot, after);

    auto otherClient = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{8},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{10}, clientView(1),
        sections(ssg::Revision{4}, "same"));
    ASSERT_FALSE(ssg::SessionSnapshotCodec{}.replay(otherClient, delta).accepted());
}

TEST(documentIdentityOnlyTransitionRoundTripsThroughSessionDeltaReplay) {
    auto oldSections = sections(ssg::Revision{4}, "same");
    oldSections.document.diffFileIdentity = std::string{"a.cpp"};
    auto newSections = oldSections;
    newSections.document.diffFileIdentity = std::string{"b.cpp"};

    auto before = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), std::move(oldSections));
    auto after = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{5}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), std::move(newSections));

    auto delta = ssg::SessionSnapshotCodec{}.deriveDelta(before, after);
    ASSERT_TRUE(delta.document().has_value());
    auto replayed = ssg::SessionSnapshotCodec{}.replay(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_TRUE(replayed.snapshot.has_value());
    ASSERT_EQ(*replayed.snapshot, after);
}

TEST(perClientCapabilitiesAndViewportsAreIsolated) {
    auto shared = sections(ssg::Revision{8}, "shared");
    auto first = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{8}, {},
        ssg::InvocationPrincipal{
            ssg::ClientId{1}, ssg::InvocationOrigin::Websocket,
            {ssg::CapabilityId{"local_file_drop"}}},
        ssg::ViewId{10}, clientView(2), shared);
    auto second = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{8}, {},
        ssg::InvocationPrincipal{ssg::ClientId{2},
                                 ssg::InvocationOrigin::Websocket},
        ssg::ViewId{11}, clientView(7), std::move(shared));

    ASSERT_EQ(first.client().capabilities.size(), std::size_t{1});
    ASSERT_TRUE(second.client().capabilities.empty());
    ASSERT_EQ(first.presentation()->viewport.firstVisualRow, std::uint32_t{2});
    ASSERT_EQ(second.presentation()->viewport.firstVisualRow, std::uint32_t{7});
    ASSERT_EQ(first.sections(), second.sections());
}

TEST(shellDeltaDetectsAPanelScrollbarOnlyChange) {
    ssg::ShellViewState oldShell;
    oldShell.panel = ssg::Rect{0, 1, 24, 10};
    oldShell.panelScrollbar = ssg::Rect{23, 2, 1, 9};
    ssg::ShellViewState newShell = oldShell;
    // Only the gutter geometry differs (e.g. a taller panel): the shell delta
    // must not treat this as unchanged.
    newShell.panelScrollbar = ssg::Rect{23, 2, 1, 12};

    auto before = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), sections(ssg::Revision{4}, "same"),
        {}, {}, std::move(oldShell));
    auto after = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{5}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), sections(ssg::Revision{5}, "same"),
        {}, {}, std::move(newShell));
    auto delta = ssg::SessionSnapshotCodec{}.deriveDelta(before, after);
    ASSERT_TRUE(delta.shell().replacement.has_value());
    auto replayed = ssg::SessionSnapshotCodec{}.replay(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(*replayed.snapshot, after);
}

TEST(shellDeltaDetectsATabHitOnlyChange) {
    ssg::ShellViewState oldShell;
    oldShell.tabHits = {ssg::TabHit{ssg::Rect{24, 0, 10, 1}, 0}};
    ssg::ShellViewState newShell = oldShell;
    // A second tab opens: only the tab hit map differs. The shell delta must not
    // treat this as unchanged (or pointer hit-testing would target a stale map).
    newShell.tabHits = {ssg::TabHit{ssg::Rect{24, 0, 10, 1}, 0},
                        ssg::TabHit{ssg::Rect{34, 0, 8, 1}, 1}};

    auto before = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), sections(ssg::Revision{4}, "same"),
        {}, {}, std::move(oldShell));
    auto after = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{5}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), sections(ssg::Revision{5}, "same"),
        {}, {}, std::move(newShell));
    auto delta = ssg::SessionSnapshotCodec{}.deriveDelta(before, after);
    ASSERT_TRUE(delta.shell().replacement.has_value());
    auto replayed = ssg::SessionSnapshotCodec{}.replay(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(*replayed.snapshot, after);
}

static_assert(!std::is_copy_constructible_v<ssg::SessionSnapshot>);
static_assert(std::is_move_constructible_v<ssg::SessionSnapshot>);
static_assert(!std::is_copy_constructible_v<ssg::SessionDelta>);

}  // namespace

int main() {
    RUN(builderThreadsServicesThroughTheCommonDispatchPath);
    RUN(fullSnapshotMatchesReplayedAggregateDelta);
    RUN(nonDocumentTransitionReplaysAndRejectsADifferentClient);
    RUN(documentIdentityOnlyTransitionRoundTripsThroughSessionDeltaReplay);
    RUN(perClientCapabilitiesAndViewportsAreIsolated);
    RUN(shellDeltaDetectsAPanelScrollbarOnlyChange);
    RUN(shellDeltaDetectsATabHitOnlyChange);
    return failed == 0 ? 0 : 1;
}
