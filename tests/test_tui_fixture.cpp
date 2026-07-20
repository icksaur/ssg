#include "fixtures/end_to_end/e2e_fixture.h"
#include "test_helpers.h"
#include "tui_fixture.h"

#include <ssg/render.h>

#include <ssg/editor_session_assembly.h>
#include <ssg/session_snapshot.h>

#include <any>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {


using CanonicalState = e2e::CanonicalState;

std::vector<e2e::WorkflowStep> workflow() {
    return e2e::load_workflow(SSG_TUI_WORKFLOW_PATH);
}

class FixtureModel {
public:
    ssg::CommandHandlerResult apply(std::string_view id,
                                    std::any const& payload) {
        return state_.apply(id, payload);
    }

    CanonicalState canonical() const { return state_.canonical(); }

    ssg::SessionSnapshotSections sections(ssg::Revision revision) const {
        auto const byte = static_cast<std::uint64_t>(state_.text.size());
        ssg::DocumentPosition const position{
            ssg::ByteOffset{byte}, ssg::LineIndex{0}, ssg::CellIndex{byte}};
        std::vector<ssg::Selection> selections{
            ssg::Selection{position, position}};
        if (state_.selection_count > 1) {
            ssg::DocumentPosition const start{
                ssg::ByteOffset{0}, ssg::LineIndex{0}, ssg::CellIndex{0}};
            selections.push_back({start, start});
        }

        ssg::PromptStatusViewState promptStatus;
        if (state_.prompt_open) {
            promptStatus.prompt = ssg::PromptViewState{
                ssg::PromptKind::Path, "Open workspace", {0, 2, 24, 1}, {}};
        }
        promptStatus.status.items.push_back(
            {ssg::StatusId{7}, ssg::StatusPriority::Information, 3,
             "Recovery ready",
             {{"reopen", "Reopen closed tab", "tab.reopen_closed"}}});

        ssg::SettingsViewState settings;
        ssg::KeymapViewState keymap{
            "tui fixture",
            {{{ssg::KeyStroke{"KeyA", true, false, false, false}},
              "select.add_cursor_down", "editor"},
             {{ssg::KeyStroke{"KeyK", true, false, false, false},
               ssg::KeyStroke{"KeyW", true, false, false, false}},
              "tab.close", "editor"}}};
        ssg::TabViewState tabs;
        if (state_.tab_open) {
            tabs.tabs.push_back(
                {ssg::TabId{1}, state_.tab_kind, std::nullopt, std::nullopt,
                 "fixture", state_.label, state_.mode, state_.dirty,
                 state_.recovery});
            tabs.active = ssg::TabId{1};
        }

        ssg::ThemeSnapshot theme{};
        for (std::size_t index = 0; index < theme.palette.size(); ++index) {
            auto channel = static_cast<std::uint8_t>(index * 16);
            theme.palette[index] = ssg::SrgbColor::fromSerializedChannels(
                channel, channel, channel);
        }
        for (std::size_t index = 0; index < theme.semanticIndices.size();
             ++index) {
            theme.semanticIndices[index] =
                static_cast<std::uint8_t>(index % ssg::kThemePaletteSize);
        }
        for (std::size_t index = 0; index < theme.syntaxIndices.size();
             ++index) {
            theme.syntaxIndices[index] =
                static_cast<std::uint8_t>((index + 1) %
                                          ssg::kThemePaletteSize);
        }

        ssg::ShellViewState shell;
        shell.viewport = {24, 8};
        shell.header = ssg::Rect{0, 0, 24, 1};
        shell.footer = ssg::Rect{0, 7, 24, 1};
        shell.tabBar = ssg::Rect{6, 1, 18, 1};
        shell.panel = ssg::Rect{0, 1, 6, 6};
        shell.panes.push_back(
            {ssg::PaneId{1}, {6, 2, 18, 5}, {6, 2, 17, 5}, {23, 2, 1, 5}});
        shell.accessibilityNodes = {
            {ssg::ShellNodeKind::Header, "header", "Workspace /fixture",
             *shell.header, ssg::SemanticRole::Header},
            {ssg::ShellNodeKind::Panel, "panel", "Files", *shell.panel,
             ssg::SemanticRole::PanelActive},
            {ssg::ShellNodeKind::TabBar, "tabs", state_.label, *shell.tabBar,
             ssg::SemanticRole::TabActive},
            {ssg::ShellNodeKind::Pane, "pane", "Editor",
             shell.panes.front().content, ssg::SemanticRole::Background},
            {ssg::ShellNodeKind::Scrollbar, "scrollbar", "Scroll",
             shell.panes.front().scrollbar,
             ssg::SemanticRole::ScrollbarTrack},
            {ssg::ShellNodeKind::Footer, "footer", "Recovery ready",
             *shell.footer, ssg::SemanticRole::Footer},
        };

        return {
            {revision, state_.text, ssg::ByteOffset{byte}},
            {ssg::SelectionSet{std::move(selections)}, state_.first_row,
             0, std::nullopt},
            {!state_.undo_text.empty(), !state_.redo_text.empty(),
             state_.undo_text.size() + state_.redo_text.size()},
            {{state_.clipboard}, state_.clipboard, std::nullopt, std::nullopt},
            std::move(promptStatus),
            {revision, false, {}, ssg::SearchMode::File, {}, std::nullopt, 0,
             false},
            {0, false, false, revision, {}, {}, {}, {}, std::nullopt,
             ssg::FindReplaceError::None, {}},
            std::move(settings),
            std::move(keymap),
            {{ssg::TextEncoding::Utf8, ssg::LineEnding::Lf, false, false}},
            std::move(tabs),
            {revision, {}},
            {revision, {}},
            {state_.follow_generation, state_.follow_mode, ssg::PaneId{1},
             std::nullopt, {}, {}},
            {ssg::TreeRevision{revision.value()}, {}},
            ssg::plainTextSyntaxViewState(
                revision, ssg::LanguageId{"plain"}, state_.text, 4),
            {revision, {}},
            {revision, {}, std::nullopt, {}, {}},
            std::move(theme),
            std::move(shell),
        };
    }

    ssg::ViewportViewState viewport() const {
        auto run = ssg::computeCellRun(state_.text);
        return ssg::computeViewport(
            std::span<const ssg::CellRun>{&run, 1},
            ssg::ViewportDimensions{17, 5}, state_.first_row);
    }

private:
    e2e::FixtureState state_;
};

class Scenario {
public:
    Scenario() {
        for (auto const& descriptor : ssg::p0CommandDescriptors()) {
            auto id = descriptor.id;
            builder_.bind(id, [this, id](ssg::CommandContext&,
                                         std::any const& value) {
                return model.apply(id, value);
            });
        }
        session = builder_.build();
    }

    ssg::SessionSnapshot snapshot(ssg::InvocationPrincipal const& principal,
                                  ssg::ViewId view) const {
        return ssg::assembleSessionSnapshot(
            session->revision(), session->topology(), principal, view,
            model.viewport(), model.sections(session->revision()));
    }

    FixtureModel model;
    std::unique_ptr<ssg::EditorSession> session;

private:
    ssg::EditorSessionBuilder builder_;
};

TEST(terminalEventsResolveOnlyThroughSnapshotInputModels) {
    Scenario scenario;
    ssg::InvocationPrincipal const principal{
        ssg::ClientId{9}, ssg::InvocationOrigin::InProcess};
    ssg::tui::TuiClient client{
        *scenario.session, principal, ssg::ViewId{9},
        [&] { return scenario.snapshot(principal, ssg::ViewId{9}); }};
    auto keymap = client.snapshot().sections().keymap;
    ssg::tui::TerminalInputCapture capture;

    auto text = ssg::CommittedText::fromUtf8("hello");
    ASSERT_TRUE(text.has_value());
    auto textCommand = capture.capture(*text, keymap, "editor");
    ASSERT_TRUE(textCommand.has_value());
    ASSERT_EQ(textCommand->commandId, std::string{"text.insert"});
    ASSERT_TRUE(client.submit(*textCommand).accepted());
    ASSERT_EQ(scenario.model.canonical().text, std::string{"alphahello"});

    auto chordStart = capture.capture(
        ssg::KeyStroke{"KeyK", true, false, false, false}, keymap, "editor");
    ASSERT_FALSE(chordStart.has_value());
    auto chordEnd = capture.capture(
        ssg::KeyStroke{"KeyW", true, false, false, false}, keymap, "editor");
    ASSERT_TRUE(chordEnd.has_value());
    ASSERT_EQ(chordEnd->commandId, std::string{"tab.close"});
    ASSERT_TRUE(client.submit(*chordEnd).accepted());
    ASSERT_FALSE(scenario.model.canonical().tab_open);

    ssg::SemanticHitTarget target{
        9, ssg::HitTargetKind::StatusAction, "Reopen",
        {"tab.reopen_closed", {}}};
    auto hit = capture.capture(target, keymap, "editor");
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(hit->commandId, std::string{"tab.reopen_closed"});
    ASSERT_TRUE(client.submit(*hit).accepted());
    ASSERT_TRUE(scenario.model.canonical().tab_open);
}

TEST(scriptedTuiCommandsMatchDirectApiAfterEveryStep) {
    Scenario direct;
    Scenario tui;
    ssg::InvocationPrincipal const directPrincipal{
        ssg::ClientId{1}, ssg::InvocationOrigin::InProcess,
        {ssg::CapabilityId{"local_file_drop"}}};
    ssg::InvocationPrincipal const tuiPrincipal{
        ssg::ClientId{2}, ssg::InvocationOrigin::InProcess,
        {ssg::CapabilityId{"local_file_drop"}}};
    ASSERT_TRUE(direct.session->attach(directPrincipal, ssg::ViewId{1})
                    .accepted());
    ssg::tui::TuiClient client{
        *tui.session, tuiPrincipal, ssg::ViewId{2},
        [&] { return tui.snapshot(tuiPrincipal, ssg::ViewId{2}); }};

    for (auto const& step : workflow()) {
        auto directResult = direct.session->dispatch(
            directPrincipal.clientId(),
            {step.command_id, direct.session->revision(), step.payload});
        auto tuiResult = client.submit(step.command_id, step.payload);
        ASSERT_EQ(directResult.accepted(), step.expected_accepted);
        ASSERT_EQ(tuiResult.accepted(), step.expected_accepted);
        ASSERT_EQ(direct.model.canonical(), tui.model.canonical());
    }
}

std::string readAll(char const* path) {
    std::ifstream input{path};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

TEST(finalWorkflowScreenMatchesHandAuthored16ColorGolden) {
    Scenario scenario;
    ssg::InvocationPrincipal const principal{
        ssg::ClientId{3}, ssg::InvocationOrigin::InProcess,
        {ssg::CapabilityId{"local_file_drop"}}};
    ssg::tui::TuiClient client{
        *scenario.session, principal, ssg::ViewId{3},
        [&] { return scenario.snapshot(principal, ssg::ViewId{3}); }};
    for (auto const& step : workflow()) {
        auto result = client.submit(step.command_id, step.payload);
        ASSERT_EQ(result.accepted(), step.expected_accepted);
    }

    auto screen = ssg::render(client.snapshot());
    ASSERT_EQ(screen.palette.size(), ssg::kThemePaletteSize);
    for (auto const& cell : screen.cells) {
        ASSERT_TRUE(cell.foreground < ssg::kThemePaletteSize);
        ASSERT_TRUE(cell.background < ssg::kThemePaletteSize);
    }
    auto actual = screen.canonical();
    auto expected = readAll(SSG_TUI_SCREEN_PATH);
    if (actual != expected) std::cerr << actual;
    ASSERT_EQ(actual, expected);
}

}  // namespace

int main() {
    RUN(terminalEventsResolveOnlyThroughSnapshotInputModels);
    RUN(scriptedTuiCommandsMatchDirectApiAfterEveryStep);
    RUN(finalWorkflowScreenMatchesHandAuthored16ColorGolden);
    return failed == 0 ? 0 : 1;
}
