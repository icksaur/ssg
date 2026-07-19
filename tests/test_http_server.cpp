#include "test_helpers.h"
#include <ssg/application_auth.h>
#include <ssg/editor_session_assembly.h>
#include <ssg/http_server.h>
#include <ssg/protocol.h>
#include <ssg/text_input_commands.h>

#include <http.h>

#include <any>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

#ifdef _WIN32
using TestSocket = SOCKET;
constexpr TestSocket invalid_test_socket = INVALID_SOCKET;
void close_test_socket(TestSocket socket) { closesocket(socket); }
#else
using TestSocket = int;
constexpr TestSocket invalid_test_socket = -1;
void close_test_socket(TestSocket socket) { close(socket); }
#endif

struct SocketOwner {
    TestSocket socket{invalid_test_socket};
    explicit SocketOwner(TestSocket value) : socket{value} {}
    ~SocketOwner() {
        if (socket != invalid_test_socket) close_test_socket(socket);
    }
    SocketOwner(SocketOwner const&) = delete;
    SocketOwner& operator=(SocketOwner const&) = delete;
    SocketOwner(SocketOwner&& other) noexcept : socket{other.socket} {
        other.socket = invalid_test_socket;
    }
};

void send_all(TestSocket socket, std::string const& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        auto const count =
            send(socket, bytes.data() + sent,
                 static_cast<int>(bytes.size() - sent), 0);
        if (count <= 0) throw std::runtime_error{"loopback send failed"};
        sent += static_cast<std::size_t>(count);
    }
}

std::string receive_some(TestSocket socket) {
    std::array<char, 65536> bytes{};
    auto const count =
        recv(socket, bytes.data(), static_cast<int>(bytes.size()), 0);
    if (count <= 0) throw std::runtime_error{"loopback receive failed"};
    return {bytes.data(), static_cast<std::size_t>(count)};
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
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(
                static_cast<char>((payload.size() >> shift) & 0xff));
        }
    }
    for (auto byte : mask) frame.push_back(static_cast<char>(byte));
    for (std::size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(
            static_cast<unsigned char>(payload[i]) ^ mask[i % mask.size()]));
    }
    return frame;
}

class FrameReader {
public:
    explicit FrameReader(TestSocket socket) : socket_{socket} {}

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
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error{"WSAStartup failed"};
    }
#endif
    SocketOwner owner{socket(AF_INET, SOCK_STREAM, 0)};
    if (owner.socket == invalid_test_socket) {
        throw std::runtime_error{"socket creation failed"};
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(owner.socket, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) != 0) {
        throw std::runtime_error{"loopback connect failed"};
    }
    send_all(owner.socket,
             "GET /session HTTP/1.1\r\nHost: 127.0.0.1\r\n"
             "Upgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    auto const response = receive_some(owner.socket);
    if (response.find("101 Switching Protocols") == std::string::npos) {
        throw std::runtime_error{"WebSocket upgrade failed"};
    }
    return owner;
}

SocketOwner connect_loopback(std::uint16_t port) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error{"WSAStartup failed"};
    }
#endif
    SocketOwner owner{socket(AF_INET, SOCK_STREAM, 0)};
    if (owner.socket == invalid_test_socket) {
        throw std::runtime_error{"socket creation failed"};
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(owner.socket, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) != 0) {
        throw std::runtime_error{"loopback connect failed"};
    }
    return owner;
}

ssg::SelectionViewState selection(std::uint64_t byte) {
    ssg::DocumentPosition const position{
        ssg::ByteOffset{byte}, ssg::LineIndex{0}, ssg::CellIndex{byte}};
    return {ssg::SelectionSet{{ssg::Selection{position, position}}},
            0, 0, std::nullopt};
}

ssg::SessionSnapshotSections sections(ssg::Revision revision,
                                      std::string const& marker) {
    ssg::SettingsViewState settings;
    ssg::ThemeSnapshot theme;
    ssg::ShellViewState shell;
    shell.viewport = {20, 8};
    return {
        {revision, marker, ssg::ByteOffset{marker.size()}},
        selection(marker.size()),
        {true, false, marker.size()},
        {{marker}, marker, std::nullopt, std::nullopt},
        {std::nullopt, {{}, 0}},
        {revision, false, {}, ssg::SearchMode::file, {}, std::nullopt, 0,
         false},
        {0, false, false, revision, {}, {}, {}, {}, std::nullopt,
         ssg::FindReplaceError::none, {}},
        settings,
        {{}, {}},
        {{ssg::TextEncoding::utf8, ssg::LineEnding::lf, false, false}},
        {{}, std::nullopt},
        {revision, {}},
        {revision, {}},
        {0, ssg::FollowMode::following, ssg::PaneId{}, std::nullopt, {}, {}},
        {ssg::TreeRevision{0}, {}},
        ssg::plain_text_syntax_view_state(revision, ssg::LanguageId{"plain"},
                                          marker, 4),
        {revision, {}},
        {revision, {}, std::nullopt, {}, {}},
        theme,
        std::move(shell),
    };
}

ssg::ViewportViewState viewport() {
    return {ssg::ViewportDimensions{20, 8},
            0,
            0,
            8,
            {},
            {},
            {8, 8, 0, 0, 0, 8}};
}

class TestHost : public ssg::HttpEditorSessionHost {
public:
    explicit TestHost(ssg::EditorSession& session) : session_{session} {}

    std::optional<ssg::AuthenticatedSession> authenticate(
        std::string_view credential) override {
        if (credential == "denied") return std::nullopt;
        auto const local = credential == "local";
        return ssg::AuthenticatedSession{
            ssg::SessionId{"test-session"},
            ssg::InvocationPrincipal{
                ssg::ClientId{local ? 11u : 12u},
                ssg::InvocationOrigin::websocket,
                local ? std::vector<ssg::CapabilityId>{
                            ssg::CapabilityId{"local_file_drop"}}
                      : std::vector<ssg::CapabilityId>{}},
            ssg::ViewId{local ? 21u : 22u}};
    }

    ssg::SessionSnapshot snapshot(ssg::SessionId const&,
                                  ssg::ClientId client_id) override {
        auto attached = session_.attached_client(client_id);
        if (!attached) throw std::logic_error{"snapshot for detached client"};
        return ssg::assemble_session_snapshot(
            session_.revision(), session_.topology(), attached->principal,
            attached->view_id, viewport(),
            sections(session_.revision(), document));
    }

    void clipboard_response(ssg::SessionId const&, ssg::ClientId,
                            ssg::ClipboardResponse const&) override {
        ++clipboard_responses;
    }
    void status_action(ssg::SessionId const&, ssg::ClientId,
                       ssg::StatusActionInvocation const&) override {
        ++status_actions;
    }
    void binary(ssg::SessionId const&, ssg::ClientId,
                ssg::BinaryFrame const&) override {
        ++binary_frames;
    }

    ssg::EditorSession& session_;
    std::string document;
    std::atomic<int> clipboard_responses{0};
    std::atomic<int> status_actions{0};
    std::atomic<int> binary_frames{0};
};

class ApplicationHost final : public TestHost {
public:
    ApplicationHost(ssg::EditorSession& session,
                    ssg::ApplicationAuthentication authentication)
        : TestHost{session}, authentication_{std::move(authentication)} {}

    std::optional<ssg::AuthenticatedSession> authenticate(
        std::string_view credential) override {
        return authentication_.authenticate(credential);
    }

private:
    ssg::ApplicationAuthentication authentication_;
};

struct Fixture {
    Fixture() {
        for (auto const& descriptor : ssg::p0_command_descriptors()) {
            auto const id = descriptor.id;
            builder.bind(id, [this, id](ssg::CommandContext&,
                                        std::any const& payload) {
                if (id == "text.insert") {
                    host->document +=
                        std::any_cast<ssg::TextInputArguments const&>(payload)
                            .text;
                }
                return ssg::CommandHandlerResult::success();
            });
        }
        session = builder.build();
        host.emplace(*session);
    }

    ssg::EditorSessionBuilder builder;
    std::unique_ptr<ssg::EditorSession> session;
    std::optional<TestHost> host;
};

void attach(TestSocket socket, std::string credential,
            std::optional<ssg::Revision> revision = std::nullopt) {
    send_all(socket,
             masked_frame(0x1, ssg::encode_session_attach_request(
                                   {std::move(credential), revision})));
}

TEST(externally_owned_route_shares_one_server_lifecycle) {
    Fixture fixture;
    Http::Server server{0, Http::BindAddress::loopback};
    server.get("/", [](Http::Context&) {
        return Http::Ok("browser asset", "text/plain");
    });
    ssg::HttpEditorRoute route{
        server, *fixture.session, ssg::build_command_argument_codec_registry(),
        *fixture.host, {"/session", 8, 8, 250ms}};

    server.start();
    auto const bound_port = server.boundPort();
    ASSERT_TRUE(bound_port.has_value());

    auto http = connect_loopback(static_cast<std::uint16_t>(*bound_port));
    send_all(http.socket, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    auto const response = receive_some(http.socket);
    ASSERT_TRUE(response.find("200 OK") != std::string::npos);
    ASSERT_TRUE(response.find("browser asset") != std::string::npos);

    auto websocket =
        connect_websocket(static_cast<std::uint16_t>(*bound_port));
    FrameReader reader{websocket.socket};
    attach(websocket.socket, "local");
    auto decoded = ssg::decode_session_snapshot(reader.next().payload);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.snapshot->client().client_id, ssg::ClientId{11});

    server.stop();
    ASSERT_FALSE(server.boundPort().has_value());
}

TEST(application_route_rejects_wrong_and_stale_bearers_before_attach) {
    Fixture fixture;
    auto stale = ssg::generate_bearer_credential();
    auto current = ssg::generate_bearer_credential();
    auto const stale_value = std::string{stale.value()};
    auto const current_value = std::string{current.value()};
    ApplicationHost host{
        *fixture.session,
        ssg::ApplicationAuthentication{
            std::move(current), ssg::SessionId{"application-session"},
            ssg::ClientId{51}, ssg::ViewId{52}}};
    Http::Server server{0, Http::BindAddress::loopback};
    ssg::HttpEditorRoute route{
        server, *fixture.session, ssg::build_command_argument_codec_registry(),
        host};
    server.start();
    auto const port = static_cast<std::uint16_t>(*server.boundPort());

    {
        auto socket = connect_websocket(port);
        attach(socket.socket, "wrong");
    }
    {
        auto socket = connect_websocket(port);
        attach(socket.socket, stale_value);
    }
    std::this_thread::sleep_for(20ms);
    ASSERT_FALSE(
        fixture.session->attached_client(ssg::ClientId{51}).has_value());

    auto socket = connect_websocket(port);
    FrameReader reader{socket.socket};
    attach(socket.socket, current_value);
    auto decoded = ssg::decode_session_snapshot(reader.next().payload);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.snapshot->client().client_id, ssg::ClientId{51});
    ASSERT_EQ(decoded.snapshot->client().capabilities.size(), std::size_t{1});

    server.stop();
}

TEST(attach_uses_host_principal_and_socket_snapshot_matches_in_process) {
    Fixture fixture;
    constexpr std::uint16_t port = 18775;
    ssg::HttpEditorServer server{
        *fixture.session, ssg::build_command_argument_codec_registry(),
        *fixture.host, {port, "/session", 8, 8, 250ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    auto socket = connect_websocket(port);
    FrameReader reader{socket.socket};
    attach(socket.socket, "local");
    auto decoded = ssg::decode_session_snapshot(reader.next().payload);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.snapshot->client().client_id, ssg::ClientId{11});
    ASSERT_EQ(decoded.snapshot->client().capabilities.size(), std::size_t{1});
    ASSERT_EQ(decoded.snapshot->revision(), fixture.session->revision());
    ASSERT_EQ(decoded.snapshot->sections().document.text,
              fixture.host->document);
    server.stop();
}

TEST(command_delta_replays_on_reconnect_and_eviction_sends_snapshot) {
    Fixture fixture;
    constexpr std::uint16_t port = 18776;
    ssg::HttpEditorServer server{
        *fixture.session, ssg::build_command_argument_codec_registry(),
        *fixture.host, {port, "/session", 8, 1, 250ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    {
        auto socket = connect_websocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket, "local");
        auto initial = ssg::decode_session_snapshot(reader.next().payload);
        ASSERT_TRUE(initial.accepted());
        auto command = ssg::encode_command_request(
            {"text.insert", ssg::Revision{1},
             ssg::TextInputArguments{"a"}},
            ssg::build_command_argument_codec_registry());
        send_all(socket.socket, masked_frame(0x2, command));
        auto delta = ssg::decode_session_delta(reader.next().payload);
        ASSERT_TRUE(delta.accepted());
        ASSERT_EQ(delta.delta->base_revision(), ssg::Revision{1});
        ASSERT_EQ(delta.delta->revision(), ssg::Revision{2});
        send_all(socket.socket, masked_frame(0x2, command));
        auto rejected = ssg::decode_command_result(reader.next().payload);
        ASSERT_TRUE(rejected.accepted());
        ASSERT_EQ(rejected.result->error,
                  ssg::CommandError::stale_revision);
        send_all(socket.socket,
                 masked_frame(0x2, ssg::encode_status_action_invocation(
                                           {ssg::StatusId{3}, "run", 1})));
        for (int attempt = 0;
             attempt < 100 && fixture.host->status_actions.load() != 1;
             ++attempt) {
            std::this_thread::sleep_for(2ms);
        }
        ASSERT_EQ(fixture.host->status_actions.load(), 1);
    }
    std::this_thread::sleep_for(20ms);
    {
        auto socket = connect_websocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket, "local", ssg::Revision{1});
        auto replay = ssg::decode_session_delta(reader.next().payload);
        ASSERT_TRUE(replay.accepted());
        ASSERT_EQ(replay.delta->revision(), ssg::Revision{2});
        auto command = ssg::encode_command_request(
            {"text.insert", ssg::Revision{2},
             ssg::TextInputArguments{"b"}},
            ssg::build_command_argument_codec_registry());
        send_all(socket.socket, masked_frame(0x2, command));
        ASSERT_TRUE(ssg::decode_session_delta(reader.next().payload).accepted());
    }
    std::this_thread::sleep_for(20ms);
    {
        auto socket = connect_websocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket, "local", ssg::Revision{1});
        auto snapshot = ssg::decode_session_snapshot(reader.next().payload);
        ASSERT_TRUE(snapshot.accepted());
        ASSERT_EQ(snapshot.snapshot->revision(), ssg::Revision{3});
        ASSERT_EQ(snapshot.snapshot->sections().document.text,
                  std::string{"ab"});
    }
    server.stop();
}

TEST(clipboard_status_and_binary_ingress_share_the_attached_connection) {
    Fixture fixture;
    constexpr std::uint16_t port = 18777;
    ssg::HttpEditorServer server{
        *fixture.session, ssg::build_command_argument_codec_registry(),
        *fixture.host, {port, "/session", 8, 8, 250ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    auto socket = connect_websocket(port);
    FrameReader reader{socket.socket};
    attach(socket.socket, "remote");
    ASSERT_TRUE(ssg::decode_session_snapshot(reader.next().payload).accepted());

    auto const clipboard = ssg::encode_clipboard_response(
        {7, ssg::Revision{1}, ssg::Revision{1},
         ssg::ClipboardResponseStatus::success, "ok"});
    auto const status = ssg::encode_status_action_invocation(
        {ssg::StatusId{3}, "run", 1});
    auto const binary = ssg::encode_binary_frame(
        {1, ssg::BinaryPayloadKind::dropped_content, 9, {1, 2, 3}});
    ASSERT_TRUE(ssg::decode_clipboard_response(clipboard).accepted());
    ASSERT_TRUE(ssg::decode_status_action_invocation(status).accepted());
    ASSERT_TRUE(ssg::decode_binary_frame(binary).accepted());
    send_all(socket.socket, masked_frame(0x2, clipboard));
    send_all(socket.socket, masked_frame(0x2, status));
    send_all(socket.socket, masked_frame(0x2, binary));
    for (int attempt = 0;
         attempt < 100 && fixture.host->binary_frames.load() != 1;
         ++attempt) {
        std::this_thread::sleep_for(2ms);
    }

    ASSERT_EQ(fixture.host->clipboard_responses.load(), 1);
    ASSERT_EQ(fixture.host->status_actions.load(), 1);
    ASSERT_EQ(fixture.host->binary_frames.load(), 1);
    server.stop();
}

TEST(replay_larger_than_the_outbound_queue_falls_back_to_snapshot) {
    Fixture fixture;
    constexpr std::uint16_t port = 18779;
    ssg::HttpEditorServer server{
        *fixture.session, ssg::build_command_argument_codec_registry(),
        *fixture.host, {port, "/session", 1, 8, 250ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    {
        auto socket = connect_websocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket, "local");
        ASSERT_TRUE(
            ssg::decode_session_snapshot(reader.next().payload).accepted());
        auto registry = ssg::build_command_argument_codec_registry();
        for (auto const& [revision, text] :
             std::vector<std::pair<std::uint64_t, std::string>>{{1, "a"},
                                                                {2, "b"}}) {
            send_all(socket.socket,
                     masked_frame(0x2, ssg::encode_command_request(
                                           {"text.insert",
                                            ssg::Revision{revision},
                                            ssg::TextInputArguments{text}},
                                           registry)));
            ASSERT_TRUE(
                ssg::decode_session_delta(reader.next().payload).accepted());
        }
    }
    std::this_thread::sleep_for(20ms);
    auto socket = connect_websocket(port);
    FrameReader reader{socket.socket};
    attach(socket.socket, "local", ssg::Revision{1});
    auto snapshot = ssg::decode_session_snapshot(reader.next().payload);
    ASSERT_TRUE(snapshot.accepted());
    ASSERT_EQ(snapshot.snapshot->revision(), ssg::Revision{3});
    server.stop();
}

TEST(config_and_attach_codec_reject_unbounded_or_client_authority_inputs) {
    auto encoded = ssg::encode_session_attach_request(
        {"opaque", ssg::Revision{9}});
    auto decoded = ssg::decode_session_attach_request(encoded);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.request->credential, std::string{"opaque"});
    ASSERT_EQ(decoded.request->last_applied_revision, ssg::Revision{9});
    ASSERT_FALSE(ssg::decode_session_attach_request(
                     "SSG1 ATTACH 9 6f7061717565 capabilities=all")
                     .accepted());

    Fixture fixture;
    ASSERT_THROWS(
        ssg::HttpEditorServer(
            *fixture.session, ssg::build_command_argument_codec_registry(),
            *fixture.host, {18778, "/session", 0, 1, 250ms}),
        std::invalid_argument);
    ASSERT_THROWS(
        ssg::HttpEditorServer(
            *fixture.session, ssg::build_command_argument_codec_registry(),
            *fixture.host, {18778, "/session", 1, 0, 250ms}),
        std::invalid_argument);
}

TEST(slow_client_cannot_grow_the_outbound_queue) {
    Fixture fixture;
    constexpr std::uint16_t port = 18778;
    ssg::HttpEditorServer server{
        *fixture.session, ssg::build_command_argument_codec_registry(),
        *fixture.host, {port, "/session", 1, 2, 1ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    auto socket = connect_websocket(port);
    attach(socket.socket, "remote");
    std::this_thread::sleep_for(20ms);

    ssg::BinaryFrame large{
        1, ssg::BinaryPayloadKind::dropped_content, 1,
        std::vector<std::uint8_t>(4 * 1024 * 1024, 0x5a)};
    for (int attempt = 0; attempt < 16; ++attempt) {
        (void)server.send_binary(ssg::ClientId{12}, large);
    }
    bool disconnected = false;
    for (int attempt = 0; attempt < 100 && !disconnected; ++attempt) {
        disconnected =
            !server.send_binary(ssg::ClientId{12},
                                {1, ssg::BinaryPayloadKind::dropped_content,
                                 2, {1}});
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_TRUE(disconnected);
    server.stop();
}

}  // namespace

int main() {
    std::cout << "=== HTTP editor server ===\n";
    RUN(externally_owned_route_shares_one_server_lifecycle);
    RUN(application_route_rejects_wrong_and_stale_bearers_before_attach);
    RUN(attach_uses_host_principal_and_socket_snapshot_matches_in_process);
    RUN(command_delta_replays_on_reconnect_and_eviction_sends_snapshot);
    RUN(clipboard_status_and_binary_ingress_share_the_attached_connection);
    RUN(replay_larger_than_the_outbound_queue_falls_back_to_snapshot);
    RUN(config_and_attach_codec_reject_unbounded_or_client_authority_inputs);
    RUN(slow_client_cannot_grow_the_outbound_queue);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
