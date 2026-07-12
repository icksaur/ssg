#include <ssg/editor_session_assembly.h>
#include <ssg/file_commands.h>
#include <ssg/http_server.h>
#include <ssg/protocol.h>
#include <ssg/session_snapshot.h>

#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::string hex(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

class FixtureModel {
public:
    ssg::CommandHandlerResult apply(std::string_view id,
                                    std::any const& payload,
                                    ssg::ClientId client,
                                    ssg::Revision revision) {
        if (trace_commands) {
            std::cout << "COMMAND " << id << '\n' << std::flush;
        }
        std::optional<ssg::ClipboardRequest> clipboard_request;
        {
            std::lock_guard lock{mutex_};
            if (id == "text.insert") {
                if (mode_ != ssg::DocumentMode::edit) {
                    return ssg::CommandHandlerResult::failure(
                        "document is not editable");
                }
                undo_text_ = text_;
                text_ += std::any_cast<ssg::TextInputArguments const&>(payload).text;
                dirty_ = true;
            } else if (id == "select.add_cursor_down") {
                selection_count_ = 2;
            } else if (id == "clipboard.copy") {
                clipboard_ = text_;
                clipboard_request = ssg::ClipboardRequest{
                    41, ssg::ClipboardRequestKind::write, revision, clipboard_};
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
                auto rows =
                    std::any_cast<ssg::ScrollLinesArguments const&>(payload).rows;
                first_row_ = static_cast<std::uint32_t>(
                    std::clamp<std::int64_t>(
                        static_cast<std::int64_t>(first_row_) + rows, 0, 80));
            } else if (id == "view.scroll_to_fraction") {
                auto const& value =
                    std::any_cast<ssg::ScrollFractionArguments const&>(payload);
                first_row_ = static_cast<std::uint32_t>(
                    80ULL * value.numerator / value.denominator);
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
                    std::any_cast<ssg::DroppedContentArguments const&>(payload);
                text_.assign(dropped.bytes.begin(), dropped.bytes.end());
                label_ = dropped.suggested_label;
                mode_ = ssg::DocumentMode::edit;
                tab_kind_ = ssg::TabKind::document;
                tab_open_ = true;
                dirty_ = true;
            }
        }
        if (clipboard_request && send_clipboard_request) {
            send_clipboard_request(client, *clipboard_request);
        }
        return ssg::CommandHandlerResult::success();
    }

    ssg::SessionSnapshotSections sections(ssg::Revision revision) const {
        std::lock_guard lock{mutex_};
        ssg::DocumentPosition const position{
            ssg::ByteOffset{text_.size()}, ssg::LineIndex{0},
            ssg::CellIndex{static_cast<std::uint32_t>(text_.size())}};
        std::vector<ssg::Selection> selections{
            ssg::Selection{position, position}};
        if (selection_count_ > 1) {
            ssg::DocumentPosition const start{
                ssg::ByteOffset{0}, ssg::LineIndex{0}, ssg::CellIndex{0}};
            selections.push_back(ssg::Selection{start, start});
        }

        ssg::PromptStatusViewState prompt_status;
        if (prompt_open_) {
            prompt_status.prompt = ssg::PromptViewState{
                ssg::PromptKind::path,
                "Open a workspace path",
                {0, 2, 80, 1},
                {{ssg::PromptControlKind::input, "path", "Workspace path", "",
                  false, {0, 2, 80, 1}}}};
        }
        prompt_status.status.items.push_back(
            {ssg::StatusId{7},
             ssg::StatusPriority::information,
             3,
             "Recovery is ready",
             {{"reopen", "Reopen closed tab", "tab.reopen_closed"}}});

        ssg::SettingsViewState settings;
        ssg::KeymapViewState keymap{
            "browser fixture",
            {{{ssg::KeyStroke{"KeyA", false, false, false, false}},
              "select.add_cursor_down", "editor"}}};
        ssg::TabViewState tabs;
        if (tab_open_) {
            tabs.tabs.push_back({ssg::TabId{1},
                                 tab_kind_,
                                 std::nullopt,
                                 std::nullopt,
                                 "fixture",
                                 label_,
                                 mode_,
                                 dirty_,
                                 recovery_});
            tabs.active = ssg::TabId{1};
        }

        ssg::ThemeSnapshot theme{};
        for (std::size_t index = 0; index < theme.palette.size(); ++index) {
            auto const channel = static_cast<std::uint8_t>(index * 16);
            theme.palette[index] = ssg::SrgbColor::from_serialized_channels(
                channel, static_cast<std::uint8_t>(255 - channel), channel);
            if (index < theme.semantic_indices.size()) {
                theme.semantic_indices[index] =
                    static_cast<std::uint8_t>(index);
            }
            if (index < theme.syntax_indices.size()) {
                theme.syntax_indices[index] = static_cast<std::uint8_t>(index);
            }
        }

        ssg::ShellViewState shell;
        shell.viewport = {80, 24};
        shell.header = ssg::Rect{0, 0, 80, 1};
        shell.footer = ssg::Rect{0, 23, 80, 1};
        shell.tab_bar = ssg::Rect{18, 1, 62, 1};
        shell.panel = ssg::Rect{0, 1, 18, 22};
        shell.prompt = prompt_open_
                           ? std::optional<ssg::Rect>{ssg::Rect{18, 2, 61, 1}}
                           : std::nullopt;
        shell.panes.push_back(
            {ssg::PaneId{1}, {18, 2, 62, 21}, {18, 2, 61, 21},
             {79, 2, 1, 21}});
        shell.accessibility_nodes = {
            {ssg::ShellNodeKind::header, "header", "Workspace /fixture",
             *shell.header, ssg::SemanticRole::header},
            {ssg::ShellNodeKind::header_field, "path",
             "Current path fixture.txt", *shell.header,
             ssg::SemanticRole::header},
            {ssg::ShellNodeKind::footer, "footer", "UTF-8 LF", *shell.footer,
             ssg::SemanticRole::footer},
            {ssg::ShellNodeKind::footer_action, "reopen", "Reopen closed tab",
             *shell.footer, ssg::SemanticRole::status_info},
            {ssg::ShellNodeKind::tab_bar, "tabs", "Open tabs", *shell.tab_bar,
             ssg::SemanticRole::tab_active},
            {ssg::ShellNodeKind::pane, "pane-1", "Editor pane",
             shell.panes.front().content, ssg::SemanticRole::background},
            {ssg::ShellNodeKind::scrollbar, "scrollbar-1",
             "Editor scrollbar", shell.panes.front().scrollbar,
             ssg::SemanticRole::scrollbar_thumb},
        };
        if (prompt_open_) {
            shell.accessibility_nodes.push_back(
                {ssg::ShellNodeKind::prompt_reservation, "prompt",
                 "Open a workspace path", *shell.prompt,
                 ssg::SemanticRole::prompt});
        }
        shell.accessibility_nodes.push_back(
            {ssg::ShellNodeKind::footer_field, "wrap",
             word_wrap_ ? "Word wrap on" : "Word wrap off", *shell.footer,
             ssg::SemanticRole::footer});

        return {
            {revision, text_, ssg::ByteOffset{text_.size()}},
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
            ssg::plain_text_syntax_view_state(revision,
                                              ssg::LanguageId{"plain"}, text_, 4),
            {revision, {}},
            {revision, {}, std::nullopt, {}, {}},
            std::move(theme),
            std::move(shell),
        };
    }

    ssg::ViewportViewState viewport() const {
        std::lock_guard lock{mutex_};
        return {
            ssg::ViewportDimensions{80, 20},
            first_row_,
            100,
            {},
            {{0, 0, 0, ssg::CellIndex{0}, 0, 1}},
            {100, 20, first_row_, 80, first_row_, 4},
        };
    }

    std::function<void(ssg::ClientId, ssg::ClipboardRequest const&)>
        send_clipboard_request;
    bool trace_commands{false};

private:
    mutable std::mutex mutex_;
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
    bool dirty_{false};
    bool tab_open_{true};
    bool word_wrap_{false};
    bool prompt_open_{true};
};

struct Scenario {
    Scenario() {
        for (auto const& descriptor : ssg::p0_command_descriptors()) {
            auto const id = descriptor.id;
            builder.bind(id, [this, id](ssg::CommandContext& context,
                                        std::any const& payload) {
                return model.apply(id, payload, context.principal().client_id(),
                                   context.revision());
            });
        }
        session = builder.build();
    }

    ssg::SessionSnapshot snapshot(ssg::InvocationPrincipal const& principal,
                                  ssg::ViewId view_id) const {
        return ssg::assemble_session_snapshot(
            session->revision(), session->topology(), principal, view_id,
            model.viewport(), model.sections(session->revision()));
    }

    FixtureModel model;
    ssg::EditorSessionBuilder builder;
    std::unique_ptr<ssg::EditorSession> session;
};

ssg::InvocationPrincipal local_principal() {
    return {ssg::ClientId{11}, ssg::InvocationOrigin::websocket,
            {ssg::CapabilityId{"local_file_drop"}}};
}

class FixtureHost final : public ssg::HttpEditorSessionHost {
public:
    explicit FixtureHost(Scenario& scenario) : scenario_{scenario} {}

    std::optional<ssg::AuthenticatedSession> authenticate(
        std::string_view credential) override {
        if (credential == "local") {
            return ssg::AuthenticatedSession{
                ssg::SessionId{"browser-fixture"}, local_principal(),
                ssg::ViewId{21}};
        }
        if (credential == "remote") {
            return ssg::AuthenticatedSession{
                ssg::SessionId{"browser-fixture"},
                {ssg::ClientId{12}, ssg::InvocationOrigin::websocket},
                ssg::ViewId{22}};
        }
        return std::nullopt;
    }

    ssg::SessionSnapshot snapshot(ssg::SessionId const&,
                                  ssg::ClientId client_id) override {
        auto attached = scenario_.session->attached_client(client_id);
        if (!attached) throw std::logic_error{"snapshot for detached client"};
        return scenario_.snapshot(attached->principal, attached->view_id);
    }

    void clipboard_response(ssg::SessionId const&, ssg::ClientId,
                            ssg::ClipboardResponse const&) override {
        std::cout << "CLIPBOARD_RESPONSE\n" << std::flush;
    }
    void status_action(ssg::SessionId const&, ssg::ClientId,
                       ssg::StatusActionInvocation const&) override {
        std::cout << "STATUS_ACTION\n" << std::flush;
    }
    void binary(ssg::SessionId const&, ssg::ClientId,
                ssg::BinaryFrame const&) override {
        std::cout << "BINARY_FRAME\n" << std::flush;
    }

private:
    Scenario& scenario_;
};

void dispatch_workflow(Scenario& scenario) {
    auto const principal = local_principal();
    auto attached = scenario.session->attach(principal, ssg::ViewId{21});
    if (!attached.accepted()) throw std::runtime_error{attached.message};
    auto dispatch = [&](std::string id, std::any payload = {}) {
        auto result = scenario.session->dispatch(
            principal.client_id(),
            {std::move(id), scenario.session->revision(), std::move(payload)});
        if (!result.accepted()) throw std::runtime_error{result.message};
    };
    dispatch("text.insert", ssg::TextInputArguments{"!"});
    dispatch("select.add_cursor_down");
    dispatch("clipboard.copy");
    dispatch("clipboard.cut");
    dispatch("clipboard.paste");
    dispatch("edit.undo");
    dispatch("edit.redo");
    dispatch("view.toggle_word_wrap");
    dispatch("view.scroll_lines", ssg::ScrollLinesArguments{1});
    dispatch("view.scroll_to_fraction", ssg::ScrollFractionArguments{1, 2});
    dispatch("file.save");
    dispatch("text.insert", ssg::TextInputArguments{" dirty"});
    dispatch("tab.close");
    dispatch("tab.reopen_closed");
    dispatch("file.reload");
    dispatch("external.open_diff");
    dispatch("follow_edits.pause");
    dispatch("follow_edits.resume");
    dispatch("prompt.submit");
    dispatch("file.open_dropped_content",
             ssg::DroppedContentArguments{{'d', 'r', 'o', 'p', 'p', 'e', 'd'},
                                          "drop.txt"});
}

void print_delta_oracle() {
    Scenario scenario;
    auto principal = local_principal();
    auto before = ssg::assemble_session_snapshot(
        ssg::Revision{4}, {}, principal, ssg::ViewId{21},
        scenario.model.viewport(), scenario.model.sections(ssg::Revision{4}));
    scenario.model.apply("text.insert", ssg::TextInputArguments{" changed"},
                         principal.client_id(), ssg::Revision{4});
    scenario.model.apply("select.add_cursor_down", {}, principal.client_id(),
                         ssg::Revision{4});
    scenario.model.apply("view.toggle_word_wrap", {}, principal.client_id(),
                         ssg::Revision{4});
    auto after = ssg::assemble_session_snapshot(
        ssg::Revision{5}, {ssg::WorkspaceId{2}, ssg::ViewId{21}}, principal,
        ssg::ViewId{21}, scenario.model.viewport(),
        scenario.model.sections(ssg::Revision{5}));
    auto delta = ssg::derive_session_delta(before, after);
    std::cout << "BEFORE " << hex(ssg::encode_session_snapshot(before)) << '\n'
              << "DELTA " << hex(ssg::encode_session_delta(delta)) << '\n'
              << "AFTER " << hex(ssg::encode_session_snapshot(after)) << '\n';
}

void print_workflow_oracle() {
    Scenario scenario;
    dispatch_workflow(scenario);
    auto snapshot = scenario.snapshot(local_principal(), ssg::ViewId{21});
    std::cout << hex(ssg::encode_session_snapshot(snapshot)) << '\n';
}

void serve(std::uint16_t port) {
    Scenario scenario;
    FixtureHost host{scenario};
    scenario.model.trace_commands = true;
    ssg::HttpEditorServer server{
        *scenario.session, ssg::build_command_argument_codec_registry(), host,
        {port, "/session", 32, 64, std::chrono::milliseconds{1000}}};
    scenario.model.send_clipboard_request =
        [&](ssg::ClientId client, ssg::ClipboardRequest const& request) {
            if (!server.send_clipboard_request(client, request)) {
                throw std::runtime_error{"failed to send clipboard request"};
            }
        };
    server.start();
    std::cout << "READY\n" << std::flush;
    std::string line;
    std::getline(std::cin, line);
    server.stop();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view{argv[1]} == "--delta-oracle") {
            print_delta_oracle();
            return 0;
        }
        if (argc == 2 && std::string_view{argv[1]} == "--workflow-oracle") {
            print_workflow_oracle();
            return 0;
        }
        if (argc == 3 && std::string_view{argv[1]} == "--serve") {
            serve(static_cast<std::uint16_t>(std::stoul(argv[2])));
            return 0;
        }
        std::cerr << "usage: browser_client_fixture "
                     "(--delta-oracle|--workflow-oracle|--serve PORT)\n";
        return 2;
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
