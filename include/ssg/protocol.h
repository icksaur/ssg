#pragma once

#include <ssg/session.h>
#include <ssg/session_snapshot.h>
#include <ssg/snapshot.h>
#include <ssg/status.h>
#include <ssg/clipboard.h>

#include <any>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ssg {

struct ProtocolLimits {
    // Aggregate snapshots can contain the bounded text and binary values below;
    // keep the default envelope large enough to decode messages emitted by the
    // default encoder.
    std::size_t max_message_bytes{32 * 1024 * 1024};
    std::size_t max_insert_bytes{32 * 1024};

    // Bounds enforced by the complete codec (Plan 6): a `ProtocolValue` wire
    // value tree is rejected rather than grown without limit.
    std::size_t max_value_depth{32};
    std::size_t max_collection_length{65536};
    std::size_t max_text_bytes{8 * 1024 * 1024};
    std::size_t max_bytes_length{16 * 1024 * 1024};
    std::size_t max_binary_frame_bytes{16 * 1024 * 1024};
};

enum class ProtocolError : std::uint8_t {
    none,
    message_too_large,
    malformed_message,
    unsupported_version,
    unsupported_command,
    insert_too_large,
    truncated_message,
    value_bounds_exceeded,
    unsupported_message_kind,
    binary_frame_too_large,
};

struct InsertRequest {
    Revision base_revision;
    std::string text;

    bool operator==(InsertRequest const&) const = default;
};

struct DecodeInsertResult {
    ProtocolError error;
    std::optional<InsertRequest> request;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::none;
    }
};

struct SliceResponse {
    ProtocolError protocol_error;
    CommandError command_error;
    DocumentViewState snapshot;
    std::optional<DocumentDelta> delta;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return protocol_error == ProtocolError::none &&
               command_error == CommandError::none;
    }
    bool operator==(SliceResponse const&) const = default;
};

[[nodiscard]] std::string encode_insert_request(InsertRequest const& request);
[[nodiscard]] DecodeInsertResult decode_insert_request(
    std::string_view message, ProtocolLimits limits = {});
[[nodiscard]] std::string encode_slice_response(
    SliceResponse const& response);
[[nodiscard]] SliceResponse decode_slice_response(
    std::string_view message, ProtocolLimits limits = {});

class CoreEditorSlice {
public:
    CoreEditorSlice();
    ~CoreEditorSlice();

    CoreEditorSlice(CoreEditorSlice const&) = delete;
    CoreEditorSlice& operator=(CoreEditorSlice const&) = delete;

    [[nodiscard]] bool attach(InvocationPrincipal principal);
    [[nodiscard]] bool detach(ClientId client_id);
    [[nodiscard]] SliceResponse execute(ClientId client_id,
                                        InsertRequest const& request);
    [[nodiscard]] DocumentViewState snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Complete protocol codec (Plan 6): a bounded, versioned wire value tree, a
// typed command-argument registry over it, aggregate snapshot/delta
// reconstruction, clipboard/status messages, and a binary-frame envelope.
// All of it is socket-free; ../http and http_server own the transport.

// A bounded, versioned tree value used as the wire representation for every
// command argument, snapshot/delta field, and auxiliary message in this
// component. Decoding enforces `ProtocolLimits` (max depth, collection
// length, string/byte length) so a malicious or corrupt buffer cannot grow
// memory without bound; violations report `ProtocolError::value_bounds_exceeded`
// through the message-level decode results below rather than throwing.
class ProtocolValue {
public:
    enum class Kind : std::uint8_t {
        null_value,
        boolean,
        integer,
        unsigned_integer,
        text,
        bytes,
        array,
        object,
    };

    using Array = std::vector<ProtocolValue>;
    using Field = std::pair<std::string, ProtocolValue>;
    using Object = std::vector<Field>;

    ProtocolValue();

    [[nodiscard]] static ProtocolValue make_null();
    [[nodiscard]] static ProtocolValue make_bool(bool value);
    [[nodiscard]] static ProtocolValue make_int(std::int64_t value);
    [[nodiscard]] static ProtocolValue make_uint(std::uint64_t value);
    [[nodiscard]] static ProtocolValue make_text(std::string value);
    [[nodiscard]] static ProtocolValue make_bytes(
        std::vector<std::uint8_t> value);
    [[nodiscard]] static ProtocolValue make_array(Array items);
    [[nodiscard]] static ProtocolValue make_object(Object fields);

    [[nodiscard]] Kind kind() const noexcept;
    [[nodiscard]] std::optional<bool> as_bool() const;
    [[nodiscard]] std::optional<std::int64_t> as_int() const;
    [[nodiscard]] std::optional<std::uint64_t> as_uint() const;
    [[nodiscard]] std::string const* as_text() const;
    [[nodiscard]] std::vector<std::uint8_t> const* as_bytes() const;
    [[nodiscard]] Array const* as_array() const;
    [[nodiscard]] Object const* as_object() const;
    // Looks up a field by key when kind() == object; nullptr if absent or
    // this value is not an object.
    [[nodiscard]] ProtocolValue const* field(std::string_view key) const;

    bool operator==(ProtocolValue const& other) const;

private:
    struct Storage;
    std::shared_ptr<Storage> storage_;

    explicit ProtocolValue(std::shared_ptr<Storage> storage);
};

// ---------------------------------------------------------------------------
// Command argument codec registry: binds every assembled P0 command ID
// (`p0_command_descriptors()`) to a converter between its typed std::any
// payload and `ProtocolValue`. There is no untyped fallback; construction
// rejects missing, extra, or duplicate command IDs.

using CommandArgumentEncoder = std::function<ProtocolValue(std::any const&)>;
using CommandArgumentDecoder =
    std::function<std::optional<std::any>(ProtocolValue const&)>;

struct CommandArgumentCodec {
    CommandArgumentEncoder encode;
    CommandArgumentDecoder decode;
};

class CommandArgumentCodecRegistry {
public:
    // Throws std::invalid_argument if `entries` does not have exactly one
    // entry per ID in `p0_command_descriptors()` (missing, extra, and
    // duplicate IDs are all rejected).
    explicit CommandArgumentCodecRegistry(
        std::vector<std::pair<std::string, CommandArgumentCodec>> entries);
    ~CommandArgumentCodecRegistry();

    CommandArgumentCodecRegistry(CommandArgumentCodecRegistry const&) = delete;
    CommandArgumentCodecRegistry& operator=(
        CommandArgumentCodecRegistry const&) = delete;
    CommandArgumentCodecRegistry(CommandArgumentCodecRegistry&&) noexcept;
    CommandArgumentCodecRegistry& operator=(
        CommandArgumentCodecRegistry&&) noexcept;

    [[nodiscard]] bool contains(std::string_view command_id) const;
    // Throws std::invalid_argument for an unknown command_id.
    [[nodiscard]] ProtocolValue encode_argument(std::string_view command_id,
                                                std::any const& payload) const;
    [[nodiscard]] std::optional<std::any> decode_argument(
        std::string_view command_id, ProtocolValue const& value) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Builds the immutable registry covering every P0 command with its wire
// converter. Feature-owned argument types (TextInputArguments,
// SelectionCommandArguments, ScrollLinesArguments, ScrollPagesArguments,
// ScrollFractionArguments, DroppedContentArguments) stay in their feature
// headers; this registry owns only their wire adapters.
[[nodiscard]] CommandArgumentCodecRegistry build_command_argument_codec_registry();

// ---------------------------------------------------------------------------
// Versioned message kinds. Each carries a distinct payload; there is no
// client-asserted capability message (capabilities live in the per-client
// snapshot/delta).

enum class ProtocolMessageKind : std::uint8_t {
    command_request,
    session_snapshot,
    session_delta,
    clipboard_request,
    clipboard_response,
    status_action_invocation,
    command_result,
};

[[nodiscard]] std::string encode_command_request(
    ClientCommand const& command, CommandArgumentCodecRegistry const& registry);

struct DecodeCommandRequestResult {
    ProtocolError error;
    std::optional<ClientCommand> command;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::none;
    }
};

[[nodiscard]] DecodeCommandRequestResult decode_command_request(
    std::string_view bytes, CommandArgumentCodecRegistry const& registry,
    ProtocolLimits limits = {});

[[nodiscard]] std::string encode_command_result(CommandResult const& result);

struct DecodeCommandResultResult {
    ProtocolError error;
    std::optional<CommandResult> result;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::none;
    }
};

[[nodiscard]] DecodeCommandResultResult decode_command_result(
    std::string_view bytes, ProtocolLimits limits = {});

[[nodiscard]] std::string encode_session_snapshot(
    SessionSnapshot const& snapshot);

struct DecodeSessionSnapshotResult {
    ProtocolError error;
    std::optional<SessionSnapshot> snapshot;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::none;
    }
};

[[nodiscard]] DecodeSessionSnapshotResult decode_session_snapshot(
    std::string_view bytes, ProtocolLimits limits = {});

[[nodiscard]] std::string encode_session_delta(SessionDelta const& delta);

struct DecodeSessionDeltaResult {
    ProtocolError error;
    std::optional<SessionDelta> delta;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::none;
    }
};

[[nodiscard]] DecodeSessionDeltaResult decode_session_delta(
    std::string_view bytes, ProtocolLimits limits = {});

[[nodiscard]] std::string encode_clipboard_request(
    ClipboardRequest const& request);

struct DecodeClipboardRequestResult {
    ProtocolError error;
    std::optional<ClipboardRequest> request;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::none;
    }
};

[[nodiscard]] DecodeClipboardRequestResult decode_clipboard_request(
    std::string_view bytes, ProtocolLimits limits = {});

[[nodiscard]] std::string encode_clipboard_response(
    ClipboardResponse const& response);

struct DecodeClipboardResponseResult {
    ProtocolError error;
    std::optional<ClipboardResponse> response;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::none;
    }
};

[[nodiscard]] DecodeClipboardResponseResult decode_clipboard_response(
    std::string_view bytes, ProtocolLimits limits = {});

[[nodiscard]] std::string encode_status_action_invocation(
    StatusActionInvocation const& invocation);

struct DecodeStatusActionInvocationResult {
    ProtocolError error;
    std::optional<StatusActionInvocation> invocation;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::none;
    }
};

[[nodiscard]] DecodeStatusActionInvocationResult
decode_status_action_invocation(std::string_view bytes,
                                ProtocolLimits limits = {});

// ---------------------------------------------------------------------------
// Binary-frame envelope: the only P0 binary-payload support. Producing
// streaming-output or image payloads remains stretch work; this envelope
// exists so ingress commands (e.g. `file.open_dropped_content`) can carry
// raw bytes with bounded, validated framing.

enum class BinaryPayloadKind : std::uint8_t {
    dropped_content,
};

struct BinaryFrame {
    std::uint8_t version;
    BinaryPayloadKind kind;
    std::uint64_t request_id;
    std::vector<std::uint8_t> bytes;

    bool operator==(BinaryFrame const&) const = default;
};

[[nodiscard]] std::string encode_binary_frame(BinaryFrame const& frame);

struct DecodeBinaryFrameResult {
    ProtocolError error;
    // Decoded bytes are copied into this owned frame independently of the
    // input buffer; the input may be destroyed immediately after decoding.
    std::optional<BinaryFrame> frame;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::none;
    }
};

[[nodiscard]] DecodeBinaryFrameResult decode_binary_frame(
    std::string_view bytes, ProtocolLimits limits = {});

}  // namespace ssg
