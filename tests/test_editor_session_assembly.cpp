#include "test_helpers.h"

#include <ssg/editor_session_assembly.h>
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

#ifndef SSG_REQUIRED_COMMANDS_PATH
#error "SSG_REQUIRED_COMMANDS_PATH must name the accepted catalog"
#endif

namespace {

std::vector<std::string> catalogIds() {
    std::ifstream input{SSG_REQUIRED_COMMANDS_PATH};
    std::string json{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
    std::regex const idPattern{R"json("id"\s*:\s*"([^"]+)")json"};
    std::vector<std::string> ids;
    for (std::sregex_iterator it{json.begin(), json.end(), idPattern}, end;
         it != end; ++it) {
        ids.push_back((*it)[1].str());
    }
    return ids;
}

std::map<std::string, std::vector<std::string>> catalogCapabilities() {
    std::ifstream input{SSG_REQUIRED_COMMANDS_PATH};
    std::string json{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
    std::regex const commandPattern{
        R"json(\{"id":"([^"]+)","owner":"[^"]+","required_capabilities":\[([^\]]*)\])json"};
    std::regex const valuePattern{R"json("([^"]+)")json"};
    std::map<std::string, std::vector<std::string>> result;
    for (std::sregex_iterator it{json.begin(), json.end(), commandPattern}, end;
         it != end; ++it) {
        std::vector<std::string> capabilities;
        std::string const values = (*it)[2].str();
        for (std::sregex_iterator value{values.begin(), values.end(),
                                        valuePattern},
             valueEnd;
             value != valueEnd; ++value) {
            capabilities.push_back((*value)[1].str());
        }
        result.emplace((*it)[1].str(), std::move(capabilities));
    }
    return result;
}

ssg::SelectionViewState selection(std::uint64_t byte,
                                  std::uint32_t firstRow) {
    ssg::DocumentPosition const position{
        ssg::ByteOffset{byte}, ssg::LineIndex{0}, ssg::CellIndex{byte}};
    return {ssg::SelectionSet{{ssg::Selection{position, position}}},
            firstRow, 0, std::nullopt};
}

ssg::SessionSnapshotSections sections(ssg::Revision revision,
                                      std::string marker) {
    ssg::SettingsViewState settings;
    settings.entries[0].effective = {
        static_cast<std::uint32_t>(marker.size()), ssg::SettingScope::User};
    ssg::ThemeSnapshot theme;
    theme.palette[0].red = static_cast<std::uint8_t>(marker.size());

    ssg::ShellViewState shell;
    shell.viewport = {static_cast<int>(20 + marker.size()), 8};

    return {
        {revision, marker, ssg::ByteOffset{marker.size()}},
        selection(marker.size(), static_cast<std::uint32_t>(marker.size())),
        {true, false, marker.size()},
        {{marker}, marker, std::nullopt, std::nullopt},
        {std::nullopt, {{}, marker.size()}},
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
        ssg::plainTextSyntaxViewState(revision, ssg::LanguageId{"plain"},
                                          marker, 4),
        {revision, {}},
        {revision, {}, std::nullopt, {}, marker},
        theme,
        std::move(shell),
    };
}

ssg::ViewportViewState clientView(std::uint32_t firstRow) {
    return {ssg::ViewportDimensions{20, 8},
            firstRow,
            0,
            firstRow + 8,
            {},
            {},
            {firstRow + 8, 8, firstRow, firstRow, 0, 8}};
}

TEST(requiredCatalogEqualsAssembledRegistryExactly) {
    auto expected = catalogIds();
    auto descriptors = ssg::p0CommandDescriptors();
    auto expectedCapabilities = catalogCapabilities();
    std::vector<std::string> actual;
    for (auto const& descriptor : descriptors) {
        actual.push_back(descriptor.id);
    }
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
    ASSERT_EQ(actual, expected);
    ASSERT_EQ(expectedCapabilities.size(), descriptors.size());
    for (auto const& descriptor : descriptors) {
        ASSERT_EQ(descriptor.effect, ssg::CommandEffect::Mutation);
        std::vector<std::string> actualCapabilities;
        for (auto const& capability : descriptor.requiredCapabilities) {
            actualCapabilities.emplace_back(capability.value());
        }
        ASSERT_EQ(actualCapabilities, expectedCapabilities.at(descriptor.id));
    }
}

TEST(builderRejectsMissingAndExtraBindings) {
    auto ids = catalogIds();
    ssg::EditorSessionBuilder missing;
    for (std::size_t i = 1; i < ids.size(); ++i) {
        missing.bind(ids[i], [](ssg::CommandContext&, std::any const&) {
            return ssg::CommandHandlerResult::success();
        });
    }
    ASSERT_THROWS(missing.build(), std::invalid_argument);

    ssg::EditorSessionBuilder extra;
    for (auto const& id : ids) {
        extra.bind(id, [](ssg::CommandContext&, std::any const&) {
            return ssg::CommandHandlerResult::success();
        });
    }
    extra.bind("not.p0", [](ssg::CommandContext&, std::any const&) {
        return ssg::CommandHandlerResult::success();
    });
    ASSERT_THROWS(extra.build(), std::invalid_argument);
}

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

TEST(builderThreadsServicesThroughTheCommonDispatchPath) {
    TestServices services;
    ssg::EditorSessionBuilder builder;
    for (auto const& id : catalogIds()) {
        builder.bind(id, [&services](ssg::CommandContext& context,
                                     std::any const&) {
            ASSERT_TRUE(context.services() == &services);
            return ssg::CommandHandlerResult::success();
        });
    }
    auto session = builder.services(services).build();
    ASSERT_TRUE(session->attach(
                    ssg::InvocationPrincipal{
                        ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                    ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(session
                    ->dispatch(ssg::ClientId{1},
                               {"text.insert", ssg::Revision{1}, {}})
                    .accepted());
}

TEST(fullSnapshotMatchesReplayedAggregateDelta) {
    auto before = ssg::assembleSessionSnapshot(
        ssg::Revision{4}, {}, ssg::InvocationPrincipal{
                                  ssg::ClientId{7},
                                  ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), sections(ssg::Revision{4}, "a"));
    auto after = ssg::assembleSessionSnapshot(
        ssg::Revision{5}, {ssg::WorkspaceId{2}, ssg::ViewId{9}},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(5), sections(ssg::Revision{5}, "changed"));

    auto delta = ssg::deriveSessionDelta(before, after);
    auto replayed = ssg::replaySessionDelta(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_TRUE(replayed.snapshot.has_value());
    ASSERT_EQ(*replayed.snapshot, after);
}

TEST(nonDocumentTransitionReplaysAndRejectsADifferentClient) {
    auto oldSections = sections(ssg::Revision{4}, "same");
    auto newSections = oldSections;
    newSections.tabs.tabs.front().label = "new label";

    auto before = ssg::assembleSessionSnapshot(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), std::move(oldSections));
    auto after = ssg::assembleSessionSnapshot(
        ssg::Revision{5}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), std::move(newSections));
    auto delta = ssg::deriveSessionDelta(before, after);
    ASSERT_FALSE(delta.document().has_value());
    auto replayed = ssg::replaySessionDelta(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(*replayed.snapshot, after);

    auto otherClient = ssg::assembleSessionSnapshot(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{8},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{10}, clientView(1),
        sections(ssg::Revision{4}, "same"));
    ASSERT_FALSE(ssg::replaySessionDelta(otherClient, delta).accepted());
}

TEST(perClientCapabilitiesAndViewportsAreIsolated) {
    auto shared = sections(ssg::Revision{8}, "shared");
    auto first = ssg::assembleSessionSnapshot(
        ssg::Revision{8}, {},
        ssg::InvocationPrincipal{
            ssg::ClientId{1}, ssg::InvocationOrigin::Websocket,
            {ssg::CapabilityId{"local_file_drop"}}},
        ssg::ViewId{10}, clientView(2), shared);
    auto second = ssg::assembleSessionSnapshot(
        ssg::Revision{8}, {},
        ssg::InvocationPrincipal{ssg::ClientId{2},
                                 ssg::InvocationOrigin::Websocket},
        ssg::ViewId{11}, clientView(7), std::move(shared));

    ASSERT_EQ(first.client().capabilities.size(), std::size_t{1});
    ASSERT_TRUE(second.client().capabilities.empty());
    ASSERT_EQ(first.client().viewport.firstVisualRow, std::uint32_t{2});
    ASSERT_EQ(second.client().viewport.firstVisualRow, std::uint32_t{7});
    ASSERT_EQ(first.sections(), second.sections());
}

TEST(shellDeltaDetectsAPanelScrollbarOnlyChange) {
    auto oldSections = sections(ssg::Revision{4}, "same");
    oldSections.shell.panel = ssg::Rect{0, 1, 24, 10};
    oldSections.shell.panelScrollbar = ssg::Rect{23, 2, 1, 9};
    auto newSections = oldSections;
    // Only the gutter geometry differs (e.g. a taller panel): the shell delta
    // must not treat this as unchanged.
    newSections.shell.panelScrollbar = ssg::Rect{23, 2, 1, 12};

    auto before = ssg::assembleSessionSnapshot(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), std::move(oldSections));
    auto after = ssg::assembleSessionSnapshot(
        ssg::Revision{5}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), std::move(newSections));
    auto delta = ssg::deriveSessionDelta(before, after);
    ASSERT_TRUE(delta.shell().replacement.has_value());
    auto replayed = ssg::replaySessionDelta(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(*replayed.snapshot, after);
}

TEST(shellDeltaDetectsATabHitOnlyChange) {
    auto oldSections = sections(ssg::Revision{4}, "same");
    oldSections.shell.tabHits = {ssg::TabHit{ssg::Rect{24, 0, 10, 1}, 0}};
    auto newSections = oldSections;
    // A second tab opens: only the tab hit map differs. The shell delta must not
    // treat this as unchanged (or pointer hit-testing would target a stale map).
    newSections.shell.tabHits = {ssg::TabHit{ssg::Rect{24, 0, 10, 1}, 0},
                                   ssg::TabHit{ssg::Rect{34, 0, 8, 1}, 1}};

    auto before = ssg::assembleSessionSnapshot(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), std::move(oldSections));
    auto after = ssg::assembleSessionSnapshot(
        ssg::Revision{5}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), std::move(newSections));
    auto delta = ssg::deriveSessionDelta(before, after);
    ASSERT_TRUE(delta.shell().replacement.has_value());
    auto replayed = ssg::replaySessionDelta(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(*replayed.snapshot, after);
}

static_assert(!std::is_copy_constructible_v<ssg::SessionSnapshot>);
static_assert(std::is_move_constructible_v<ssg::SessionSnapshot>);
static_assert(!std::is_copy_constructible_v<ssg::SessionDelta>);

}  // namespace

int main() {
    RUN(requiredCatalogEqualsAssembledRegistryExactly);
    RUN(builderRejectsMissingAndExtraBindings);
    RUN(builderThreadsServicesThroughTheCommonDispatchPath);
    RUN(fullSnapshotMatchesReplayedAggregateDelta);
    RUN(nonDocumentTransitionReplaysAndRejectsADifferentClient);
    RUN(perClientCapabilitiesAndViewportsAreIsolated);
    RUN(shellDeltaDetectsAPanelScrollbarOnlyChange);
    RUN(shellDeltaDetectsATabHitOnlyChange);
    return failed == 0 ? 0 : 1;
}
