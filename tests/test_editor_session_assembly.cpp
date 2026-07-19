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

std::vector<std::string> catalog_ids() {
    std::ifstream input{SSG_REQUIRED_COMMANDS_PATH};
    std::string json{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
    std::regex const id_pattern{R"json("id"\s*:\s*"([^"]+)")json"};
    std::vector<std::string> ids;
    for (std::sregex_iterator it{json.begin(), json.end(), id_pattern}, end;
         it != end; ++it) {
        ids.push_back((*it)[1].str());
    }
    return ids;
}

std::map<std::string, std::vector<std::string>> catalog_capabilities() {
    std::ifstream input{SSG_REQUIRED_COMMANDS_PATH};
    std::string json{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
    std::regex const command_pattern{
        R"json(\{"id":"([^"]+)","owner":"[^"]+","required_capabilities":\[([^\]]*)\])json"};
    std::regex const value_pattern{R"json("([^"]+)")json"};
    std::map<std::string, std::vector<std::string>> result;
    for (std::sregex_iterator it{json.begin(), json.end(), command_pattern}, end;
         it != end; ++it) {
        std::vector<std::string> capabilities;
        std::string const values = (*it)[2].str();
        for (std::sregex_iterator value{values.begin(), values.end(),
                                        value_pattern},
             value_end;
             value != value_end; ++value) {
            capabilities.push_back((*value)[1].str());
        }
        result.emplace((*it)[1].str(), std::move(capabilities));
    }
    return result;
}

ssg::SelectionViewState selection(std::uint64_t byte,
                                  std::uint32_t first_row) {
    ssg::DocumentPosition const position{
        ssg::ByteOffset{byte}, ssg::LineIndex{0}, ssg::CellIndex{byte}};
    return {ssg::SelectionSet{{ssg::Selection{position, position}}},
            first_row, std::nullopt};
}

ssg::SessionSnapshotSections sections(ssg::Revision revision,
                                      std::string marker) {
    ssg::SettingsViewState settings;
    settings.entries[0].effective = {
        static_cast<std::uint32_t>(marker.size()), ssg::SettingScope::user};
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
        {revision, true, marker, ssg::SearchMode::file, {}, std::nullopt,
         marker.size(), false},
        {marker.size(), true, false, revision, marker, {}, {}, {}, std::nullopt,
         ssg::FindReplaceError::none, {}},
        settings,
        {marker, {}},
        {{marker.size() > 1 ? ssg::TextEncoding::utf16le
                           : ssg::TextEncoding::utf8,
          ssg::LineEnding::lf, false,
          !marker.empty()}},
        {{{ssg::TabId{1}, ssg::TabKind::read_only_output, std::nullopt,
           std::nullopt, "output", marker, ssg::DocumentMode::read_only,
           false, ssg::TabRecoveryBadge::none}},
         ssg::TabId{1}},
        {revision, {}},
        {revision, {}},
        {marker.size(), ssg::FollowMode::following, ssg::PaneId{},
         std::nullopt, {}, {}},
        {ssg::TreeRevision{marker.size()}, {}},
        ssg::plain_text_syntax_view_state(revision, ssg::LanguageId{"plain"},
                                          marker, 4),
        {revision, {}},
        {revision, {}, std::nullopt, {}, marker},
        theme,
        std::move(shell),
    };
}

ssg::ViewportViewState client_view(std::uint32_t first_row) {
    return {ssg::ViewportDimensions{20, 8},
            first_row,
            0,
            first_row + 8,
            {},
            {},
            {first_row + 8, 8, first_row, first_row, 0, 8}};
}

TEST(required_catalog_equals_assembled_registry_exactly) {
    auto expected = catalog_ids();
    auto descriptors = ssg::p0_command_descriptors();
    auto expected_capabilities = catalog_capabilities();
    std::vector<std::string> actual;
    for (auto const& descriptor : descriptors) {
        actual.push_back(descriptor.id);
    }
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
    ASSERT_EQ(actual, expected);
    ASSERT_EQ(expected_capabilities.size(), descriptors.size());
    for (auto const& descriptor : descriptors) {
        ASSERT_EQ(descriptor.effect, ssg::CommandEffect::mutation);
        std::vector<std::string> actual_capabilities;
        for (auto const& capability : descriptor.required_capabilities) {
            actual_capabilities.emplace_back(capability.value());
        }
        ASSERT_EQ(actual_capabilities, expected_capabilities.at(descriptor.id));
    }
}

TEST(builder_rejects_missing_and_extra_bindings) {
    auto ids = catalog_ids();
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
    ssg::CommandHandlerResult run_transaction(
        std::function<ssg::CommandHandlerResult()> operation) override {
        return operation();
    }

private:
    std::any state_{std::uint32_t{0}};

    std::any& feature_state_value(std::type_index) override { return state_; }
    void publish_status_value(std::type_index, std::any) override {}
    void publish_delta_value(std::type_index, std::any) override {}
};

TEST(builder_threads_services_through_the_common_dispatch_path) {
    TestServices services;
    ssg::EditorSessionBuilder builder;
    for (auto const& id : catalog_ids()) {
        builder.bind(id, [&services](ssg::CommandContext& context,
                                     std::any const&) {
            ASSERT_TRUE(context.services() == &services);
            return ssg::CommandHandlerResult::success();
        });
    }
    auto session = builder.services(services).build();
    ASSERT_TRUE(session->attach(
                    ssg::InvocationPrincipal{
                        ssg::ClientId{1}, ssg::InvocationOrigin::in_process},
                    ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(session
                    ->dispatch(ssg::ClientId{1},
                               {"text.insert", ssg::Revision{1}, {}})
                    .accepted());
}

TEST(full_snapshot_matches_replayed_aggregate_delta) {
    auto before = ssg::assemble_session_snapshot(
        ssg::Revision{4}, {}, ssg::InvocationPrincipal{
                                  ssg::ClientId{7},
                                  ssg::InvocationOrigin::in_process},
        ssg::ViewId{9}, client_view(1), sections(ssg::Revision{4}, "a"));
    auto after = ssg::assemble_session_snapshot(
        ssg::Revision{5}, {ssg::WorkspaceId{2}, ssg::ViewId{9}},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::in_process},
        ssg::ViewId{9}, client_view(5), sections(ssg::Revision{5}, "changed"));

    auto delta = ssg::derive_session_delta(before, after);
    auto replayed = ssg::replay_session_delta(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_TRUE(replayed.snapshot.has_value());
    ASSERT_EQ(*replayed.snapshot, after);
}

TEST(non_document_transition_replays_and_rejects_a_different_client) {
    auto old_sections = sections(ssg::Revision{4}, "same");
    auto new_sections = old_sections;
    new_sections.tabs.tabs.front().label = "new label";

    auto before = ssg::assemble_session_snapshot(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::in_process},
        ssg::ViewId{9}, client_view(1), std::move(old_sections));
    auto after = ssg::assemble_session_snapshot(
        ssg::Revision{5}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::in_process},
        ssg::ViewId{9}, client_view(1), std::move(new_sections));
    auto delta = ssg::derive_session_delta(before, after);
    ASSERT_FALSE(delta.document().has_value());
    auto replayed = ssg::replay_session_delta(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(*replayed.snapshot, after);

    auto other_client = ssg::assemble_session_snapshot(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{8},
                                 ssg::InvocationOrigin::in_process},
        ssg::ViewId{10}, client_view(1),
        sections(ssg::Revision{4}, "same"));
    ASSERT_FALSE(ssg::replay_session_delta(other_client, delta).accepted());
}

TEST(per_client_capabilities_and_viewports_are_isolated) {
    auto shared = sections(ssg::Revision{8}, "shared");
    auto first = ssg::assemble_session_snapshot(
        ssg::Revision{8}, {},
        ssg::InvocationPrincipal{
            ssg::ClientId{1}, ssg::InvocationOrigin::websocket,
            {ssg::CapabilityId{"local_file_drop"}}},
        ssg::ViewId{10}, client_view(2), shared);
    auto second = ssg::assemble_session_snapshot(
        ssg::Revision{8}, {},
        ssg::InvocationPrincipal{ssg::ClientId{2},
                                 ssg::InvocationOrigin::websocket},
        ssg::ViewId{11}, client_view(7), std::move(shared));

    ASSERT_EQ(first.client().capabilities.size(), std::size_t{1});
    ASSERT_TRUE(second.client().capabilities.empty());
    ASSERT_EQ(first.client().viewport.first_visual_row, std::uint32_t{2});
    ASSERT_EQ(second.client().viewport.first_visual_row, std::uint32_t{7});
    ASSERT_EQ(first.sections(), second.sections());
}

TEST(shell_delta_detects_a_panel_scrollbar_only_change) {
    auto old_sections = sections(ssg::Revision{4}, "same");
    old_sections.shell.panel = ssg::Rect{0, 1, 24, 10};
    old_sections.shell.panel_scrollbar = ssg::Rect{23, 2, 1, 9};
    auto new_sections = old_sections;
    // Only the gutter geometry differs (e.g. a taller panel): the shell delta
    // must not treat this as unchanged.
    new_sections.shell.panel_scrollbar = ssg::Rect{23, 2, 1, 12};

    auto before = ssg::assemble_session_snapshot(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::in_process},
        ssg::ViewId{9}, client_view(1), std::move(old_sections));
    auto after = ssg::assemble_session_snapshot(
        ssg::Revision{5}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::in_process},
        ssg::ViewId{9}, client_view(1), std::move(new_sections));
    auto delta = ssg::derive_session_delta(before, after);
    ASSERT_TRUE(delta.shell().replacement.has_value());
    auto replayed = ssg::replay_session_delta(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(*replayed.snapshot, after);
}

TEST(shell_delta_detects_a_tab_hit_only_change) {
    auto old_sections = sections(ssg::Revision{4}, "same");
    old_sections.shell.tab_hits = {ssg::TabHit{ssg::Rect{24, 0, 10, 1}, 0}};
    auto new_sections = old_sections;
    // A second tab opens: only the tab hit map differs. The shell delta must not
    // treat this as unchanged (or pointer hit-testing would target a stale map).
    new_sections.shell.tab_hits = {ssg::TabHit{ssg::Rect{24, 0, 10, 1}, 0},
                                   ssg::TabHit{ssg::Rect{34, 0, 8, 1}, 1}};

    auto before = ssg::assemble_session_snapshot(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::in_process},
        ssg::ViewId{9}, client_view(1), std::move(old_sections));
    auto after = ssg::assemble_session_snapshot(
        ssg::Revision{5}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::in_process},
        ssg::ViewId{9}, client_view(1), std::move(new_sections));
    auto delta = ssg::derive_session_delta(before, after);
    ASSERT_TRUE(delta.shell().replacement.has_value());
    auto replayed = ssg::replay_session_delta(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(*replayed.snapshot, after);
}

static_assert(!std::is_copy_constructible_v<ssg::SessionSnapshot>);
static_assert(std::is_move_constructible_v<ssg::SessionSnapshot>);
static_assert(!std::is_copy_constructible_v<ssg::SessionDelta>);

}  // namespace

int main() {
    RUN(required_catalog_equals_assembled_registry_exactly);
    RUN(builder_rejects_missing_and_extra_bindings);
    RUN(builder_threads_services_through_the_common_dispatch_path);
    RUN(full_snapshot_matches_replayed_aggregate_delta);
    RUN(non_document_transition_replays_and_rejects_a_different_client);
    RUN(per_client_capabilities_and_viewports_are_isolated);
    RUN(shell_delta_detects_a_panel_scrollbar_only_change);
    RUN(shell_delta_detects_a_tab_hit_only_change);
    return failed == 0 ? 0 : 1;
}
