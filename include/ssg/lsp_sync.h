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
    none,
    header_too_large,
    missing_content_length,
    duplicate_content_length,
    invalid_content_length,
    message_too_large,
    decoder_failed,
};

struct LspFrameResult {
    std::vector<std::string> messages;
    LspFrameError error = LspFrameError::none;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspFrameError::none;
    }
};

[[nodiscard]] std::string encode_lsp_frame(std::string_view payload);

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
    none,
    invalid_utf8,
    invalid_utf8_boundary,
    line_out_of_range,
    character_out_of_range,
    split_surrogate,
};

struct LspPositionResult {
    LspPosition position;
    LspPositionError error = LspPositionError::none;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspPositionError::none;
    }
};

struct LspByteOffsetResult {
    ByteOffset offset;
    LspPositionError error = LspPositionError::none;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspPositionError::none;
    }
};

[[nodiscard]] LspPositionResult byte_offset_to_lsp_position(
    std::string_view utf8, ByteOffset offset);
[[nodiscard]] LspByteOffsetResult lsp_position_to_byte_offset(
    std::string_view utf8, LspPosition position);

struct LspRange {
    LspPosition start;
    LspPosition end;
    friend bool operator==(const LspRange&, const LspRange&) = default;
};

enum class LspDiagnosticSeverity : std::uint8_t {
    error = 1,
    warning = 2,
    information = 3,
    hint = 4,
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

[[nodiscard]] LspSyncDelta derive_lsp_sync_delta(
    const LspSyncViewState& base, const LspSyncViewState& target);

enum class LspSyncReplayError : std::uint8_t {
    none,
    stale_revision,
    malformed_delta,
};

struct LspSyncReplayResult {
    std::optional<LspSyncViewState> state;
    LspSyncReplayError error = LspSyncReplayError::none;
    [[nodiscard]] bool accepted() const noexcept { return state.has_value(); }
};

[[nodiscard]] LspSyncReplayResult replay_lsp_sync_delta(
    const LspSyncViewState& base, const LspSyncDelta& delta);

enum class LspIoStatus : std::uint8_t { ok, timeout, closed, error };

struct LspIoResult {
    LspIoStatus status = LspIoStatus::ok;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return status == LspIoStatus::ok;
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
    stopped,
    initializing,
    ready,
    shutting_down,
    failed,
};

enum class LspSyncError : std::uint8_t {
    none,
    invalid_state,
    invalid_argument,
    timeout,
    stream_closed,
    stream_error,
    malformed_message,
    stale_document,
    unknown_document,
    stale_diagnostics,
    diagnostic_limit_exceeded,
    request_limit_exceeded,
    unknown_request,
    server_error,
};

struct LspSyncResult {
    LspSyncError error = LspSyncError::none;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return error == LspSyncError::none;
    }
};

struct LspRequestResult {
    std::uint64_t id = 0;
    LspSyncError error = LspSyncError::none;
    std::string message;
    [[nodiscard]] bool accepted() const noexcept {
        return id != 0 && error == LspSyncError::none;
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
    result,
    cancelled,
    server_error,
};

struct LspCompletedResponse {
    std::uint64_t id = 0;
    LspCompletedResponseStatus status = LspCompletedResponseStatus::result;
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

    [[nodiscard]] LspSyncResult open_document(
        std::string uri, std::string language_id, Revision revision,
        std::string text);
    [[nodiscard]] LspSyncResult change_document(
        std::string_view uri, Revision revision, std::string text);
    [[nodiscard]] LspSyncResult close_document(std::string_view uri);
    [[nodiscard]] std::optional<std::int64_t> document_version(
        std::string_view uri) const;
    [[nodiscard]] std::optional<LspDocumentSnapshot> document_snapshot(
        std::string_view uri) const;

    [[nodiscard]] LspRequestResult request(std::string method,
                                           std::string params_json);
    [[nodiscard]] LspSyncResult cancel(std::uint64_t request_id);
    [[nodiscard]] std::vector<LspCompletedResponse>
    take_completed_responses();

    [[nodiscard]] LspLifecycleState state() const noexcept;
    [[nodiscard]] const LspSyncViewState& view_state() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ssg
