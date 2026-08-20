#include "test_helpers.h"
#include <ssg/EditorRuntime.h>
#include <ssg/FileCommands.h>
#include <ssg/HttpEditorServer.h>
#include <ssg/Protocol.h>
#include <ssg/TextInputCommands.h>

#include <http.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
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

class TestPolicy : public ssg::HttpEditorConnectionPolicy {
public:
    // `remote` is fixed at construction: a host's locality policy is a property
    // of its deployment, not something a client toggles. A remote host grants no
    // capability; a local host grants local_file_drop.
    explicit TestPolicy(bool remote = false) : remote{remote} {}

    std::optional<ssg::AttachedSession> attach() override {
        if (reject) return std::nullopt;
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

    bool remote{false};
    bool reject{false};
    // A host binds a Websocket-origin principal onto a socket; the origin is a
    // property of the deployment, exposed here so a seam test can drive a
    // non-Websocket origin through the same attach path.
    ssg::InvocationOrigin attachOrigin{ssg::InvocationOrigin::Websocket};
};

struct Fixture {
    explicit Fixture(bool remote = false) : remote_{remote} {
        root = std::filesystem::current_path() / "http_server_runtime";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "workspace");
        auto created = ssg::EditorRuntime::create(
            {.cwd = root / "workspace",
             .scratchRoot = root / "scratch",
             .recoveryRoot = root / "recovery",
             .enableGitDiffWorker = false,
             .enableFilesystemWatcher = false});
        if (!created.accepted()) {
            throw std::runtime_error{created.message};
        }
        runtime = std::move(created.runtime);
        auto const setup = ssg::InvocationPrincipal{
            ssg::ClientId{99}, ssg::InvocationOrigin::InProcess};
        if (!runtime->attach(setup, ssg::ViewId{99}).accepted() ||
            !runtime
                 ->dispatch(ssg::ClientId{99},
                            {"file.new", runtime->revision(), {}})
                 .accepted()) {
            throw std::runtime_error{"failed to initialize HTTP test runtime"};
        }
        (void)runtime->detach(ssg::ClientId{99});
        policy.emplace(remote_);
    }
    ~Fixture() { std::filesystem::remove_all(root); }

    bool remote_;
    std::filesystem::path root;
    std::unique_ptr<ssg::EditorRuntime> runtime;
    std::optional<TestPolicy> policy;
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
        server, *fixture.runtime,
        *fixture.policy, {"/session", 8, 8, 250ms}};

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

    ASSERT_TRUE(fixture.runtime
                    ->applyGitDiffScan(
                        {.revision = ssg::Revision{1},
                         .baselineIdentity = "head:index",
                         .files = {{.id = ssg::DiffFileId{"changed"},
                                    .path = "changed.txt",
                                    .baselineContent = "",
                                    .workingContent = "changed\n"}}})
                    .accepted());
    route.publish();
    auto published =
        ssg::ProtocolCodec{}.decodeSessionDelta(reader.next().payload);
    ASSERT_TRUE(published.accepted());
    ASSERT_EQ(published.delta->revision(), fixture.runtime->revision());

    server.stop();
    ASSERT_FALSE(server.boundPort().has_value());
}

TEST(typedClientInputPublishesStateBeforeItsResult) {
    Fixture fixture;
    constexpr std::uint16_t port = 18786;
    ssg::HttpEditorServer server{
        *fixture.runtime,
        *fixture.policy, {port, "/session", 8, 8, 250ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    auto socket = connectWebsocket(port);
    FrameReader reader{socket.socket};
    attach(socket.socket);
    auto initial = ssg::ProtocolCodec{}.decodeSessionSnapshot(
        reader.next().payload);
    ASSERT_TRUE(initial.accepted());

    sendAll(socket.socket,
            maskedFrame(
                0x2, ssg::ProtocolCodec{}.encodeClientInput(
                         {ssg::KeyStroke{}, "typed"})));
    auto firstFrame = reader.next().payload;
    auto delta =
        ssg::ProtocolCodec{}.decodeSessionDelta(firstFrame);
    ASSERT_TRUE(delta.accepted());
    auto completion = ssg::ProtocolCodec{}.decodeClientInputResult(
        reader.next().payload);
    ASSERT_TRUE(completion.accepted());
    ASSERT_EQ(completion.result->outcome,
              ssg::ClientInputOutcome::Dispatched);
    ASSERT_TRUE(completion.result->command.has_value());
    ASSERT_TRUE(completion.result->command->accepted());
    ASSERT_EQ(completion.result->command->revision,
              delta.delta->revision());
    ASSERT_EQ(fixture.runtime->activeDocumentText(),
              std::string{"typed"});
    server.stop();
}

TEST(acceptedNoChangeCommandStillReceivesAResult) {
    Fixture fixture;
    auto handle = fixture.runtime->registerCommand(
        ssg::CommandSpecBuilder{"test.observe"}
            .owner("test")
            .summary("Observe")
            .observes()
            .handler([](ssg::CommandContext&) {
                return ssg::CommandHandlerResult::success();
            }));
    ASSERT_TRUE(handle.valid());
    constexpr std::uint16_t port = 18787;
    ssg::HttpEditorServer server{
        *fixture.runtime,
        *fixture.policy, {port, "/session", 8, 8, 250ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    auto socket = connectWebsocket(port);
    FrameReader reader{socket.socket};
    attach(socket.socket);
    auto initial = ssg::ProtocolCodec{}.decodeSessionSnapshot(
        reader.next().payload);
    ASSERT_TRUE(initial.accepted());
    auto registry = ssg::CommandArgumentCodecRegistry{
        fixture.runtime->commandCatalog()};
    sendAll(socket.socket,
            maskedFrame(
                0x2, ssg::ProtocolCodec{}.encodeCommandRequest(
                         {"test.observe", initial.snapshot->revision(), {}},
                         registry)));
    auto completion = ssg::ProtocolCodec{}.decodeCommandResult(
        reader.next().payload);
    ASSERT_TRUE(completion.accepted());
    ASSERT_TRUE(completion.result->accepted());
    ASSERT_EQ(completion.result->revision,
              initial.snapshot->revision());
    server.stop();
}

TEST(attachUsesHostPrincipalAndSocketSnapshotMatchesInProcess) {
    Fixture fixture;
    constexpr std::uint16_t port = 18775;
    ssg::HttpEditorServer server{
        *fixture.runtime,
        *fixture.policy, {port, "/session", 8, 8, 250ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    auto socket = connectWebsocket(port);
    FrameReader reader{socket.socket};
    attach(socket.socket);
    auto decoded = ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.snapshot->client().clientId, ssg::ClientId{11});
    ASSERT_EQ(decoded.snapshot->client().capabilities.size(), std::size_t{1});
    ASSERT_EQ(decoded.snapshot->revision(), fixture.runtime->revision());
    ASSERT_EQ(decoded.snapshot->sections().document.text, std::string{});
    server.stop();
}

TEST(commandDeltaReplaysOnReconnectAndEvictionSendsSnapshot) {
    Fixture fixture;
    constexpr std::uint16_t port = 18776;
    ssg::HttpEditorServer server{
        *fixture.runtime,
        *fixture.policy, {port, "/session", 8, 1, 250ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    ssg::Revision initialRevision;
    ssg::Revision afterFirstInsert;
    {
        auto socket = connectWebsocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket);
        auto initial = ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload);
        ASSERT_TRUE(initial.accepted());
        initialRevision = initial.snapshot->revision();
        auto const registry =
            ssg::CommandArgumentCodecRegistry{fixture.runtime->commandCatalog()};
        auto command = ssg::ProtocolCodec{}.encodeCommandRequest(
            {"text.insert", initialRevision,
             ssg::TextInputArguments{"a"}}, registry);
        sendAll(socket.socket, maskedFrame(0x2, command));
        auto delta = ssg::ProtocolCodec{}.decodeSessionDelta(reader.next().payload);
        ASSERT_TRUE(delta.accepted());
        ASSERT_EQ(delta.delta->baseRevision(), initialRevision);
        afterFirstInsert = delta.delta->revision();
        auto accepted = ssg::ProtocolCodec{}.decodeCommandResult(
            reader.next().payload);
        ASSERT_TRUE(accepted.accepted());
        ASSERT_TRUE(accepted.result->accepted());
        ASSERT_EQ(accepted.result->revision, afterFirstInsert);
        sendAll(socket.socket, maskedFrame(0x2, command));
        auto rejected = ssg::ProtocolCodec{}.decodeCommandResult(reader.next().payload);
        ASSERT_TRUE(rejected.accepted());
        ASSERT_EQ(rejected.result->error,
                  ssg::CommandError::StaleRevision);
    }
    std::this_thread::sleep_for(20ms);
    {
        auto socket = connectWebsocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket, initialRevision);
        auto replay = ssg::ProtocolCodec{}.decodeSessionDelta(reader.next().payload);
        ASSERT_TRUE(replay.accepted());
        ASSERT_EQ(replay.delta->revision(), afterFirstInsert);
        auto const registry =
            ssg::CommandArgumentCodecRegistry{fixture.runtime->commandCatalog()};
        auto command = ssg::ProtocolCodec{}.encodeCommandRequest(
            {"text.insert", afterFirstInsert,
             ssg::TextInputArguments{"b"}}, registry);
        sendAll(socket.socket, maskedFrame(0x2, command));
        ASSERT_TRUE(ssg::ProtocolCodec{}.decodeSessionDelta(reader.next().payload).accepted());
        ASSERT_TRUE(ssg::ProtocolCodec{}
                        .decodeCommandResult(reader.next().payload)
                        .result->accepted());
    }
    std::this_thread::sleep_for(20ms);
    {
        auto socket = connectWebsocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket, initialRevision);
        auto snapshot = ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload);
        ASSERT_TRUE(snapshot.accepted());
        ASSERT_EQ(snapshot.snapshot->revision(), fixture.runtime->revision());
        ASSERT_EQ(snapshot.snapshot->sections().document.text,
                  std::string{"ab"});
    }
    server.stop();
}

TEST(statusAndDroppedContentUseAggregateCommands) {
    Fixture fixture{/*remote=*/true};
    constexpr std::uint16_t port = 18777;
    ssg::HttpEditorServer server{
        *fixture.runtime,
        *fixture.policy, {port, "/session", 8, 8, 250ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    auto socket = connectWebsocket(port);
    FrameReader reader{socket.socket};
    attach(socket.socket);
    ASSERT_TRUE(ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload).accepted());

    auto const status = ssg::ProtocolCodec{}.encodeStatusActionInvocation(
        {ssg::StatusId{3}, "run", 1});
    ASSERT_TRUE(ssg::ProtocolCodec{}.decodeStatusActionInvocation(status).accepted());
    sendAll(socket.socket, maskedFrame(0x2, status));
    auto statusResult =
        ssg::ProtocolCodec{}.decodeCommandResult(reader.next().payload);
    ASSERT_TRUE(statusResult.accepted());
    ASSERT_FALSE(statusResult.result->accepted());

    auto registry = ssg::CommandArgumentCodecRegistry{
        fixture.runtime->commandCatalog()};
    auto dropped = ssg::ProtocolCodec{}.encodeCommandRequest(
        {"file.open_dropped_content", fixture.runtime->revision(),
         ssg::DroppedContentArguments{{1, 2, 3}, "drop.bin"}},
        registry);
    sendAll(socket.socket, maskedFrame(0x2, dropped));
    auto denied = ssg::ProtocolCodec{}.decodeCommandResult(reader.next().payload);
    ASSERT_TRUE(denied.accepted());
    ASSERT_EQ(denied.result->error, ssg::CommandError::CapabilityDenied);
    server.stop();

    Fixture local;
    constexpr std::uint16_t localPort = 18784;
    ssg::HttpEditorServer localServer{
        *local.runtime,
        *local.policy, {localPort, "/session", 8, 8, 250ms}};
    localServer.start();
    std::this_thread::sleep_for(20ms);
    auto localSocket = connectWebsocket(localPort);
    FrameReader localReader{localSocket.socket};
    attach(localSocket.socket);
    ASSERT_TRUE(ssg::ProtocolCodec{}
                    .decodeSessionSnapshot(localReader.next().payload)
                    .accepted());
    auto localRegistry =
        ssg::CommandArgumentCodecRegistry{local.runtime->commandCatalog()};
    auto acceptedDrop = ssg::ProtocolCodec{}.encodeCommandRequest(
        {"file.open_dropped_content", local.runtime->revision(),
         ssg::DroppedContentArguments{{4, 5, 6}, "named-drop.bin"}},
        localRegistry);
    sendAll(localSocket.socket, maskedFrame(0x2, acceptedDrop));
    auto update = localReader.next();
    ASSERT_TRUE(ssg::ProtocolCodec{}.decodeSessionDelta(update.payload).accepted() ||
                ssg::ProtocolCodec{}
                    .decodeSessionSnapshot(update.payload)
                    .accepted());
    ASSERT_TRUE(ssg::ProtocolCodec{}
                    .decodeCommandResult(localReader.next().payload)
                    .result->accepted());
    auto snapshot = local.runtime->snapshot(ssg::ClientId{11});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(std::any_of(
        snapshot->sections().tabs.tabs.begin(),
        snapshot->sections().tabs.tabs.end(),
        [](ssg::TabState const& tab) { return tab.label == "named-drop.bin"; }));
    localServer.stop();
}

TEST(replayLargerThanTheOutboundQueueFallsBackToSnapshot) {
    Fixture fixture;
    constexpr std::uint16_t port = 18779;
    ssg::HttpEditorServer server{
        *fixture.runtime,
        *fixture.policy, {port, "/session", 1, 8, 250ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    ssg::Revision initialRevision;
    {
        auto socket = connectWebsocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket);
        auto initial =
            ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload);
        ASSERT_TRUE(initial.accepted());
        initialRevision = initial.snapshot->revision();
        auto registry = ssg::CommandArgumentCodecRegistry{
            fixture.runtime->commandCatalog()};
        auto revision = initialRevision;
        for (auto const& text : {std::string{"a"}, std::string{"b"}}) {
            sendAll(socket.socket,
                     maskedFrame(0x2, ssg::ProtocolCodec{}.encodeCommandRequest(
                                           {"text.insert",
                                            revision,
                                            ssg::TextInputArguments{text}},
                                           registry)));
            auto delta =
                ssg::ProtocolCodec{}.decodeSessionDelta(reader.next().payload);
            ASSERT_TRUE(delta.accepted());
            revision = delta.delta->revision();
            ASSERT_TRUE(ssg::ProtocolCodec{}
                            .decodeCommandResult(reader.next().payload)
                            .result->accepted());
        }
    }
    std::this_thread::sleep_for(20ms);
    auto socket = connectWebsocket(port);
    FrameReader reader{socket.socket};
    attach(socket.socket, initialRevision);
    auto snapshot = ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload);
    ASSERT_TRUE(snapshot.accepted());
    ASSERT_EQ(snapshot.snapshot->revision(), fixture.runtime->revision());
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
            *fixture.runtime,
            *fixture.policy, {18778, "/session", 0, 1, 250ms}),
        std::invalid_argument);
    ASSERT_THROWS(
        ssg::HttpEditorServer(
            *fixture.runtime,
            *fixture.policy, {18778, "/session", 1, 0, 250ms}),
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
            *fixture.runtime,
            *fixture.policy, {port, "/session", 8, 8, 250ms}};
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
        fixture.policy->attachOrigin = ssg::InvocationOrigin::InProcess;
        constexpr std::uint16_t port = 18781;
        ssg::HttpEditorServer server{
            *fixture.runtime,
            *fixture.policy, {port, "/session", 8, 8, 250ms}};
        server.start();
        std::this_thread::sleep_for(20ms);
        auto socket = connectWebsocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket);
        ASSERT_TRUE(connectionClosed(reader));
        server.stop();
    }
    {
        Fixture fixture;
        fixture.policy->reject = true;
        constexpr std::uint16_t port = 18785;
        ssg::HttpEditorServer server{
            *fixture.runtime,
            *fixture.policy, {port, "/session", 8, 8, 250ms}};
        server.start();
        std::this_thread::sleep_for(20ms);
        auto socket = connectWebsocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket);
        ASSERT_TRUE(connectionClosed(reader));
        ASSERT_FALSE(
            fixture.runtime->snapshot(ssg::ClientId{11}).has_value());
        server.stop();
    }
}

TEST(aBoundConnectionRejectsAnyFrameThatIsNotATypedCommand) {
    // The whole product travels one ordered channel; a bound connection accepts
    // only supported typed binary frames. A text frame, or a binary frame that
    // decodes as no typed message, opens no second channel: it closes.
    {
        Fixture fixture{/*remote=*/true};
        constexpr std::uint16_t port = 18782;
        ssg::HttpEditorServer server{
            *fixture.runtime,
            *fixture.policy, {port, "/session", 8, 8, 250ms}};
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
            *fixture.runtime,
            *fixture.policy, {port, "/session", 8, 8, 250ms}};
        server.start();
        std::this_thread::sleep_for(20ms);
        auto socket = connectWebsocket(port);
        FrameReader reader{socket.socket};
        attach(socket.socket);
        ASSERT_TRUE(
            ssg::ProtocolCodec{}.decodeSessionSnapshot(reader.next().payload)
                .accepted());
        sendAll(socket.socket,
                maskedFrame(0x2, "not a command or status frame"));
        ASSERT_TRUE(connectionClosed(reader));
        server.stop();
    }
}

}  // namespace

int main() {
    std::cout << "=== HTTP editor server ===\n";
    RUN(externallyOwnedRouteSharesOneServerLifecycle);
    RUN(attachUsesHostPrincipalAndSocketSnapshotMatchesInProcess);
    RUN(typedClientInputPublishesStateBeforeItsResult);
    RUN(acceptedNoChangeCommandStillReceivesAResult);
    RUN(commandDeltaReplaysOnReconnectAndEvictionSendsSnapshot);
    RUN(statusAndDroppedContentUseAggregateCommands);
    RUN(replayLargerThanTheOutboundQueueFallsBackToSnapshot);
    RUN(attachRequestNeverCarriesAClientGrantedCapability);
    RUN(attachRejectsAForeignPreambleWithoutAPartialRequest);
    RUN(serverConstructionRejectsAZeroQueueOrReplay);
    RUN(attachOnlyBindsAWebsocketOriginPrincipal);
    RUN(aBoundConnectionRejectsAnyFrameThatIsNotATypedCommand);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
