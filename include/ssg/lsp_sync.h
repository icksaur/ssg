#pragma once

#include <ssg/types.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

struct LspFrameConfig {
    std::size_t maximum_header_bytes = 8 * 1024;
    std::size_t maximum_message_bytes = 4 * 1024 * 1024;
};

enum class LspFrameError : std::uint8_t {
    None,
    HeaderTooLarge,
    MissingContentLength,
    DuplicateContentLength,
    InvalidContentLength,
    MessageTooLarge,
    DecoderFailed,
};

struct LspFrameResult {
    std::vector<std::string> messages;
    LspFrameError error = LspFrameError::None;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspFrameError::None;
    }
};

[[nodiscard]] std::string encodeLspFrame(std::string_view payload);

class LspFrameDecoder {
public:
    explicit LspFrameDecoder(LspFrameConfig config = {});
    [[nodiscard]] LspFrameResult feed(std::string_view bytes);

private:
    LspFrameConfig config_;
    std::string buffer_;
    bool failed_ = false;
};

struct LspPosition {
    std::uint64_t line = 0;
    std::uint64_t character = 0;
    friend bool operator==(const LspPosition&, const LspPosition&) = default;
};

enum class LspPositionError : std::uint8_t {
    None,
    InvalidUtf8,
    InvalidUtf8Boundary,
    LineOutOfRange,
    CharacterOutOfRange,
    SplitSurrogate,
};

struct LspPositionResult {
    LspPosition position;
    LspPositionError error = LspPositionError::None;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspPositionError::None;
    }
};

struct LspByteOffsetResult {
    ByteOffset offset;
    LspPositionError error = LspPositionError::None;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspPositionError::None;
    }
};

[[nodiscard]] LspPositionResult byteOffsetToLspPosition(
    std::string_view utf8, ByteOffset offset);
[[nodiscard]] LspByteOffsetResult lspPositionToByteOffset(
    std::string_view utf8, LspPosition position);

struct LspRange {
    LspPosition start;
    LspPosition end;
    friend bool operator==(const LspRange&, const LspRange&) = default;
};

enum class LspDiagnosticSeverity : std::uint8_t {
    Error = 1,
    Warning = 2,
    Information = 3,
    Hint = 4,
};

struct LspDiagnostic {
    LspRange range;
    std::optional<LspDiagnosticSeverity> severity;
    std::string code;
    std::string message;
    friend bool operator==(const LspDiagnostic&, const LspDiagnostic&) = default;
};

struct LspDocumentDiagnostics {
    std::string uri;
    Revision revision{0};
    std::vector<LspDiagnostic> diagnostics;
    friend bool operator==(const LspDocumentDiagnostics&,
                           const LspDocumentDiagnostics&) = default;
};

struct LspSyncViewState {
    Revision revision{0};
    std::vector<LspDocumentDiagnostics> documents;
    friend bool operator==(const LspSyncViewState&,
                           const LspSyncViewState&) = default;
};

struct LspSyncDelta {
    Revision base_revision{0};
    Revision revision{0};
    std::optional<LspSyncViewState> state;
    friend bool operator==(const LspSyncDelta&, const LspSyncDelta&) = default;
};

[[nodiscard]] LspSyncDelta deriveLspSyncDelta(
    const LspSyncViewState& base, const LspSyncViewState& target);

enum class LspSyncReplayError : std::uint8_t {
    None,
    StaleRevision,
    MalformedDelta,
};

struct LspSyncReplayResult {
    std::optional<LspSyncViewState> state;
    LspSyncReplayError error = LspSyncReplayError::None;
    [[nodiscard]] bool accepted() const noexcept { return state.has_value(); }
};

[[nodiscard]] LspSyncReplayResult replayLspSyncDelta(
    const LspSyncViewState& base, const LspSyncDelta& delta);

enum class LspIoStatus : std::uint8_t { Ok, Timeout, Closed, Error };

struct LspIoResult {
    LspIoStatus status = LspIoStatus::Ok;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return status == LspIoStatus::Ok;
    }
};

class LspByteStream {
public:
    virtual ~LspByteStream() = default;
    [[nodiscard]] virtual LspIoResult write(
        std::string_view bytes, std::chrono::milliseconds timeout) = 0;
    [[nodiscard]] virtual LspIoResult read(
        std::string& bytes, std::size_t maximum_bytes,
        std::chrono::milliseconds timeout) = 0;
};

struct LspSyncConfig {
    LspFrameConfig framing;
    std::size_t maximum_read_bytes = 64 * 1024;
    std::size_t maximum_documents = 256;
    std::size_t maximum_pending_requests = 256;
    std::size_t maximum_diagnostics_per_document = 1000;
    std::size_t maximum_diagnostic_message_bytes = 1024 * 1024;
    std::size_t maximum_json_depth = 64;
};

enum class LspLifecycleState : std::uint8_t {
    Stopped,
    Initializing,
    Ready,
    ShuttingDown,
    Failed,
};

enum class LspSyncError : std::uint8_t {
    None,
    InvalidState,
    InvalidArgument,
    Timeout,
    StreamClosed,
    StreamError,
    MalformedMessage,
    StaleDocument,
    UnknownDocument,
    StaleDiagnostics,
    DiagnosticLimitExceeded,
    RequestLimitExceeded,
    UnknownRequest,
    ServerError,
};

struct LspSyncResult {
    LspSyncError error = LspSyncError::None;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspSyncError::None;
    }
};

struct LspRequestResult {
    std::uint64_t id = 0;
    LspSyncError error = LspSyncError::None;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return id != 0 && error == LspSyncError::None;
    }
};

struct LspDocumentSnapshot {
    std::string uri;
    Revision revision{0};
    std::int64_t version = 0;
    std::string text;
    friend bool operator==(const LspDocumentSnapshot&,
                           const LspDocumentSnapshot&) = default;
};

enum class LspCompletedResponseStatus : std::uint8_t {
    Result,
    Cancelled,
    ServerError,
};

struct LspCompletedResponse {
    std::uint64_t id = 0;
    LspCompletedResponseStatus status = LspCompletedResponseStatus::Result;
    std::string payload_json;
    std::string message;
    friend bool operator==(const LspCompletedResponse&,
                           const LspCompletedResponse&) = default;
};

class LspSyncClient {
public:
    LspSyncClient(LspByteStream& stream, LspSyncConfig config = {},
                  std::chrono::milliseconds io_timeout =
                      std::chrono::milliseconds{100});
    ~LspSyncClient();
    LspSyncClient(const LspSyncClient&) = delete;
    LspSyncClient& operator=(const LspSyncClient&) = delete;
    LspSyncClient(LspSyncClient&&) noexcept;
    LspSyncClient& operator=(LspSyncClient&&) noexcept;

    [[nodiscard]] LspSyncResult initialize(std::string root_uri);
    [[nodiscard]] LspSyncResult shutdown();
    [[nodiscard]] LspSyncResult poll();

    [[nodiscard]] LspSyncResult openDocument(
        std::string uri, std::string language_id, Revision revision,
        std::string text);
    [[nodiscard]] LspSyncResult changeDocument(
        std::string_view uri, Revision revision, std::string text);
    [[nodiscard]] LspSyncResult closeDocument(std::string_view uri);
    [[nodiscard]] std::optional<std::int64_t> documentVersion(
        std::string_view uri) const;
    [[nodiscard]] std::optional<LspDocumentSnapshot> documentSnapshot(
        std::string_view uri) const;

    [[nodiscard]] LspRequestResult request(std::string method,
                                           std::string params_json);
    [[nodiscard]] LspSyncResult cancel(std::uint64_t request_id);
    [[nodiscard]] std::vector<LspCompletedResponse>
    takeCompletedResponses();

    [[nodiscard]] LspLifecycleState state() const noexcept;
    [[nodiscard]] const LspSyncViewState& viewState() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ssg
