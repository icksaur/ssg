#include "test_helpers.h"
#include <ssg/EditorSessionBuilder.h>

#include "all_command_ids.h"
#include <ssg/HttpEditorServer.h>
#include <ssg/Protocol.h>
#include <ssg/TextInputCommands.h>

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
constexpr TestSocket kInvalidTestSocket = -1;
void closeTestSocket(TestSocket socket) { close(socket); }
#endif

struct SocketOwner {
    TestSocket socket{kInvalidTestSocket};
    explicit SocketOwner(TestSocket value) : socket{value} {}
    ~SocketOwner() {
        if (socket != kInvalidTestSocket) closeTestSocket(socket);
    }
    SocketOwner(SocketOwner const&) = delete;
    SocketOwner& operator=(SocketOwner const&) = delete;
    SocketOwner(SocketOwner&& other) noexcept : socket{other.socket} {
        other.socket = kInvalidTestSocket;
    }
};

void sendAll(TestSocket socket, std::string const& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        auto const count =
            send(socket, bytes.data() + sent,
                 static_cast<int>(bytes.size() - sent), 0);
        if (count <= 0) throw std::runtime_error{"loopback send failed"};
        sent += static_cast<std::size_t>(count);
    }
}

std::string receiveSome(TestSocket socket) {
    std::array<char, 65536> bytes{};
    auto const count =
        recv(socket, bytes.data(), static_cast<int>(bytes.size()), 0);
    if (count <= 0) throw std::runtime_error{"loopback receive failed"};
    return {bytes.data(), static_cast<std::size_t>(count)};
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
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error{"WSAStartup failed"};
    }
#endif
    SocketOwner owner{socket(AF_INET, SOCK_STREAM, 0)};
    if (owner.socket == kInvalidTestSocket) {
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
    sendAll(owner.socket,
             "GET /session HTTP/1.1\r\nHost: 127.0.0.1\r\n"
             "Upgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    auto const response = receiveSome(owner.socket);
    if (response.find("101 Switching Protocols") == std::string::npos) {
        throw std::runtime_error{"WebSocket upgrade failed"};
    }
    return owner;
}

SocketOwner connectLoopback(std::uint16_t port) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error{"WSAStartup failed"};
    }
#endif
    SocketOwner owner{socket(AF_INET, SOCK_STREAM, 0)};
    if (owner.socket == kInvalidTestSocket) {
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
        {{marker}, marker, std::nullopt},
        {{{}, 0}},
        {revision, false, {}, ssg::SearchMode::File, {}, std::nullopt, 0,
         false},
        {0, false, false, revision, {}, {}, {}, {}, std::nullopt,
         ssg::FindReplaceError::None, {}},
        settings,
        {{}, {}},
        {{ssg::TextEncoding::Utf8, ssg::LineEnding::Lf, false, false}},
        {{}, std::nullopt},
        {revision, {}},
        {revision, {}},
        {0, ssg::FollowMode::Following, ssg::PaneId{}, std::nullopt, {}, {}},
        {ssg::TreeRevision{0}, {}},
        ssg::SyntaxViewState::plainText(revision, ssg::LanguageId{"plain"},
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
            {},
            {8, 8, 0, 0, 0, 8}};
}

class TestHost : public ssg::HttpEditorSessionHost {
public:
    // `remote` is fixed at construction: a host's locality policy is a property
    // of its deployment, not something a client toggles. A remote host grants no
    // capability; a local host grants local_file_drop.
    explicit TestHost(ssg::EditorSession& session, bool remote = false)
        : session{session}, remote{remote} {}

    std::optional<ssg::AttachedSession> attach() override {
        return ssg::AttachedSession{
            ssg::SessionId{"test-session"},
            ssg::InvocationPrincipal{
                ssg::ClientId{remote ? 12u : 11u},
                attachOrigin,
                remote ? std::vector<ssg::CapabilityId>{}
                       : std::vector<ssg::CapabilityId>{
                             ssg::CapabilityId{"local_file_drop"}}},
            ssg::ViewId{remote ? 22u : 21u}};
    }

    ssg::SessionSnapshot snapshot(ssg::SessionId const&,
                                  ssg::ClientId clientId) override {
        auto attached = session.attachedClient(clientId);
        if (!attached) throw std::logic_error{"snapshot for detached client"};
        return ssg::SessionSnapshotCodec{}.assemble(
            session.revision(), session.topology(), attached->principal,
            attached->viewId, viewport(),
            sections(session.revision(), document));
    }

    void statusAction(ssg::SessionId const&, ssg::ClientId,
                       ssg::StatusActionInvocation const&) override {
        ++statusActions;
    }
    void binary(ssg::SessionId const&, ssg::ClientId,
                ssg::BinaryFrame const&) override {
        ++binaryFrames;
    }

    ssg::EditorSession& session;
    std::string document;
    bool remote{false};
    // A host binds a Websocket-origin principal onto a socket; the origin is a
    // property of the deployment, exposed here so a seam test can drive a
    // non-Websocket origin through the same attach path.
    ssg::InvocationOrigin attachOrigin{ssg::InvocationOrigin::Websocket};
    std::atomic<int> statusActions{0};
    std::atomic<int> binaryFrames{0};
};

struct Fixture {
    explicit Fixture(bool remote = false) : remote_{remote} {
        ssg::testing::registerStandIns(builder, [this](std::string id) {
            return [this, id](ssg::CommandContext&, std::any const& payload) {
                if (id == "text.insert") {
                    host->document +=
                        std::any_cast<ssg::TextInputArguments const&>(payload)
                            .text;
                }
                return ssg::CommandHandlerResult::success();
            };
        });
        session = builder.build();
        host.emplace(*session, remote_);
    }

    bool remote_;
    ssg::EditorSessionBuilder builder;
    std::unique_ptr<ssg::EditorSession> session;
    std::optional<TestHost> host;
};

void attach(TestSocket socket,
            std::optional<ssg::Revision> revision = std::nullopt) {
    sendAll(socket,
             maskedFrame(0x1, ssg::encodeSessionAttachRequest({revision})));
}

// True when the server has closed the connection: the next frame is a WebSocket
// close (opcode 0x8) or the socket has already gone away.
bool connectionClosed(FrameReader& reader) {
    try {
        return reader.next().opcode == 0x8;
    } catch (std::exception const&) {
        return true;
    }
}

TEST(externallyOwnedRouteSharesOneServerLifecycle) {
    Fixture fixture;
    Http::Server server{0, Http::BindAddress::loopback};
    server.get("/", [](Http::Context&) {
        return Http::Ok("browser asset", "text/plain");
    });
    ssg::HttpEditorRoute route{
        server, *fixture.session,
        *fixture.host, {"/session", 8, 8, 250ms}};

    server.start();
    auto const boundPort = server.boundPort();
    ASSERT_TRUE(boundPort.has_value());

    auto http = connectLoopback(static_cast<std::uint16_t>(*boundPort));
    sendAll(http.socket, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    auto const response = receiveSome(http.socket);
    ASSERT_TRUE(response.find("200 OK") != std::string::npos);
    ASSERT_TRUE(response.find("browser asset") != std::string::npos);

    auto websocket =
        connectWebsocket(static_cast<std::uint16_t>(*boundPort));
    FrameReader reader{websocket.socket};
    attach(websocket.socket);
    auto decoded = ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.snapshot->client().clientId, ssg::ClientId{11});

    server.stop();
    ASSERT_FALSE(server.boundPort().has_value());
}

TEST(attachUsesHostPrincipalAndSocketSnapshotMatchesInProcess) {
    Fixture fixture;
    constexpr std::uint16_t port = 18775;
    ssg::HttpEditorServer server{
        *fixture.session,
        *fixture.host, {port, "/session", 8, 8, 250ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    auto socket = connectWebsocket(port);
    FrameReader reader{socket.socket};
    attach(socket.socket);
    auto decoded = ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.snapshot->client().clientId, ssg::ClientId{11});
    ASSERT_EQ(decoded.snapshot->client().capabilities.size(), std::size_t{1});
    ASSERT_EQ(decoded.snapshot->revision(), fixture.session->revision());
    ASSERT_EQ(decoded.snapshot->sections().document.text,
              fixture.host->document);
    server.stop();
}

TEST(commandDeltaReplaysOnReconnectAndEvictionSendsSnapshot) {
    Fixture fixture;
    constexpr std::uint16_t port = 18776;
    ssg::HttpEditorServer server{
        *fixture.session,
        *fixture.host, {port, "/session", 8, 1, 250ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    {
        auto socket = connectWebsocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket);
        auto initial = ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload);
        ASSERT_TRUE(initial.accepted());
        auto const registry =
            ssg::CommandArgumentCodecRegistry{fixture.session->catalog()};
        auto command = ssg::ProtocolCodec{}.encodeCommandRequest(
            {"text.insert", ssg::Revision{1},
             ssg::TextInputArguments{"a"}}, registry);
        sendAll(socket.socket, maskedFrame(0x2, command));
        auto delta = ssg::ProtocolCodec{}.decodeSessionDelta(reader.next().payload);
        ASSERT_TRUE(delta.accepted());
        ASSERT_EQ(delta.delta->baseRevision(), ssg::Revision{1});
        ASSERT_EQ(delta.delta->revision(), ssg::Revision{2});
        sendAll(socket.socket, maskedFrame(0x2, command));
        auto rejected = ssg::ProtocolCodec{}.decodeCommandResult(reader.next().payload);
        ASSERT_TRUE(rejected.accepted());
        ASSERT_EQ(rejected.result->error,
                  ssg::CommandError::StaleRevision);
        sendAll(socket.socket,
                 maskedFrame(0x2, ssg::ProtocolCodec{}.encodeStatusActionInvocation(
                                           {ssg::StatusId{3}, "run", 1})));
        for (int attempt = 0;
             attempt < 100 && fixture.host->statusActions.load() != 1;
             ++attempt) {
            std::this_thread::sleep_for(2ms);
        }
        ASSERT_EQ(fixture.host->statusActions.load(), 1);
    }
    std::this_thread::sleep_for(20ms);
    {
        auto socket = connectWebsocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket, ssg::Revision{1});
        auto replay = ssg::ProtocolCodec{}.decodeSessionDelta(reader.next().payload);
        ASSERT_TRUE(replay.accepted());
        ASSERT_EQ(replay.delta->revision(), ssg::Revision{2});
        auto const registry =
            ssg::CommandArgumentCodecRegistry{fixture.session->catalog()};
        auto command = ssg::ProtocolCodec{}.encodeCommandRequest(
            {"text.insert", ssg::Revision{2},
             ssg::TextInputArguments{"b"}}, registry);
        sendAll(socket.socket, maskedFrame(0x2, command));
        ASSERT_TRUE(ssg::ProtocolCodec{}.decodeSessionDelta(reader.next().payload).accepted());
    }
    std::this_thread::sleep_for(20ms);
    {
        auto socket = connectWebsocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket, ssg::Revision{1});
        auto snapshot = ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload);
        ASSERT_TRUE(snapshot.accepted());
        ASSERT_EQ(snapshot.snapshot->revision(), ssg::Revision{3});
        ASSERT_EQ(snapshot.snapshot->sections().document.text,
                  std::string{"ab"});
    }
    server.stop();
}

TEST(statusAndBinaryIngressShareTheAttachedConnection) {
    Fixture fixture{/*remote=*/true};
    constexpr std::uint16_t port = 18777;
    ssg::HttpEditorServer server{
        *fixture.session,
        *fixture.host, {port, "/session", 8, 8, 250ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    auto socket = connectWebsocket(port);
    FrameReader reader{socket.socket};
    attach(socket.socket);
    ASSERT_TRUE(ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload).accepted());

    auto const status = ssg::ProtocolCodec{}.encodeStatusActionInvocation(
        {ssg::StatusId{3}, "run", 1});
    auto const binary = ssg::ProtocolCodec{}.encodeBinaryFrame(
        {1, ssg::BinaryPayloadKind::DroppedContent, 9, {1, 2, 3}});
    ASSERT_TRUE(ssg::ProtocolCodec{}.decodeStatusActionInvocation(status).accepted());
    ASSERT_TRUE(ssg::ProtocolCodec{}.decodeBinaryFrame(binary).accepted());
    sendAll(socket.socket, maskedFrame(0x2, status));
    sendAll(socket.socket, maskedFrame(0x2, binary));
    for (int attempt = 0;
         attempt < 100 && fixture.host->binaryFrames.load() != 1;
         ++attempt) {
        std::this_thread::sleep_for(2ms);
    }

    ASSERT_EQ(fixture.host->statusActions.load(), 1);
    ASSERT_EQ(fixture.host->binaryFrames.load(), 1);
    server.stop();
}

TEST(replayLargerThanTheOutboundQueueFallsBackToSnapshot) {
    Fixture fixture;
    constexpr std::uint16_t port = 18779;
    ssg::HttpEditorServer server{
        *fixture.session,
        *fixture.host, {port, "/session", 1, 8, 250ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    {
        auto socket = connectWebsocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket);
        ASSERT_TRUE(
            ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload).accepted());
        auto registry = ssg::CommandArgumentCodecRegistry{fixture.session->catalog()};
        for (auto const& [revision, text] :
             std::vector<std::pair<std::uint64_t, std::string>>{{1, "a"},
                                                                {2, "b"}}) {
            sendAll(socket.socket,
                     maskedFrame(0x2, ssg::ProtocolCodec{}.encodeCommandRequest(
                                           {"text.insert",
                                            ssg::Revision{revision},
                                            ssg::TextInputArguments{text}},
                                           registry)));
            ASSERT_TRUE(
                ssg::ProtocolCodec{}.decodeSessionDelta(reader.next().payload).accepted());
        }
    }
    std::this_thread::sleep_for(20ms);
    auto socket = connectWebsocket(port);
    FrameReader reader{socket.socket};
    attach(socket.socket, ssg::Revision{1});
    auto snapshot = ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload);
    ASSERT_TRUE(snapshot.accepted());
    ASSERT_EQ(snapshot.snapshot->revision(), ssg::Revision{3});
    server.stop();
}

TEST(attachRequestNeverCarriesAClientGrantedCapability) {
    // The only field a client can put on the attach wire is the last-applied
    // revision; a capability grant is structurally inexpressible, and the codec
    // rejects any attempt to smuggle one in as extra tokens. Capabilities are
    // host policy, never a client assertion.
    auto encoded = ssg::encodeSessionAttachRequest({ssg::Revision{9}});
    auto decoded = ssg::decodeSessionAttachRequest(encoded);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.request->lastAppliedRevision, ssg::Revision{9});
    ASSERT_FALSE(ssg::decodeSessionAttachRequest(
                     "SSG1 ATTACH 9 capabilities=all")
                     .accepted());
}

TEST(attachRejectsAForeignPreambleWithoutAPartialRequest) {
    // A bad preamble yields no request object at all, so attach() can never act
    // on a half-parsed frame. A foreign SSG1 prefix is an unsupported version
    // (matching the command codec); a wrong keyword or a truncated frame is
    // malformed. Either way there is no partial request.
    struct Case {
        char const* preamble;
        ssg::ProtocolError error;
    };
    for (auto const& c : {
             Case{"XXXX ATTACH 9", ssg::ProtocolError::UnsupportedVersion},
             Case{"SSG1 HELLO 9", ssg::ProtocolError::MalformedMessage},
             Case{"SSG1 ATTACH", ssg::ProtocolError::MalformedMessage},
         }) {
        auto const decoded = ssg::decodeSessionAttachRequest(c.preamble);
        ASSERT_EQ(decoded.error, c.error);
        ASSERT_FALSE(decoded.request.has_value());
    }
}

TEST(serverConstructionRejectsAZeroQueueOrReplay) {
    Fixture fixture;
    ASSERT_THROWS(
        ssg::HttpEditorServer(
            *fixture.session,
            *fixture.host, {18778, "/session", 0, 1, 250ms}),
        std::invalid_argument);
    ASSERT_THROWS(
        ssg::HttpEditorServer(
            *fixture.session,
            *fixture.host, {18778, "/session", 1, 0, 250ms}),
        std::invalid_argument);
}

TEST(attachOnlyBindsAWebsocketOriginPrincipal) {
    // Binding another origin's principal onto a socket would route its authority
    // over the wire. A Websocket-origin attach succeeds; an in-process one is
    // refused and the connection closed with no snapshot.
    {
        Fixture fixture;
        constexpr std::uint16_t port = 18780;
        ssg::HttpEditorServer server{
            *fixture.session,
            *fixture.host, {port, "/session", 8, 8, 250ms}};
        server.start();
        std::this_thread::sleep_for(20ms);
        auto socket = connectWebsocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket);
        ASSERT_TRUE(
            ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload)
                .accepted());
        server.stop();
    }
    {
        Fixture fixture;
        fixture.host->attachOrigin = ssg::InvocationOrigin::InProcess;
        constexpr std::uint16_t port = 18781;
        ssg::HttpEditorServer server{
            *fixture.session,
            *fixture.host, {port, "/session", 8, 8, 250ms}};
        server.start();
        std::this_thread::sleep_for(20ms);
        auto socket = connectWebsocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket);
        ASSERT_TRUE(connectionClosed(reader));
        server.stop();
    }
}

TEST(aBoundConnectionRejectsAnyFrameThatIsNotATypedCommand) {
    // The whole product travels one ordered channel; a bound connection accepts
    // only typed binary command frames. A text frame, or a binary frame that
    // decodes as no typed message, opens no second channel: it closes.
    {
        Fixture fixture{/*remote=*/true};
        constexpr std::uint16_t port = 18782;
        ssg::HttpEditorServer server{
            *fixture.session,
            *fixture.host, {port, "/session", 8, 8, 250ms}};
        server.start();
        std::this_thread::sleep_for(20ms);
        auto socket = connectWebsocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket);
        ASSERT_TRUE(
            ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload)
                .accepted());
        sendAll(socket.socket, maskedFrame(0x1, "a control text frame"));
        ASSERT_TRUE(connectionClosed(reader));
        server.stop();
    }
    {
        Fixture fixture{/*remote=*/true};
        constexpr std::uint16_t port = 18783;
        ssg::HttpEditorServer server{
            *fixture.session,
            *fixture.host, {port, "/session", 8, 8, 250ms}};
        server.start();
        std::this_thread::sleep_for(20ms);
        auto socket = connectWebsocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket);
        ASSERT_TRUE(
            ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload)
                .accepted());
        sendAll(socket.socket,
                maskedFrame(0x2, "not a command, status, or binary frame"));
        ASSERT_TRUE(connectionClosed(reader));
        server.stop();
    }
}

TEST(slowClientCannotGrowTheOutboundQueue) {
    Fixture fixture{/*remote=*/true};
    constexpr std::uint16_t port = 18778;
    ssg::HttpEditorServer server{
        *fixture.session,
        *fixture.host, {port, "/session", 1, 2, 1ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    auto socket = connectWebsocket(port);
    attach(socket.socket);
    std::this_thread::sleep_for(20ms);

    ssg::BinaryFrame large{
        1, ssg::BinaryPayloadKind::DroppedContent, 1,
        std::vector<std::uint8_t>(4 * 1024 * 1024, 0x5a)};
    for (int attempt = 0; attempt < 16; ++attempt) {
        (void)server.sendBinary(ssg::ClientId{12}, large);
    }
    bool disconnected = false;
    for (int attempt = 0; attempt < 100 && !disconnected; ++attempt) {
        disconnected =
            !server.sendBinary(ssg::ClientId{12},
                                {1, ssg::BinaryPayloadKind::DroppedContent,
                                 2, {1}});
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_TRUE(disconnected);
    server.stop();
}

}  // namespace

int main() {
    std::cout << "=== HTTP editor server ===\n";
    RUN(externallyOwnedRouteSharesOneServerLifecycle);
    RUN(attachUsesHostPrincipalAndSocketSnapshotMatchesInProcess);
    RUN(commandDeltaReplaysOnReconnectAndEvictionSendsSnapshot);
    RUN(statusAndBinaryIngressShareTheAttachedConnection);
    RUN(replayLargerThanTheOutboundQueueFallsBackToSnapshot);
    RUN(attachRequestNeverCarriesAClientGrantedCapability);
    RUN(attachRejectsAForeignPreambleWithoutAPartialRequest);
    RUN(serverConstructionRejectsAZeroQueueOrReplay);
    RUN(attachOnlyBindsAWebsocketOriginPrincipal);
    RUN(aBoundConnectionRejectsAnyFrameThatIsNotATypedCommand);
    RUN(slowClientCannotGrowTheOutboundQueue);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
