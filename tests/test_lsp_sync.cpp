#include <ssg/lsp_sync_client.h>

#include "fake_lsp_server.h"
#include "test_helpers.h"

#include <fstream>
#include <iterator>
#include <string>

namespace {

using namespace ssg;
using ssg::test::FakeLspServer;

constexpr auto kTimeout = std::chrono::milliseconds{5};
const std::string kUri = "file:///workspace/main.cpp";

template <typename Result>
auto errorOf(Result result) {
    return result.error;
}

LspPosition positionOf(LspPositionResult result) {
    return result.position;
}

ByteOffset offsetOf(LspByteOffsetResult result) {
    return result.offset;
}

std::int64_t versionOf(const LspSyncClient& client) {
    return client.documentVersion(kUri).value_or(0);
}

std::string fixture() {
    std::ifstream input(std::string{SSG_TEST_SOURCE_DIR} +
                        "/tests/fixtures/lsp/sync/utf_positions.txt",
                        std::ios::binary);
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

TEST(frameDecoderHandlesFragmentationAndMultipleMessages) {
    LspFrameDecoder decoder;
    const auto frames = encodeLspFrame("{}") + encodeLspFrame("[]");
    auto first = decoder.feed(std::string_view{frames}.substr(0, 11));
    ASSERT_TRUE(first.accepted());
    ASSERT_EQ(first.messages.size(), 0U);
    auto second = decoder.feed(std::string_view{frames}.substr(11));
    ASSERT_TRUE(second.accepted());
    ASSERT_EQ(second.messages.size(), 2U);
    ASSERT_EQ(second.messages[0], "{}");
    ASSERT_EQ(second.messages[1], "[]");
}

TEST(frameDecoderRejectsMalformedOrUnboundedHeaders) {
    LspFrameDecoder decoder{{64, 8}};
    ASSERT_EQ(errorOf(decoder.feed("Content-Length: nope\r\n\r\n{}")),
              LspFrameError::InvalidContentLength);

    LspFrameDecoder duplicate;
    ASSERT_EQ(errorOf(duplicate.feed(
                  "Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}")),
              LspFrameError::DuplicateContentLength);

    LspFrameDecoder oversized{{64, 1}};
    ASSERT_EQ(errorOf(oversized.feed("Content-Length: 2\r\n\r\n{}")),
              LspFrameError::MessageTooLarge);
}

TEST(utf8Utf16PositionsMatchHandComputedFixture) {
    const auto text = fixture();
    ASSERT_EQ(positionOf(byteOffsetToLspPosition(text, ByteOffset{0})),
              (LspPosition{0, 0}));
    ASSERT_EQ(positionOf(byteOffsetToLspPosition(text, ByteOffset{1})),
              (LspPosition{0, 1}));
    ASSERT_EQ(positionOf(byteOffsetToLspPosition(text, ByteOffset{3})),
              (LspPosition{0, 2}));
    ASSERT_EQ(positionOf(byteOffsetToLspPosition(text, ByteOffset{7})),
              (LspPosition{0, 4}));
    ASSERT_EQ(offsetOf(lspPositionToByteOffset(text, {0, 4})),
              ByteOffset{7});
    ASSERT_EQ(errorOf(lspPositionToByteOffset(text, {0, 3})),
              LspPositionError::SplitSurrogate);
    ASSERT_EQ(offsetOf(lspPositionToByteOffset(text, {1, 0})),
              ByteOffset{9});
    ASSERT_EQ(offsetOf(lspPositionToByteOffset("a\r\nb", {1, 0})),
              ByteOffset{3});
    ASSERT_EQ(errorOf(byteOffsetToLspPosition(text, ByteOffset{2})),
              LspPositionError::InvalidUtf8Boundary);
    ASSERT_EQ(errorOf(byteOffsetToLspPosition(std::string{"\xff", 1},
                                                   ByteOffset{0})),
              LspPositionError::InvalidUtf8);
}

TEST(scriptedServerCoversInitializeSyncAndShutdownLifecycle) {
    FakeLspServer server;
    LspSyncClient client{server, {}, kTimeout};

    ASSERT_EQ(errorOf(client.initialize("file:///workspace")), LspSyncError::None);
    ASSERT_EQ(client.state(), LspLifecycleState::Initializing);
    ASSERT_TRUE(server.received_payloads().back().find("\"method\":\"initialize\"") !=
                std::string::npos);

    server.queue_payload(ssg::test::response(1, "{\"capabilities\":{}}"), 7);
    while (client.state() == LspLifecycleState::Initializing) {
        ASSERT_EQ(errorOf(client.poll()), LspSyncError::None);
    }
    ASSERT_EQ(client.state(), LspLifecycleState::Ready);
    ASSERT_TRUE(server.received_payloads().back().find(
                    "\"method\":\"initialized\"") != std::string::npos);

    ASSERT_EQ(errorOf(client.openDocument(kUri, "cpp", std::uint64_t{40}, "one")),
              LspSyncError::None);
    ASSERT_EQ(versionOf(client), 1);
    ASSERT_EQ(errorOf(client.changeDocument(kUri, std::uint64_t{41}, "two")),
              LspSyncError::None);
    ASSERT_EQ(versionOf(client), 2);
    ASSERT_EQ(errorOf(client.changeDocument(kUri, std::uint64_t{41}, "stale")),
              LspSyncError::StaleDocument);
    ASSERT_EQ(versionOf(client), 2);
    ASSERT_EQ(errorOf(client.closeDocument(kUri)), LspSyncError::None);
    ASSERT_FALSE(client.documentVersion(kUri).has_value());

    ASSERT_EQ(errorOf(client.shutdown()), LspSyncError::None);
    server.queue_payload(ssg::test::response(2));
    ASSERT_EQ(errorOf(client.poll()), LspSyncError::None);
    ASSERT_EQ(client.state(), LspLifecycleState::Stopped);
    ASSERT_TRUE(server.received_payloads().back().find("\"method\":\"exit\"") !=
                std::string::npos);
}

TEST(diagnosticsAreVersionCheckedBoundedCoalescedAndReplayable) {
    FakeLspServer server;
    LspSyncConfig config;
    config.maximumDiagnosticsPerDocument = 2;
    LspSyncClient client{server, config, kTimeout};
    ASSERT_EQ(errorOf(client.initialize("file:///workspace")), LspSyncError::None);
    server.queue_payload(ssg::test::response(1));
    ASSERT_EQ(errorOf(client.poll()), LspSyncError::None);
    ASSERT_EQ(errorOf(client.openDocument(kUri, "cpp", std::uint64_t{10}, "abc\n")),
              LspSyncError::None);

    const std::string one =
        "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{"
        "\"line\":0,\"character\":1}},\"severity\":1,\"message\":\"bad\"}]";
    server.queue_payload(ssg::test::diagnostics(kUri, 1, one));
    ASSERT_EQ(errorOf(client.poll()), LspSyncError::None);
    const auto accepted = client.viewState();
    ASSERT_EQ(accepted.documents.size(), 1U);
    ASSERT_EQ(accepted.documents[0].revision, std::uint64_t{10});
    ASSERT_EQ(accepted.documents[0].diagnostics[0].message, "bad");

    ASSERT_EQ(errorOf(client.changeDocument(kUri, std::uint64_t{11}, "abcd\n")),
              LspSyncError::None);
    server.queue_payload(ssg::test::diagnostics(kUri, 1, "[]"));
    ASSERT_EQ(errorOf(client.poll()), LspSyncError::StaleDiagnostics);
    ASSERT_EQ(client.viewState().documents[0].diagnostics[0].message, "bad");

    server.queue_payload(ssg::test::diagnostics(kUri, 2, "[]"));
    ASSERT_EQ(errorOf(client.poll()), LspSyncError::None);
    ASSERT_EQ(client.viewState().documents[0].diagnostics.size(), 0U);

    const auto target = client.viewState();
    const auto three = "[" + one.substr(1, one.size() - 2) + "," +
                       one.substr(1, one.size() - 2) + "," +
                       one.substr(1, one.size() - 2) + "]";
    server.queue_payload(ssg::test::diagnostics(kUri, 2, three));
    ASSERT_EQ(errorOf(client.poll()), LspSyncError::DiagnosticLimitExceeded);
    ASSERT_EQ(client.viewState(), target);
}

TEST(diagnosticSeverityMatchesLspProtocolValues) {
    ASSERT_EQ(static_cast<std::uint8_t>(LspDiagnosticSeverity::Error),
              std::uint8_t{1});
    ASSERT_EQ(static_cast<std::uint8_t>(LspDiagnosticSeverity::Warning),
              std::uint8_t{2});
    ASSERT_EQ(static_cast<std::uint8_t>(LspDiagnosticSeverity::Information),
              std::uint8_t{3});
    ASSERT_EQ(static_cast<std::uint8_t>(LspDiagnosticSeverity::Hint),
              std::uint8_t{4});
}

TEST(cancellationTimeoutAndMalformedInputAreFailureAtomic) {
    FakeLspServer server;
    LspSyncClient client{server, {}, kTimeout};
    ASSERT_EQ(errorOf(client.initialize("file:///workspace")), LspSyncError::None);
    server.queue_payload(ssg::test::response(1));
    ASSERT_EQ(errorOf(client.poll()), LspSyncError::None);

    const auto request = client.request("workspace/symbol", "{\"query\":\"x\"}");
    ASSERT_TRUE(request.accepted());
    ASSERT_EQ(errorOf(client.cancel(request.id)), LspSyncError::None);
    ASSERT_TRUE(server.received_payloads().back().find("$/cancelRequest") !=
                std::string::npos);
    server.queue_payload(ssg::test::response(request.id));
    ASSERT_EQ(errorOf(client.poll()), LspSyncError::None);

    const auto before = client.viewState();
    server.timeout_next_read();
    ASSERT_EQ(errorOf(client.poll()), LspSyncError::Timeout);
    ASSERT_EQ(client.viewState(), before);

    server.queue_raw("Content-Length: nope\r\n\r\n{}");
    ASSERT_EQ(errorOf(client.poll()), LspSyncError::MalformedMessage);
    ASSERT_EQ(client.state(), LspLifecycleState::Failed);
    ASSERT_EQ(client.viewState(), before);
}

TEST(writeTimeoutDoesNotAdvanceLifecycleOrDocumentState) {
    FakeLspServer server;
    LspSyncClient client{server, {}, kTimeout};
    server.timeout_next_write();
    ASSERT_EQ(errorOf(client.initialize("file:///workspace")),
              LspSyncError::Timeout);
    ASSERT_EQ(client.state(), LspLifecycleState::Stopped);

    ASSERT_EQ(errorOf(client.initialize("file:///workspace")), LspSyncError::None);
    server.queue_payload(ssg::test::response(1));
    ASSERT_EQ(errorOf(client.poll()), LspSyncError::None);
    server.timeout_next_write();
    ASSERT_EQ(errorOf(client.openDocument(kUri, "cpp", std::uint64_t{1}, "x")),
              LspSyncError::Timeout);
    ASSERT_FALSE(client.documentVersion(kUri).has_value());
}

TEST(softDiagnosticRejectionDoesNotDropLaterFramedMessages) {
    FakeLspServer server;
    LspSyncClient client{server, {}, kTimeout};
    ASSERT_EQ(errorOf(client.initialize("file:///workspace")),
              LspSyncError::None);
    server.queue_payload(ssg::test::response(1));
    ASSERT_EQ(errorOf(client.poll()), LspSyncError::None);
    const std::string secondUri = "file:///workspace/other.cpp";
    ASSERT_EQ(errorOf(client.openDocument(kUri, "cpp", std::uint64_t{1}, "a")),
              LspSyncError::None);
    ASSERT_EQ(errorOf(client.openDocument(secondUri, "cpp", std::uint64_t{2}, "b")),
              LspSyncError::None);
    ASSERT_EQ(errorOf(client.changeDocument(kUri, std::uint64_t{3}, "aa")),
              LspSyncError::None);

    const std::string valid =
        "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{"
        "\"line\":0,\"character\":1}},\"message\":\"later\"}]";
    server.queue_raw(
        encodeLspFrame(ssg::test::diagnostics(kUri, 1, "[]")) +
        encodeLspFrame(ssg::test::diagnostics(secondUri, 1, valid)));
    ASSERT_EQ(errorOf(client.poll()), LspSyncError::StaleDiagnostics);
    ASSERT_EQ(client.viewState().documents.size(), 1U);
    ASSERT_EQ(client.viewState().documents[0].uri, secondUri);
    ASSERT_EQ(client.viewState().documents[0].diagnostics[0].message, "later");

    const auto beforeMalformed = client.viewState();
    server.queue_payload(ssg::test::diagnostics(
        secondUri, 1,
        "[{\"range\":{\"start\":{\"line\":0,\"character\":1},\"end\":{"
        "\"line\":0,\"character\":0}},\"message\":\"reversed\"}]"));
    ASSERT_EQ(errorOf(client.poll()), LspSyncError::MalformedMessage);
    ASSERT_EQ(client.state(), LspLifecycleState::Ready);
    ASSERT_EQ(client.viewState(), beforeMalformed);

    server.queue_raw({});
    ASSERT_EQ(errorOf(client.poll()), LspSyncError::StreamClosed);
    ASSERT_EQ(client.state(), LspLifecycleState::Ready);
}

} // namespace

SSG_TEST_SUITE(test_lsp_sync) {
    RUN(frameDecoderHandlesFragmentationAndMultipleMessages);
    RUN(frameDecoderRejectsMalformedOrUnboundedHeaders);
    RUN(utf8Utf16PositionsMatchHandComputedFixture);
    RUN(scriptedServerCoversInitializeSyncAndShutdownLifecycle);
    RUN(diagnosticsAreVersionCheckedBoundedCoalescedAndReplayable);
    RUN(diagnosticSeverityMatchesLspProtocolValues);
    RUN(cancellationTimeoutAndMalformedInputAreFailureAtomic);
    RUN(writeTimeoutDoesNotAdvanceLifecycleOrDocumentState);
    RUN(softDiagnosticRejectionDoesNotDropLaterFramedMessages);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
