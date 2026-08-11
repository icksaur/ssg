#include "fixtures/end_to_end/e2e_fixture.h"
#include "test_helpers.h"
#include "tui_fixture.h"

#include <ssg/EditorSessionBuilder.h>

#include "all_command_ids.h"
#include <ssg/FollowEditsModel.h>
#include <ssg/HttpEditorServer.h>
#include <ssg/Protocol.h>
#include <ssg/session_snapshot.h>
#include <ssg/SyntaxModel.h>

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
constexpr TestSocket kInvalidTestSocket = -1;
void closeTestSocket(TestSocket s) { close(s); }
#endif

struct SocketOwner {
    TestSocket socket{kInvalidTestSocket};
    explicit SocketOwner(TestSocket s) : socket{s} {}
    ~SocketOwner() {
        if (socket != kInvalidTestSocket) closeTestSocket(socket);
    }
    SocketOwner(SocketOwner const&) = delete;
    SocketOwner& operator=(SocketOwner const&) = delete;
    SocketOwner(SocketOwner&& other) noexcept : socket{other.socket} {
        other.socket = kInvalidTestSocket;
    }
};

void sendAll(TestSocket s, std::string const& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        auto count = send(s, bytes.data() + sent,
                          static_cast<int>(bytes.size() - sent), 0);
        if (count <= 0) throw std::runtime_error{"loopback send failed"};
        sent += static_cast<std::size_t>(count);
    }
}

std::string receiveSome(TestSocket s) {
    std::array<char, 65536> buf{};
    auto count = recv(s, buf.data(), static_cast<int>(buf.size()), 0);
    if (count <= 0) throw std::runtime_error{"loopback receive failed"};
    return {buf.data(), static_cast<std::size_t>(count)};
}

std::string maskedFrame(std::uint8_t opcode, std::string const& payload) {
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
            bytes_ += receiveSome(socket_);
        }
    }

private:
    TestSocket socket_;
    std::string bytes_;
};

SocketOwner connectWebsocket(std::uint16_t port) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        throw std::runtime_error{"WSAStartup failed"};
#endif
    SocketOwner owner{socket(AF_INET, SOCK_STREAM, 0)};
    if (owner.socket == kInvalidTestSocket)
        throw std::runtime_error{"socket creation failed"};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(owner.socket, reinterpret_cast<sockaddr*>(&addr),
                sizeof(addr)) != 0)
        throw std::runtime_error{"loopback connect failed"};
    sendAll(owner.socket,
             "GET /session HTTP/1.1\r\nHost: 127.0.0.1\r\n"
             "Upgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    std::string resp;
    while (resp.find("\r\n\r\n") == std::string::npos)
        resp += receiveSome(owner.socket);
    if (resp.find("101") == std::string::npos)
        throw std::runtime_error{"WebSocket upgrade failed"};
    return owner;
}

void wsAttach(TestSocket s, std::optional<ssg::Revision> base = std::nullopt) {
    sendAll(s, maskedFrame(0x1, ssg::encodeSessionAttachRequest({base})));
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

        ssg::PromptStatusViewState promptStatus;
        if (state_.prompt_open) {
            promptStatus.prompt = ssg::PromptViewState{
                ssg::PromptKind::Path, "Open a workspace path",
                {0, 2, 80, 1},
                {{ssg::PromptControlKind::Input, "path", "Workspace path", "",
                  false, {0, 2, 80, 1}}}};
        }
        promptStatus.status.items.push_back(
            {ssg::StatusId{7}, ssg::StatusPriority::Information, 3,
             "Recovery is ready",
             {{"reopen", "Reopen closed tab", "tab.reopen_closed"}}});

        ssg::SettingsViewState settings;
        ssg::KeymapViewState keymap{
            "end-to-end fixture",
            {{{ssg::KeyStroke{ssg::KeyCode::KeyA, false, false, false, false}},
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
        for (std::size_t i = 0; i < theme.roleColors.size(); ++i) {
            auto ch = static_cast<std::uint8_t>(i * 7);
            theme.roleColors[i] = ssg::SrgbColor::fromSerializedChannels(
                ch, static_cast<std::uint8_t>(255 - ch), ch);
        }
        for (std::size_t i = 0; i < theme.syntaxColors.size(); ++i) {
            auto ch = static_cast<std::uint8_t>((i + 1) * 5);
            theme.syntaxColors[i] = ssg::SrgbColor::fromSerializedChannels(
                ch, static_cast<std::uint8_t>(255 - ch), ch);
        }

        ssg::ShellViewState shell;
        shell.viewport = {80, 24};
        shell.header = ssg::Rect{0, 0, 80, 1};
        shell.footer = ssg::Rect{0, 23, 80, 1};
        shell.tabBar = ssg::Rect{18, 1, 62, 1};
        shell.panel = ssg::Rect{0, 1, 18, 22};
        shell.prompt = state_.prompt_open
                           ? std::optional<ssg::Rect>{ssg::Rect{18, 2, 61, 1}}
                           : std::nullopt;
        shell.panes.push_back(
            {ssg::PaneId{1}, {18, 2, 62, 21}, {18, 2, 61, 21},
             {79, 2, 1, 21}});
        shell.accessibilityNodes = {
            {ssg::ShellNodeKind::Header, "header", "Workspace /fixture",
             *shell.header, ssg::SemanticRole::Header},
            {ssg::ShellNodeKind::HeaderField, "path",
             "Current path fixture.txt", *shell.header,
             ssg::SemanticRole::Header},
            {ssg::ShellNodeKind::Footer, "footer", "UTF-8 LF", *shell.footer,
             ssg::SemanticRole::Footer},
            {ssg::ShellNodeKind::FooterAction, "reopen", "Reopen closed tab",
             *shell.footer, ssg::SemanticRole::StatusInfo},
            {ssg::ShellNodeKind::TabBar, "tabs", "Open tabs", *shell.tabBar,
             ssg::SemanticRole::TabActive},
            {ssg::ShellNodeKind::Pane, "pane-1", "Editor pane",
             shell.panes.front().content, ssg::SemanticRole::Canvas},
            {ssg::ShellNodeKind::Scrollbar, "scrollbar-1", "Editor scrollbar",
             shell.panes.front().scrollbar, ssg::SemanticRole::ScrollbarThumb},
        };
        if (state_.prompt_open) {
            shell.accessibilityNodes.push_back(
                {ssg::ShellNodeKind::PromptReservation, "prompt",
                 "Open a workspace path", *shell.prompt,
                 ssg::SemanticRole::Prompt});
        }
        shell.accessibilityNodes.push_back(
            {ssg::ShellNodeKind::FooterField, "wrap",
             state_.word_wrap ? "Word wrap on" : "Word wrap off", *shell.footer,
             ssg::SemanticRole::Footer});

        return {
            {revision, state_.text, ssg::ByteOffset{state_.text.size()}},
            {ssg::SelectionSet{std::move(sels)}, state_.first_row, 0, std::nullopt},
            {!state_.undo_text.empty(), !state_.redo_text.empty(),
             state_.undo_text.size() + state_.redo_text.size()},
            {{state_.clipboard}, state_.clipboard, std::nullopt},
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
            ssg::SyntaxViewState::plainText(revision,
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
            0,
            100,
            {},
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
        ssg::testing::registerStandIns(builder, [this](std::string id) {
            return [this, id](ssg::CommandContext& ctx,
                              std::any const& payload) {
                return model.apply(id, payload, ctx.principal().clientId(),
                                   ctx.revision());
            };
        });
        session = builder.build();
    }

    ssg::SessionSnapshot snapshot(ssg::InvocationPrincipal const& principal,
                                  ssg::ViewId viewId) const {
        return ssg::SessionSnapshotCodec{}.assemble(
            session->revision(), session->topology(), principal, viewId,
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

    std::optional<ssg::AttachedSession> attach() override {
        return ssg::AttachedSession{
            ssg::SessionId{"e2e-ws"}, peer_, ssg::ViewId{31}};
    }

    ssg::SessionSnapshot snapshot(ssg::SessionId const&,
                                  ssg::ClientId clientId) override {
        auto attached = scenario_.session->attachedClient(clientId);
        if (!attached) throw std::logic_error{"snapshot for detached client"};
        return scenario_.snapshot(attached->principal, attached->viewId);
    }

    void statusAction(ssg::SessionId const&, ssg::ClientId,
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

TEST(directApiLoopbackWebsocketTuiCanonicalStateMatchesPerStep) {
    auto const steps = e2e::load_workflow(SSG_E2E_WORKFLOW_PATH);
    ASSERT_TRUE(!steps.empty());

    // ── Direct API scenario ──
    EndToEndScenario direct;
    ssg::InvocationPrincipal const directPrincipal{
        ssg::ClientId{21}, ssg::InvocationOrigin::InProcess,
        {ssg::CapabilityId{"local_file_drop"}}};
    ASSERT_TRUE(direct.session->attach(directPrincipal, ssg::ViewId{21})
                    .accepted());

    // ── TUI scenario ──
    EndToEndScenario tuiScenario;
    ssg::InvocationPrincipal const tuiPrincipal{
        ssg::ClientId{22}, ssg::InvocationOrigin::InProcess,
        {ssg::CapabilityId{"local_file_drop"}}};
    ssg::tui::TuiClient tuiClient{
        *tuiScenario.session, tuiPrincipal, ssg::ViewId{22},
        [&] {
            return tuiScenario.snapshot(tuiPrincipal, ssg::ViewId{22});
        }};

    // ── WebSocket scenario ──
    EndToEndScenario wsScenario;
    ssg::InvocationPrincipal const wsPeerPrincipal{
        ssg::ClientId{31}, ssg::InvocationOrigin::Websocket,
        {ssg::CapabilityId{"local_file_drop"}}};
    PeerHost wsHost{wsScenario, wsPeerPrincipal};
    constexpr std::uint16_t wsPort = 18800;
    ssg::HttpEditorServer wsServer{
        *wsScenario.session,
        wsHost, {wsPort, "/session", 64, 128, 500ms}};
    wsServer.start();
    std::this_thread::sleep_for(30ms);

    auto ws = connectWebsocket(wsPort);
    FrameReader wsReader{ws.socket};
    wsAttach(ws.socket);
    auto initialSnap = ssg::ProtocolCodec{}.decodeSessionSnapshot(wsReader.next().payload);
    ASSERT_TRUE(initialSnap.accepted());
    auto wsSnapshot = std::move(*initialSnap.snapshot);
    auto wsState = e2e::canonical(wsSnapshot);
    ssg::Revision wsRevision = wsSnapshot.revision();

    auto const codec = ssg::CommandArgumentCodecRegistry{wsScenario.session->catalog()};

    for (auto const& step : steps) {
        // Direct dispatch
        auto directResult = direct.session->dispatch(
            directPrincipal.clientId(),
            {step.command_id, direct.session->revision(), step.payload});
        ASSERT_EQ(directResult.accepted(), step.expected_accepted);

        // TUI dispatch
        auto tuiResult =
            tuiClient.submit(step.command_id, step.payload);
        ASSERT_EQ(tuiResult.accepted(), step.expected_accepted);

        // WebSocket dispatch
        auto wsEncoded = ssg::ProtocolCodec{}.encodeCommandRequest(
            {step.command_id, wsRevision, step.payload}, codec);
        sendAll(ws.socket, maskedFrame(0x2, wsEncoded));
        auto wsFrame = wsReader.next();
        if (step.expected_accepted) {
            auto wsDelta =
                ssg::ProtocolCodec{}.decodeSessionDelta(wsFrame.payload);
            ASSERT_TRUE(wsDelta.accepted());
            e2e::apply(wsState, *wsDelta.delta);
            wsRevision = wsState.revision;
        } else {
            auto wsCmdResult =
                ssg::ProtocolCodec{}.decodeCommandResult(wsFrame.payload);
            ASSERT_TRUE(wsCmdResult.accepted());
            ASSERT_FALSE(wsCmdResult.result->accepted());
        }

        // All paths project state from the snapshots each client observes.
        auto directState =
            e2e::canonical(direct.snapshot(directPrincipal, ssg::ViewId{11}));
        auto tuiState = e2e::canonical(tuiClient.snapshot());
        ASSERT_EQ(directState, tuiState);
        ASSERT_EQ(directState, wsState);
    }

    wsServer.stop();
}

// ─── Concurrent fixture model with real FollowEditsModel ─────────────────────
// Used by the concurrent follow-interruption test where two clients attach to
// one session and independently navigate while sharing follow state.

class ConcurrentFixtureModel {
public:
    ConcurrentFixtureModel() : followModel_{{.queueCapacity = 4}} {}

    void registerClient(ssg::ClientId client,
                         ssg::ViewportDimensions dims) {
        std::lock_guard lock{mutex_};
        (void)followModel_.attachClient(client, dims);
    }

    ssg::CommandHandlerResult apply(std::string_view id,
                                    std::any const& payload,
                                    ssg::ClientId client,
                                    ssg::Revision) {
        std::lock_guard lock{mutex_};
        if (id == "view.scroll_lines") {
            auto rows =
                std::any_cast<ssg::ScrollLinesArguments const&>(payload).rows;
            perClientRow_[client] = static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(perClientRow_[client]) + rows,
                    0, 80));
            (void)followModel_.applyNavigation(
                {client, ssg::NavigationClass::User, ssg::PaneId{1},
                 ssg::FollowScrollOffset{perClientRow_[client], 0}});
        } else if (id == "follow_edits.pause") {
            (void)followModel_.pause();
        } else if (id == "follow_edits.resume") {
            (void)followModel_.resume(currentDiff_);
        } else if (id == "follow_edits.toggle") {
            (void)followModel_.toggle(currentDiff_);
        }
        return ssg::CommandHandlerResult::success();
    }

    void acceptExternalChange(std::string const& fileId,
                                std::filesystem::path path,
                                ssg::Revision sourceRev) {
        std::lock_guard lock{mutex_};
        ssg::DiffFileView file{ssg::DiffFileId{fileId}};
        file.path = std::move(path);
        file.hunks.push_back(
            {.baselineStart = 10,
             .targetStart = 10,
             .baselineLines = {"old line\n"},
             .targetLines = {"new line\n"}});
        currentDiff_.revision = sourceRev;
        currentDiff_.files.push_back(file);
        (void)followModel_.acceptExternalChange(
            currentDiff_.files.back(), sourceRev);
    }

    ssg::FollowEditsViewState followViewState() const {
        std::lock_guard lock{mutex_};
        return followModel_.viewState();
    }

    ssg::SessionSnapshotSections sections(ssg::Revision revision,
                                          ssg::ClientId client) const {
        std::lock_guard lock{mutex_};
        auto firstRow = perClientRow_.count(client)
                             ? perClientRow_.at(client)
                             : std::uint32_t{0};
        ssg::DocumentPosition const pos{
            ssg::ByteOffset{0}, ssg::LineIndex{0}, ssg::CellIndex{0}};
        ssg::SettingsViewState settings;
        ssg::ThemeSnapshot theme{};
        ssg::ShellViewState shell;
        shell.viewport = {80, 24};
        return {
            {revision, "concurrent", ssg::ByteOffset{0}},
            {ssg::SelectionSet{{ssg::Selection{pos, pos}}}, firstRow,
             0, std::nullopt},
            {false, false, 0},
            {{}, {}, std::nullopt},
            {std::nullopt, {{}, 0}},
            {revision, false, {}, ssg::SearchMode::File, {}, std::nullopt, 0,
             false},
            {0, false, false, revision, {}, {}, {}, {}, std::nullopt,
             ssg::FindReplaceError::None, {}},
            std::move(settings),
            {"concurrent", {}},
            {{ssg::TextEncoding::Utf8, ssg::LineEnding::Lf, false, false}},
            {{}, std::nullopt},
            {revision, {}},
            {revision, {}},
            followModel_.viewState(),
            {ssg::TreeRevision{revision.value()}, {}},
            ssg::SyntaxViewState::plainText(
                revision, ssg::LanguageId{"plain"}, "concurrent", 4),
            {revision, {}},
            {revision, {}, std::nullopt, {}, {}},
            std::move(theme),
            std::move(shell),
        };
    }

    ssg::ViewportViewState viewport(ssg::ClientId client) const {
        std::lock_guard lock{mutex_};
        auto firstRow = perClientRow_.count(client)
                             ? perClientRow_.at(client)
                             : std::uint32_t{0};
        return {ssg::ViewportDimensions{80, 20}, firstRow, 0, 100, {}, {},
                {{0, 0, 0, ssg::CellIndex{0}, 0, 1}},
                {100, 20, firstRow, 80, firstRow, 4}};
    }

private:
    mutable std::mutex mutex_;
    ssg::FollowEditsModel followModel_;
    ssg::DiffViewState currentDiff_;
    std::map<ssg::ClientId, std::uint32_t> perClientRow_;
};

struct ConcurrentScenario {
    ConcurrentScenario() {
        ssg::testing::registerStandIns(builder, [this](std::string id) {
            return [this, id](ssg::CommandContext& ctx,
                              std::any const& payload) {
                return model.apply(id, payload, ctx.principal().clientId(),
                                   ctx.revision());
            };
        });
        session = builder.build();
    }

    ssg::SessionSnapshot snapshot(ssg::InvocationPrincipal const& principal,
                                  ssg::ViewId viewId) const {
        return ssg::SessionSnapshotCodec{}.assemble(
            session->revision(), session->topology(), principal, viewId,
            model.viewport(principal.clientId()),
            model.sections(session->revision(), principal.clientId()));
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

    std::optional<ssg::AttachedSession> attach() override {
        return ssg::AttachedSession{
            ssg::SessionId{"e2e-conc"}, peer_, ssg::ViewId{42}};
    }

    ssg::SessionSnapshot snapshot(ssg::SessionId const&,
                                  ssg::ClientId clientId) override {
        auto attached = scenario_.session->attachedClient(clientId);
        if (!attached) throw std::logic_error{"snapshot for detached client"};
        return scenario_.snapshot(attached->principal, attached->viewId);
    }

    void statusAction(ssg::SessionId const&, ssg::ClientId,
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

TEST(concurrentTuiAndWebsocketClientsShareFollowInterruption) {
    ConcurrentScenario scenario;
    ssg::InvocationPrincipal const directPrincipal{
        ssg::ClientId{41}, ssg::InvocationOrigin::InProcess};
    ssg::InvocationPrincipal const wsPeerPrincipal{
        ssg::ClientId{42}, ssg::InvocationOrigin::Websocket};

    scenario.model.registerClient(ssg::ClientId{41},
                                   ssg::ViewportDimensions{80, 20});

    ASSERT_TRUE(
        scenario.session->attach(directPrincipal, ssg::ViewId{41}).accepted());

    constexpr std::uint16_t concPort = 18801;
    ConcurrentHost concHost{scenario, wsPeerPrincipal};
    ssg::HttpEditorServer concServer{
        *scenario.session,
        concHost, {concPort, "/session", 64, 128, 500ms}};
    concServer.start();
    std::this_thread::sleep_for(30ms);

    auto ws = connectWebsocket(concPort);
    FrameReader wsReader{ws.socket};
    wsAttach(ws.socket);
    auto initialWs = ssg::ProtocolCodec{}.decodeSessionSnapshot(wsReader.next().payload);
    ASSERT_TRUE(initialWs.accepted());
    ssg::Revision wsRev = initialWs.snapshot->revision();

    // Register WS client in the follow model now that we know it attached.
    scenario.model.registerClient(ssg::ClientId{42},
                                   ssg::ViewportDimensions{80, 20});

    auto const codec = ssg::CommandArgumentCodecRegistry{scenario.session->catalog()};

    // Navigate direct client to row 3 and WS client to row 10 to establish
    // independent viewport offsets.
    auto directScrollResult = scenario.session->dispatch(
        directPrincipal.clientId(),
        {"view.scroll_lines", scenario.session->revision(),
         ssg::ScrollLinesArguments{3}});
    ASSERT_TRUE(directScrollResult.accepted());
    // Sync ws_rev: direct dispatch advanced the session revision.
    wsRev = directScrollResult.revision;

    auto wsScroll = ssg::ProtocolCodec{}.encodeCommandRequest(
        {"view.scroll_lines", wsRev, ssg::ScrollLinesArguments{10}}, codec);
    sendAll(ws.socket, maskedFrame(0x2, wsScroll));
    auto scrollDelta = ssg::ProtocolCodec{}.decodeSessionDelta(wsReader.next().payload);
    ASSERT_TRUE(scrollDelta.accepted());
    wsRev = scrollDelta.delta->revision();

    // Both clients have different scroll offsets in the follow model.
    auto followStateBefore = scenario.model.followViewState();
    bool foundDiff = false;
    if (followStateBefore.clients.size() == 2) {
        foundDiff =
            followStateBefore.clients[0].offset.firstRow !=
            followStateBefore.clients[1].offset.firstRow;
    }
    // Viewport-independent: per-client offsets diverged from independent navigation.
    ASSERT_TRUE(foundDiff);

    // After user navigation both clients are now in paused mode (apply_navigation
    // with NavigationClass::user transitions the shared FollowEditsModel to paused).
    auto directSnapAfterScroll =
        scenario.snapshot(directPrincipal, ssg::ViewId{41});
    ASSERT_EQ(directSnapAfterScroll.sections().followEdits.mode,
              ssg::FollowMode::Paused);

    // Accept changes to two watched files while paused; resume must choose the
    // newest target across the multi-file queue.
    scenario.model.acceptExternalChange(
        "watched-file-a", std::filesystem::path{"/workspace/watched-a.txt"},
        ssg::Revision{scenario.session->revision().value() + 1});
    scenario.model.acceptExternalChange(
        "watched-file-b", std::filesystem::path{"/workspace/watched-b.txt"},
        ssg::Revision{scenario.session->revision().value() + 2});

    auto queuedState = scenario.model.followViewState();
    ASSERT_EQ(queuedState.mode, ssg::FollowMode::Paused);
    ASSERT_EQ(queuedState.queuedTargets.size(), std::size_t{2});

    // Explicit pause dispatch is idempotent from the already-paused state.
    auto pauseResult = scenario.session->dispatch(
        directPrincipal.clientId(),
        {"follow_edits.pause", scenario.session->revision(), {}});
    ASSERT_TRUE(pauseResult.accepted());
    // Sync ws_rev: direct pause advanced the session revision.
    wsRev = pauseResult.revision;

    // Direct snapshot confirms paused.
    auto directSnapPaused =
        scenario.snapshot(directPrincipal, ssg::ViewId{41});
    ASSERT_EQ(directSnapPaused.sections().followEdits.mode,
              ssg::FollowMode::Paused);

    // WebSocket client resumes follow: shared state transitions both clients.
    auto wsResume = ssg::ProtocolCodec{}.encodeCommandRequest(
        {"follow_edits.resume", wsRev, std::any{}}, codec);
    sendAll(ws.socket, maskedFrame(0x2, wsResume));
    auto resumeDelta = ssg::ProtocolCodec{}.decodeSessionDelta(wsReader.next().payload);
    ASSERT_TRUE(resumeDelta.accepted());
    wsRev = resumeDelta.delta->revision();

    // Both clients now see following: the global mode is shared across paths.
    auto directSnapResumed =
        scenario.snapshot(directPrincipal, ssg::ViewId{41});
    ASSERT_EQ(directSnapResumed.sections().followEdits.mode,
              ssg::FollowMode::Following);
    auto followStateAfter = scenario.model.followViewState();
    ASSERT_EQ(followStateAfter.mode, ssg::FollowMode::Following);
    ASSERT_TRUE(followStateAfter.activeTarget.has_value());
    ASSERT_EQ(followStateAfter.activeTarget->id,
              ssg::DiffFileId{"watched-file-b"});

    // After resume+activate, both clients jump to the same hunk position
    // (designed behavior: activate() resets offsets to the newest hunk line).
    // The independence property was already verified in follow_state_before above.
    if (followStateAfter.clients.size() == 2) {
        ASSERT_EQ(followStateAfter.clients[0].offset.firstRow,
                  followStateAfter.clients[1].offset.firstRow);
    }

    concServer.stop();
}

}  // namespace

int main() {
    std::cout << "=== End-to-end parity ===\n";
    RUN(directApiLoopbackWebsocketTuiCanonicalStateMatchesPerStep);
    RUN(concurrentTuiAndWebsocketClientsShareFollowInterruption);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
