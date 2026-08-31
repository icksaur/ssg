#pragma once

#include <ssg/EditorClient.h>
#include <ssg/session_snapshot.h>
#include <ssg/snapshot.h>
#include <ssg/StatusQueue.h>
#include <ssg/ClipboardRegister.h>
#include <ssg/ClientInput.h>
#include <ssg/detail/generated/semantic_wire_manifest.h>

#include <any>
#include <typeindex>
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

class CommandCatalog;

struct ProtocolLimits {
    // Aggregate snapshots can contain the bounded text and binary values below;
    // keep the default envelope large enough to decode messages emitted by the
    // default encoder.
    std::size_t maxMessageBytes{32 * 1024 * 1024};
    // Bounds enforced by the complete codec (Plan 6): a `ProtocolValue` wire
    // value tree is rejected rather than grown without limit.
    std::size_t maxValueDepth{32};
    std::size_t maxCollectionLength{65536};
    std::size_t maxTextBytes{8 * 1024 * 1024};
    std::size_t maxBytesLength{16 * 1024 * 1024};
};

enum class ProtocolError : std::uint8_t {
    None,
    MessageTooLarge,
    MalformedMessage,
    UnsupportedVersion,
    UnsupportedCommand,
    TruncatedMessage,
    ValueBoundsExceeded,
    UnsupportedMessageKind,
};

// Complete protocol codec (Plan 6): a bounded, versioned wire value tree, a
// typed command-argument registry over it, aggregate snapshot/delta
// reconstruction and clipboard/status messages.
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

// How each command's arguments travel.
//
// Backed by the catalog rather than by a snapshot of it: a command registered
// after this object was created is decodable immediately, because there is
// nothing here to go stale.  Holding a copy would be a staleness bug the moment
// registration became dynamic.
class CommandArgumentCodecRegistry {
public:
    explicit CommandArgumentCodecRegistry(
        std::shared_ptr<CommandCatalog const> catalog);
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
//
// Values are PINNED, and a retired kind's value is reserved rather than reused:
// the ordinal is what goes on the wire, so removing an entry would silently
// renumber every kind after it and make two peers disagree about what a message
// means while both still parse it.
enum class ProtocolMessageKind : std::uint8_t {
#define SSG_PROTOCOL_MESSAGE_KIND_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_PROTOCOL_MESSAGE_KIND_ENUMERATORS(
        SSG_PROTOCOL_MESSAGE_KIND_ENUMERATOR)
#undef SSG_PROTOCOL_MESSAGE_KIND_ENUMERATOR
};
#undef SSG_PROTOCOL_MESSAGE_KIND_ENUMERATORS

inline constexpr std::uint8_t kSemanticUiWireVersion = 4;
inline constexpr std::uint8_t kProtocolWireVersion =
    kSemanticUiWireVersion;

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

struct DecodeClientInputResult {
    ProtocolError error;
    std::optional<ClientInput> input;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::None;
    }
};

struct DecodeClientInputResultResult {
    ProtocolError error;
    std::optional<ClientInputResult> result;
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

struct DecodeLegacyPresentationSnapshotResult {
    ProtocolError error;
    std::optional<LegacyPresentationSnapshot> snapshot;
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

class ProtocolCodec {
public:
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
    [[nodiscard]] std::string encodeClientInput(
        ClientInput const& input) const;
    [[nodiscard]] DecodeClientInputResult decodeClientInput(
        std::string_view bytes, ProtocolLimits limits = {}) const;
    [[nodiscard]] std::string encodeClientInputResult(
        ClientInputResult const& result) const;
    [[nodiscard]] DecodeClientInputResultResult decodeClientInputResult(
        std::string_view bytes, ProtocolLimits limits = {}) const;
    [[nodiscard]] std::string encodeSessionSnapshot(
        SessionSnapshot const& snapshot) const;
    [[nodiscard]] DecodeSessionSnapshotResult decodeSessionSnapshot(
        std::string_view bytes, ProtocolLimits limits = {}) const;
    // CONTRACT: These APIs preserve the frozen presentation-bearing snapshot
    // until kSemanticUiWireVersion removes the compatibility wire path in Plan 6.
    [[nodiscard]] std::string encodeLegacyPresentationSnapshot(
        LegacyPresentationSnapshot const& snapshot) const;
    [[nodiscard]] DecodeLegacyPresentationSnapshotResult
    decodeLegacyPresentationSnapshot(
        std::string_view bytes, ProtocolLimits limits = {}) const;
    [[nodiscard]] std::string encodeSessionDelta(SessionDelta const& delta) const;
    [[nodiscard]] DecodeSessionDeltaResult decodeSessionDelta(
        std::string_view bytes, ProtocolLimits limits = {}) const;
};

// Introspection for the style.define key-parity guard: the field names the
// Style wire codec actually emits, derived from the codec itself (not a second
// hand-kept list).  A test asserts this equals ssg::styleDefineKeys(), so a
// field added to one surface but not the other is caught.  Not on any runtime
// path.
[[nodiscard]] std::vector<std::string> styleWireFieldNames();
// CONTRACT: declaration order matches SessionSnapshotSections. This is the
// semantic field inventory consumed by cross-language replay oracles; grid
// presentation fields are never included.
[[nodiscard]] std::vector<std::string> semanticSessionWireFieldNames();
[[nodiscard]] std::vector<std::string> semanticSessionDeltaWireFieldNames();

}  // namespace ssg
