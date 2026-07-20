#include <ssg/lsp_sync.h>

#include "fake_lsp_server.h"
#include "test_helpers.h"

#include <fstream>
#include <iterator>
#include <string>

namespace {

using namespace ssg;
using ssg::test::FakeLspServer;

constexpr auto timeout = std::chrono::milliseconds{5};
const std::string uri = "file:///workspace/main.cpp";

template <typename Result>
auto error_of(Result result) {
    return result.error;
}

LspPosition position_of(LspPositionResult result) {
    return result.position;
}

ByteOffset offset_of(LspByteOffsetResult result) {
    return result.offset;
}

std::int64_t version_of(const LspSyncClient& client) {
    return client.document_version(uri).value_or(0);
}

std::string fixture() {
    std::ifstream input(std::string{SSG_TEST_SOURCE_DIR} +
                        "/tests/fixtures/lsp/sync/utf_positions.txt",
                        std::ios::binary);
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

TEST(frame_decoder_handles_fragmentation_and_multiple_messages) {
    LspFrameDecoder decoder;
    const auto frames = encode_lsp_frame("{}") + encode_lsp_frame("[]");
    auto first = decoder.feed(std::string_view{frames}.substr(0, 11));
    ASSERT_TRUE(first.accepted());
    ASSERT_EQ(first.messages.size(), 0U);
    auto second = decoder.feed(std::string_view{frames}.substr(11));
    ASSERT_TRUE(second.accepted());
    ASSERT_EQ(second.messages.size(), 2U);
    ASSERT_EQ(second.messages[0], "{}");
    ASSERT_EQ(second.messages[1], "[]");
}

TEST(frame_decoder_rejects_malformed_or_unbounded_headers) {
    LspFrameDecoder decoder{{64, 8}};
    ASSERT_EQ(error_of(decoder.feed("Content-Length: nope\r\n\r\n{}")),
              LspFrameError::InvalidContentLength);

    LspFrameDecoder duplicate;
    ASSERT_EQ(error_of(duplicate.feed(
                  "Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}")),
              LspFrameError::DuplicateContentLength);

    LspFrameDecoder oversized{{64, 1}};
    ASSERT_EQ(error_of(oversized.feed("Content-Length: 2\r\n\r\n{}")),
              LspFrameError::MessageTooLarge);
}

TEST(utf8_utf16_positions_match_hand_computed_fixture) {
    const auto text = fixture();
    ASSERT_EQ(position_of(byte_offset_to_lsp_position(text, ByteOffset{0})),
              (LspPosition{0, 0}));
    ASSERT_EQ(position_of(byte_offset_to_lsp_position(text, ByteOffset{1})),
              (LspPosition{0, 1}));
    ASSERT_EQ(position_of(byte_offset_to_lsp_position(text, ByteOffset{3})),
              (LspPosition{0, 2}));
    ASSERT_EQ(position_of(byte_offset_to_lsp_position(text, ByteOffset{7})),
              (LspPosition{0, 4}));
    ASSERT_EQ(offset_of(lsp_position_to_byte_offset(text, {0, 4})),
              ByteOffset{7});
    ASSERT_EQ(error_of(lsp_position_to_byte_offset(text, {0, 3})),
              LspPositionError::SplitSurrogate);
    ASSERT_EQ(offset_of(lsp_position_to_byte_offset(text, {1, 0})),
              ByteOffset{9});
    ASSERT_EQ(offset_of(lsp_position_to_byte_offset("a\r\nb", {1, 0})),
              ByteOffset{3});
    ASSERT_EQ(error_of(byte_offset_to_lsp_position(text, ByteOffset{2})),
              LspPositionError::InvalidUtf8Boundary);
    ASSERT_EQ(error_of(byte_offset_to_lsp_position(std::string{"\xff", 1},
                                                   ByteOffset{0})),
              LspPositionError::InvalidUtf8);
}

TEST(scripted_server_covers_initialize_sync_and_shutdown_lifecycle) {
    FakeLspServer server;
    LspSyncClient client{server, {}, timeout};

    ASSERT_EQ(error_of(client.initialize("file:///workspace")), LspSyncError::None);
    ASSERT_EQ(client.state(), LspLifecycleState::Initializing);
    ASSERT_TRUE(server.received_payloads().back().find("\"method\":\"initialize\"") !=
                std::string::npos);

    server.queue_payload(ssg::test::response(1, "{\"capabilities\":{}}"), 7);
    while (client.state() == LspLifecycleState::Initializing) {
        ASSERT_EQ(error_of(client.poll()), LspSyncError::None);
    }
    ASSERT_EQ(client.state(), LspLifecycleState::Ready);
    ASSERT_TRUE(server.received_payloads().back().find(
                    "\"method\":\"initialized\"") != std::string::npos);

    ASSERT_EQ(error_of(client.open_document(uri, "cpp", Revision{40}, "one")),
              LspSyncError::None);
    ASSERT_EQ(version_of(client), 1);
    ASSERT_EQ(error_of(client.change_document(uri, Revision{41}, "two")),
              LspSyncError::None);
    ASSERT_EQ(version_of(client), 2);
    ASSERT_EQ(error_of(client.change_document(uri, Revision{41}, "stale")),
              LspSyncError::StaleDocument);
    ASSERT_EQ(version_of(client), 2);
    ASSERT_EQ(error_of(client.close_document(uri)), LspSyncError::None);
    ASSERT_FALSE(client.document_version(uri).has_value());

    ASSERT_EQ(error_of(client.shutdown()), LspSyncError::None);
    server.queue_payload(ssg::test::response(2));
    ASSERT_EQ(error_of(client.poll()), LspSyncError::None);
    ASSERT_EQ(client.state(), LspLifecycleState::Stopped);
    ASSERT_TRUE(server.received_payloads().back().find("\"method\":\"exit\"") !=
                std::string::npos);
}

TEST(diagnostics_are_version_checked_bounded_coalesced_and_replayable) {
    FakeLspServer server;
    LspSyncConfig config;
    config.maximum_diagnostics_per_document = 2;
    LspSyncClient client{server, config, timeout};
    ASSERT_EQ(error_of(client.initialize("file:///workspace")), LspSyncError::None);
    server.queue_payload(ssg::test::response(1));
    ASSERT_EQ(error_of(client.poll()), LspSyncError::None);
    ASSERT_EQ(error_of(client.open_document(uri, "cpp", Revision{10}, "abc\n")),
              LspSyncError::None);

    const std::string one =
        "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{"
        "\"line\":0,\"character\":1}},\"severity\":1,\"message\":\"bad\"}]";
    server.queue_payload(ssg::test::diagnostics(uri, 1, one));
    ASSERT_EQ(error_of(client.poll()), LspSyncError::None);
    const auto accepted = client.view_state();
    ASSERT_EQ(accepted.documents.size(), 1U);
    ASSERT_EQ(accepted.documents[0].revision, Revision{10});
    ASSERT_EQ(accepted.documents[0].diagnostics[0].message, "bad");

    ASSERT_EQ(error_of(client.change_document(uri, Revision{11}, "abcd\n")),
              LspSyncError::None);
    server.queue_payload(ssg::test::diagnostics(uri, 1, "[]"));
    ASSERT_EQ(error_of(client.poll()), LspSyncError::StaleDiagnostics);
    ASSERT_EQ(client.view_state().documents[0].diagnostics[0].message, "bad");

    server.queue_payload(ssg::test::diagnostics(uri, 2, "[]"));
    ASSERT_EQ(error_of(client.poll()), LspSyncError::None);
    ASSERT_EQ(client.view_state().documents[0].diagnostics.size(), 0U);

    const auto base = accepted;
    const auto target = client.view_state();
    const auto delta = derive_lsp_sync_delta(base, target);
    const auto replayed = replay_lsp_sync_delta(base, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(replayed.state.value(), target);
    ASSERT_EQ(error_of(replay_lsp_sync_delta(target, delta)),
              LspSyncReplayError::StaleRevision);

    const auto three = "[" + one.substr(1, one.size() - 2) + "," +
                       one.substr(1, one.size() - 2) + "," +
                       one.substr(1, one.size() - 2) + "]";
    server.queue_payload(ssg::test::diagnostics(uri, 2, three));
    ASSERT_EQ(error_of(client.poll()), LspSyncError::DiagnosticLimitExceeded);
    ASSERT_EQ(client.view_state(), target);
}

TEST(cancellation_timeout_and_malformed_input_are_failure_atomic) {
    FakeLspServer server;
    LspSyncClient client{server, {}, timeout};
    ASSERT_EQ(error_of(client.initialize("file:///workspace")), LspSyncError::None);
    server.queue_payload(ssg::test::response(1));
    ASSERT_EQ(error_of(client.poll()), LspSyncError::None);

    const auto request = client.request("workspace/symbol", "{\"query\":\"x\"}");
    ASSERT_TRUE(request.accepted());
    ASSERT_EQ(error_of(client.cancel(request.id)), LspSyncError::None);
    ASSERT_TRUE(server.received_payloads().back().find("$/cancelRequest") !=
                std::string::npos);
    server.queue_payload(ssg::test::response(request.id));
    ASSERT_EQ(error_of(client.poll()), LspSyncError::None);

    const auto before = client.view_state();
    server.timeout_next_read();
    ASSERT_EQ(error_of(client.poll()), LspSyncError::Timeout);
    ASSERT_EQ(client.view_state(), before);

    server.queue_raw("Content-Length: nope\r\n\r\n{}");
    ASSERT_EQ(error_of(client.poll()), LspSyncError::MalformedMessage);
    ASSERT_EQ(client.state(), LspLifecycleState::Failed);
    ASSERT_EQ(client.view_state(), before);
}

TEST(write_timeout_does_not_advance_lifecycle_or_document_state) {
    FakeLspServer server;
    LspSyncClient client{server, {}, timeout};
    server.timeout_next_write();
    ASSERT_EQ(error_of(client.initialize("file:///workspace")),
              LspSyncError::Timeout);
    ASSERT_EQ(client.state(), LspLifecycleState::Stopped);

    ASSERT_EQ(error_of(client.initialize("file:///workspace")), LspSyncError::None);
    server.queue_payload(ssg::test::response(1));
    ASSERT_EQ(error_of(client.poll()), LspSyncError::None);
    server.timeout_next_write();
    ASSERT_EQ(error_of(client.open_document(uri, "cpp", Revision{1}, "x")),
              LspSyncError::Timeout);
    ASSERT_FALSE(client.document_version(uri).has_value());
}

TEST(soft_diagnostic_rejection_does_not_drop_later_framed_messages) {
    FakeLspServer server;
    LspSyncClient client{server, {}, timeout};
    ASSERT_EQ(error_of(client.initialize("file:///workspace")),
              LspSyncError::None);
    server.queue_payload(ssg::test::response(1));
    ASSERT_EQ(error_of(client.poll()), LspSyncError::None);
    const std::string second_uri = "file:///workspace/other.cpp";
    ASSERT_EQ(error_of(client.open_document(uri, "cpp", Revision{1}, "a")),
              LspSyncError::None);
    ASSERT_EQ(error_of(client.open_document(second_uri, "cpp", Revision{2}, "b")),
              LspSyncError::None);
    ASSERT_EQ(error_of(client.change_document(uri, Revision{3}, "aa")),
              LspSyncError::None);

    const std::string valid =
        "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{"
        "\"line\":0,\"character\":1}},\"message\":\"later\"}]";
    server.queue_raw(
        encode_lsp_frame(ssg::test::diagnostics(uri, 1, "[]")) +
        encode_lsp_frame(ssg::test::diagnostics(second_uri, 1, valid)));
    ASSERT_EQ(error_of(client.poll()), LspSyncError::StaleDiagnostics);
    ASSERT_EQ(client.view_state().documents.size(), 1U);
    ASSERT_EQ(client.view_state().documents[0].uri, second_uri);
    ASSERT_EQ(client.view_state().documents[0].diagnostics[0].message, "later");

    const auto before_malformed = client.view_state();
    server.queue_payload(ssg::test::diagnostics(
        second_uri, 1,
        "[{\"range\":{\"start\":{\"line\":0,\"character\":1},\"end\":{"
        "\"line\":0,\"character\":0}},\"message\":\"reversed\"}]"));
    ASSERT_EQ(error_of(client.poll()), LspSyncError::MalformedMessage);
    ASSERT_EQ(client.state(), LspLifecycleState::Ready);
    ASSERT_EQ(client.view_state(), before_malformed);

    server.queue_raw({});
    ASSERT_EQ(error_of(client.poll()), LspSyncError::StreamClosed);
    ASSERT_EQ(client.state(), LspLifecycleState::Ready);
}

} // namespace

int main() {
    RUN(frame_decoder_handles_fragmentation_and_multiple_messages);
    RUN(frame_decoder_rejects_malformed_or_unbounded_headers);
    RUN(utf8_utf16_positions_match_hand_computed_fixture);
    RUN(scripted_server_covers_initialize_sync_and_shutdown_lifecycle);
    RUN(diagnostics_are_version_checked_bounded_coalesced_and_replayable);
    RUN(cancellation_timeout_and_malformed_input_are_failure_atomic);
    RUN(write_timeout_does_not_advance_lifecycle_or_document_state);
    RUN(soft_diagnostic_rejection_does_not_drop_later_framed_messages);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
