#pragma once

#include <ssg/session.h>
#include <ssg/session_snapshot.h>
#include <ssg/snapshot.h>
#include <ssg/status_queue.h>
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
    std::size_t maxMessageBytes{32 * 1024 * 1024};
    std::size_t maxInsertBytes{32 * 1024};

    // Bounds enforced by the complete codec (Plan 6): a `ProtocolValue` wire
    // value tree is rejected rather than grown without limit.
    std::size_t maxValueDepth{32};
    std::size_t maxCollectionLength{65536};
    std::size_t maxTextBytes{8 * 1024 * 1024};
    std::size_t maxBytesLength{16 * 1024 * 1024};
    std::size_t maxBinaryFrameBytes{16 * 1024 * 1024};
};

enum class ProtocolError : std::uint8_t {
    None,
    MessageTooLarge,
    MalformedMessage,
    UnsupportedVersion,
    UnsupportedCommand,
    InsertTooLarge,
    TruncatedMessage,
    ValueBoundsExceeded,
    UnsupportedMessageKind,
    BinaryFrameTooLarge,
};

struct InsertRequest {
    Revision baseRevision;
    std::string text;

    bool operator==(InsertRequest const&) const = default;
};

struct DecodeInsertResult {
    ProtocolError error;
    std::optional<InsertRequest> request;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::None;
    }
};

struct SliceResponse {
    ProtocolError protocolError;
    CommandError commandError;
    DocumentViewState snapshot;
    std::optional<DocumentDelta> delta;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return protocolError == ProtocolError::None &&
               commandError == CommandError::None;
    }
    bool operator==(SliceResponse const&) const = default;
};

class CoreEditorSlice {
public:
    CoreEditorSlice();
    ~CoreEditorSlice();

    CoreEditorSlice(CoreEditorSlice const&) = delete;
    CoreEditorSlice& operator=(CoreEditorSlice const&) = delete;

    [[nodiscard]] bool attach(InvocationPrincipal principal);
    [[nodiscard]] bool detach(ClientId clientId);
    [[nodiscard]] SliceResponse execute(ClientId clientId,
                                        InsertRequest const& request);
    [[nodiscard]] DocumentViewState snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

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
        NullValue,
        Boolean,
        Integer,
        UnsignedInteger,
        Text,
        Bytes,
        Array,
        Object,
    };

    using Array = std::vector<ProtocolValue>;
    using Field = std::pair<std::string, ProtocolValue>;
    using Object = std::vector<Field>;

    ProtocolValue();

    [[nodiscard]] static ProtocolValue makeNull();
    [[nodiscard]] static ProtocolValue makeBool(bool value);
    [[nodiscard]] static ProtocolValue makeInt(std::int64_t value);
    [[nodiscard]] static ProtocolValue makeUint(std::uint64_t value);
    [[nodiscard]] static ProtocolValue makeText(std::string value);
    [[nodiscard]] static ProtocolValue makeBytes(
        std::vector<std::uint8_t> value);
    [[nodiscard]] static ProtocolValue makeArray(Array items);
    [[nodiscard]] static ProtocolValue makeObject(Object fields);

    [[nodiscard]] Kind kind() const noexcept;
    [[nodiscard]] std::optional<bool> asBool() const;
    [[nodiscard]] std::optional<std::int64_t> asInt() const;
    [[nodiscard]] std::optional<std::uint64_t> asUint() const;
    [[nodiscard]] std::string const* asText() const;
    [[nodiscard]] std::vector<std::uint8_t> const* asBytes() const;
    [[nodiscard]] Array const* asArray() const;
    [[nodiscard]] Object const* asObject() const;
    // Looks up a field by key when kind() == object; nullptr if absent or
    // this value is not an object.
    [[nodiscard]] ProtocolValue const* field(std::string_view key) const;

    bool operator==(ProtocolValue const& other) const;

private:
    struct Storage;
    std::shared_ptr<Storage> storage_;

    explicit ProtocolValue(std::shared_ptr<Storage> storage);
};

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

    [[nodiscard]] bool contains(std::string_view commandId) const;
    // Throws std::invalid_argument for an unknown command_id.
    [[nodiscard]] ProtocolValue encodeArgument(std::string_view commandId,
                                                std::any const& payload) const;
    [[nodiscard]] std::optional<std::any> decodeArgument(
        std::string_view commandId, ProtocolValue const& value) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Builds the immutable registry covering every P0 command with its wire
// converter. Feature-owned argument types (TextInputArguments,
// SelectionCommandArguments, ScrollLinesArguments, ScrollPagesArguments,
// ScrollFractionArguments, DroppedContentArguments) stay in their feature
// headers; this registry owns only their wire adapters.
// Versioned message kinds. Each carries a distinct payload; there is no
// client-asserted capability message (capabilities live in the per-client
// snapshot/delta).

enum class ProtocolMessageKind : std::uint8_t {
    CommandRequest,
    SessionSnapshot,
    SessionDelta,
    ClipboardRequest,
    ClipboardResponse,
    StatusActionInvocation,
    CommandResult,
};

struct DecodeCommandRequestResult {
    ProtocolError error;
    std::optional<ClientCommand> command;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::None;
    }
};

struct DecodeCommandResultResult {
    ProtocolError error;
    std::optional<CommandResult> result;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::None;
    }
};

struct DecodeSessionSnapshotResult {
    ProtocolError error;
    std::optional<SessionSnapshot> snapshot;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::None;
    }
};

struct DecodeSessionDeltaResult {
    ProtocolError error;
    std::optional<SessionDelta> delta;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::None;
    }
};

struct DecodeClipboardRequestResult {
    ProtocolError error;
    std::optional<ClipboardRequest> request;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::None;
    }
};

struct DecodeClipboardResponseResult {
    ProtocolError error;
    std::optional<ClipboardResponse> response;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::None;
    }
};

struct DecodeStatusActionInvocationResult {
    ProtocolError error;
    std::optional<StatusActionInvocation> invocation;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::None;
    }
};

// Binary-frame envelope: the only P0 binary-payload support. Producing
// streaming-output or image payloads remains stretch work; this envelope
// exists so ingress commands (e.g. `file.open_dropped_content`) can carry
// raw bytes with bounded, validated framing.

enum class BinaryPayloadKind : std::uint8_t {
    DroppedContent,
};

struct BinaryFrame {
    std::uint8_t version;
    BinaryPayloadKind kind;
    std::uint64_t requestId;
    std::vector<std::uint8_t> bytes;

    bool operator==(BinaryFrame const&) const = default;
};

struct DecodeBinaryFrameResult {
    ProtocolError error;
    // Decoded bytes are copied into this owned frame independently of the
    // input buffer; the input may be destroyed immediately after decoding.
    std::optional<BinaryFrame> frame;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::None;
    }
};

class ProtocolCodec {
public:
    [[nodiscard]] std::string encodeInsertRequest(
        InsertRequest const& request) const;
    [[nodiscard]] DecodeInsertResult decodeInsertRequest(
        std::string_view message, ProtocolLimits limits = {}) const;
    [[nodiscard]] std::string encodeSliceResponse(
        SliceResponse const& response) const;
    [[nodiscard]] SliceResponse decodeSliceResponse(
        std::string_view message, ProtocolLimits limits = {}) const;

    [[nodiscard]] CommandArgumentCodecRegistry
    buildCommandArgumentCodecRegistry() const;

    [[nodiscard]] std::string encodeCommandRequest(
        ClientCommand const& command,
        CommandArgumentCodecRegistry const& registry) const;
    [[nodiscard]] DecodeCommandRequestResult decodeCommandRequest(
        std::string_view bytes, CommandArgumentCodecRegistry const& registry,
        ProtocolLimits limits = {}) const;
    [[nodiscard]] std::string encodeCommandResult(
        CommandResult const& result) const;
    [[nodiscard]] DecodeCommandResultResult decodeCommandResult(
        std::string_view bytes, ProtocolLimits limits = {}) const;
    [[nodiscard]] std::string encodeSessionSnapshot(
        SessionSnapshot const& snapshot) const;
    [[nodiscard]] DecodeSessionSnapshotResult decodeSessionSnapshot(
        std::string_view bytes, ProtocolLimits limits = {}) const;
    [[nodiscard]] std::string encodeSessionDelta(SessionDelta const& delta) const;
    [[nodiscard]] DecodeSessionDeltaResult decodeSessionDelta(
        std::string_view bytes, ProtocolLimits limits = {}) const;
    [[nodiscard]] std::string encodeClipboardRequest(
        ClipboardRequest const& request) const;
    [[nodiscard]] DecodeClipboardRequestResult decodeClipboardRequest(
        std::string_view bytes, ProtocolLimits limits = {}) const;
    [[nodiscard]] std::string encodeClipboardResponse(
        ClipboardResponse const& response) const;
    [[nodiscard]] DecodeClipboardResponseResult decodeClipboardResponse(
        std::string_view bytes, ProtocolLimits limits = {}) const;
    [[nodiscard]] std::string encodeStatusActionInvocation(
        StatusActionInvocation const& invocation) const;
    [[nodiscard]] DecodeStatusActionInvocationResult
    decodeStatusActionInvocation(std::string_view bytes,
                                 ProtocolLimits limits = {}) const;
    [[nodiscard]] std::string encodeBinaryFrame(BinaryFrame const& frame) const;
    [[nodiscard]] DecodeBinaryFrameResult decodeBinaryFrame(
        std::string_view bytes, ProtocolLimits limits = {}) const;
};

}  // namespace ssg
