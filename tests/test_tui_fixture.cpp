#include "test_helpers.h"
#include "tui_fixture.h"

#include <ssg/editor_session_assembly.h>
#include <ssg/file_commands.h>
#include <ssg/session_snapshot.h>

#include <any>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct ScriptStep {
    std::string command_id;
    std::any payload;
    bool accepted;
};

std::vector<std::string> fields(std::string const& line) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        auto const end = line.find('\t', begin);
        result.push_back(line.substr(begin, end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

std::vector<ScriptStep> workflow() {
    std::ifstream input{SSG_TUI_WORKFLOW_PATH};
    if (!input) throw std::runtime_error{"cannot open TUI workflow"};
    std::vector<ScriptStep> result;
    for (std::string line; std::getline(input, line);) {
        if (line.empty() || line.front() == '#') continue;
        auto columns = fields(line);
        if (columns.size() != 4) throw std::runtime_error{"invalid TUI workflow"};
        std::any arguments;
        if (columns[1] == "text") {
            arguments = ssg::TextInputArguments{columns[2]};
        } else if (columns[1].starts_with("lines:")) {
            arguments = ssg::ScrollLinesArguments{
                std::stoll(columns[1].substr(6))};
        } else if (columns[1].starts_with("fraction:")) {
            auto value = columns[1].substr(9);
            auto slash = value.find('/');
            arguments = ssg::ScrollFractionArguments{
                static_cast<std::uint32_t>(std::stoul(value.substr(0, slash))),
                static_cast<std::uint32_t>(std::stoul(value.substr(slash + 1)))};
        } else if (columns[1].starts_with("dropped:")) {
            auto value = columns[1].substr(8);
            auto slash = value.find('/');
            arguments = ssg::DroppedContentArguments{
                std::vector<std::uint8_t>{value.begin(),
                                          value.begin() + slash},
                value.substr(slash + 1)};
        } else if (columns[1] != "none") {
            throw std::runtime_error{"unknown TUI workflow argument"};
        }
        result.push_back({std::move(columns[0]), std::move(arguments),
                          columns[3] == "accepted"});
    }
    return result;
}

struct CanonicalState {
    std::string text;
    std::string clipboard;
    std::string label;
    std::size_t selection_count;
    std::uint32_t first_row;
    ssg::DocumentMode mode;
    ssg::TabKind tab_kind;
    ssg::FollowMode follow_mode;
    bool workspace_open;
    bool dirty;
    bool tab_open;
    bool word_wrap;

    bool operator==(CanonicalState const&) const = default;
};

class FixtureModel {
public:
    ssg::CommandHandlerResult apply(std::string_view id,
                                    std::any const& command_payload) {
        if (id == "workspace.open_directory") {
            workspace_open_ = true;
        } else if (id == "file.open") {
            tab_open_ = true;
            mode_ = ssg::DocumentMode::edit;
        } else if (id == "text.insert") {
            if (mode_ != ssg::DocumentMode::edit) {
                return ssg::CommandHandlerResult::failure(
                    "document is not editable");
            }
            undo_text_ = text_;
            text_ += std::any_cast<ssg::TextInputArguments const&>(
                         command_payload)
                         .text;
            dirty_ = true;
        } else if (id == "select.add_cursor_down") {
            selection_count_ = 2;
        } else if (id == "clipboard.copy") {
            clipboard_ = text_;
        } else if (id == "clipboard.cut") {
            undo_text_ = text_;
            clipboard_ = text_;
            text_.clear();
            dirty_ = true;
        } else if (id == "clipboard.paste") {
            undo_text_ = text_;
            text_ += clipboard_;
            dirty_ = true;
        } else if (id == "edit.undo") {
            redo_text_ = text_;
            text_ = undo_text_;
            dirty_ = true;
        } else if (id == "edit.redo") {
            undo_text_ = text_;
            text_ = redo_text_;
            dirty_ = true;
        } else if (id == "view.toggle_word_wrap") {
            word_wrap_ = !word_wrap_;
        } else if (id == "view.scroll_lines") {
            auto rows = std::any_cast<ssg::ScrollLinesArguments const&>(
                            command_payload)
                            .rows;
            first_row_ = static_cast<std::uint32_t>(
                std::max<std::int64_t>(0, first_row_ + rows));
        } else if (id == "view.scroll_to_fraction") {
            auto const& fraction =
                std::any_cast<ssg::ScrollFractionArguments const&>(
                    command_payload);
            first_row_ = static_cast<std::uint32_t>(
                80ULL * fraction.numerator / fraction.denominator);
        } else if (id == "file.save") {
            dirty_ = false;
        } else if (id == "tab.close") {
            tab_open_ = false;
            recovery_ = ssg::TabRecoveryBadge::durable;
        } else if (id == "tab.reopen_closed") {
            tab_open_ = true;
        } else if (id == "file.reload") {
            mode_ = ssg::DocumentMode::read_only;
        } else if (id == "external.open_diff") {
            mode_ = ssg::DocumentMode::diff;
            tab_kind_ = ssg::TabKind::live_diff;
        } else if (id == "follow_edits.pause") {
            follow_mode_ = ssg::FollowMode::paused;
            ++follow_generation_;
        } else if (id == "follow_edits.resume") {
            follow_mode_ = ssg::FollowMode::following;
            ++follow_generation_;
        } else if (id == "prompt.submit") {
            prompt_open_ = false;
        } else if (id == "file.open_dropped_content") {
            auto const& dropped =
                std::any_cast<ssg::DroppedContentArguments const&>(
                    command_payload);
            text_.assign(dropped.bytes.begin(), dropped.bytes.end());
            label_ = dropped.suggested_label;
            mode_ = ssg::DocumentMode::edit;
            tab_kind_ = ssg::TabKind::document;
            tab_open_ = true;
            dirty_ = true;
        }
        return ssg::CommandHandlerResult::success();
    }

    CanonicalState canonical() const {
        return {text_,          clipboard_, label_,      selection_count_,
                first_row_,     mode_,      tab_kind_,   follow_mode_,
                workspace_open_, dirty_,    tab_open_,   word_wrap_};
    }

    ssg::SessionSnapshotSections sections(ssg::Revision revision) const {
        auto const byte = static_cast<std::uint64_t>(text_.size());
        ssg::DocumentPosition const position{
            ssg::ByteOffset{byte}, ssg::LineIndex{0}, ssg::CellIndex{byte}};
        std::vector<ssg::Selection> selections{
            ssg::Selection{position, position}};
        if (selection_count_ > 1) {
            ssg::DocumentPosition const start{
                ssg::ByteOffset{0}, ssg::LineIndex{0}, ssg::CellIndex{0}};
            selections.push_back({start, start});
        }

        ssg::PromptStatusViewState prompt_status;
        if (prompt_open_) {
            prompt_status.prompt = ssg::PromptViewState{
                ssg::PromptKind::path, "Open workspace", {0, 2, 24, 1}, {}};
        }
        prompt_status.status.items.push_back(
            {ssg::StatusId{7}, ssg::StatusPriority::information, 3,
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
        if (tab_open_) {
            tabs.tabs.push_back(
                {ssg::TabId{1}, tab_kind_, std::nullopt, std::nullopt,
                 "fixture", label_, mode_, dirty_, recovery_});
            tabs.active = ssg::TabId{1};
        }

        ssg::ThemeSnapshot theme{};
        for (std::size_t index = 0; index < theme.palette.size(); ++index) {
            auto channel = static_cast<std::uint8_t>(index * 16);
            theme.palette[index] = ssg::SrgbColor::from_serialized_channels(
                channel, channel, channel);
        }
        for (std::size_t index = 0; index < theme.semantic_indices.size();
             ++index) {
            theme.semantic_indices[index] =
                static_cast<std::uint8_t>(index % ssg::theme_palette_size);
        }
        for (std::size_t index = 0; index < theme.syntax_indices.size();
             ++index) {
            theme.syntax_indices[index] =
                static_cast<std::uint8_t>((index + 1) %
                                          ssg::theme_palette_size);
        }

        ssg::ShellViewState shell;
        shell.viewport = {24, 8};
        shell.header = ssg::Rect{0, 0, 24, 1};
        shell.footer = ssg::Rect{0, 7, 24, 1};
        shell.tab_bar = ssg::Rect{6, 1, 18, 1};
        shell.panel = ssg::Rect{0, 1, 6, 6};
        shell.panes.push_back(
            {ssg::PaneId{1}, {6, 2, 18, 5}, {6, 2, 17, 5}, {23, 2, 1, 5}});
        shell.accessibility_nodes = {
            {ssg::ShellNodeKind::header, "header", "Workspace /fixture",
             *shell.header, ssg::SemanticRole::header},
            {ssg::ShellNodeKind::panel, "panel", "Files", *shell.panel,
             ssg::SemanticRole::panel_active},
            {ssg::ShellNodeKind::tab_bar, "tabs", label_, *shell.tab_bar,
             ssg::SemanticRole::tab_active},
            {ssg::ShellNodeKind::pane, "pane", "Editor",
             shell.panes.front().content, ssg::SemanticRole::background},
            {ssg::ShellNodeKind::scrollbar, "scrollbar", "Scroll",
             shell.panes.front().scrollbar,
             ssg::SemanticRole::scrollbar_track},
            {ssg::ShellNodeKind::footer, "footer", "Recovery ready",
             *shell.footer, ssg::SemanticRole::footer},
        };

        return {
            {revision, text_, ssg::ByteOffset{byte}},
            {ssg::SelectionSet{std::move(selections)}, first_row_, std::nullopt},
            {!undo_text_.empty(), !redo_text_.empty(),
             undo_text_.size() + redo_text_.size()},
            {{clipboard_}, clipboard_, std::nullopt, std::nullopt},
            std::move(prompt_status),
            {revision, false, {}, ssg::SearchMode::file, {}, std::nullopt, 0,
             false},
            {0, false, false, revision, {}, {}, {}, std::nullopt,
             ssg::FindReplaceError::none, {}},
            std::move(settings),
            std::move(keymap),
            {{ssg::TextEncoding::utf8, ssg::LineEnding::lf, false, false}},
            std::move(tabs),
            {revision, {}},
            {revision, {}},
            {follow_generation_, follow_mode_, ssg::PaneId{1}, std::nullopt, {},
             {}},
            {ssg::TreeRevision{revision.value()}, {}},
            ssg::plain_text_syntax_view_state(
                revision, ssg::LanguageId{"plain"}, text_, 4),
            {revision, {}},
            {revision, {}, std::nullopt, {}, {}},
            std::move(theme),
            std::move(shell),
        };
    }

    ssg::ViewportViewState viewport() const {
        auto run = ssg::compute_cell_run(text_);
        return ssg::compute_viewport(
            std::span<const ssg::CellRun>{&run, 1},
            ssg::ViewportDimensions{17, 5}, first_row_);
    }

private:
    std::string text_{"alpha"};
    std::string undo_text_;
    std::string redo_text_;
    std::string clipboard_;
    std::string label_{"fixture.txt"};
    std::size_t selection_count_{1};
    std::uint32_t first_row_{0};
    std::uint64_t follow_generation_{0};
    ssg::FollowMode follow_mode_{ssg::FollowMode::following};
    ssg::DocumentMode mode_{ssg::DocumentMode::edit};
    ssg::TabKind tab_kind_{ssg::TabKind::document};
    ssg::TabRecoveryBadge recovery_{ssg::TabRecoveryBadge::none};
    bool workspace_open_{false};
    bool dirty_{false};
    bool tab_open_{true};
    bool word_wrap_{false};
    bool prompt_open_{true};
};

class Scenario {
public:
    Scenario() {
        for (auto const& descriptor : ssg::p0_command_descriptors()) {
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
        return ssg::assemble_session_snapshot(
            session->revision(), session->topology(), principal, view,
            model.viewport(), model.sections(session->revision()));
    }

    FixtureModel model;
    std::unique_ptr<ssg::EditorSession> session;

private:
    ssg::EditorSessionBuilder builder_;
};

TEST(terminal_events_resolve_only_through_snapshot_input_models) {
    Scenario scenario;
    ssg::InvocationPrincipal const principal{
        ssg::ClientId{9}, ssg::InvocationOrigin::in_process};
    ssg::tui::TuiClient client{
        *scenario.session, principal, ssg::ViewId{9},
        [&] { return scenario.snapshot(principal, ssg::ViewId{9}); }};
    auto keymap = client.snapshot().sections().keymap;
    ssg::tui::TerminalInputCapture capture;

    auto text = ssg::CommittedText::from_utf8("hello");
    ASSERT_TRUE(text.has_value());
    auto text_command = capture.capture(*text, keymap, "editor");
    ASSERT_TRUE(text_command.has_value());
    ASSERT_EQ(text_command->command_id, std::string{"text.insert"});
    ASSERT_TRUE(client.submit(*text_command).accepted());
    ASSERT_EQ(scenario.model.canonical().text, std::string{"alphahello"});

    auto chord_start = capture.capture(
        ssg::KeyStroke{"KeyK", true, false, false, false}, keymap, "editor");
    ASSERT_FALSE(chord_start.has_value());
    auto chord_end = capture.capture(
        ssg::KeyStroke{"KeyW", true, false, false, false}, keymap, "editor");
    ASSERT_TRUE(chord_end.has_value());
    ASSERT_EQ(chord_end->command_id, std::string{"tab.close"});
    ASSERT_TRUE(client.submit(*chord_end).accepted());
    ASSERT_FALSE(scenario.model.canonical().tab_open);

    ssg::SemanticHitTarget target{
        9, ssg::HitTargetKind::status_action, "Reopen",
        {"tab.reopen_closed", {}}};
    auto hit = capture.capture(target, keymap, "editor");
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(hit->command_id, std::string{"tab.reopen_closed"});
    ASSERT_TRUE(client.submit(*hit).accepted());
    ASSERT_TRUE(scenario.model.canonical().tab_open);
}

TEST(scripted_tui_commands_match_direct_api_after_every_step) {
    Scenario direct;
    Scenario tui;
    ssg::InvocationPrincipal const direct_principal{
        ssg::ClientId{1}, ssg::InvocationOrigin::in_process,
        {ssg::CapabilityId{"local_file_drop"}}};
    ssg::InvocationPrincipal const tui_principal{
        ssg::ClientId{2}, ssg::InvocationOrigin::in_process,
        {ssg::CapabilityId{"local_file_drop"}}};
    ASSERT_TRUE(direct.session->attach(direct_principal, ssg::ViewId{1})
                    .accepted());
    ssg::tui::TuiClient client{
        *tui.session, tui_principal, ssg::ViewId{2},
        [&] { return tui.snapshot(tui_principal, ssg::ViewId{2}); }};

    for (auto const& step : workflow()) {
        auto direct_result = direct.session->dispatch(
            direct_principal.client_id(),
            {step.command_id, direct.session->revision(), step.payload});
        auto tui_result = client.submit(step.command_id, step.payload);
        ASSERT_EQ(direct_result.accepted(), step.accepted);
        ASSERT_EQ(tui_result.accepted(), step.accepted);
        ASSERT_EQ(direct.model.canonical(), tui.model.canonical());
    }
}

std::string read_all(char const* path) {
    std::ifstream input{path};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

TEST(final_workflow_screen_matches_hand_authored_16_color_golden) {
    Scenario scenario;
    ssg::InvocationPrincipal const principal{
        ssg::ClientId{3}, ssg::InvocationOrigin::in_process,
        {ssg::CapabilityId{"local_file_drop"}}};
    ssg::tui::TuiClient client{
        *scenario.session, principal, ssg::ViewId{3},
        [&] { return scenario.snapshot(principal, ssg::ViewId{3}); }};
    for (auto const& step : workflow()) {
        auto result = client.submit(step.command_id, step.payload);
        ASSERT_EQ(result.accepted(), step.accepted);
    }

    auto screen = ssg::tui::render_screen(client.snapshot());
    ASSERT_EQ(screen.palette.size(), ssg::theme_palette_size);
    for (auto const& cell : screen.cells) {
        ASSERT_TRUE(cell.foreground < ssg::theme_palette_size);
        ASSERT_TRUE(cell.background < ssg::theme_palette_size);
    }
    auto actual = screen.canonical();
    auto expected = read_all(SSG_TUI_SCREEN_PATH);
    if (actual != expected) std::cerr << actual;
    ASSERT_EQ(actual, expected);
}

}  // namespace

int main() {
    RUN(terminal_events_resolve_only_through_snapshot_input_models);
    RUN(scripted_tui_commands_match_direct_api_after_every_step);
    RUN(final_workflow_screen_matches_hand_authored_16_color_golden);
    return failed == 0 ? 0 : 1;
}
