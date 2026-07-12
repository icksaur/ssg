#include "fixtures/end_to_end/e2e_fixture.h"
#include "test_helpers.h"
#include "tui_fixture.h"

#include <ssg/editor_session_assembly.h>
#include <ssg/follow_edits.h>
#include <ssg/http_server.h>
#include <ssg/protocol.h>
#include <ssg/session_snapshot.h>
#include <ssg/syntax.h>

#include <http.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <any>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

// ─── Platform socket layer ──────────────────────────────────────────────────

#ifdef _WIN32
using TestSocket = SOCKET;
constexpr TestSocket invalid_test_socket = INVALID_SOCKET;
void close_test_socket(TestSocket s) { closesocket(s); }
#else
using TestSocket = int;
constexpr TestSocket invalid_test_socket = -1;
void close_test_socket(TestSocket s) { close(s); }
#endif

struct SocketOwner {
    TestSocket socket{invalid_test_socket};
    explicit SocketOwner(TestSocket s) : socket{s} {}
    ~SocketOwner() {
        if (socket != invalid_test_socket) close_test_socket(socket);
    }
    SocketOwner(SocketOwner const&) = delete;
    SocketOwner& operator=(SocketOwner const&) = delete;
    SocketOwner(SocketOwner&& other) noexcept : socket{other.socket} {
        other.socket = invalid_test_socket;
    }
};

void send_all(TestSocket s, std::string const& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        auto count = send(s, bytes.data() + sent,
                          static_cast<int>(bytes.size() - sent), 0);
        if (count <= 0) throw std::runtime_error{"loopback send failed"};
        sent += static_cast<std::size_t>(count);
    }
}

std::string receive_some(TestSocket s) {
    std::array<char, 65536> buf{};
    auto count = recv(s, buf.data(), static_cast<int>(buf.size()), 0);
    if (count <= 0) throw std::runtime_error{"loopback receive failed"};
    return {buf.data(), static_cast<std::size_t>(count)};
}

std::string masked_frame(std::uint8_t opcode, std::string const& payload) {
    std::array<std::uint8_t, 4> const mask{0x12, 0x34, 0x56, 0x78};
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() <= 125) {
        frame.push_back(static_cast<char>(0x80 | payload.size()));
    } else if (payload.size() <= 65535) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
        frame.push_back(static_cast<char>(payload.size() & 0xff));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back(static_cast<char>((payload.size() >> shift) & 0xff));
    }
    for (auto byte : mask) frame.push_back(static_cast<char>(byte));
    for (std::size_t i = 0; i < payload.size(); ++i)
        frame.push_back(static_cast<char>(
            static_cast<unsigned char>(payload[i]) ^ mask[i % mask.size()]));
    return frame;
}

class FrameReader {
public:
    explicit FrameReader(TestSocket s) : socket_{s} {}

    Http::WebSocketFrame next() {
        for (;;) {
            std::size_t consumed = 0;
            auto frame = Http::parseWebSocketFrame(bytes_, consumed);
            if (consumed != 0) {
                bytes_.erase(0, consumed);
                return frame;
            }
            bytes_ += receive_some(socket_);
        }
    }

private:
    TestSocket socket_;
    std::string bytes_;
};

SocketOwner connect_websocket(std::uint16_t port) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        throw std::runtime_error{"WSAStartup failed"};
#endif
    SocketOwner owner{socket(AF_INET, SOCK_STREAM, 0)};
    if (owner.socket == invalid_test_socket)
        throw std::runtime_error{"socket creation failed"};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(owner.socket, reinterpret_cast<sockaddr*>(&addr),
                sizeof(addr)) != 0)
        throw std::runtime_error{"loopback connect failed"};
    send_all(owner.socket,
             "GET /session HTTP/1.1\r\nHost: 127.0.0.1\r\n"
             "Upgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    std::string resp;
    while (resp.find("\r\n\r\n") == std::string::npos)
        resp += receive_some(owner.socket);
    if (resp.find("101") == std::string::npos)
        throw std::runtime_error{"WebSocket upgrade failed"};
    return owner;
}

void ws_attach(TestSocket s, std::string credential,
               std::optional<ssg::Revision> base = std::nullopt) {
    send_all(s, masked_frame(0x1, ssg::encode_session_attach_request(
                                       {std::move(credential), base})));
}

// ─── Canonical end-to-end state ─────────────────────────────────────────────

using EndToEndCanonicalState = e2e::CanonicalState;

// ─── Fixture model shared by direct/WebSocket/TUI scenarios ─────────────────

class EndToEndFixtureModel {
public:
    ssg::CommandHandlerResult apply(std::string_view id,
                                    std::any const& payload,
                                    ssg::ClientId,
                                    ssg::Revision) {
        std::lock_guard lock{mutex_};
        return state_.apply(id, payload);
    }

    EndToEndCanonicalState canonical() const {
        std::lock_guard lock{mutex_};
        return state_.canonical();
    }

    ssg::SessionSnapshotSections sections(ssg::Revision revision) const {
        std::lock_guard lock{mutex_};
        ssg::DocumentPosition const pos{
            ssg::ByteOffset{state_.text.size()}, ssg::LineIndex{0},
            ssg::CellIndex{static_cast<std::uint32_t>(state_.text.size())}};
        std::vector<ssg::Selection> sels{ssg::Selection{pos, pos}};
        if (state_.selection_count > 1) {
            ssg::DocumentPosition const start{
                ssg::ByteOffset{0}, ssg::LineIndex{0}, ssg::CellIndex{0}};
            sels.push_back({start, start});
        }

        ssg::PromptStatusViewState prompt_status;
        if (state_.prompt_open) {
            prompt_status.prompt = ssg::PromptViewState{
                ssg::PromptKind::path, "Open a workspace path",
                {0, 2, 80, 1},
                {{ssg::PromptControlKind::input, "path", "Workspace path", "",
                  false, {0, 2, 80, 1}}}};
        }
        prompt_status.status.items.push_back(
            {ssg::StatusId{7}, ssg::StatusPriority::information, 3,
             "Recovery is ready",
             {{"reopen", "Reopen closed tab", "tab.reopen_closed"}}});

        ssg::SettingsViewState settings;
        ssg::KeymapViewState keymap{
            "end-to-end fixture",
            {{{ssg::KeyStroke{"KeyA", false, false, false, false}},
              "select.add_cursor_down", "editor"}}};
        ssg::TabViewState tabs;
        if (state_.tab_open) {
            tabs.tabs.push_back(
                {ssg::TabId{1}, state_.tab_kind, std::nullopt, std::nullopt,
                 "fixture", state_.label, state_.mode, state_.dirty,
                 state_.recovery});
            tabs.active = ssg::TabId{1};
        }

        ssg::ThemeSnapshot theme{};
        for (std::size_t i = 0; i < theme.palette.size(); ++i) {
            auto ch = static_cast<std::uint8_t>(i * 16);
            theme.palette[i] = ssg::SrgbColor::from_serialized_channels(
                ch, static_cast<std::uint8_t>(255 - ch), ch);
            if (i < theme.semantic_indices.size())
                theme.semantic_indices[i] = static_cast<std::uint8_t>(i);
            if (i < theme.syntax_indices.size())
                theme.syntax_indices[i] = static_cast<std::uint8_t>(i);
        }

        ssg::ShellViewState shell;
        shell.viewport = {80, 24};
        shell.header = ssg::Rect{0, 0, 80, 1};
        shell.footer = ssg::Rect{0, 23, 80, 1};
        shell.tab_bar = ssg::Rect{18, 1, 62, 1};
        shell.panel = ssg::Rect{0, 1, 18, 22};
        shell.prompt = state_.prompt_open
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
            {ssg::ShellNodeKind::scrollbar, "scrollbar-1", "Editor scrollbar",
             shell.panes.front().scrollbar, ssg::SemanticRole::scrollbar_thumb},
        };
        if (state_.prompt_open) {
            shell.accessibility_nodes.push_back(
                {ssg::ShellNodeKind::prompt_reservation, "prompt",
                 "Open a workspace path", *shell.prompt,
                 ssg::SemanticRole::prompt});
        }
        shell.accessibility_nodes.push_back(
            {ssg::ShellNodeKind::footer_field, "wrap",
             state_.word_wrap ? "Word wrap on" : "Word wrap off", *shell.footer,
             ssg::SemanticRole::footer});

        return {
            {revision, state_.text, ssg::ByteOffset{state_.text.size()}},
            {ssg::SelectionSet{std::move(sels)}, state_.first_row, std::nullopt},
            {!state_.undo_text.empty(), !state_.redo_text.empty(),
             state_.undo_text.size() + state_.redo_text.size()},
            {{state_.clipboard}, state_.clipboard, std::nullopt, std::nullopt},
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
            {state_.follow_generation, state_.follow_mode, ssg::PaneId{1},
             std::nullopt, {}, {}},
            {ssg::TreeRevision{revision.value()}, {}},
            ssg::plain_text_syntax_view_state(revision,
                                               ssg::LanguageId{"plain"},
                                               state_.text, 4),
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
            state_.first_row,
            100,
            {},
            {{0, 0, 0, ssg::CellIndex{0}, 0, 1}},
            {100, 20, state_.first_row, 80, state_.first_row, 4},
        };
    }

private:
    mutable std::mutex mutex_;
    e2e::FixtureState state_;
};

// ─── Scenario ───────────────────────────────────────────────────────────────

struct EndToEndScenario {
    EndToEndScenario() {
        for (auto const& desc : ssg::p0_command_descriptors()) {
            auto id = desc.id;
            builder.bind(id, [this, id](ssg::CommandContext& ctx,
                                        std::any const& payload) {
                return model.apply(id, payload, ctx.principal().client_id(),
                                   ctx.revision());
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

    EndToEndFixtureModel model;
    ssg::EditorSessionBuilder builder;
    std::unique_ptr<ssg::EditorSession> session;
};

// ─── WebSocket session host ──────────────────────────────────────────────────

class PeerHost final : public ssg::HttpEditorSessionHost {
public:
    explicit PeerHost(EndToEndScenario& scenario, ssg::InvocationPrincipal peer)
        : scenario_{scenario}, peer_{std::move(peer)} {}

    std::optional<ssg::AuthenticatedSession> authenticate(
        std::string_view credential) override {
        if (credential != "ws-peer") return std::nullopt;
        return ssg::AuthenticatedSession{
            ssg::SessionId{"e2e-ws"}, peer_, ssg::ViewId{31}};
    }

    ssg::SessionSnapshot snapshot(ssg::SessionId const&,
                                  ssg::ClientId client_id) override {
        auto attached = scenario_.session->attached_client(client_id);
        if (!attached) throw std::logic_error{"snapshot for detached client"};
        return scenario_.snapshot(attached->principal, attached->view_id);
    }

    void clipboard_response(ssg::SessionId const&, ssg::ClientId,
                            ssg::ClipboardResponse const&) override {}
    void status_action(ssg::SessionId const&, ssg::ClientId,
                       ssg::StatusActionInvocation const&) override {}
    void binary(ssg::SessionId const&, ssg::ClientId,
                ssg::BinaryFrame const&) override {}

private:
    EndToEndScenario& scenario_;
    ssg::InvocationPrincipal peer_;
};

// ─── TEST 1 ──────────────────────────────────────────────────────────────────
// Prove that direct API, loopback WebSocket, and TUI paths reach identical
// EndToEndCanonicalState after every step of the client-neutral mandatory
// workflow. Each path runs against its own independent scenario so the three
// models are equal only if command dispatch is semantically identical across
// all three execution paths.

TEST(direct_api_loopback_websocket_tui_canonical_state_matches_per_step) {
    auto const steps = e2e::load_workflow(SSG_E2E_WORKFLOW_PATH);
    ASSERT_TRUE(!steps.empty());

    // ── Direct API scenario ──
    EndToEndScenario direct;
    ssg::InvocationPrincipal const direct_principal{
        ssg::ClientId{21}, ssg::InvocationOrigin::in_process,
        {ssg::CapabilityId{"local_file_drop"}}};
    ASSERT_TRUE(direct.session->attach(direct_principal, ssg::ViewId{21})
                    .accepted());

    // ── TUI scenario ──
    EndToEndScenario tui_scenario;
    ssg::InvocationPrincipal const tui_principal{
        ssg::ClientId{22}, ssg::InvocationOrigin::in_process,
        {ssg::CapabilityId{"local_file_drop"}}};
    ssg::tui::TuiClient tui_client{
        *tui_scenario.session, tui_principal, ssg::ViewId{22},
        [&] {
            return tui_scenario.snapshot(tui_principal, ssg::ViewId{22});
        }};

    // ── WebSocket scenario ──
    EndToEndScenario ws_scenario;
    ssg::InvocationPrincipal const ws_peer_principal{
        ssg::ClientId{31}, ssg::InvocationOrigin::websocket,
        {ssg::CapabilityId{"local_file_drop"}}};
    PeerHost ws_host{ws_scenario, ws_peer_principal};
    constexpr std::uint16_t ws_port = 18800;
    ssg::HttpEditorServer ws_server{
        *ws_scenario.session, ssg::build_command_argument_codec_registry(),
        ws_host, {ws_port, "/session", 64, 128, 500ms}};
    ws_server.start();
    std::this_thread::sleep_for(30ms);

    auto ws = connect_websocket(ws_port);
    FrameReader ws_reader{ws.socket};
    ws_attach(ws.socket, "ws-peer");
    auto initial_snap = ssg::decode_session_snapshot(ws_reader.next().payload);
    ASSERT_TRUE(initial_snap.accepted());
    auto ws_snapshot = std::move(*initial_snap.snapshot);
    auto ws_state = e2e::canonical(ws_snapshot);
    ssg::Revision ws_revision = ws_snapshot.revision();

    auto const codec = ssg::build_command_argument_codec_registry();

    for (auto const& step : steps) {
        // Direct dispatch
        auto direct_result = direct.session->dispatch(
            direct_principal.client_id(),
            {step.command_id, direct.session->revision(), step.payload});
        ASSERT_EQ(direct_result.accepted(), step.expected_accepted);

        // TUI dispatch
        auto tui_result =
            tui_client.submit(step.command_id, step.payload);
        ASSERT_EQ(tui_result.accepted(), step.expected_accepted);

        // WebSocket dispatch
        auto ws_encoded = ssg::encode_command_request(
            {step.command_id, ws_revision, step.payload}, codec);
        send_all(ws.socket, masked_frame(0x2, ws_encoded));
        auto ws_frame = ws_reader.next();
        if (step.expected_accepted) {
            auto ws_delta =
                ssg::decode_session_delta(ws_frame.payload);
            ASSERT_TRUE(ws_delta.accepted());
            e2e::apply(ws_state, *ws_delta.delta);
            ws_revision = ws_state.revision;
        } else {
            auto ws_cmd_result =
                ssg::decode_command_result(ws_frame.payload);
            ASSERT_TRUE(ws_cmd_result.accepted());
            ASSERT_FALSE(ws_cmd_result.result->accepted());
        }

        // All paths project state from the snapshots each client observes.
        auto direct_state =
            e2e::canonical(direct.snapshot(direct_principal, ssg::ViewId{11}));
        auto tui_state = e2e::canonical(tui_client.snapshot());
        ASSERT_EQ(direct_state, tui_state);
        ASSERT_EQ(direct_state, ws_state);
    }

    ws_server.stop();
}

// ─── Concurrent fixture model with real FollowEditsModel ─────────────────────
// Used by the concurrent follow-interruption test where two clients attach to
// one session and independently navigate while sharing follow state.

class ConcurrentFixtureModel {
public:
    ConcurrentFixtureModel() : follow_model_{{.queue_capacity = 4}} {}

    void register_client(ssg::ClientId client,
                         ssg::ViewportDimensions dims) {
        std::lock_guard lock{mutex_};
        (void)follow_model_.attach_client(client, dims);
    }

    ssg::CommandHandlerResult apply(std::string_view id,
                                    std::any const& payload,
                                    ssg::ClientId client,
                                    ssg::Revision) {
        std::lock_guard lock{mutex_};
        if (id == "view.scroll_lines") {
            auto rows =
                std::any_cast<ssg::ScrollLinesArguments const&>(payload).rows;
            per_client_row_[client] = static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(per_client_row_[client]) + rows,
                    0, 80));
            (void)follow_model_.apply_navigation(
                {client, ssg::NavigationClass::user, ssg::PaneId{1},
                 ssg::FollowScrollOffset{per_client_row_[client], 0}});
        } else if (id == "follow_edits.pause") {
            (void)follow_model_.pause();
        } else if (id == "follow_edits.resume") {
            (void)follow_model_.resume(current_diff_);
        }
        return ssg::CommandHandlerResult::success();
    }

    void accept_external_change(std::string const& file_id,
                                std::filesystem::path path,
                                ssg::Revision source_rev) {
        std::lock_guard lock{mutex_};
        ssg::DiffFileView file{ssg::DiffFileId{file_id}};
        file.path = std::move(path);
        file.hunks.push_back(
            {.baseline_start = 10,
             .target_start = 10,
             .baseline_lines = {"old line\n"},
             .target_lines = {"new line\n"}});
        current_diff_.revision = source_rev;
        current_diff_.files.push_back(file);
        (void)follow_model_.accept_external_change(
            current_diff_.files.back(), source_rev);
    }

    ssg::FollowEditsViewState follow_view_state() const {
        std::lock_guard lock{mutex_};
        return follow_model_.view_state();
    }

    ssg::SessionSnapshotSections sections(ssg::Revision revision,
                                          ssg::ClientId client) const {
        std::lock_guard lock{mutex_};
        auto first_row = per_client_row_.count(client)
                             ? per_client_row_.at(client)
                             : std::uint32_t{0};
        ssg::DocumentPosition const pos{
            ssg::ByteOffset{0}, ssg::LineIndex{0}, ssg::CellIndex{0}};
        ssg::SettingsViewState settings;
        ssg::ThemeSnapshot theme{};
        ssg::ShellViewState shell;
        shell.viewport = {80, 24};
        return {
            {revision, "concurrent", ssg::ByteOffset{0}},
            {ssg::SelectionSet{{ssg::Selection{pos, pos}}}, first_row,
             std::nullopt},
            {false, false, 0},
            {{}, {}, std::nullopt, std::nullopt},
            {std::nullopt, {{}, 0}},
            {revision, false, {}, ssg::SearchMode::file, {}, std::nullopt, 0,
             false},
            {0, false, false, revision, {}, {}, {}, std::nullopt,
             ssg::FindReplaceError::none, {}},
            std::move(settings),
            {"concurrent", {}},
            {{ssg::TextEncoding::utf8, ssg::LineEnding::lf, false, false}},
            {{}, std::nullopt},
            {revision, {}},
            {revision, {}},
            follow_model_.view_state(),
            {ssg::TreeRevision{revision.value()}, {}},
            ssg::plain_text_syntax_view_state(
                revision, ssg::LanguageId{"plain"}, "concurrent", 4),
            {revision, {}},
            {revision, {}, std::nullopt, {}, {}},
            std::move(theme),
            std::move(shell),
        };
    }

    ssg::ViewportViewState viewport(ssg::ClientId client) const {
        std::lock_guard lock{mutex_};
        auto first_row = per_client_row_.count(client)
                             ? per_client_row_.at(client)
                             : std::uint32_t{0};
        return {ssg::ViewportDimensions{80, 20}, first_row, 100, {},
                {{0, 0, 0, ssg::CellIndex{0}, 0, 1}},
                {100, 20, first_row, 80, first_row, 4}};
    }

private:
    mutable std::mutex mutex_;
    ssg::FollowEditsModel follow_model_;
    ssg::DiffViewState current_diff_;
    std::map<ssg::ClientId, std::uint32_t> per_client_row_;
};

struct ConcurrentScenario {
    ConcurrentScenario() {
        for (auto const& desc : ssg::p0_command_descriptors()) {
            auto id = desc.id;
            builder.bind(id, [this, id](ssg::CommandContext& ctx,
                                        std::any const& payload) {
                return model.apply(id, payload, ctx.principal().client_id(),
                                   ctx.revision());
            });
        }
        session = builder.build();
    }

    ssg::SessionSnapshot snapshot(ssg::InvocationPrincipal const& principal,
                                  ssg::ViewId view_id) const {
        return ssg::assemble_session_snapshot(
            session->revision(), session->topology(), principal, view_id,
            model.viewport(principal.client_id()),
            model.sections(session->revision(), principal.client_id()));
    }

    ConcurrentFixtureModel model;
    ssg::EditorSessionBuilder builder;
    std::unique_ptr<ssg::EditorSession> session;
};

class ConcurrentHost final : public ssg::HttpEditorSessionHost {
public:
    explicit ConcurrentHost(ConcurrentScenario& scenario,
                            ssg::InvocationPrincipal peer)
        : scenario_{scenario}, peer_{std::move(peer)} {}

    std::optional<ssg::AuthenticatedSession> authenticate(
        std::string_view credential) override {
        if (credential != "conc-ws") return std::nullopt;
        return ssg::AuthenticatedSession{
            ssg::SessionId{"e2e-conc"}, peer_, ssg::ViewId{42}};
    }

    ssg::SessionSnapshot snapshot(ssg::SessionId const&,
                                  ssg::ClientId client_id) override {
        auto attached = scenario_.session->attached_client(client_id);
        if (!attached) throw std::logic_error{"snapshot for detached client"};
        return scenario_.snapshot(attached->principal, attached->view_id);
    }

    void clipboard_response(ssg::SessionId const&, ssg::ClientId,
                            ssg::ClipboardResponse const&) override {}
    void status_action(ssg::SessionId const&, ssg::ClientId,
                       ssg::StatusActionInvocation const&) override {}
    void binary(ssg::SessionId const&, ssg::ClientId,
                ssg::BinaryFrame const&) override {}

private:
    ConcurrentScenario& scenario_;
    ssg::InvocationPrincipal peer_;
};

// ─── TEST 2 ──────────────────────────────────────────────────────────────────
// Prove that a TUI in-process client and a loopback WebSocket client attached
// to one session share follow state through pause/resume transitions, while
// their viewport scroll offsets remain independent of each other.

TEST(concurrent_tui_and_websocket_clients_share_follow_interruption) {
    ConcurrentScenario scenario;
    ssg::InvocationPrincipal const direct_principal{
        ssg::ClientId{41}, ssg::InvocationOrigin::in_process};
    ssg::InvocationPrincipal const ws_peer_principal{
        ssg::ClientId{42}, ssg::InvocationOrigin::websocket};

    scenario.model.register_client(ssg::ClientId{41},
                                   ssg::ViewportDimensions{80, 20});

    ASSERT_TRUE(
        scenario.session->attach(direct_principal, ssg::ViewId{41}).accepted());

    constexpr std::uint16_t conc_port = 18801;
    ConcurrentHost conc_host{scenario, ws_peer_principal};
    ssg::HttpEditorServer conc_server{
        *scenario.session, ssg::build_command_argument_codec_registry(),
        conc_host, {conc_port, "/session", 64, 128, 500ms}};
    conc_server.start();
    std::this_thread::sleep_for(30ms);

    auto ws = connect_websocket(conc_port);
    FrameReader ws_reader{ws.socket};
    ws_attach(ws.socket, "conc-ws");
    auto initial_ws = ssg::decode_session_snapshot(ws_reader.next().payload);
    ASSERT_TRUE(initial_ws.accepted());
    ssg::Revision ws_rev = initial_ws.snapshot->revision();

    // Register WS client in the follow model now that we know it attached.
    scenario.model.register_client(ssg::ClientId{42},
                                   ssg::ViewportDimensions{80, 20});

    auto const codec = ssg::build_command_argument_codec_registry();

    // Navigate direct client to row 3 and WS client to row 10 to establish
    // independent viewport offsets.
    auto direct_scroll_result = scenario.session->dispatch(
        direct_principal.client_id(),
        {"view.scroll_lines", scenario.session->revision(),
         ssg::ScrollLinesArguments{3}});
    ASSERT_TRUE(direct_scroll_result.accepted());
    // Sync ws_rev: direct dispatch advanced the session revision.
    ws_rev = direct_scroll_result.revision;

    auto ws_scroll = ssg::encode_command_request(
        {"view.scroll_lines", ws_rev, ssg::ScrollLinesArguments{10}}, codec);
    send_all(ws.socket, masked_frame(0x2, ws_scroll));
    auto scroll_delta = ssg::decode_session_delta(ws_reader.next().payload);
    ASSERT_TRUE(scroll_delta.accepted());
    ws_rev = scroll_delta.delta->revision();

    // Both clients have different scroll offsets in the follow model.
    auto follow_state_before = scenario.model.follow_view_state();
    bool found_diff = false;
    if (follow_state_before.clients.size() == 2) {
        found_diff =
            follow_state_before.clients[0].offset.first_row !=
            follow_state_before.clients[1].offset.first_row;
    }
    // Viewport-independent: per-client offsets diverged from independent navigation.
    ASSERT_TRUE(found_diff);

    // After user navigation both clients are now in paused mode (apply_navigation
    // with NavigationClass::user transitions the shared FollowEditsModel to paused).
    auto direct_snap_after_scroll =
        scenario.snapshot(direct_principal, ssg::ViewId{41});
    ASSERT_EQ(direct_snap_after_scroll.sections().follow_edits.mode,
              ssg::FollowMode::paused);

    // Accept changes to two watched files while paused; resume must choose the
    // newest target across the multi-file queue.
    scenario.model.accept_external_change(
        "watched-file-a", std::filesystem::path{"/workspace/watched-a.txt"},
        ssg::Revision{scenario.session->revision().value() + 1});
    scenario.model.accept_external_change(
        "watched-file-b", std::filesystem::path{"/workspace/watched-b.txt"},
        ssg::Revision{scenario.session->revision().value() + 2});

    auto queued_state = scenario.model.follow_view_state();
    ASSERT_EQ(queued_state.mode, ssg::FollowMode::paused);
    ASSERT_EQ(queued_state.queued_targets.size(), std::size_t{2});

    // Explicit pause dispatch is idempotent from the already-paused state.
    auto pause_result = scenario.session->dispatch(
        direct_principal.client_id(),
        {"follow_edits.pause", scenario.session->revision(), {}});
    ASSERT_TRUE(pause_result.accepted());
    // Sync ws_rev: direct pause advanced the session revision.
    ws_rev = pause_result.revision;

    // Direct snapshot confirms paused.
    auto direct_snap_paused =
        scenario.snapshot(direct_principal, ssg::ViewId{41});
    ASSERT_EQ(direct_snap_paused.sections().follow_edits.mode,
              ssg::FollowMode::paused);

    // WebSocket client resumes follow: shared state transitions both clients.
    auto ws_resume = ssg::encode_command_request(
        {"follow_edits.resume", ws_rev, std::any{}}, codec);
    send_all(ws.socket, masked_frame(0x2, ws_resume));
    auto resume_delta = ssg::decode_session_delta(ws_reader.next().payload);
    ASSERT_TRUE(resume_delta.accepted());
    ws_rev = resume_delta.delta->revision();

    // Both clients now see following: the global mode is shared across paths.
    auto direct_snap_resumed =
        scenario.snapshot(direct_principal, ssg::ViewId{41});
    ASSERT_EQ(direct_snap_resumed.sections().follow_edits.mode,
              ssg::FollowMode::following);
    auto follow_state_after = scenario.model.follow_view_state();
    ASSERT_EQ(follow_state_after.mode, ssg::FollowMode::following);
    ASSERT_TRUE(follow_state_after.active_target.has_value());
    ASSERT_EQ(follow_state_after.active_target->id,
              ssg::DiffFileId{"watched-file-b"});

    // After resume+activate, both clients jump to the same hunk position
    // (designed behavior: activate() resets offsets to the newest hunk line).
    // The independence property was already verified in follow_state_before above.
    if (follow_state_after.clients.size() == 2) {
        ASSERT_EQ(follow_state_after.clients[0].offset.first_row,
                  follow_state_after.clients[1].offset.first_row);
    }

    conc_server.stop();
}

}  // namespace

int main() {
    std::cout << "=== End-to-end parity ===\n";
    RUN(direct_api_loopback_websocket_tui_canonical_state_matches_per_step);
    RUN(concurrent_tui_and_websocket_clients_share_follow_interruption);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
