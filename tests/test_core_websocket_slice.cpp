#include "test_helpers.h"

#include <ssg/http_server.h>
#include <ssg/protocol.h>

#include <http.h>

#include <array>
#include <chrono>
#include <cstdint>
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
    ~SocketOwner() {
        if (socket != invalid_test_socket) {
            close_test_socket(socket);
        }
    }
};

void send_all(TestSocket socket, std::string const& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        auto const count =
            send(socket, bytes.data() + sent,
                 static_cast<int>(bytes.size() - sent), 0);
        if (count <= 0) {
            throw std::runtime_error{"loopback send failed"};
        }
        sent += static_cast<std::size_t>(count);
    }
}

std::string receive_some(TestSocket socket) {
    std::array<char, 8192> bytes{};
    auto const count = recv(socket, bytes.data(), static_cast<int>(bytes.size()), 0);
    if (count <= 0) {
        throw std::runtime_error{"loopback receive failed"};
    }
    return {bytes.data(), static_cast<std::size_t>(count)};
}

std::string masked_text_frame(std::string const& payload) {
    if (payload.size() > 125) {
        throw std::runtime_error{"test payload exceeds short frame"};
    }
    std::array<std::uint8_t, 4> const mask{0x12, 0x34, 0x56, 0x78};
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    frame.push_back(static_cast<char>(0x80 | payload.size()));
    for (auto byte : mask) {
        frame.push_back(static_cast<char>(byte));
    }
    for (std::size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(
            static_cast<unsigned char>(payload[i]) ^ mask[i % mask.size()]));
    }
    return frame;
}

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

ssg::SliceResponse websocket_command(TestSocket socket,
                                     ssg::InsertRequest const& request) {
    send_all(socket, masked_text_frame(ssg::encode_insert_request(request)));
    std::string bytes;
    std::size_t consumed = 0;
    Http::WebSocketFrame frame;
    do {
        bytes += receive_some(socket);
        frame = Http::parseWebSocketFrame(bytes, consumed);
    } while (consumed == 0);
    return ssg::decode_slice_response(frame.payload);
}

TEST(codec_round_trip_and_malformed_corpus) {
    ssg::ProtocolLimits const limits{256, 32};
    auto const wire =
        ssg::encode_insert_request({ssg::Revision{9}, "a b\n\xC3\xA9"});
    auto const decoded = ssg::decode_insert_request(wire, limits);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.request->base_revision, ssg::Revision{9});
    ASSERT_EQ(decoded.request->text, std::string{"a b\n\xC3\xA9"});

    for (auto const& malformed :
         std::vector<std::string>{"", "SSG0 INSERT 1 61", "SSG1 DELETE 1 61",
                                  "SSG1 INSERT x 61", "SSG1 INSERT 1 6",
                                  "SSG1 INSERT 1 zz", "SSG1 INSERT 1 6100"}) {
        auto const result = ssg::decode_insert_request(malformed, limits);
        ASSERT_FALSE(result.accepted());
    }
    ASSERT_EQ(ssg::decode_insert_request(std::string(257, 'x'), limits).error,
              ssg::ProtocolError::message_too_large);
    ASSERT_EQ(ssg::decode_insert_request("SSG1 INSERT 1 616263", {256, 2}).error,
              ssg::ProtocolError::insert_too_large);
}

TEST(direct_and_loopback_scripts_have_identical_snapshots) {
    ssg::CoreEditorSlice direct;
    ASSERT_TRUE(direct.attach({ssg::ClientId{1},
                               ssg::InvocationOrigin::in_process}));

    constexpr std::uint16_t port = 18765;
    ssg::CoreEditorSlice remote;
    ssg::HttpEditorServer server{remote, {port, "/session", 8, 250ms}};
    server.start();
    std::this_thread::sleep_for(20ms);
    auto socket = connect_websocket(port);

    ssg::Revision revision{1};
    for (auto const& text : std::vector<std::string>{"hello", " ", "world"}) {
        ssg::InsertRequest const request{revision, text};
        auto const direct_result = direct.execute(ssg::ClientId{1}, request);
        auto const remote_result = websocket_command(socket.socket, request);
        ASSERT_TRUE(direct_result.accepted());
        ASSERT_EQ(remote_result, direct_result);
        ASSERT_TRUE(direct_result.delta.has_value());
        revision = direct_result.snapshot.revision;
    }
    ASSERT_EQ(direct.snapshot().text, std::string{"hello world"});
    server.stop();
}

TEST(stale_and_malformed_requests_are_failure_atomic) {
    ssg::CoreEditorSlice direct;
    ASSERT_TRUE(direct.attach({ssg::ClientId{1},
                               ssg::InvocationOrigin::in_process}));
    auto const accepted =
        direct.execute(ssg::ClientId{1}, {ssg::Revision{1}, "first"});
    auto const stale =
        direct.execute(ssg::ClientId{1}, {ssg::Revision{1}, "stale"});
    ASSERT_EQ(stale.command_error, ssg::CommandError::stale_revision);
    ASSERT_EQ(stale.snapshot, accepted.snapshot);
    ASSERT_FALSE(stale.delta.has_value());

    auto const malformed = ssg::decode_insert_request("not a command");
    ASSERT_EQ(malformed.error, ssg::ProtocolError::malformed_message);
    ASSERT_EQ(direct.snapshot(), accepted.snapshot);
}

}  // namespace

int main() {
    std::cout << "=== Core WebSocket slice ===\n";
    RUN(codec_round_trip_and_malformed_corpus);
    RUN(direct_and_loopback_scripts_have_identical_snapshots);
    RUN(stale_and_malformed_requests_are_failure_atomic);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
