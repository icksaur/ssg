#include <ssg/Protocol.h>

#include <ssg/CommandCatalog.h>
#include <ssg/CommandSpecBuilder.h>

#include <ssg/FileCommands.h>
#include <ssg/FindReplace.h>
#include <ssg/Keymap.h>
#include <ssg/Selection.h>
#include <ssg/Settings.h>
#include <ssg/TextCodec.h>
#include <ssg/TextInputCommands.h>
#include <ssg/UiTreeProtocol.h>
#include <ssg/UiStateProtocol.h>
#include <ssg/PresenceProtocol.h>
#include <ssg/PaletteProtocol.h>
#include <ssg/PaletteSearcher.h>

#include "legacy_focus_compat.h"
#include "legacy_prompt_compat.h"

#include <any>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace ssg {
namespace {

}  // namespace
struct ProtocolValue::Storage {
    using Value = std::variant<std::monostate, bool, std::int64_t,
                                std::uint64_t, std::string,
                                std::vector<std::uint8_t>, ProtocolValue::Array,
                                ProtocolValue::Object>;
    Value value;
};

ProtocolValue::ProtocolValue()
    : storage_{std::make_shared<Storage>(Storage{Storage::Value{std::monostate{}}})} {}

ProtocolValue::ProtocolValue(std::shared_ptr<Storage> storage)
    : storage_{std::move(storage)} {}

ProtocolValue ProtocolValue::makeNull() { return ProtocolValue{}; }

ProtocolValue ProtocolValue::makeBool(bool value) {
    return ProtocolValue{std::make_shared<Storage>(Storage{Storage::Value{value}})};
}

ProtocolValue ProtocolValue::makeInt(std::int64_t value) {
    return ProtocolValue{std::make_shared<Storage>(Storage{Storage::Value{value}})};
}

ProtocolValue ProtocolValue::makeUint(std::uint64_t value) {
    return ProtocolValue{std::make_shared<Storage>(Storage{Storage::Value{value}})};
}

ProtocolValue ProtocolValue::makeText(std::string value) {
    return ProtocolValue{
        std::make_shared<Storage>(Storage{Storage::Value{std::move(value)}})};
}

ProtocolValue ProtocolValue::makeBytes(std::vector<std::uint8_t> value) {
    return ProtocolValue{
        std::make_shared<Storage>(Storage{Storage::Value{std::move(value)}})};
}

ProtocolValue ProtocolValue::makeArray(Array items) {
    return ProtocolValue{
        std::make_shared<Storage>(Storage{Storage::Value{std::move(items)}})};
}

ProtocolValue ProtocolValue::makeObject(Object fields) {
    return ProtocolValue{
        std::make_shared<Storage>(Storage{Storage::Value{std::move(fields)}})};
}

ProtocolValue::Kind ProtocolValue::kind() const noexcept {
    switch (storage_->value.index()) {
        case 0:
            return Kind::NullValue;
        case 1:
            return Kind::Boolean;
        case 2:
            return Kind::Integer;
        case 3:
            return Kind::UnsignedInteger;
        case 4:
            return Kind::Text;
        case 5:
            return Kind::Bytes;
        case 6:
            return Kind::Array;
        case 7:
            return Kind::Object;
        default:
            return Kind::NullValue;
    }
}

std::optional<bool> ProtocolValue::asBool() const {
    if (auto const* value = std::get_if<bool>(&storage_->value)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::int64_t> ProtocolValue::asInt() const {
    if (auto const* value = std::get_if<std::int64_t>(&storage_->value)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> ProtocolValue::asUint() const {
    if (auto const* value = std::get_if<std::uint64_t>(&storage_->value)) {
        return *value;
    }
    return std::nullopt;
}

std::string const* ProtocolValue::asText() const {
    return std::get_if<std::string>(&storage_->value);
}

std::vector<std::uint8_t> const* ProtocolValue::asBytes() const {
    return std::get_if<std::vector<std::uint8_t>>(&storage_->value);
}

ProtocolValue::Array const* ProtocolValue::asArray() const {
    return std::get_if<ProtocolValue::Array>(&storage_->value);
}

ProtocolValue::Object const* ProtocolValue::asObject() const {
    return std::get_if<ProtocolValue::Object>(&storage_->value);
}

ProtocolValue const* ProtocolValue::field(std::string_view key) const {
    auto const* object = asObject();
    if (object == nullptr) {
        return nullptr;
    }
    for (auto const& entry : *object) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

bool ProtocolValue::operator==(ProtocolValue const& other) const {
    return storage_->value == other.storage_->value;
}

namespace {

// Binary wire encoding for a ProtocolValue tree.
//
// Layout: [u8 tag][tag-specific payload]. null: nothing; boolean: 1 byte;
// integer/unsigned_integer: 8 bytes little-endian; text/bytes: u32 length +
// raw bytes; array: u32 count + N values; object: u32 count + N x (u16 key
// length + key bytes + value). Top-level messages wrap this in
// [u8 wire_version][u8 message_kind][payload].

void writeU8(std::string& out, std::uint8_t value) {
    out.push_back(static_cast<char>(value));
}

void writeU32(std::string& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

void writeU64(std::string& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

void writeI64(std::string& out, std::int64_t value) {
    writeU64(out, static_cast<std::uint64_t>(value));
}

void writeRawBytes(std::string& out, std::vector<std::uint8_t> const& bytes) {
    out.append(reinterpret_cast<char const*>(bytes.data()), bytes.size());
}

class ByteReader {
public:
    explicit ByteReader(std::string_view data) : data_{data} {}

    [[nodiscard]] bool readU8(std::uint8_t& out) {
        if (pos_ + 1 > data_.size()) {
            return false;
        }
        out = static_cast<std::uint8_t>(data_[pos_]);
        pos_ += 1;
        return true;
    }

    [[nodiscard]] bool readU32(std::uint32_t& out) {
        if (pos_ + 4 > data_.size()) {
            return false;
        }
        out = 0;
        for (int index = 0; index < 4; ++index) {
            out |= static_cast<std::uint32_t>(
                       static_cast<std::uint8_t>(data_[pos_ + index]))
                   << (8 * index);
        }
        pos_ += 4;
        return true;
    }

    [[nodiscard]] bool readU64(std::uint64_t& out) {
        if (pos_ + 8 > data_.size()) {
            return false;
        }
        out = 0;
        for (int index = 0; index < 8; ++index) {
            out |= static_cast<std::uint64_t>(
                       static_cast<std::uint8_t>(data_[pos_ + index]))
                   << (8 * index);
        }
        pos_ += 8;
        return true;
    }

    [[nodiscard]] bool readI64(std::int64_t& out) {
        std::uint64_t raw = 0;
        if (!readU64(raw)) {
            return false;
        }
        out = static_cast<std::int64_t>(raw);
        return true;
    }

    [[nodiscard]] bool readBytes(std::size_t length, std::string_view& out) {
        if (pos_ + length > data_.size()) {
            return false;
        }
        out = data_.substr(pos_, length);
        pos_ += length;
        return true;
    }

    [[nodiscard]] bool atEnd() const noexcept { return pos_ == data_.size(); }

private:
    std::string_view data_;
    std::size_t pos_ = 0;
};

enum class ValueReadStatus : std::uint8_t { Ok, Truncated, Malformed, BoundsExceeded };

void writeValue(std::string& out, ProtocolValue const& value) {
    switch (value.kind()) {
        case ProtocolValue::Kind::NullValue:
            writeU8(out, 0);
            break;
        case ProtocolValue::Kind::Boolean:
            writeU8(out, 1);
            writeU8(out, *value.asBool() ? 1 : 0);
            break;
        case ProtocolValue::Kind::Integer:
            writeU8(out, 2);
            writeI64(out, *value.asInt());
            break;
        case ProtocolValue::Kind::UnsignedInteger:
            writeU8(out, 3);
            writeU64(out, *value.asUint());
            break;
        case ProtocolValue::Kind::Text: {
            writeU8(out, 4);
            auto const& text = *value.asText();
            writeU32(out, static_cast<std::uint32_t>(text.size()));
            out.append(text);
            break;
        }
        case ProtocolValue::Kind::Bytes: {
            writeU8(out, 5);
            auto const& bytes = *value.asBytes();
            writeU32(out, static_cast<std::uint32_t>(bytes.size()));
            writeRawBytes(out, bytes);
            break;
        }
        case ProtocolValue::Kind::Array: {
            writeU8(out, 6);
            auto const& items = *value.asArray();
            writeU32(out, static_cast<std::uint32_t>(items.size()));
            for (auto const& item : items) {
                writeValue(out, item);
            }
            break;
        }
        case ProtocolValue::Kind::Object: {
            writeU8(out, 7);
            auto const& fields = *value.asObject();
            writeU32(out, static_cast<std::uint32_t>(fields.size()));
            for (auto const& [key, field_value] : fields) {
                writeU32(out, static_cast<std::uint32_t>(key.size()));
                out.append(key);
                writeValue(out, field_value);
            }
            break;
        }
    }
}

ValueReadStatus readValue(ByteReader& reader, ProtocolValue& out,
                           std::size_t depth, ProtocolLimits const& limits) {
    if (depth > limits.maxValueDepth) {
        return ValueReadStatus::BoundsExceeded;
    }
    std::uint8_t tag = 0;
    if (!reader.readU8(tag)) {
        return ValueReadStatus::Truncated;
    }
    switch (tag) {
        case 0:
            out = ProtocolValue::makeNull();
            return ValueReadStatus::Ok;
        case 1: {
            std::uint8_t raw = 0;
            if (!reader.readU8(raw)) {
                return ValueReadStatus::Truncated;
            }
            if (raw > 1) {
                return ValueReadStatus::Malformed;
            }
            out = ProtocolValue::makeBool(raw != 0);
            return ValueReadStatus::Ok;
        }
        case 2: {
            std::int64_t value = 0;
            if (!reader.readI64(value)) {
                return ValueReadStatus::Truncated;
            }
            out = ProtocolValue::makeInt(value);
            return ValueReadStatus::Ok;
        }
        case 3: {
            std::uint64_t value = 0;
            if (!reader.readU64(value)) {
                return ValueReadStatus::Truncated;
            }
            out = ProtocolValue::makeUint(value);
            return ValueReadStatus::Ok;
        }
        case 4: {
            std::uint32_t length = 0;
            if (!reader.readU32(length)) {
                return ValueReadStatus::Truncated;
            }
            if (length > limits.maxTextBytes) {
                return ValueReadStatus::BoundsExceeded;
            }
            std::string_view bytes;
            if (!reader.readBytes(length, bytes)) {
                return ValueReadStatus::Truncated;
            }
            out = ProtocolValue::makeText(std::string{bytes});
            return ValueReadStatus::Ok;
        }
        case 5: {
            std::uint32_t length = 0;
            if (!reader.readU32(length)) {
                return ValueReadStatus::Truncated;
            }
            if (length > limits.maxBytesLength) {
                return ValueReadStatus::BoundsExceeded;
            }
            std::string_view bytes;
            if (!reader.readBytes(length, bytes)) {
                return ValueReadStatus::Truncated;
            }
            out = ProtocolValue::makeBytes(
                std::vector<std::uint8_t>{bytes.begin(), bytes.end()});
            return ValueReadStatus::Ok;
        }
        case 6: {
            std::uint32_t count = 0;
            if (!reader.readU32(count)) {
                return ValueReadStatus::Truncated;
            }
            if (count > limits.maxCollectionLength) {
                return ValueReadStatus::BoundsExceeded;
            }
            ProtocolValue::Array items;
            items.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                ProtocolValue item;
                auto status = readValue(reader, item, depth + 1, limits);
                if (status != ValueReadStatus::Ok) {
                    return status;
                }
                items.push_back(std::move(item));
            }
            out = ProtocolValue::makeArray(std::move(items));
            return ValueReadStatus::Ok;
        }
        case 7: {
            std::uint32_t count = 0;
            if (!reader.readU32(count)) {
                return ValueReadStatus::Truncated;
            }
            if (count > limits.maxCollectionLength) {
                return ValueReadStatus::BoundsExceeded;
            }
            ProtocolValue::Object fields;
            fields.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                std::uint32_t keyLength = 0;
                if (!reader.readU32(keyLength)) {
                    return ValueReadStatus::Truncated;
                }
                if (keyLength > limits.maxTextBytes) {
                    return ValueReadStatus::BoundsExceeded;
                }
                std::string_view keyBytes;
                if (!reader.readBytes(keyLength, keyBytes)) {
                    return ValueReadStatus::Truncated;
                }
                ProtocolValue fieldValue;
                auto status = readValue(reader, fieldValue, depth + 1, limits);
                if (status != ValueReadStatus::Ok) {
                    return status;
                }
                fields.emplace_back(std::string{keyBytes}, std::move(fieldValue));
            }
            out = ProtocolValue::makeObject(std::move(fields));
            return ValueReadStatus::Ok;
        }
        default:
            return ValueReadStatus::Malformed;
    }
}

ProtocolError toProtocolError(ValueReadStatus status) {
    switch (status) {
        case ValueReadStatus::Ok:
            return ProtocolError::None;
        case ValueReadStatus::Truncated:
            return ProtocolError::TruncatedMessage;
        case ValueReadStatus::Malformed:
            return ProtocolError::MalformedMessage;
        case ValueReadStatus::BoundsExceeded:
            return ProtocolError::ValueBoundsExceeded;
    }
    return ProtocolError::MalformedMessage;
}


std::string encodeMessage(ProtocolMessageKind kind, ProtocolValue const& payload) {
    std::string out;
    writeU8(out, kProtocolWireVersion);
    writeU8(out, static_cast<std::uint8_t>(kind));
    writeValue(out, payload);
    return out;
}

struct DecodedMessage {
    ProtocolError error;
    std::optional<ProtocolValue> payload;
    std::string message;
};

DecodedMessage decodeMessage(std::string_view bytes,
                              ProtocolMessageKind expectedKind,
                              ProtocolLimits const& limits) {
    if (bytes.size() > limits.maxMessageBytes) {
        return {ProtocolError::MessageTooLarge, std::nullopt,
                "message exceeds the configured byte limit"};
    }
    ByteReader reader{bytes};
    std::uint8_t version = 0;
    if (!reader.readU8(version)) {
        return {ProtocolError::TruncatedMessage, std::nullopt,
                "message is missing its version byte"};
    }
    if (version != kProtocolWireVersion) {
        return {ProtocolError::UnsupportedVersion, std::nullopt,
                "message declares an unsupported protocol version"};
    }
    std::uint8_t kindByte = 0;
    if (!reader.readU8(kindByte)) {
        return {ProtocolError::TruncatedMessage, std::nullopt,
                "message is missing its kind byte"};
    }
    if (kindByte != static_cast<std::uint8_t>(expectedKind)) {
        return {ProtocolError::UnsupportedMessageKind, std::nullopt,
                "message kind does not match the requested decoder"};
    }
    ProtocolValue payload;
    auto status = readValue(reader, payload, 0, limits);
    if (status != ValueReadStatus::Ok) {
        return {toProtocolError(status), std::nullopt,
                "message payload is malformed"};
    }
    if (!reader.atEnd()) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "message has unexpected trailing bytes"};
    }
    return {ProtocolError::None, std::move(payload), {}};
}

}  // namespace

//
// Every leaf/composite type reachable from SessionSnapshotSections,
// SessionDelta and StatusActionInvocation has a
// toValue()/decodePresent() pair, plumbed through one generic fromValue<T>
// entry point defined once below.
//
// Rationale for the decodePresent() split: several domain types (DiffFileId,
// TreeNodeId, TreeProviderId, LanguageId, TreeRevision, ViewportDimensions,
// ScrollFractionArguments, SelectionSet and therefore SelectionViewState,
// JournalDocumentKey, and every aggregate embedding one of these) have no
// default constructor, so a generic "T out{}; decode into out" pattern does
// not compile. Every decodePresent() overload instead receives a
// std::optional<T>& and constructs the result in place via out.emplace(...)
// from already-decoded parts -- never a bare default-constructed T. The
// single generic fromValue<T>() wrapper interprets a wire null as "field is
// legitimately absent" (out.reset(); return true) before delegating to
// decodePresent() for the non-null case. This one signature serves both:
//   - decoding a genuinely domain-optional field (std::optional<T> in the
//     C++ struct): the caller keeps the resulting std::optional<T> as-is.
//   - decoding a domain-required field (plain T in the C++ struct): the
//     caller uses require_field<T>() (below), which treats an empty result
//     -- whether from a malformed payload or an unexpected wire null in a
//     required-field position -- as a decode failure.
//
// Ordering constraint: a function template defined in this anonymous
// namespace that makes a dependent call does not pick up sibling
// anonymous-namespace overloads declared later in the file (verified
// empirically), so every concrete/generic toValue()/decodePresent()
// overload is forward-declared before fromValue<T>, toValue<optional<T>>,
// toValue<vector<T>>, and toValue<array<T,N>> -- the generic templates
// that call them -- are *defined*. Forward declarations are added to this
// block as new leaf/composite types are introduced further down the file.
namespace {

ProtocolValue toValue(bool value);
bool decodePresent(ProtocolValue const& value, std::optional<bool>& out);
ProtocolValue toValue(std::uint8_t value);
bool decodePresent(ProtocolValue const& value, std::optional<std::uint8_t>& out);
ProtocolValue toValue(std::uint32_t value);
bool decodePresent(ProtocolValue const& value, std::optional<std::uint32_t>& out);
ProtocolValue toValue(std::uint64_t value);
bool decodePresent(ProtocolValue const& value, std::optional<std::uint64_t>& out);
ProtocolValue toValue(std::int64_t value);
bool decodePresent(ProtocolValue const& value, std::optional<std::int64_t>& out);
ProtocolValue toValue(int value);
bool decodePresent(ProtocolValue const& value, std::optional<int>& out);
ProtocolValue toValue(std::string const& value);
bool decodePresent(ProtocolValue const& value, std::optional<std::string>& out);
ProtocolValue toValue(std::vector<std::uint8_t> const& value);
bool decodePresent(ProtocolValue const& value,
                    std::optional<std::vector<std::uint8_t>>& out);
ProtocolValue toValue(std::filesystem::path const& value);
bool decodePresent(ProtocolValue const& value,
                    std::optional<std::filesystem::path>& out);

template <typename Enum, typename = std::enable_if_t<std::is_enum_v<Enum>>>
ProtocolValue toValue(Enum value) {
    return ProtocolValue::makeUint(static_cast<std::uint64_t>(
        static_cast<std::underlying_type_t<Enum>>(value)));
}

ProtocolValue toValue(bool value) { return ProtocolValue::makeBool(value); }
bool decodePresent(ProtocolValue const& value, std::optional<bool>& out) {
    auto decoded = value.asBool();
    if (!decoded) return false;
    out = *decoded;
    return true;
}

ProtocolValue toValue(std::uint8_t value) {
    return ProtocolValue::makeUint(value);
}
bool decodePresent(ProtocolValue const& value, std::optional<std::uint8_t>& out) {
    auto decoded = value.asUint();
    if (!decoded || *decoded > std::numeric_limits<std::uint8_t>::max()) {
        return false;
    }
    out = static_cast<std::uint8_t>(*decoded);
    return true;
}

ProtocolValue toValue(std::uint32_t value) {
    return ProtocolValue::makeUint(value);
}
bool decodePresent(ProtocolValue const& value, std::optional<std::uint32_t>& out) {
    auto decoded = value.asUint();
    if (!decoded || *decoded > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    out = static_cast<std::uint32_t>(*decoded);
    return true;
}

ProtocolValue toValue(std::uint64_t value) {
    return ProtocolValue::makeUint(value);
}
bool decodePresent(ProtocolValue const& value, std::optional<std::uint64_t>& out) {
    auto decoded = value.asUint();
    if (!decoded) return false;
    out = *decoded;
    return true;
}

ProtocolValue toValue(std::int64_t value) {
    return ProtocolValue::makeInt(value);
}
bool decodePresent(ProtocolValue const& value, std::optional<std::int64_t>& out) {
    auto decoded = value.asInt();
    if (!decoded) return false;
    out = *decoded;
    return true;
}

ProtocolValue toValue(int value) {
    return ProtocolValue::makeInt(static_cast<std::int64_t>(value));
}
bool decodePresent(ProtocolValue const& value, std::optional<int>& out) {
    auto decoded = value.asInt();
    if (!decoded || *decoded < std::numeric_limits<int>::min() ||
        *decoded > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(*decoded);
    return true;
}

ProtocolValue toValue(std::string const& value) {
    return ProtocolValue::makeText(value);
}
bool decodePresent(ProtocolValue const& value, std::optional<std::string>& out) {
    auto const* decoded = value.asText();
    if (!decoded) return false;
    out = *decoded;
    return true;
}

ProtocolValue toValue(std::vector<std::uint8_t> const& value) {
    return ProtocolValue::makeBytes(value);
}
bool decodePresent(ProtocolValue const& value,
                    std::optional<std::vector<std::uint8_t>>& out) {
    auto const* decoded = value.asBytes();
    if (!decoded) return false;
    out = *decoded;
    return true;
}

ProtocolValue toValue(std::filesystem::path const& value) {
    return ProtocolValue::makeText(value.string());
}
bool decodePresent(ProtocolValue const& value,
                    std::optional<std::filesystem::path>& out) {
    auto const* decoded = value.asText();
    if (!decoded) return false;
    out = std::filesystem::path{*decoded};
    return true;
}

// (toValue(Enum) is served generically above; only decodePresent needs a
// forward declaration per enum, each implemented via decode_enum().)
bool decodePresent(ProtocolValue const& value, std::optional<DocumentMode>& out);
bool decodePresent(ProtocolValue const& value, std::optional<StatusPriority>& out);
bool decodePresent(ProtocolValue const& value, std::optional<PromptKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<PromptControlKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<SearchMode>& out);
bool decodePresent(ProtocolValue const& value, std::optional<FindReplaceError>& out);
bool decodePresent(ProtocolValue const& value, std::optional<SettingScope>& out);
bool decodePresent(ProtocolValue const& value, std::optional<SettingKey>& out);
bool decodePresent(ProtocolValue const& value, std::optional<TextEncoding>& out);
bool decodePresent(ProtocolValue const& value, std::optional<IndentStyle>& out);
bool decodePresent(ProtocolValue const& value, std::optional<LineEnding>& out);
bool decodePresent(ProtocolValue const& value, std::optional<TabKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<TabRecoveryBadge>& out);
bool decodePresent(ProtocolValue const& value, std::optional<JournalDocumentKeyKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<DiffLineKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<DiffFileStatus>& out);
bool decodePresent(ProtocolValue const& value, std::optional<ExternalAction>& out);
bool decodePresent(ProtocolValue const& value, std::optional<ExternalDocumentStatus>& out);
bool decodePresent(ProtocolValue const& value, std::optional<FollowMode>& out);
bool decodePresent(ProtocolValue const& value, std::optional<TreeProviderKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<TreeNodeKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<GitTreeStatus>& out);
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxScope>& out);
bool decodePresent(ProtocolValue const& value, std::optional<BracketKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<BracketRole>& out);
bool decodePresent(ProtocolValue const& value, std::optional<CommentKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<CommentTokenRole>& out);
bool decodePresent(ProtocolValue const& value, std::optional<LspDiagnosticSeverity>& out);
bool decodePresent(ProtocolValue const& value, std::optional<ShellNodeKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<FocusTarget>& out);
bool decodePresent(ProtocolValue const& value, std::optional<SemanticRole>& out);
bool decodePresent(ProtocolValue const& value, std::optional<ClientInputKind>& out);
bool decodePresent(ProtocolValue const& value,
                   std::optional<InputPointerButton>& out);
bool decodePresent(ProtocolValue const& value,
                   std::optional<InputPointerPhase>& out);
bool decodePresent(ProtocolValue const& value,
                   std::optional<DocumentPointerEdge>& out);
bool decodePresent(ProtocolValue const& value,
                   std::optional<SemanticScrollTarget>& out);

ProtocolValue toValue(Revision const& value);
bool decodePresent(ProtocolValue const& value, std::optional<Revision>& out);
ProtocolValue toValue(ByteOffset const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ByteOffset>& out);
ProtocolValue toValue(LineIndex const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LineIndex>& out);
ProtocolValue toValue(CellIndex const& value);
bool decodePresent(ProtocolValue const& value, std::optional<CellIndex>& out);
ProtocolValue toValue(ClientId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ClientId>& out);
ProtocolValue toValue(ViewId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ViewId>& out);
ProtocolValue toValue(WorkspaceId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<WorkspaceId>& out);
ProtocolValue toValue(CapabilityId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<CapabilityId>& out);
ProtocolValue toValue(StatusId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<StatusId>& out);
ProtocolValue toValue(TabId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TabId>& out);
ProtocolValue toValue(FileDocumentId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FileDocumentId>& out);
ProtocolValue toValue(DiffFileId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DiffFileId>& out);
ProtocolValue toValue(PaneId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PaneId>& out);
ProtocolValue toValue(TreeProviderId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeProviderId>& out);
ProtocolValue toValue(TreeProviderBinding const& value);
bool decodePresent(ProtocolValue const& value,
                   std::optional<TreeProviderBinding>& out);
ProtocolValue toValue(TreeNodeId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeNodeId>& out);
ProtocolValue toValue(TreeRevision const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeRevision>& out);
ProtocolValue toValue(LanguageId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LanguageId>& out);
ProtocolValue toValue(UntitledDocumentId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<UntitledDocumentId>& out);
ProtocolValue toValue(JournalDocumentKey const& value);
bool decodePresent(ProtocolValue const& value, std::optional<JournalDocumentKey>& out);

ProtocolValue toValue(DocumentPosition const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DocumentPosition>& out);
ProtocolValue toValue(DocumentViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DocumentViewState>& out);
ProtocolValue toValue(DocumentDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DocumentDelta>& out);
ProtocolValue toValue(Selection const& value);
bool decodePresent(ProtocolValue const& value, std::optional<Selection>& out);
ProtocolValue toValue(SelectionSet const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SelectionSet>& out);
ProtocolValue toValue(SelectionViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SelectionViewState>& out);
ProtocolValue toValue(SelectionViewDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SelectionViewDelta>& out);
ProtocolValue toValue(SelectionNavigation const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SelectionNavigation>& out);
ProtocolValue toValue(SelectionNavigationDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SelectionNavigationDelta>& out);
ProtocolValue toValue(SelectionSetDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SelectionSetDelta>& out);
ProtocolValue toValue(PromptProjectionDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PromptProjectionDelta>& out);
ProtocolValue toValue(LegacyPromptViewDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LegacyPromptViewDelta>& out);
ProtocolValue toValue(NoticeViewSectionDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<NoticeViewSectionDelta>& out);
ProtocolValue toValue(TreeWindowsDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeWindowsDelta>& out);
ProtocolValue toValue(HistoryViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<HistoryViewState>& out);
ProtocolValue toValue(HistoryDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<HistoryDelta>& out);
ProtocolValue toValue(ClipboardWrite const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ClipboardWrite>& out);
ProtocolValue toValue(ClipboardViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ClipboardViewState>& out);
ProtocolValue toValue(ClipboardDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ClipboardDelta>& out);
ProtocolValue toValue(StatusAction const& value);
bool decodePresent(ProtocolValue const& value, std::optional<StatusAction>& out);
ProtocolValue toValue(StatusItemView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<StatusItemView>& out);
ProtocolValue toValue(StatusViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<StatusViewState>& out);
ProtocolValue toValue(StatusActionInvocation const& value);
bool decodePresent(ProtocolValue const& value, std::optional<StatusActionInvocation>& out);
ProtocolValue toValue(Rect const& value);
bool decodePresent(ProtocolValue const& value, std::optional<Rect>& out);
ProtocolValue toValue(PromptControlView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PromptControlView>& out);
ProtocolValue toValue(PromptViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PromptViewState>& out);
ProtocolValue toValue(PromptControl const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PromptControl>& out);
ProtocolValue toValue(PromptView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PromptView>& out);
ProtocolValue toValue(NoticeAction const& value);
bool decodePresent(ProtocolValue const& value, std::optional<NoticeAction>& out);
ProtocolValue toValue(NoticeView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<NoticeView>& out);
ProtocolValue toValue(PromptStatusViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PromptStatusViewState>& out);
ProtocolValue toValue(PromptStatusDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PromptStatusDelta>& out);
ProtocolValue toValue(SearchResult const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SearchResult>& out);
ProtocolValue toValue(SearchViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SearchViewState>& out);
ProtocolValue toValue(SearchDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SearchDelta>& out);
ProtocolValue toValue(ByteRange const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ByteRange>& out);
ProtocolValue toValue(FindOptions const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FindOptions>& out);
ProtocolValue toValue(FindRequest const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FindRequest>& out);
ProtocolValue toValue(FindMatch const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FindMatch>& out);
ProtocolValue toValue(WorkspaceFileReplacement const& value);
bool decodePresent(ProtocolValue const& value, std::optional<WorkspaceFileReplacement>& out);
ProtocolValue toValue(WorkspaceReplacePreview const& value);
bool decodePresent(ProtocolValue const& value, std::optional<WorkspaceReplacePreview>& out);
ProtocolValue toValue(FindReplaceViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FindReplaceViewState>& out);
ProtocolValue toValue(FindReplaceDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FindReplaceDelta>& out);
ProtocolValue toValue(SettingValue const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SettingValue>& out);
ProtocolValue toValue(EffectiveSetting const& value);
bool decodePresent(ProtocolValue const& value, std::optional<EffectiveSetting>& out);
ProtocolValue toValue(SettingViewEntry const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SettingViewEntry>& out);
ProtocolValue toValue(SettingsViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SettingsViewState>& out);
ProtocolValue toValue(SettingsDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SettingsDelta>& out);
ProtocolValue toValue(SettingsSectionDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SettingsSectionDelta>& out);
ProtocolValue toValue(KeyStroke const& value);
bool decodePresent(ProtocolValue const& value, std::optional<KeyStroke>& out);
ProtocolValue toValue(KeyBinding const& value);
bool decodePresent(ProtocolValue const& value, std::optional<KeyBinding>& out);
ProtocolValue toValue(KeymapViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<KeymapViewState>& out);
ProtocolValue toValue(KeymapDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<KeymapDelta>& out);
ProtocolValue toValue(TextEncodingStatus const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TextEncodingStatus>& out);
ProtocolValue toValue(TextEncodingViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TextEncodingViewState>& out);
ProtocolValue toValue(TextEncodingDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TextEncodingDelta>& out);
ProtocolValue toValue(TabState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TabState>& out);
ProtocolValue toValue(TabViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TabViewState>& out);
ProtocolValue toValue(TabDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TabDelta>& out);
ProtocolValue toValue(DiffHunk const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DiffHunk>& out);
ProtocolValue toValue(DiffWordRange const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DiffWordRange>& out);
ProtocolValue toValue(DiffLineChange const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DiffLineChange>& out);
ProtocolValue toValue(DiffFileView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DiffFileView>& out);
ProtocolValue toValue(DiffViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DiffViewState>& out);
ProtocolValue toValue(DiffDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DiffDelta>& out);
ProtocolValue toValue(ExternalDocumentView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ExternalDocumentView>& out);
ProtocolValue toValue(ExternalActionAffordance const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ExternalActionAffordance>& out);
ProtocolValue toValue(ExternalModificationViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ExternalModificationViewState>& out);
ProtocolValue toValue(ExternalModificationDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ExternalModificationDelta>& out);
ProtocolValue toValue(ViewportDimensions const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ViewportDimensions>& out);
ProtocolValue toValue(FollowScrollOffset const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FollowScrollOffset>& out);
ProtocolValue toValue(FollowTarget const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FollowTarget>& out);
ProtocolValue toValue(FollowClientView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FollowClientView>& out);
ProtocolValue toValue(FollowEditsViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FollowEditsViewState>& out);
ProtocolValue toValue(FollowEditsDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FollowEditsDelta>& out);
ProtocolValue toValue(ResolvedSelectionRange const& value);
bool decodePresent(ProtocolValue const& value,
                   std::optional<ResolvedSelectionRange>& out);
ProtocolValue toValue(TreeNodeCommand const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeNodeCommand>& out);
ProtocolValue toValue(GitTreeAffordance const& value);
bool decodePresent(ProtocolValue const& value, std::optional<GitTreeAffordance>& out);
ProtocolValue toValue(TreeNode const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeNode>& out);
ProtocolValue toValue(TreeNodeView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeNodeView>& out);
ProtocolValue toValue(TreeProviderView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeProviderView>& out);
ProtocolValue toValue(TreeWindow const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeWindow>& out);
ProtocolValue toValue(TreeViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeViewState>& out);
ProtocolValue toValue(TreeProviderDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeProviderDelta>& out);
ProtocolValue toValue(TreeDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeDelta>& out);
ProtocolValue toValue(SyntaxSpan const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxSpan>& out);
ProtocolValue toValue(SyntaxBracketPair const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxBracketPair>& out);
ProtocolValue toValue(UnmatchedBracket const& value);
bool decodePresent(ProtocolValue const& value, std::optional<UnmatchedBracket>& out);
ProtocolValue toValue(SyntaxRange const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxRange>& out);
ProtocolValue toValue(CommentToken const& value);
bool decodePresent(ProtocolValue const& value, std::optional<CommentToken>& out);
ProtocolValue toValue(CommentRange const& value);
bool decodePresent(ProtocolValue const& value, std::optional<CommentRange>& out);
ProtocolValue toValue(LineIndentation const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LineIndentation>& out);
ProtocolValue toValue(SyntaxViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxViewState>& out);
ProtocolValue toValue(SyntaxDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxDelta>& out);
ProtocolValue toValue(LspPosition const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspPosition>& out);
ProtocolValue toValue(LspRange const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspRange>& out);
ProtocolValue toValue(LspDiagnostic const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspDiagnostic>& out);
ProtocolValue toValue(LspDocumentDiagnostics const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspDocumentDiagnostics>& out);
ProtocolValue toValue(LspSyncViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspSyncViewState>& out);
ProtocolValue toValue(LspSyncDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspSyncDelta>& out);
ProtocolValue toValue(LspCompletionItem const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspCompletionItem>& out);
ProtocolValue toValue(LspCompletionViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspCompletionViewState>& out);
ProtocolValue toValue(LspHover const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspHover>& out);
ProtocolValue toValue(LspNavigationTarget const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspNavigationTarget>& out);
ProtocolValue toValue(LspNavigationViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspNavigationViewState>& out);
ProtocolValue toValue(LspFeatureViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspFeatureViewState>& out);
ProtocolValue toValue(LspFeatureDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspFeatureDelta>& out);
ProtocolValue toValue(SrgbColor const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SrgbColor>& out);
ProtocolValue toValue(ThemeSnapshot const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ThemeSnapshot>& out);
ProtocolValue toValue(Style const& value);
bool decodePresent(ProtocolValue const& value, std::optional<Style>& out);
ProtocolValue toValue(ThemeSectionDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ThemeSectionDelta>& out);
ProtocolValue toValue(StyleSectionDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<StyleSectionDelta>& out);
ProtocolValue toValue(GridSize const& value);
bool decodePresent(ProtocolValue const& value, std::optional<GridSize>& out);
ProtocolValue toValue(AccessibilityNode const& value);
bool decodePresent(ProtocolValue const& value, std::optional<AccessibilityNode>& out);
ProtocolValue toValue(PaneGeometry const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PaneGeometry>& out);
ProtocolValue toValue(TabHit const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TabHit>& out);
ProtocolValue toValue(ShellViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ShellViewState>& out);
ProtocolValue toValue(ShellSectionDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ShellSectionDelta>& out);
ProtocolValue toValue(VisualRow const& value);
bool decodePresent(ProtocolValue const& value, std::optional<VisualRow>& out);
ProtocolValue toValue(ProjectedRow const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ProjectedRow>& out);
ProtocolValue toValue(CellHitTarget const& value);
bool decodePresent(ProtocolValue const& value, std::optional<CellHitTarget>& out);
ProtocolValue toValue(ScrollbarMetrics const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ScrollbarMetrics>& out);
ProtocolValue toValue(ViewportViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ViewportViewState>& out);
ProtocolValue toValue(ViewportDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ViewportDelta>& out);
ProtocolValue toValue(SessionTopology const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SessionTopology>& out);
ProtocolValue toValue(ClientSnapshotState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ClientSnapshotState>& out);
ProtocolValue toValue(PresentationSnapshot const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PresentationSnapshot>& out);
ProtocolValue toValue(SessionSnapshotSections const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SessionSnapshotSections>& out);

ProtocolValue toValue(TextInputArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TextInputArguments>& out);
ProtocolValue toValue(PaletteExecuteArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PaletteExecuteArguments>& out);
ProtocolValue toValue(PickerSubmitArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PickerSubmitArguments>& out);
ProtocolValue toValue(TreeSelectArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeSelectArguments>& out);
ProtocolValue toValue(ExternalActionInvocation const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ExternalActionInvocation>& out);
ProtocolValue toValue(FindQueryArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FindQueryArguments>& out);
ProtocolValue toValue(PromptValueArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PromptValueArguments>& out);
ProtocolValue toValue(PromptFocusArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PromptFocusArguments>& out);
ProtocolValue toValue(UiNodeActivationArguments const& value);
bool decodePresent(ProtocolValue const& value,
                   std::optional<UiNodeActivationArguments>& out);
ProtocolValue toValue(SelectionCommandArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SelectionCommandArguments>& out);
ProtocolValue toValue(ScrollLinesArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ScrollLinesArguments>& out);
ProtocolValue toValue(ScrollPagesArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ScrollPagesArguments>& out);
ProtocolValue toValue(ScrollFractionArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ScrollFractionArguments>& out);
ProtocolValue toValue(DroppedContentArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DroppedContentArguments>& out);
ProtocolValue toValue(ReopenWithEncodingArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ReopenWithEncodingArguments>& out);
ProtocolValue toValue(SetEncodingArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SetEncodingArguments>& out);
ProtocolValue toValue(SetLineEndingArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SetLineEndingArguments>& out);
ProtocolValue toValue(SetFinalNewlineArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SetFinalNewlineArguments>& out);
ProtocolValue toValue(SettingSetArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SettingSetArguments>& out);
ProtocolValue toValue(SettingResetArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SettingResetArguments>& out);
ProtocolValue toValue(SettingResetScopeArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SettingResetScopeArguments>& out);
ProtocolValue toValue(WorkspaceReplaceArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<WorkspaceReplaceArguments>& out);

template <typename T>
ProtocolValue toValue(std::optional<T> const& value);
template <typename T>
ProtocolValue toValue(std::vector<T> const& value);
template <typename T, std::size_t N>
ProtocolValue toValue(std::array<T, N> const& value);

template <typename T>
[[nodiscard]] bool fromValue(ProtocolValue const& value, std::optional<T>& out);
template <typename T>
bool decodePresent(ProtocolValue const& value, std::optional<std::vector<T>>& out);
template <typename T, std::size_t N>
bool decodePresent(ProtocolValue const& value, std::optional<std::array<T, N>>& out);

template <typename T>
ProtocolValue toValue(std::optional<T> const& value) {
    if (!value) {
        return ProtocolValue::makeNull();
    }
    return toValue(*value);
}

template <typename T>
ProtocolValue toValue(std::vector<T> const& value) {
    std::vector<ProtocolValue> items;
    items.reserve(value.size());
    for (auto const& item : value) {
        items.push_back(toValue(item));
    }
    return ProtocolValue::makeArray(std::move(items));
}

template <typename T, std::size_t N>
ProtocolValue toValue(std::array<T, N> const& value) {
    std::vector<ProtocolValue> items;
    items.reserve(N);
    for (auto const& item : value) {
        items.push_back(toValue(item));
    }
    return ProtocolValue::makeArray(std::move(items));
}

template <typename T>
[[nodiscard]] bool fromValue(ProtocolValue const& value, std::optional<T>& out) {
    if (value.kind() == ProtocolValue::Kind::NullValue) {
        out.reset();
        return true;
    }
    try {
        return decodePresent(value, out);
    } catch (std::invalid_argument const&) {
        // Domain constructors enforce invariants for trusted in-process
        // callers. Invalid wire values are ordinary decode failures, not
        // exceptions escaping into the transport.
        out.reset();
        return false;
    }
}

template <typename T>
bool decodePresent(ProtocolValue const& value, std::optional<std::vector<T>>& out) {
    auto const* items = value.asArray();
    if (items == nullptr) {
        return false;
    }
    std::vector<T> result;
    result.reserve(items->size());
    for (auto const& item : *items) {
        std::optional<T> decoded;
        if (!fromValue(item, decoded) || !decoded.has_value()) {
            return false;
        }
        result.push_back(std::move(*decoded));
    }
    out.emplace(std::move(result));
    return true;
}

template <typename T, std::size_t N>
bool decodePresent(ProtocolValue const& value, std::optional<std::array<T, N>>& out) {
    auto const* items = value.asArray();
    if (items == nullptr || items->size() != N) {
        return false;
    }
    std::array<T, N> result{};
    for (std::size_t i = 0; i < N; ++i) {
        std::optional<T> decoded;
        if (!fromValue((*items)[i], decoded) || !decoded.has_value()) {
            return false;
        }
        result[i] = std::move(*decoded);
    }
    out.emplace(std::move(result));
    return true;
}

// Unwraps a required (non-optional) field: a null field pointer (absent from
// the wire object), a decode failure, and an explicit wire null all converge
// to std::nullopt, so composite decoders can uniformly reject missing or
// malformed required data with a single `if (!field) return false;`.
template <typename T>
[[nodiscard]] std::optional<T> requireField(ProtocolValue const* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    std::optional<T> decoded;
    if (!fromValue(*value, decoded)) {
        return std::nullopt;
    }
    return decoded;
}

// Decodes a domain-optional field: a null field pointer (absent from the
// wire object) is a legitimate absence (out.reset(), success); a present
// field delegates to fromValue(), which itself treats an explicit wire null
// as absence and rejects malformed data. Unlike require_field(), a
// genuinely-absent field is not an error here.
template <typename T>
[[nodiscard]] bool decodeOptionalField(ProtocolValue const* fieldValue,
                                         std::optional<T>& out) {
    if (fieldValue == nullptr) {
        out.reset();
        return true;
    }
    return fromValue(*fieldValue, out);
}

template <typename Enum, std::size_t N>
[[nodiscard]] bool decodeEnum(ProtocolValue const& value, std::optional<Enum>& out,
                               std::array<Enum, N> const& validValues) {
    auto raw = value.asUint();
    if (!raw) {
        return false;
    }
    for (Enum candidate : validValues) {
        if (static_cast<std::uint64_t>(
                static_cast<std::underlying_type_t<Enum>>(candidate)) ==
            *raw) {
            out.emplace(candidate);
            return true;
        }
    }
    return false;
}


// Enum decodePresent() definitions, each delegating to decode_enum() with
// the closed set of valid values for that enum.

bool decodePresent(ProtocolValue const& value, std::optional<DocumentMode>& out) {
    static constexpr std::array values{DocumentMode::Edit, DocumentMode::ReadOnly,
                                       DocumentMode::Diff};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<StatusPriority>& out) {
    static constexpr std::array values{StatusPriority::Error, StatusPriority::Warning,
                                       StatusPriority::Information, StatusPriority::Progress};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<PromptKind>& out) {
    static constexpr std::array values{PromptKind::Path, PromptKind::Find,
                                       PromptKind::Replace, PromptKind::Settings,
                                       PromptKind::CommandArgument,
                                       PromptKind::Palette};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<PromptControlKind>& out) {
    static constexpr std::array values{PromptControlKind::Input, PromptControlKind::Toggle,
                                       PromptControlKind::Count};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<SearchMode>& out) {
    return decodeEnum(value, out, kAllSearchModes);
}

bool decodePresent(ProtocolValue const& value, std::optional<FindReplaceError>& out) {
    static constexpr std::array values{
        FindReplaceError::None, FindReplaceError::InvalidPattern,
        FindReplaceError::InvalidUtf8, FindReplaceError::InvalidSelection,
        FindReplaceError::BudgetExhausted, FindReplaceError::Cancelled,
        FindReplaceError::NoMatch, FindReplaceError::StaleRevision,
        FindReplaceError::DocumentRejected, FindReplaceError::WorkspaceRejected,
        FindReplaceError::RecoveryRejected};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<SettingScope>& out) {
    static constexpr std::array values{SettingScope::Defaults, SettingScope::User,
                                       SettingScope::Workspace, SettingScope::Language,
                                       SettingScope::Document};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<SettingKey>& out) {
    static constexpr std::array values{
        SettingKey::IndentWidth, SettingKey::IndentStyle, SettingKey::IndentDetection,
        SettingKey::AutoIndent, SettingKey::LineEnding, SettingKey::FinalNewline,
        SettingKey::Encoding, SettingKey::WordWrap, SettingKey::Theme, SettingKey::Keymap,
        SettingKey::SearchCaseSensitive, SettingKey::SearchWholeWord,
        SettingKey::SearchRegularExpression, SettingKey::UndoByteBudget,
        SettingKey::RecoveryByteBudget, SettingKey::TypingCoalescingMs,
        SettingKey::FileFinderRespectGitignore, SettingKey::AutosaveDebounceMs,
        SettingKey::LineNumbers};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<TextEncoding>& out) {
    static constexpr std::array values{TextEncoding::Utf8, TextEncoding::Utf8Bom,
                                       TextEncoding::Utf16le, TextEncoding::Utf16be,
                                       TextEncoding::Windows1252, TextEncoding::Iso88591};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<IndentStyle>& out) {
    static constexpr std::array values{IndentStyle::Spaces, IndentStyle::Tabs};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<LineEnding>& out) {
    static constexpr std::array values{LineEnding::Lf, LineEnding::Crlf, LineEnding::Cr,
                                       LineEnding::Mixed};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<TabKind>& out) {
    static constexpr std::array values{TabKind::Document, TabKind::LiveDiff,
                                       TabKind::ReadOnlyOutput, TabKind::SearchResults,
                                       TabKind::TreeView};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<TabRecoveryBadge>& out) {
    static constexpr std::array values{TabRecoveryBadge::None, TabRecoveryBadge::Pending,
                                       TabRecoveryBadge::Durable, TabRecoveryBadge::Failed};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<JournalDocumentKeyKind>& out) {
    static constexpr std::array values{JournalDocumentKeyKind::Saved,
                                       JournalDocumentKeyKind::Untitled};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<DiffLineKind>& out) {
    static constexpr std::array values{DiffLineKind::Added, DiffLineKind::Removed,
                                       DiffLineKind::Modified};
    return decodeEnum(value, out, values);
}
bool decodePresent(ProtocolValue const& value, std::optional<DiffFileStatus>& out) {
    static constexpr std::array values{DiffFileStatus::Added,
                                       DiffFileStatus::Modified,
                                       DiffFileStatus::Deleted,
                                       DiffFileStatus::Renamed};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<ExternalAction>& out) {
    static constexpr std::array values{ExternalAction::Reload, ExternalAction::KeepBuffer,
                                       ExternalAction::OpenDiff};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<ExternalDocumentStatus>& out) {
    static constexpr std::array values{ExternalDocumentStatus::ExternallyModified,
                                       ExternalDocumentStatus::ExternallyRemoved};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<FollowMode>& out) {
    static constexpr std::array values{FollowMode::Following, FollowMode::Paused};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<TreeProviderKind>& out) {
    static constexpr std::array values{TreeProviderKind::Filesystem, TreeProviderKind::Git,
                                       TreeProviderKind::Symbols};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<TreeNodeKind>& out) {
    static constexpr std::array values{TreeNodeKind::Root, TreeNodeKind::Directory,
                                       TreeNodeKind::File, TreeNodeKind::Symlink,
                                       TreeNodeKind::GitEntry, TreeNodeKind::Symbol};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<GitTreeStatus>& out) {
    static constexpr std::array values{GitTreeStatus::Added, GitTreeStatus::Modified,
                                       GitTreeStatus::Deleted, GitTreeStatus::Renamed,
                                       GitTreeStatus::Untracked};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<SyntaxScope>& out) {
    return decodeEnum(value, out, kAllSyntaxScopes);
}

bool decodePresent(ProtocolValue const& value, std::optional<BracketKind>& out) {
    static constexpr std::array values{BracketKind::Round, BracketKind::Square,
                                       BracketKind::Curly};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<BracketRole>& out) {
    static constexpr std::array values{BracketRole::Open, BracketRole::Close};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<CommentKind>& out) {
    static constexpr std::array values{CommentKind::Line, CommentKind::Block};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<CommentTokenRole>& out) {
    static constexpr std::array values{CommentTokenRole::Line, CommentTokenRole::BlockOpen,
                                       CommentTokenRole::BlockClose};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<LspDiagnosticSeverity>& out) {
    static constexpr std::array values{
        LspDiagnosticSeverity::Error, LspDiagnosticSeverity::Warning,
        LspDiagnosticSeverity::Information, LspDiagnosticSeverity::Hint};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<ShellNodeKind>& out) {
    static constexpr std::array values{
        ShellNodeKind::Header, ShellNodeKind::HeaderField, ShellNodeKind::Footer,
        ShellNodeKind::FooterField, ShellNodeKind::FooterAction, ShellNodeKind::TabBar,
        ShellNodeKind::Tab, ShellNodeKind::Panel, ShellNodeKind::PanelProvider,
        ShellNodeKind::Pane, ShellNodeKind::Scrollbar, ShellNodeKind::PromptReservation,
        ShellNodeKind::EmptyState, ShellNodeKind::NoticeBar,
        ShellNodeKind::NoticeAction, ShellNodeKind::FooterHint,
        ShellNodeKind::TabSeparator};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<FocusTarget>& out) {
    static constexpr std::array values{FocusTarget::Editor, FocusTarget::Panel,
                                       FocusTarget::Prompt};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<SemanticRole>& out) {
    return decodeEnum(value, out, kAllSemanticRoles);
}

bool decodePresent(ProtocolValue const& value, std::optional<ClientInputKind>& out) {
    static constexpr std::array values{
        ClientInputKind::Key, ClientInputKind::Tab, ClientInputKind::Tree,
        ClientInputKind::Picker, ClientInputKind::PromptControl,
        ClientInputKind::ExternalAction, ClientInputKind::StatusAction,
        ClientInputKind::PublishedUiAction, ClientInputKind::NoticeAction,
        ClientInputKind::Document, ClientInputKind::ScrollLines,
        ClientInputKind::ScrollFraction, ClientInputKind::ViewNavigation,
        ClientInputKind::ResolvedPaneFocus,
        ClientInputKind::ResolvedSelection};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value,
                   std::optional<InputPointerButton>& out) {
    static constexpr std::array values{
        InputPointerButton::Primary, InputPointerButton::Auxiliary,
        InputPointerButton::Secondary};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value,
                   std::optional<InputPointerPhase>& out) {
    static constexpr std::array values{
        InputPointerPhase::Press, InputPointerPhase::Move,
        InputPointerPhase::Release, InputPointerPhase::Cancel};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value,
                   std::optional<DocumentPointerEdge>& out) {
    static constexpr std::array values{
        DocumentPointerEdge::None, DocumentPointerEdge::Before,
        DocumentPointerEdge::After};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value,
                   std::optional<SemanticScrollTarget>& out) {
    static constexpr std::array values{
        SemanticScrollTarget::Document, SemanticScrollTarget::Tree};
    return decodeEnum(value, out, values);
}

// Strong-id toValue()/decodePresent() definitions.

ProtocolValue toValue(Revision const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decodePresent(ProtocolValue const& value, std::optional<Revision>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(ByteOffset const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decodePresent(ProtocolValue const& value, std::optional<ByteOffset>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(LineIndex const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decodePresent(ProtocolValue const& value, std::optional<LineIndex>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(CellIndex const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decodePresent(ProtocolValue const& value, std::optional<CellIndex>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(ClientId const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decodePresent(ProtocolValue const& value, std::optional<ClientId>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(ViewId const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decodePresent(ProtocolValue const& value, std::optional<ViewId>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(WorkspaceId const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decodePresent(ProtocolValue const& value, std::optional<WorkspaceId>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(CapabilityId const& value) {
    return ProtocolValue::makeText(std::string{value.value()});
}
bool decodePresent(ProtocolValue const& value, std::optional<CapabilityId>& out) {
    auto const* text = value.asText();
    if (!text) return false;
    out.emplace(*text);
    return true;
}

ProtocolValue toValue(StatusId const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decodePresent(ProtocolValue const& value, std::optional<StatusId>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(TabId const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decodePresent(ProtocolValue const& value, std::optional<TabId>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(FileDocumentId const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decodePresent(ProtocolValue const& value, std::optional<FileDocumentId>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(DiffFileId const& value) {
    return ProtocolValue::makeText(value.value());
}
bool decodePresent(ProtocolValue const& value, std::optional<DiffFileId>& out) {
    auto const* text = value.asText();
    if (!text) return false;
    out.emplace(*text);
    return true;
}

ProtocolValue toValue(PaneId const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decodePresent(ProtocolValue const& value, std::optional<PaneId>& out) {
    auto raw = value.asUint();
    if (!raw || *raw > std::numeric_limits<std::uint32_t>::max()) return false;
    out.emplace(static_cast<std::uint32_t>(*raw));
    return true;
}

ProtocolValue toValue(TreeProviderId const& value) {
    return ProtocolValue::makeText(value.value());
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeProviderId>& out) {
    auto const* text = value.asText();
    if (!text) return false;
    out.emplace(*text);
    return true;
}

ProtocolValue toValue(TreeProviderBinding const& value) {
    return ProtocolValue::makeObject(
        {{"provider_id", toValue(value.id)}, {"kind", toValue(value.kind)}});
}
bool decodePresent(ProtocolValue const& value,
                   std::optional<TreeProviderBinding>& out) {
    if (!value.asObject()) return false;
    auto providerId = requireField<TreeProviderId>(value.field("provider_id"));
    auto kind = requireField<TreeProviderKind>(value.field("kind"));
    if (!providerId || !kind) return false;
    out.emplace(TreeProviderBinding{std::move(*providerId), *kind});
    return true;
}

ProtocolValue toValue(TreeNodeId const& value) {
    return ProtocolValue::makeText(value.value());
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeNodeId>& out) {
    auto const* text = value.asText();
    if (!text) return false;
    out.emplace(*text);
    return true;
}

ProtocolValue toValue(TreeRevision const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeRevision>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(LanguageId const& value) {
    return ProtocolValue::makeText(value.value());
}
bool decodePresent(ProtocolValue const& value, std::optional<LanguageId>& out) {
    auto const* text = value.asText();
    if (!text) return false;
    out.emplace(*text);
    return true;
}

ProtocolValue toValue(UntitledDocumentId const& value) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(value.bytes().size());
    for (std::byte b : value.bytes()) bytes.push_back(static_cast<std::uint8_t>(b));
    return ProtocolValue::makeBytes(std::move(bytes));
}
bool decodePresent(ProtocolValue const& value, std::optional<UntitledDocumentId>& out) {
    auto const* bytes = value.asBytes();
    if (!bytes || bytes->size() != 16) return false;
    std::array<std::byte, 16> raw{};
    for (std::size_t i = 0; i < 16; ++i) raw[i] = static_cast<std::byte>((*bytes)[i]);
    out.emplace(raw);
    return true;
}

ProtocolValue toValue(JournalDocumentKey const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", toValue(value.kind()));
    if (value.kind() == JournalDocumentKeyKind::Saved) {
        fields.emplace_back("path", toValue(value.savedPath()));
    } else {
        fields.emplace_back("untitled_id", toValue(value.untitledId()));
    }
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<JournalDocumentKey>& out) {
    auto kind = requireField<JournalDocumentKeyKind>(value.field("kind"));
    if (!kind) return false;
    if (*kind == JournalDocumentKeyKind::Saved) {
        auto path = requireField<std::string>(value.field("path"));
        if (!path) return false;
        out.emplace(JournalDocumentKey::saved(*path));
    } else {
        auto id = requireField<UntitledDocumentId>(value.field("untitled_id"));
        if (!id) return false;
        out.emplace(JournalDocumentKey::untitled(*id));
    }
    return true;
}

//
// Every composite decodePresent() below starts by rejecting a non-object
// wire value outright: value.field() already returns nullptr for every key
// when the value is not an object, which require_field() and
// decode_optional_field() both turn into "field absent" -- but a struct
// whose fields are *all* domain-optional (e.g. SessionTopology) would then
// wrongly decode a malformed non-object value (an array, a bare integer) as
// "every field absent" instead of rejecting it. The explicit as_object()
// check below closes that gap uniformly.

ProtocolValue toValue(DocumentPosition const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("byte_offset", toValue(value.byteOffset));
    fields.emplace_back("line", toValue(value.line));
    fields.emplace_back("cell", toValue(value.cell));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DocumentPosition>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto byteOffset = requireField<ByteOffset>(value.field("byte_offset"));
    auto line = requireField<LineIndex>(value.field("line"));
    auto cell = requireField<CellIndex>(value.field("cell"));
    if (!byteOffset || !line || !cell) return false;
    out.emplace(DocumentPosition{*byteOffset, *line, *cell});
    return true;
}

ProtocolValue toValue(DocumentViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("text", toValue(value.text));
    fields.emplace_back("caret", toValue(value.caret));
    fields.emplace_back("diff_file_identity", toValue(value.diffFileIdentity));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DocumentViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto text = requireField<std::string>(value.field("text"));
    auto caret = requireField<ByteOffset>(value.field("caret"));
    std::optional<std::string> diffFileIdentity;
    if (!decodeOptionalField(value.field("diff_file_identity"),
                             diffFileIdentity)) {
        return false;
    }
    if (!revision || !text || !caret) return false;
    out.emplace(DocumentViewState{*revision, *text, *caret,
                                  std::move(diffFileIdentity)});
    return true;
}

ProtocolValue toValue(DocumentDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(value.baseRevision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("start", toValue(value.start));
    fields.emplace_back("erased_bytes", toValue(value.erasedBytes));
    fields.emplace_back("inserted_text", toValue(value.insertedText));
    fields.emplace_back("diff_file_identity", toValue(value.diffFileIdentity));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DocumentDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto baseRevision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    auto start = requireField<ByteOffset>(value.field("start"));
    auto erasedBytes = requireField<std::uint64_t>(value.field("erased_bytes"));
    auto insertedText = requireField<std::string>(value.field("inserted_text"));
    std::optional<std::string> diffFileIdentity;
    if (!decodeOptionalField(value.field("diff_file_identity"),
                             diffFileIdentity)) {
        return false;
    }
    if (!baseRevision || !revision || !start || !erasedBytes || !insertedText) {
        return false;
    }
    out.emplace(DocumentDelta{*baseRevision, *revision, *start, *erasedBytes,
                              *insertedText, std::move(diffFileIdentity)});
    return true;
}

ProtocolValue toValue(Selection const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("anchor", toValue(value.anchor));
    fields.emplace_back("active", toValue(value.active));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<Selection>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto anchor = requireField<DocumentPosition>(value.field("anchor"));
    auto active = requireField<DocumentPosition>(value.field("active"));
    if (!anchor || !active) return false;
    out.emplace(Selection{*anchor, *active});
    return true;
}

ProtocolValue toValue(SelectionSet const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("selections", toValue(value.items()));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SelectionSet>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto selections =
        requireField<std::vector<Selection>>(value.field("selections"));
    if (!selections || selections->empty()) return false;
    out.emplace(*selections);
    return true;
}

ProtocolValue toValue(SelectionViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("selections", toValue(value.selections));
    fields.emplace_back("first_visual_row", toValue(value.firstVisualRow));
    fields.emplace_back("first_visual_column", toValue(value.firstVisualColumn));
    fields.emplace_back("desired_cell", toValue(value.desiredCell));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SelectionViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto selections = requireField<SelectionSet>(value.field("selections"));
    auto firstVisualRow =
        requireField<std::uint32_t>(value.field("first_visual_row"));
    auto firstVisualColumn =
        requireField<std::uint32_t>(value.field("first_visual_column"));
    if (!selections || !firstVisualRow || !firstVisualColumn) return false;
    std::optional<CellIndex> desiredCell;
    if (!decodeOptionalField(value.field("desired_cell"), desiredCell)) return false;
    out.emplace(SelectionViewState{*selections, *firstVisualRow,
                                   *firstVisualColumn, desiredCell});
    return true;
}

ProtocolValue toValue(SelectionViewDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SelectionViewDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<SelectionViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(SelectionViewDelta{*changed, std::move(replacement)});
    return true;
}

ProtocolValue toValue(SelectionNavigation const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("first_visual_row", toValue(value.firstVisualRow));
    fields.emplace_back("first_visual_column", toValue(value.firstVisualColumn));
    fields.emplace_back("desired_cell", toValue(value.desiredCell));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SelectionNavigation>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto firstVisualRow =
        requireField<std::uint32_t>(value.field("first_visual_row"));
    auto firstVisualColumn =
        requireField<std::uint32_t>(value.field("first_visual_column"));
    if (!firstVisualRow || !firstVisualColumn) return false;
    std::optional<CellIndex> desiredCell;
    if (!decodeOptionalField(value.field("desired_cell"), desiredCell)) return false;
    out.emplace(SelectionNavigation{*firstVisualRow, *firstVisualColumn,
                                    desiredCell});
    return true;
}

ProtocolValue toValue(SelectionNavigationDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SelectionNavigationDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<SelectionNavigation> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(SelectionNavigationDelta{*changed, std::move(replacement)});
    return true;
}

ProtocolValue toValue(SelectionSetDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SelectionSetDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<SelectionSet> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(SelectionSetDelta{*changed, std::move(replacement)});
    return true;
}

ProtocolValue toValue(PromptProjectionDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptProjectionDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<PromptViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(PromptProjectionDelta{*changed, std::move(replacement)});
    return true;
}

ProtocolValue toValue(LegacyPromptViewDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LegacyPromptViewDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<PromptView> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(LegacyPromptViewDelta{*changed, std::move(replacement)});
    return true;
}

ProtocolValue toValue(NoticeViewSectionDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<NoticeViewSectionDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<NoticeView> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(NoticeViewSectionDelta{*changed, std::move(replacement)});
    return true;
}

ProtocolValue toValue(TreeWindowsDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeWindowsDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<std::vector<TreeWindow>> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(TreeWindowsDelta{*changed, std::move(replacement)});
    return true;
}

ProtocolValue toValue(HistoryViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("can_undo", toValue(value.canUndo));
    fields.emplace_back("can_redo", toValue(value.canRedo));
    fields.emplace_back("retained_bytes", toValue(value.retainedBytes));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<HistoryViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto canUndo = requireField<bool>(value.field("can_undo"));
    auto canRedo = requireField<bool>(value.field("can_redo"));
    auto retainedBytes = requireField<std::uint64_t>(value.field("retained_bytes"));
    if (!canUndo || !canRedo || !retainedBytes) return false;
    out.emplace(HistoryViewState{*canUndo, *canRedo, *retainedBytes});
    return true;
}

ProtocolValue toValue(HistoryDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<HistoryDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<HistoryViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(HistoryDelta{*changed, std::move(replacement)});
    return true;
}


ProtocolValue toValue(Rect const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("x", toValue(value.x));
    fields.emplace_back("y", toValue(value.y));
    fields.emplace_back("width", toValue(value.width));
    fields.emplace_back("height", toValue(value.height));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<Rect>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto x = requireField<int>(value.field("x"));
    auto y = requireField<int>(value.field("y"));
    auto width = requireField<int>(value.field("width"));
    auto height = requireField<int>(value.field("height"));
    if (!x || !y || !width || !height) return false;
    out.emplace(Rect{*x, *y, *width, *height});
    return true;
}

ProtocolValue toValue(GridSize const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("columns", toValue(value.columns));
    fields.emplace_back("rows", toValue(value.rows));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<GridSize>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto columns = requireField<int>(value.field("columns"));
    auto rows = requireField<int>(value.field("rows"));
    if (!columns || !rows) return false;
    out.emplace(GridSize{*columns, *rows});
    return true;
}

ProtocolValue toValue(AccessibilityNode const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("label", toValue(value.label));
    fields.emplace_back("rect", toValue(value.rect));
    fields.emplace_back("role", toValue(value.role));
    fields.emplace_back("content", toValue(value.content));
    fields.emplace_back("command_id", toValue(value.commandId));
    if (value.statusInvocation) {
        fields.emplace_back("status_invocation", toValue(*value.statusInvocation));
    }
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<AccessibilityNode>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto kind = requireField<ShellNodeKind>(value.field("kind"));
    auto id = requireField<std::string>(value.field("id"));
    auto label = requireField<std::string>(value.field("label"));
    auto rect = requireField<Rect>(value.field("rect"));
    auto role = requireField<SemanticRole>(value.field("role"));
    auto content = requireField<std::string>(value.field("content"));
    std::optional<std::string> commandId;
    if (!decodeOptionalField(value.field("command_id"), commandId)) return false;
    std::optional<StatusActionInvocation> statusInvocation;
    if (!decodeOptionalField(value.field("status_invocation"), statusInvocation)) return false;
    if (!kind || !id || !label || !rect || !role || !content) return false;
    out.emplace(AccessibilityNode{*kind, *id, *label, *rect, *role, *content,
                                  commandId, statusInvocation});
    return true;
}

ProtocolValue toValue(PaneGeometry const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("frame", toValue(value.frame));
    fields.emplace_back("content", toValue(value.content));
    fields.emplace_back("scrollbar", toValue(value.scrollbar));
    fields.emplace_back("line_numbers", toValue(value.lineNumbers));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PaneGeometry>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<PaneId>(value.field("id"));
    auto frame = requireField<Rect>(value.field("frame"));
    auto content = requireField<Rect>(value.field("content"));
    auto scrollbar = requireField<Rect>(value.field("scrollbar"));
    auto lineNumbers = requireField<Rect>(value.field("line_numbers"));
    if (!id || !frame || !content || !scrollbar || !lineNumbers) return false;
    out.emplace(PaneGeometry{*id, *frame, *content, *scrollbar, *lineNumbers});
    return true;
}

ProtocolValue toValue(TabHit const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("rect", toValue(value.rect));
    fields.emplace_back("index", toValue(value.index));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TabHit>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto rect = requireField<Rect>(value.field("rect"));
    auto index = requireField<std::uint32_t>(value.field("index"));
    if (!rect || !index) return false;
    out.emplace(TabHit{*rect, *index});
    return true;
}

ProtocolValue toValue(ShellViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("viewport", toValue(value.viewport));
    fields.emplace_back("header", toValue(value.header));
    fields.emplace_back("footer", toValue(value.footer));
    fields.emplace_back("tab_bar", toValue(value.tabBar));
    fields.emplace_back("panel", toValue(value.panel));
    fields.emplace_back("panel_scrollbar", toValue(value.panelScrollbar));
    fields.emplace_back("prompt", toValue(value.prompt));
    fields.emplace_back("panes", toValue(value.panes));
    fields.emplace_back("tab_hits", toValue(value.tabHits));
    fields.emplace_back("accessibility_nodes", toValue(value.accessibilityNodes));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ShellViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto viewport = requireField<GridSize>(value.field("viewport"));
    auto panes = requireField<std::vector<PaneGeometry>>(value.field("panes"));
    auto tabHits = requireField<std::vector<TabHit>>(value.field("tab_hits"));
    auto accessibilityNodes =
        requireField<std::vector<AccessibilityNode>>(value.field("accessibility_nodes"));
    if (!viewport || !panes || !tabHits || !accessibilityNodes) return false;
    ShellViewState result;
    result.viewport = *viewport;
    if (!decodeOptionalField(value.field("header"), result.header)) return false;
    if (!decodeOptionalField(value.field("footer"), result.footer)) return false;
    if (!decodeOptionalField(value.field("tab_bar"), result.tabBar)) return false;
    if (!decodeOptionalField(value.field("panel"), result.panel)) return false;
    if (!decodeOptionalField(value.field("panel_scrollbar"), result.panelScrollbar)) return false;
    if (!decodeOptionalField(value.field("prompt"), result.prompt)) return false;
    result.panes = *panes;
    result.tabHits = *tabHits;
    result.accessibilityNodes = *accessibilityNodes;
    out.emplace(std::move(result));
    return true;
}


ProtocolValue toValue(PromptInput const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessibleLabel));
    fields.emplace_back("value", toValue(value.value));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptInput>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto accessibleLabel = requireField<std::string>(value.field("accessible_label"));
    auto textValue = requireField<std::string>(value.field("value"));
    if (!id || !accessibleLabel || !textValue) return false;
    out.emplace(PromptInput{*id, *accessibleLabel, *textValue});
    return true;
}

ProtocolValue toValue(PromptToggle const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessibleLabel));
    fields.emplace_back("value", toValue(value.value));
    fields.emplace_back("width", toValue(value.width));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptToggle>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto accessibleLabel = requireField<std::string>(value.field("accessible_label"));
    auto toggleValue = requireField<bool>(value.field("value"));
    auto width = requireField<int>(value.field("width"));
    if (!id || !accessibleLabel || !toggleValue || !width) return false;
    out.emplace(PromptToggle{*id, *accessibleLabel, *toggleValue, *width});
    return true;
}

ProtocolValue toValue(PromptMatchCount const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessibleLabel));
    fields.emplace_back("value", toValue(value.value));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptMatchCount>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto accessibleLabel = requireField<std::string>(value.field("accessible_label"));
    auto textValue = requireField<std::string>(value.field("value"));
    if (!id || !accessibleLabel || !textValue) return false;
    out.emplace(PromptMatchCount{*id, *accessibleLabel, *textValue});
    return true;
}

ProtocolValue toValue(PromptControlView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessibleLabel));
    fields.emplace_back("value", toValue(value.value));
    fields.emplace_back("checked", toValue(value.checked));
    fields.emplace_back("rect", toValue(value.rect));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptControlView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto kind = requireField<PromptControlKind>(value.field("kind"));
    auto id = requireField<std::string>(value.field("id"));
    auto accessibleLabel = requireField<std::string>(value.field("accessible_label"));
    auto textValue = requireField<std::string>(value.field("value"));
    auto checked = requireField<bool>(value.field("checked"));
    auto rect = requireField<Rect>(value.field("rect"));
    if (!kind || !id || !accessibleLabel || !textValue || !checked || !rect) {
        return false;
    }
    out.emplace(PromptControlView{*kind, *id, *accessibleLabel, *textValue,
                                  *checked, *rect});
    return true;
}

ProtocolValue toValue(PromptViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("accessible_label", toValue(value.accessibleLabel));
    fields.emplace_back("rect", toValue(value.rect));
    fields.emplace_back("controls", toValue(value.controls));
    fields.emplace_back("active_input",
                        toValue(static_cast<std::uint64_t>(value.activeInput)));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto kind = requireField<PromptKind>(value.field("kind"));
    auto accessibleLabel = requireField<std::string>(value.field("accessible_label"));
    auto rect = requireField<Rect>(value.field("rect"));
    auto controls = requireField<std::vector<PromptControlView>>(value.field("controls"));
    auto activeInput =
        requireField<std::uint64_t>(value.field("active_input"));
    if (!kind || !accessibleLabel || !rect || !controls || !activeInput) {
        return false;
    }
    out.emplace(PromptViewState{*kind, *accessibleLabel, *rect, *controls,
                                static_cast<std::size_t>(*activeInput)});
    return true;
}

ProtocolValue toValue(PromptControl const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessibleLabel));
    fields.emplace_back("value", toValue(value.value));
    fields.emplace_back("checked", toValue(value.checked));
    fields.emplace_back("command", toValue(value.command));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptControl>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto kind = requireField<PromptControlKind>(value.field("kind"));
    auto id = requireField<std::string>(value.field("id"));
    auto accessibleLabel = requireField<std::string>(value.field("accessible_label"));
    auto textValue = requireField<std::string>(value.field("value"));
    auto checked = requireField<bool>(value.field("checked"));
    auto command = requireField<std::string>(value.field("command"));
    if (!kind || !id || !accessibleLabel || !textValue || !checked || !command) {
        return false;
    }
    out.emplace(PromptControl{*kind, *id, *accessibleLabel, *textValue, *checked,
                              *command});
    return true;
}

ProtocolValue toValue(PromptView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("accessible_label", toValue(value.accessibleLabel));
    fields.emplace_back("controls", toValue(value.controls));
    fields.emplace_back("active_input",
                        toValue(static_cast<std::uint64_t>(value.activeInput)));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto kind = requireField<PromptKind>(value.field("kind"));
    auto accessibleLabel = requireField<std::string>(value.field("accessible_label"));
    auto controls = requireField<std::vector<PromptControl>>(value.field("controls"));
    auto activeInput = requireField<std::uint64_t>(value.field("active_input"));
    if (!kind || !accessibleLabel || !controls || !activeInput) return false;
    std::size_t inputCount = 0;
    for (auto const& control : *controls) {
        if (control.kind == PromptControlKind::Input) ++inputCount;
    }
    if (inputCount == 0 ? *activeInput != 0 : *activeInput >= inputCount) {
        return false;
    }
    out.emplace(PromptView{*kind, *accessibleLabel, std::move(*controls),
                           static_cast<std::size_t>(*activeInput)});
    return true;
}

ProtocolValue toValue(NoticeAction const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("label", toValue(value.label));
    fields.emplace_back("command", toValue(value.command));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<NoticeAction>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto label = requireField<std::string>(value.field("label"));
    auto command = requireField<std::string>(value.field("command"));
    if (!id || !label || !command) return false;
    if (id->empty() || label->empty() || command->empty()) return false;
    out.emplace(NoticeAction{*id, *label, *command});
    return true;
}

ProtocolValue toValue(NoticeView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("text", toValue(value.text));
    fields.emplace_back("actions", toValue(value.actions));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<NoticeView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto text = requireField<std::string>(value.field("text"));
    auto actions = requireField<std::vector<NoticeAction>>(value.field("actions"));
    if (!text || !actions) return false;
    if (text->empty() || actions->empty()) return false;
    out.emplace(NoticeView{*text, std::move(*actions)});
    return true;
}


ProtocolValue toValue(StatusAction const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessibleLabel));
    fields.emplace_back("command_id", toValue(value.commandId));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<StatusAction>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto accessibleLabel = requireField<std::string>(value.field("accessible_label"));
    auto commandId = requireField<std::string>(value.field("command_id"));
    if (!id || !accessibleLabel || !commandId) return false;
    out.emplace(StatusAction{*id, *accessibleLabel, *commandId});
    return true;
}

ProtocolValue toValue(StatusItemView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("priority", toValue(value.priority));
    fields.emplace_back("generation", toValue(value.generation));
    fields.emplace_back("accessible_label", toValue(value.accessibleLabel));
    fields.emplace_back("actions", toValue(value.actions));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<StatusItemView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<StatusId>(value.field("id"));
    auto priority = requireField<StatusPriority>(value.field("priority"));
    auto generation = requireField<std::uint64_t>(value.field("generation"));
    auto accessibleLabel = requireField<std::string>(value.field("accessible_label"));
    auto actions = requireField<std::vector<StatusAction>>(value.field("actions"));
    if (!id || !priority || !generation || !accessibleLabel || !actions) {
        return false;
    }
    out.emplace(StatusItemView{*id, *priority, *generation, *accessibleLabel,
                               *actions});
    return true;
}

ProtocolValue toValue(StatusViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("items", toValue(value.items));
    fields.emplace_back("selected", toValue(static_cast<std::uint64_t>(value.selected)));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<StatusViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto items = requireField<std::vector<StatusItemView>>(value.field("items"));
    auto selected = requireField<std::uint64_t>(value.field("selected"));
    if (!items || !selected) return false;
    out.emplace(StatusViewState{*items, static_cast<std::size_t>(*selected)});
    return true;
}

ProtocolValue toValue(StatusActionInvocation const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("status_id", toValue(value.statusId));
    fields.emplace_back("action_id", toValue(value.actionId));
    fields.emplace_back("generation", toValue(value.generation));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<StatusActionInvocation>& out) {
    auto const* object = value.asObject();
    if (!object || object->size() != 3 || !value.field("status_id") ||
        !value.field("action_id") || !value.field("generation")) {
        return false;
    }
    auto statusId = requireField<StatusId>(value.field("status_id"));
    auto actionId = requireField<std::string>(value.field("action_id"));
    auto generation = requireField<std::uint64_t>(value.field("generation"));
    if (!statusId || !actionId || !generation) return false;
    out.emplace(StatusActionInvocation{*statusId, *actionId, *generation});
    return true;
}

ProtocolValue toValue(PromptStatusViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("active_kind", toValue(value.activeKind));
    fields.emplace_back("status", toValue(value.status));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptStatusViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto status = requireField<StatusViewState>(value.field("status"));
    if (!status) return false;
    std::optional<PromptKind> activeKind;
    if (!decodeOptionalField(value.field("active_kind"), activeKind)) return false;
    out.emplace(PromptStatusViewState{*status, activeKind});
    return true;
}

ProtocolValue toValue(PromptStatusDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptStatusDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<PromptStatusViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(PromptStatusDelta{*changed, std::move(replacement)});
    return true;
}


ProtocolValue toValue(ClipboardWrite const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("request_revision", toValue(value.requestRevision));
    fields.emplace_back("text", toValue(value.text));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ClipboardWrite>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::uint64_t>(value.field("id"));
    auto requestRevision = requireField<Revision>(value.field("request_revision"));
    auto text = requireField<std::string>(value.field("text"));
    if (!id || !requestRevision || !text) return false;
    out.emplace(ClipboardWrite{*id, *requestRevision, *text});
    return true;
}

ProtocolValue toValue(ClipboardViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("fragments", toValue(value.fragments));
    fields.emplace_back("plain_text", toValue(value.plainText));
    fields.emplace_back("system_write", toValue(value.systemWrite));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ClipboardViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto fragments = requireField<std::vector<std::string>>(value.field("fragments"));
    auto plainText = requireField<std::string>(value.field("plain_text"));
    if (!fragments || !plainText) return false;
    ClipboardViewState result;
    result.fragments = *fragments;
    result.plainText = *plainText;
    if (!decodeOptionalField(value.field("system_write"), result.systemWrite)) {
        return false;
    }
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(ClipboardDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ClipboardDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<ClipboardViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(ClipboardDelta{*changed, std::move(replacement)});
    return true;
}


ProtocolValue toValue(SearchResult const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("mode", toValue(value.mode));
    fields.emplace_back("path", toValue(value.path));
    fields.emplace_back("label", toValue(value.label));
    fields.emplace_back("line", toValue(value.line));
    fields.emplace_back("column", toValue(static_cast<std::uint64_t>(value.column)));
    fields.emplace_back("score", toValue(value.score));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SearchResult>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto mode = requireField<SearchMode>(value.field("mode"));
    auto path = requireField<std::string>(value.field("path"));
    auto label = requireField<std::string>(value.field("label"));
    auto column = requireField<std::uint64_t>(value.field("column"));
    auto score = requireField<int>(value.field("score"));
    if (!mode || !path || !label || !column || !score) return false;
    SearchResult result;
    result.mode = *mode;
    result.path = *path;
    result.label = *label;
    if (!decodeOptionalField(value.field("line"), result.line)) return false;
    result.column = static_cast<std::size_t>(*column);
    result.score = *score;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(SearchViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("palette_open", toValue(value.paletteOpen));
    fields.emplace_back("query", toValue(value.query));
    fields.emplace_back("mode", toValue(value.mode));
    fields.emplace_back("results", toValue(value.results));
    if (value.selectedIndex) {
        fields.emplace_back("selected_index",
                            toValue(static_cast<std::uint64_t>(*value.selectedIndex)));
    } else {
        fields.emplace_back("selected_index", ProtocolValue::makeNull());
    }
    fields.emplace_back("search_generation", toValue(value.searchGeneration));
    fields.emplace_back("searching", toValue(value.searching));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SearchViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto paletteOpen = requireField<bool>(value.field("palette_open"));
    auto query = requireField<std::string>(value.field("query"));
    auto mode = requireField<SearchMode>(value.field("mode"));
    auto results = requireField<std::vector<SearchResult>>(value.field("results"));
    auto searchGeneration = requireField<std::uint64_t>(value.field("search_generation"));
    auto searching = requireField<bool>(value.field("searching"));
    if (!revision || !paletteOpen || !query || !mode || !results ||
        !searchGeneration || !searching) {
        return false;
    }
    SearchViewState result;
    result.revision = *revision;
    result.paletteOpen = *paletteOpen;
    result.query = *query;
    result.mode = *mode;
    result.results = *results;
    std::optional<std::uint64_t> selectedIndex;
    if (!decodeOptionalField(value.field("selected_index"), selectedIndex)) {
        return false;
    }
    if (selectedIndex) {
        result.selectedIndex = static_cast<std::size_t>(*selectedIndex);
    }
    result.searchGeneration = *searchGeneration;
    result.searching = *searching;
    out.emplace(std::move(result));
    return true;
}


ProtocolValue toValue(SearchDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(value.baseRevision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("state", toValue(value.state));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SearchDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto baseRevision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    if (!baseRevision || !revision) return false;
    SearchDelta result;
    result.baseRevision = *baseRevision;
    result.revision = *revision;
    if (!decodeOptionalField(value.field("state"), result.state)) return false;
    out.emplace(std::move(result));
    return true;
}



ProtocolValue toValue(ByteRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("begin", toValue(value.begin));
    fields.emplace_back("end", toValue(value.end));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ByteRange>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto begin = requireField<ByteOffset>(value.field("begin"));
    auto end = requireField<ByteOffset>(value.field("end"));
    if (!begin || !end) return false;
    out.emplace(ByteRange{*begin, *end});
    return true;
}

ProtocolValue toValue(FindOptions const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("case_sensitive", toValue(value.caseSensitive));
    fields.emplace_back("whole_word", toValue(value.wholeWord));
    fields.emplace_back("regex", toValue(value.regex));
    fields.emplace_back("selection_only", toValue(value.selectionOnly));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FindOptions>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto caseSensitive = requireField<bool>(value.field("case_sensitive"));
    auto wholeWord = requireField<bool>(value.field("whole_word"));
    auto regex = requireField<bool>(value.field("regex"));
    auto selectionOnly = requireField<bool>(value.field("selection_only"));
    if (!caseSensitive || !wholeWord || !regex || !selectionOnly) return false;
    out.emplace(FindOptions{*caseSensitive, *wholeWord, *regex, *selectionOnly});
    return true;
}

ProtocolValue toValue(FindRequest const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("query", toValue(value.query));
    fields.emplace_back("options", toValue(value.options));
    fields.emplace_back("selection", toValue(value.selection));
    fields.emplace_back("work_budget", toValue(value.workBudget));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FindRequest>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto query = requireField<std::string>(value.field("query"));
    auto options = requireField<FindOptions>(value.field("options"));
    std::optional<ByteRange> selection;
    auto workBudget = requireField<std::uint64_t>(value.field("work_budget"));
    if (!query || !options ||
        !decodeOptionalField(value.field("selection"), selection) ||
        !workBudget) {
        return false;
    }
    out.emplace(FindRequest{*query, *options, std::move(selection),
                            *workBudget, nullptr});
    return true;
}

ProtocolValue toValue(FindMatch const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("begin", toValue(value.begin));
    fields.emplace_back("end", toValue(value.end));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FindMatch>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto begin = requireField<ByteOffset>(value.field("begin"));
    auto end = requireField<ByteOffset>(value.field("end"));
    if (!begin || !end) return false;
    out.emplace(FindMatch{*begin, *end});
    return true;
}

ProtocolValue toValue(WorkspaceFileReplacement const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("path", toValue(value.path));
    fields.emplace_back("before", toValue(value.before));
    fields.emplace_back("after", toValue(value.after));
    fields.emplace_back("matches", toValue(value.matches));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<WorkspaceFileReplacement>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto path = requireField<std::string>(value.field("path"));
    auto before = requireField<std::string>(value.field("before"));
    auto after = requireField<std::string>(value.field("after"));
    auto matches = requireField<std::vector<FindMatch>>(value.field("matches"));
    if (!path || !before || !after || !matches) return false;
    out.emplace(WorkspaceFileReplacement{*path, *before, *after, *matches});
    return true;
}

ProtocolValue toValue(WorkspaceReplacePreview const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("source_revision", toValue(value.sourceRevision));
    fields.emplace_back("query", toValue(value.query));
    fields.emplace_back("replacement", toValue(value.replacement));
    fields.emplace_back("options", toValue(value.options));
    fields.emplace_back("changes", toValue(value.changes));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<WorkspaceReplacePreview>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto sourceRevision = requireField<Revision>(value.field("source_revision"));
    auto query = requireField<std::string>(value.field("query"));
    auto replacement = requireField<std::string>(value.field("replacement"));
    auto options = requireField<FindOptions>(value.field("options"));
    auto changes = requireField<std::vector<WorkspaceFileReplacement>>(value.field("changes"));
    if (!sourceRevision || !query || !replacement || !options || !changes) return false;
    out.emplace(WorkspaceReplacePreview{*sourceRevision, *query,
                                        *replacement, *options, *changes});
    return true;
}

ProtocolValue toValue(FindReplaceViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("generation", toValue(value.generation));
    fields.emplace_back("open", toValue(value.open));
    fields.emplace_back("replace_mode", toValue(value.replaceMode));
    fields.emplace_back("source_revision", toValue(value.sourceRevision));
    fields.emplace_back("query", toValue(value.query));
    fields.emplace_back("replacement", toValue(value.replacement));
    fields.emplace_back("options", toValue(value.options));
    fields.emplace_back("matches", toValue(value.matches));
    if (value.activeMatch) {
        fields.emplace_back("active_match",
                            toValue(static_cast<std::uint64_t>(*value.activeMatch)));
    } else {
        fields.emplace_back("active_match", ProtocolValue::makeNull());
    }
    fields.emplace_back("error", toValue(value.error));
    fields.emplace_back("message", toValue(value.message));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FindReplaceViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto generation = requireField<std::uint64_t>(value.field("generation"));
    auto open = requireField<bool>(value.field("open"));
    auto replaceMode = requireField<bool>(value.field("replace_mode"));
    auto sourceRevision = requireField<Revision>(value.field("source_revision"));
    auto query = requireField<std::string>(value.field("query"));
    auto replacement = requireField<std::string>(value.field("replacement"));
    auto options = requireField<FindOptions>(value.field("options"));
    auto matches = requireField<std::vector<FindMatch>>(value.field("matches"));
    auto error = requireField<FindReplaceError>(value.field("error"));
    auto message = requireField<std::string>(value.field("message"));
    if (!generation || !open || !replaceMode || !sourceRevision || !query ||
        !replacement || !options || !matches || !error || !message) {
        return false;
    }
    FindReplaceViewState result;
    result.generation = *generation;
    result.open = *open;
    result.replaceMode = *replaceMode;
    result.sourceRevision = *sourceRevision;
    result.query = *query;
    result.replacement = *replacement;
    result.options = *options;
    result.matches = *matches;
    std::optional<std::uint64_t> activeMatch;
    if (!decodeOptionalField(value.field("active_match"), activeMatch)) {
        return false;
    }
    if (activeMatch) result.activeMatch = static_cast<std::size_t>(*activeMatch);
    result.error = *error;
    result.message = *message;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(FindReplaceDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("base_generation", toValue(value.baseGeneration));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FindReplaceDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    auto baseGeneration = requireField<std::uint64_t>(value.field("base_generation"));
    if (!changed || !baseGeneration) return false;
    FindReplaceDelta result;
    result.changed = *changed;
    result.baseGeneration = *baseGeneration;
    if (!decodeOptionalField(value.field("replacement"), result.replacement)) {
        return false;
    }
    out.emplace(std::move(result));
    return true;
}


ProtocolValue toValue(SettingValue const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("index", toValue(static_cast<std::uint64_t>(value.index())));
    std::visit(
        [&fields](auto const& alt) { fields.emplace_back("value", toValue(alt)); },
        value);
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SettingValue>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto index = requireField<std::uint64_t>(value.field("index"));
    if (!index) return false;
    auto const* altValue = value.field("value");
    if (!altValue) return false;
    switch (*index) {
        case 0: {
            auto decoded = requireField<bool>(altValue);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<0>, *decoded});
            return true;
        }
        case 1: {
            auto decoded = requireField<std::uint32_t>(altValue);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<1>, *decoded});
            return true;
        }
        case 2: {
            auto decoded = requireField<std::uint64_t>(altValue);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<2>, *decoded});
            return true;
        }
        case 3: {
            auto decoded = requireField<IndentStyle>(altValue);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<3>, *decoded});
            return true;
        }
        case 4: {
            auto decoded = requireField<LineEnding>(altValue);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<4>, *decoded});
            return true;
        }
        case 5: {
            auto decoded = requireField<TextEncoding>(altValue);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<5>, *decoded});
            return true;
        }
        case 6: {
            auto decoded = requireField<std::string>(altValue);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<6>, *decoded});
            return true;
        }
        default:
            return false;
    }
}

ProtocolValue toValue(EffectiveSetting const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("value", toValue(value.value));
    fields.emplace_back("source", toValue(value.source));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<EffectiveSetting>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto settingValue = requireField<SettingValue>(value.field("value"));
    auto source = requireField<SettingScope>(value.field("source"));
    if (!settingValue || !source) return false;
    out.emplace(EffectiveSetting{*settingValue, *source});
    return true;
}

ProtocolValue toValue(SettingViewEntry const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("key", toValue(value.key));
    fields.emplace_back("effective", toValue(value.effective));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SettingViewEntry>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto key = requireField<SettingKey>(value.field("key"));
    auto effective = requireField<EffectiveSetting>(value.field("effective"));
    if (!key || !effective) return false;
    out.emplace(SettingViewEntry{*key, *effective});
    return true;
}

ProtocolValue toValue(SettingsViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("entries", toValue(value.entries));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SettingsViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto entries =
        requireField<std::array<SettingViewEntry, kSettingKeyCount>>(
            value.field("entries"));
    if (!entries) return false;
    SettingsViewState result;
    result.entries = *entries;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(SettingsDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("key", toValue(value.key));
    fields.emplace_back("before", toValue(value.before));
    fields.emplace_back("after", toValue(value.after));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SettingsDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto key = requireField<SettingKey>(value.field("key"));
    auto before = requireField<EffectiveSetting>(value.field("before"));
    auto after = requireField<EffectiveSetting>(value.field("after"));
    if (!key || !before || !after) return false;
    out.emplace(SettingsDelta{*key, *before, *after});
    return true;
}

ProtocolValue toValue(SettingsSectionDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changes", toValue(value.changes));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SettingsSectionDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changes = requireField<std::vector<SettingsDelta>>(value.field("changes"));
    if (!changes) return false;
    out.emplace(SettingsSectionDelta{*changes});
    return true;
}


ProtocolValue toValue(KeyStroke const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("code", toValue(std::string{keyCodeName(value.code)}));
    fields.emplace_back("control", toValue(value.control));
    fields.emplace_back("alt", toValue(value.alt));
    fields.emplace_back("meta", toValue(value.meta));
    fields.emplace_back("shift", toValue(value.shift));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<KeyStroke>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto code = requireField<std::string>(value.field("code"));
    auto control = requireField<bool>(value.field("control"));
    auto alt = requireField<bool>(value.field("alt"));
    auto meta = requireField<bool>(value.field("meta"));
    auto shift = requireField<bool>(value.field("shift"));
    if (!code || !control || !alt || !meta || !shift) return false;
    // The wire carries the key's NAME; a name outside the decoder's key set
    // names no key, so the stroke is undecodable rather than silently dead.
    auto const key = keyCodeFromName(*code);
    if (key == KeyCode::None) return false;
    out.emplace(KeyStroke{key, *control, *alt, *meta, *shift});
    return true;
}

ProtocolValue toValue(KeyBinding const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("sequence", toValue(value.sequence));
    fields.emplace_back("command_id", toValue(value.commandId));
    fields.emplace_back("context", toValue(value.context));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<KeyBinding>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto sequence = requireField<std::vector<KeyStroke>>(value.field("sequence"));
    auto commandId = requireField<std::string>(value.field("command_id"));
    auto context = requireField<std::string>(value.field("context"));
    if (!sequence || !commandId || !context) return false;
    out.emplace(KeyBinding{*sequence, *commandId, *context});
    return true;
}

ProtocolValue toValue(KeymapViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("name", toValue(value.name));
    fields.emplace_back("bindings", toValue(value.bindings));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<KeymapViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto name = requireField<std::string>(value.field("name"));
    auto bindings = requireField<std::vector<KeyBinding>>(value.field("bindings"));
    if (!name || !bindings) return false;
    out.emplace(KeymapViewState{*name, *bindings});
    return true;
}

ProtocolValue toValue(KeymapDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<KeymapDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<KeymapViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(KeymapDelta{*changed, std::move(replacement)});
    return true;
}


ProtocolValue toValue(TextEncodingStatus const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("encoding", toValue(value.encoding));
    fields.emplace_back("line_ending", toValue(value.lineEnding));
    fields.emplace_back("had_bom", toValue(value.hadBom));
    fields.emplace_back("final_newline", toValue(value.finalNewline));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TextEncodingStatus>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto encoding = requireField<TextEncoding>(value.field("encoding"));
    auto lineEnding = requireField<LineEnding>(value.field("line_ending"));
    auto hadBom = requireField<bool>(value.field("had_bom"));
    auto finalNewline = requireField<bool>(value.field("final_newline"));
    if (!encoding || !lineEnding || !hadBom || !finalNewline) return false;
    out.emplace(TextEncodingStatus{*encoding, *lineEnding, *hadBom, *finalNewline});
    return true;
}

ProtocolValue toValue(TextEncodingViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("status", toValue(value.status));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TextEncodingViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto status = requireField<TextEncodingStatus>(value.field("status"));
    if (!status) return false;
    out.emplace(TextEncodingViewState{*status});
    return true;
}

ProtocolValue toValue(TextEncodingDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("before", toValue(value.before));
    fields.emplace_back("after", toValue(value.after));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TextEncodingDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto before = requireField<TextEncodingViewState>(value.field("before"));
    auto after = requireField<TextEncodingViewState>(value.field("after"));
    if (!before || !after) return false;
    out.emplace(TextEncodingDelta{*before, *after});
    return true;
}


ProtocolValue toValue(TabState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("document", toValue(value.document));
    fields.emplace_back("document_key", toValue(value.documentKey));
    fields.emplace_back("content_identity", toValue(value.contentIdentity));
    fields.emplace_back("label", toValue(value.label));
    fields.emplace_back("mode", toValue(value.mode));
    fields.emplace_back("dirty", toValue(value.dirty));
    fields.emplace_back("recovery", toValue(value.recovery));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TabState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<TabId>(value.field("id"));
    auto kind = requireField<TabKind>(value.field("kind"));
    auto contentIdentity = requireField<std::string>(value.field("content_identity"));
    auto label = requireField<std::string>(value.field("label"));
    auto mode = requireField<DocumentMode>(value.field("mode"));
    auto dirty = requireField<bool>(value.field("dirty"));
    auto recovery = requireField<TabRecoveryBadge>(value.field("recovery"));
    if (!id || !kind || !contentIdentity || !label || !mode || !dirty || !recovery) {
        return false;
    }
    TabState result;
    result.id = *id;
    result.kind = *kind;
    if (!decodeOptionalField(value.field("document"), result.document)) return false;
    if (!decodeOptionalField(value.field("document_key"), result.documentKey)) {
        return false;
    }
    result.contentIdentity = *contentIdentity;
    result.label = *label;
    result.mode = *mode;
    result.dirty = *dirty;
    result.recovery = *recovery;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(TabViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("tabs", toValue(value.tabs));
    fields.emplace_back("active", toValue(value.active));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TabViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto tabs = requireField<std::vector<TabState>>(value.field("tabs"));
    if (!tabs) return false;
    TabViewState result;
    result.tabs = *tabs;
    if (!decodeOptionalField(value.field("active"), result.active)) return false;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(TabDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("state", toValue(value.state));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TabDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    TabDelta result;
    if (!decodeOptionalField(value.field("state"), result.state)) return false;
    out.emplace(std::move(result));
    return true;
}


ProtocolValue toValue(DiffWordRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back(
        "byte_start", toValue(static_cast<std::uint64_t>(value.byteStart)));
    fields.emplace_back(
        "byte_length", toValue(static_cast<std::uint64_t>(value.byteLength)));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value,
                   std::optional<DiffWordRange>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto byteStart = requireField<std::uint64_t>(value.field("byte_start"));
    auto byteLength = requireField<std::uint64_t>(value.field("byte_length"));
    if (!byteStart || !byteLength) return false;
    out.emplace(DiffWordRange{static_cast<std::size_t>(*byteStart),
                              static_cast<std::size_t>(*byteLength)});
    return true;
}

ProtocolValue toValue(DiffLineChange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", toValue(value.kind));
    if (value.baselineLine) {
        fields.emplace_back("baseline_line",
                            toValue(static_cast<std::uint64_t>(*value.baselineLine)));
    } else {
        fields.emplace_back("baseline_line", ProtocolValue::makeNull());
    }
    if (value.targetLine) {
        fields.emplace_back("target_line",
                            toValue(static_cast<std::uint64_t>(*value.targetLine)));
    } else {
        fields.emplace_back("target_line", ProtocolValue::makeNull());
    }
    fields.emplace_back("target_added_word_ranges",
                        toValue(value.targetAddedWordRanges));
    fields.emplace_back("baseline_removed_word_ranges",
                        toValue(value.baselineRemovedWordRanges));
    fields.emplace_back("target_modified_word_ranges",
                        toValue(value.targetModifiedWordRanges));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DiffLineChange>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto kind = requireField<DiffLineKind>(value.field("kind"));
    if (!kind) return false;
    DiffLineChange result;
    result.kind = *kind;
    std::optional<std::uint64_t> baselineLine;
    if (!decodeOptionalField(value.field("baseline_line"), baselineLine)) return false;
    if (baselineLine) result.baselineLine = static_cast<std::size_t>(*baselineLine);
    std::optional<std::uint64_t> targetLine;
    if (!decodeOptionalField(value.field("target_line"), targetLine)) return false;
    if (targetLine) result.targetLine = static_cast<std::size_t>(*targetLine);
    if (auto const* field = value.field("target_added_word_ranges")) {
        auto ranges = requireField<std::vector<DiffWordRange>>(field);
        if (!ranges) return false;
        result.targetAddedWordRanges = std::move(*ranges);
    }
    if (auto const* field = value.field("baseline_removed_word_ranges")) {
        auto ranges = requireField<std::vector<DiffWordRange>>(field);
        if (!ranges) return false;
        result.baselineRemovedWordRanges = std::move(*ranges);
    }
    if (auto const* field = value.field("target_modified_word_ranges")) {
        auto ranges = requireField<std::vector<DiffWordRange>>(field);
        if (!ranges) return false;
        result.targetModifiedWordRanges = std::move(*ranges);
    }
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(DiffHunk const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("baseline_start",
                        toValue(static_cast<std::uint64_t>(value.baselineStart)));
    fields.emplace_back("target_start",
                        toValue(static_cast<std::uint64_t>(value.targetStart)));
    fields.emplace_back("baseline_lines", toValue(value.baselineLines));
    fields.emplace_back("target_lines", toValue(value.targetLines));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DiffHunk>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto baselineStart = requireField<std::uint64_t>(value.field("baseline_start"));
    auto targetStart = requireField<std::uint64_t>(value.field("target_start"));
    auto baselineLines = requireField<std::vector<std::string>>(value.field("baseline_lines"));
    auto targetLines = requireField<std::vector<std::string>>(value.field("target_lines"));
    if (!baselineStart || !targetStart || !baselineLines || !targetLines) {
        return false;
    }
    DiffHunk result;
    result.baselineStart = static_cast<std::size_t>(*baselineStart);
    result.targetStart = static_cast<std::size_t>(*targetStart);
    result.baselineLines = *baselineLines;
    result.targetLines = *targetLines;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(DiffFileView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("path", toValue(value.path));
    fields.emplace_back("previous_path", toValue(value.previousPath));
    fields.emplace_back("deleted", toValue(value.deleted));
    fields.emplace_back("status", toValue(value.status));
    fields.emplace_back("baseline_identity", toValue(value.baselineIdentity));
    fields.emplace_back("current_content", toValue(value.currentContent));
    fields.emplace_back("hunks", toValue(value.hunks));
    fields.emplace_back("changed_lines", toValue(value.changedLines));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DiffFileView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<DiffFileId>(value.field("id"));
    auto path = requireField<std::filesystem::path>(value.field("path"));
    auto deleted = requireField<bool>(value.field("deleted"));
    auto status = requireField<DiffFileStatus>(value.field("status"));
    auto baselineIdentity = requireField<std::string>(value.field("baseline_identity"));
    auto currentContent = requireField<std::string>(value.field("current_content"));
    auto hunks = requireField<std::vector<DiffHunk>>(value.field("hunks"));
    auto changedLines = requireField<std::vector<DiffLineChange>>(value.field("changed_lines"));
    if (!id || !path || !deleted || !baselineIdentity || !currentContent || !hunks ||
        !changedLines) {
        return false;
    }
    std::optional<std::filesystem::path> previousPath;
    if (!decodeOptionalField(value.field("previous_path"), previousPath)) {
        return false;
    }
    auto const inferLegacyStatus = [&]() {
        if (*deleted) {
            return DiffFileStatus::Deleted;
        }
        bool baselineLooksAbsent = true;
        std::string addedOnlyReconstruction;
        for (const auto& hunk : *hunks) {
            if (!hunk.baselineLines.empty()) {
                baselineLooksAbsent = false;
                break;
            }
            for (const auto& line : hunk.targetLines) {
                addedOnlyReconstruction += line;
            }
        }
        if (baselineLooksAbsent) {
            for (const auto& change : *changedLines) {
                if (change.kind != DiffLineKind::Added ||
                    change.baselineLine.has_value()) {
                    baselineLooksAbsent = false;
                    break;
                }
            }
        }
        if (baselineLooksAbsent && addedOnlyReconstruction == *currentContent) {
            return DiffFileStatus::Added;
        }
        if (previousPath.has_value()) {
            return DiffFileStatus::Renamed;
        }
        return DiffFileStatus::Modified;
    };
    const auto decodedStatus = status.value_or(inferLegacyStatus());
    out.emplace(DiffFileView{.id = *id,
                             .path = *path,
                             .previousPath = std::move(previousPath),
                             .deleted = *deleted,
                             .status = decodedStatus,
                             .baselineIdentity = *baselineIdentity,
                             .currentContent = *currentContent,
                             .hunks = *hunks,
                             .changedLines = *changedLines});
    return true;
}

ProtocolValue toValue(DiffViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("files", toValue(value.files));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DiffViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto files = requireField<std::vector<DiffFileView>>(value.field("files"));
    if (!revision || !files) return false;
    out.emplace(DiffViewState{*revision, *files});
    return true;
}

ProtocolValue toValue(DiffDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(value.baseRevision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("upserted", toValue(value.upserted));
    fields.emplace_back("removed", toValue(value.removed));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DiffDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto baseRevision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    auto upserted = requireField<std::vector<DiffFileView>>(value.field("upserted"));
    auto removed = requireField<std::vector<DiffFileId>>(value.field("removed"));
    if (!baseRevision || !revision || !upserted || !removed) return false;
    out.emplace(DiffDelta{*baseRevision, *revision, *upserted, *removed});
    return true;
}


ProtocolValue toValue(ExternalActionAffordance const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("action", toValue(value.action));
    fields.emplace_back("label", toValue(value.label));
    fields.emplace_back("command", toValue(value.command));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value,
                   std::optional<ExternalActionAffordance>& out) {
    if (!value.asObject()) return false;
    auto action = requireField<ExternalAction>(value.field("action"));
    auto label = requireField<std::string>(value.field("label"));
    auto command = requireField<std::string>(value.field("command"));
    if (!action || !label || !command) return false;
    auto const expected = externalActionAffordance(*action);
    if (*label != expected.label || *command != expected.command) return false;
    out.emplace(std::move(expected));
    return true;
}

ProtocolValue toValue(ExternalDocumentView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("path", toValue(value.path));
    fields.emplace_back("status", toValue(value.status));
    fields.emplace_back("accessible_status", toValue(value.accessibleStatus));
    fields.emplace_back("status_label", toValue(value.statusLabel));
    fields.emplace_back("actions", toValue(value.actions));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ExternalDocumentView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<DiffFileId>(value.field("id"));
    auto path = requireField<std::filesystem::path>(value.field("path"));
    auto status = requireField<ExternalDocumentStatus>(value.field("status"));
    auto accessibleStatus = requireField<std::string>(value.field("accessible_status"));
    auto statusLabel = requireField<std::string>(value.field("status_label"));
    auto actions = requireField<std::vector<ExternalActionAffordance>>(
        value.field("actions"));
    if (!id || !path || !status || !accessibleStatus || !statusLabel ||
        !actions) return false;
    out.emplace(ExternalDocumentView{*id, *path, *status, *accessibleStatus,
                                     *statusLabel, *actions});
    return true;
}

ProtocolValue toValue(ExternalModificationViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("message", toValue(value.message));
    fields.emplace_back("files", toValue(value.files));
    fields.emplace_back("selected", toValue(value.selected));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ExternalModificationViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto message = requireField<std::string>(value.field("message"));
    auto files = requireField<std::vector<ExternalDocumentView>>(value.field("files"));
    if (!revision || !message || !files) return false;
    std::optional<DiffFileId> selected;
    if (!decodeOptionalField(value.field("selected"), selected)) return false;
    // A present selection MUST name a file in this view -- a dangling selection is
    // rejected loud, never silently carried.
    if (selected.has_value() &&
        std::none_of(files->begin(), files->end(),
                     [&](auto const& file) { return file.id == *selected; })) {
        return false;
    }
    out.emplace(ExternalModificationViewState{*revision, *message, *files,
                                               selected});
    return true;
}

ProtocolValue toValue(ExternalModificationDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(value.baseRevision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("message", toValue(value.message));
    fields.emplace_back("upserted", toValue(value.upserted));
    fields.emplace_back("removed", toValue(value.removed));
    fields.emplace_back("selected", toValue(value.selected));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ExternalModificationDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto baseRevision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    auto message = requireField<std::string>(value.field("message"));
    auto upserted = requireField<std::vector<ExternalDocumentView>>(value.field("upserted"));
    auto removed = requireField<std::vector<DiffFileId>>(value.field("removed"));
    if (!baseRevision || !revision || !message || !upserted || !removed)
        return false;
    std::optional<DiffFileId> selected;
    if (!decodeOptionalField(value.field("selected"), selected)) return false;
    out.emplace(ExternalModificationDelta{*baseRevision, *revision, *message,
                                           *upserted, *removed, selected});
    return true;
}


ProtocolValue toValue(ViewportDimensions const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("columns", toValue(value.columns));
    fields.emplace_back("rows", toValue(value.rows));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ViewportDimensions>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto columns = requireField<std::uint32_t>(value.field("columns"));
    auto rows = requireField<std::uint32_t>(value.field("rows"));
    if (!columns || !rows) return false;
    out.emplace(*columns, *rows);
    return true;
}

ProtocolValue toValue(VisualRow const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("logical_line", toValue(value.logicalLine));
    fields.emplace_back("first_span", toValue(value.firstSpan));
    fields.emplace_back("span_count", toValue(value.spanCount));
    fields.emplace_back("start_cell", toValue(value.startCell));
    fields.emplace_back("content_cells", toValue(value.contentCells));
    fields.emplace_back("visible_cells", toValue(value.visibleCells));
    fields.emplace_back("end_byte_offset", toValue(value.endByteOffset));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<VisualRow>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto logicalLine = requireField<std::uint32_t>(value.field("logical_line"));
    auto firstSpan = requireField<std::uint32_t>(value.field("first_span"));
    auto spanCount = requireField<std::uint32_t>(value.field("span_count"));
    auto startCell = requireField<CellIndex>(value.field("start_cell"));
    auto contentCells = requireField<std::uint32_t>(value.field("content_cells"));
    auto visibleCells = requireField<std::uint32_t>(value.field("visible_cells"));
    auto endByteOffset = requireField<std::uint32_t>(value.field("end_byte_offset"));
    if (!logicalLine || !firstSpan || !spanCount || !startCell || !contentCells ||
        !visibleCells || !endByteOffset) {
        return false;
    }
    out.emplace(VisualRow{*logicalLine, *firstSpan, *spanCount, *startCell,
                          *contentCells, *visibleCells, *endByteOffset});
    return true;
}

ProtocolValue toValue(ProjectedRow const& value) {
    std::vector<ProtocolValue::Field> fields;
    if (const auto* real = std::get_if<RealRow>(&value)) {
        fields.emplace_back("kind", toValue(std::string{"real"}));
        fields.emplace_back("buffer_line", toValue(real->bufferLine));
        fields.emplace_back("buffer_visual_row",
                            toValue(real->bufferVisualRow));
        fields.emplace_back("start_byte_offset",
                            toValue(real->startByteOffset));
        fields.emplace_back("end_byte_offset", toValue(real->endByteOffset));
        fields.emplace_back("start_cell", toValue(real->startCell));
        fields.emplace_back("end_cell", toValue(real->endCell));
    } else {
        const auto& phantom = std::get<PhantomRow>(value);
        fields.emplace_back("kind", toValue(std::string{"phantom"}));
        fields.emplace_back("baseline_line", toValue(phantom.baselineLine));
        fields.emplace_back("text", toValue(phantom.text));
        fields.emplace_back("following_byte_offset",
                            toValue(phantom.followingByteOffset));
        fields.emplace_back("removed_word_ranges",
                            toValue(phantom.removedWordRanges));
    }
    return ProtocolValue::makeObject(std::move(fields));
}

bool decodePresent(ProtocolValue const& value,
                   std::optional<ProjectedRow>& out) {
    auto kind = requireField<std::string>(value.field("kind"));
    if (!kind) return false;
    if (*kind == "real") {
        auto bufferLine =
            requireField<std::uint32_t>(value.field("buffer_line"));
        auto bufferVisualRow =
            requireField<std::uint32_t>(value.field("buffer_visual_row"));
        auto startByteOffset =
            requireField<std::uint32_t>(value.field("start_byte_offset"));
        auto endByteOffset =
            requireField<std::uint32_t>(value.field("end_byte_offset"));
        auto startCell =
            requireField<std::uint32_t>(value.field("start_cell"));
        auto endCell =
            requireField<std::uint32_t>(value.field("end_cell"));
        if (!bufferLine || !bufferVisualRow || !startByteOffset ||
            !endByteOffset || !startCell || !endCell) {
            return false;
        }
        out.emplace(RealRow{
            *bufferLine, *bufferVisualRow, *startByteOffset, *endByteOffset,
            *startCell, *endCell});
        return true;
    }
    if (*kind == "phantom") {
        auto baselineLine =
            requireField<std::uint32_t>(value.field("baseline_line"));
        auto text = requireField<std::string>(value.field("text"));
        auto followingByteOffset = requireField<std::uint32_t>(
            value.field("following_byte_offset"));
        auto removedWordRanges = requireField<std::vector<DiffWordRange>>(
            value.field("removed_word_ranges"));
        if (!baselineLine || !text || !followingByteOffset ||
            !removedWordRanges) {
            return false;
        }
        out.emplace(PhantomRow{
            *baselineLine, std::move(*text), *followingByteOffset,
            std::move(*removedWordRanges)});
        return true;
    }
    return false;
}

ProtocolValue toValue(CellHitTarget const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("viewport_row", toValue(value.viewportRow));
    fields.emplace_back("viewport_column", toValue(value.viewportColumn));
    fields.emplace_back("logical_line", toValue(value.logicalLine));
    fields.emplace_back("cell", toValue(value.cell));
    fields.emplace_back("byte_offset", toValue(value.byteOffset));
    fields.emplace_back("byte_len", toValue(value.byteLen));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<CellHitTarget>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto viewportRow = requireField<std::uint32_t>(value.field("viewport_row"));
    auto viewportColumn = requireField<std::uint32_t>(value.field("viewport_column"));
    auto logicalLine = requireField<std::uint32_t>(value.field("logical_line"));
    auto cell = requireField<CellIndex>(value.field("cell"));
    auto byteOffset = requireField<std::uint32_t>(value.field("byte_offset"));
    auto byteLen = requireField<std::uint32_t>(value.field("byte_len"));
    if (!viewportRow || !viewportColumn || !logicalLine || !cell || !byteOffset ||
        !byteLen) {
        return false;
    }
    out.emplace(CellHitTarget{*viewportRow, *viewportColumn, *logicalLine, *cell,
                              *byteOffset, *byteLen});
    return true;
}

ProtocolValue toValue(ScrollbarMetrics const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("total_rows", toValue(value.totalRows));
    fields.emplace_back("viewport_rows", toValue(value.viewportRows));
    fields.emplace_back("first_row", toValue(value.firstRow));
    fields.emplace_back("maximum_first_row", toValue(value.maximumFirstRow));
    fields.emplace_back("thumb_start", toValue(value.thumbStart));
    fields.emplace_back("thumb_size", toValue(value.thumbSize));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ScrollbarMetrics>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto totalRows = requireField<std::uint32_t>(value.field("total_rows"));
    auto viewportRows = requireField<std::uint32_t>(value.field("viewport_rows"));
    auto firstRow = requireField<std::uint32_t>(value.field("first_row"));
    auto maximumFirstRow = requireField<std::uint32_t>(value.field("maximum_first_row"));
    auto thumbStart = requireField<std::uint32_t>(value.field("thumb_start"));
    auto thumbSize = requireField<std::uint32_t>(value.field("thumb_size"));
    if (!totalRows || !viewportRows || !firstRow || !maximumFirstRow || !thumbStart ||
        !thumbSize) {
        return false;
    }
    out.emplace(ScrollbarMetrics{*totalRows, *viewportRows, *firstRow,
                                 *maximumFirstRow, *thumbStart, *thumbSize});
    return true;
}

ProtocolValue toValue(ViewportViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("dimensions", toValue(value.dimensions));
    fields.emplace_back("first_visual_row", toValue(value.firstVisualRow));
    fields.emplace_back("first_visual_column", toValue(value.firstVisualColumn));
    fields.emplace_back("total_visual_rows", toValue(value.totalVisualRows));
    fields.emplace_back("visible_rows", toValue(value.visibleRows));
    fields.emplace_back("row_projection", toValue(value.rowProjection));
    fields.emplace_back("hit_targets", toValue(value.hitTargets));
    fields.emplace_back("scrollbar", toValue(value.scrollbar));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ViewportViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto dimensions = requireField<ViewportDimensions>(value.field("dimensions"));
    auto firstVisualRow = requireField<std::uint32_t>(value.field("first_visual_row"));
    auto firstVisualColumn = requireField<std::uint32_t>(value.field("first_visual_column"));
    auto totalVisualRows = requireField<std::uint32_t>(value.field("total_visual_rows"));
    auto visibleRows = requireField<std::vector<VisualRow>>(value.field("visible_rows"));
    auto rowProjection =
        requireField<std::vector<ProjectedRow>>(value.field("row_projection"));
    auto hitTargets = requireField<std::vector<CellHitTarget>>(value.field("hit_targets"));
    auto scrollbar = requireField<ScrollbarMetrics>(value.field("scrollbar"));
    if (!dimensions || !firstVisualRow || !firstVisualColumn ||
        !totalVisualRows || !visibleRows || !rowProjection || !hitTargets ||
        !scrollbar) {
        return false;
    }
    out.emplace(ViewportViewState{*dimensions, *firstVisualRow,
                                  *firstVisualColumn, *totalVisualRows,
                                  *visibleRows, *rowProjection, *hitTargets,
                                  *scrollbar});
    return true;
}

ProtocolValue toValue(ViewportDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ViewportDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<ViewportViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(ViewportDelta{*changed, std::move(replacement)});
    return true;
}


ProtocolValue toValue(FollowScrollOffset const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("first_row", toValue(value.firstRow));
    fields.emplace_back("first_column", toValue(value.firstColumn));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FollowScrollOffset>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto firstRow = requireField<std::uint64_t>(value.field("first_row"));
    auto firstColumn = requireField<std::uint64_t>(value.field("first_column"));
    if (!firstRow || !firstColumn) return false;
    out.emplace(FollowScrollOffset{*firstRow, *firstColumn});
    return true;
}

ProtocolValue toValue(FollowTarget const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("path", toValue(value.path));
    fields.emplace_back("deleted", toValue(value.deleted));
    fields.emplace_back("newest_hunk_line",
                        toValue(static_cast<std::uint64_t>(value.newestHunkLine)));
    fields.emplace_back("source_revision", toValue(value.sourceRevision));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FollowTarget>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<DiffFileId>(value.field("id"));
    auto path = requireField<std::filesystem::path>(value.field("path"));
    auto deleted = requireField<bool>(value.field("deleted"));
    auto newestHunkLine = requireField<std::uint64_t>(value.field("newest_hunk_line"));
    auto sourceRevision = requireField<Revision>(value.field("source_revision"));
    if (!id || !path || !deleted || !newestHunkLine || !sourceRevision) return false;
    out.emplace(FollowTarget{*id, *path, *deleted,
                             static_cast<std::size_t>(*newestHunkLine),
                             *sourceRevision});
    return true;
}

ProtocolValue toValue(FollowClientView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("client", toValue(value.client));
    fields.emplace_back("dimensions", toValue(value.dimensions));
    fields.emplace_back("offset", toValue(value.offset));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FollowClientView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto client = requireField<ClientId>(value.field("client"));
    auto dimensions = requireField<ViewportDimensions>(value.field("dimensions"));
    auto offset = requireField<FollowScrollOffset>(value.field("offset"));
    if (!client || !dimensions || !offset) return false;
    out.emplace(FollowClientView{*client, *dimensions, *offset});
    return true;
}

ProtocolValue toValue(ResolvedSelectionRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("anchor", toValue(value.anchor));
    fields.emplace_back("active", toValue(value.active));
    return ProtocolValue::makeObject(std::move(fields));
}

bool decodePresent(ProtocolValue const& value,
                   std::optional<ResolvedSelectionRange>& out) {
    const auto* object = value.asObject();
    if (object == nullptr || object->size() != 2) return false;
    auto anchor = requireField<ByteOffset>(value.field("anchor"));
    auto active = requireField<ByteOffset>(value.field("active"));
    if (!anchor || !active) return false;
    out.emplace(ResolvedSelectionRange{*anchor, *active});
    return true;
}

ProtocolValue toValue(FollowEditsViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("generation", toValue(value.generation));
    fields.emplace_back("mode", toValue(value.mode));
    fields.emplace_back("active_pane", toValue(value.activePane));
    fields.emplace_back("active_target", toValue(value.activeTarget));
    fields.emplace_back("queued_targets", toValue(value.queuedTargets));
    fields.emplace_back("clients", toValue(value.clients));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FollowEditsViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto generation = requireField<std::uint64_t>(value.field("generation"));
    auto mode = requireField<FollowMode>(value.field("mode"));
    auto activePane = requireField<PaneId>(value.field("active_pane"));
    auto queuedTargets = requireField<std::vector<FollowTarget>>(value.field("queued_targets"));
    auto clients = requireField<std::vector<FollowClientView>>(value.field("clients"));
    if (!generation || !mode || !activePane || !queuedTargets || !clients) return false;
    FollowEditsViewState result;
    result.generation = *generation;
    result.mode = *mode;
    result.activePane = *activePane;
    if (!decodeOptionalField(value.field("active_target"), result.activeTarget)) {
        return false;
    }
    result.queuedTargets = *queuedTargets;
    result.clients = *clients;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(FollowEditsDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_generation", toValue(value.baseGeneration));
    fields.emplace_back("generation", toValue(value.generation));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FollowEditsDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto baseGeneration = requireField<std::uint64_t>(value.field("base_generation"));
    auto generation = requireField<std::uint64_t>(value.field("generation"));
    if (!baseGeneration || !generation) return false;
    FollowEditsDelta result;
    result.baseGeneration = *baseGeneration;
    result.generation = *generation;
    if (!decodeOptionalField(value.field("replacement"), result.replacement)) {
        return false;
    }
    out.emplace(std::move(result));
    return true;
}


ProtocolValue toValue(TreeNodeCommand const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("label", toValue(value.label));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeNodeCommand>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto label = requireField<std::string>(value.field("label"));
    if (!id || !label) return false;
    out.emplace(TreeNodeCommand{*id, *label});
    return true;
}

ProtocolValue toValue(GitTreeAffordance const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("status", toValue(value.status));
    fields.emplace_back("short_label", toValue(value.shortLabel));
    fields.emplace_back("role", toValue(value.role));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value,
                   std::optional<GitTreeAffordance>& out) {
    if (!value.asObject()) return false;
    auto status = requireField<GitTreeStatus>(value.field("status"));
    auto shortLabel = requireField<std::string>(value.field("short_label"));
    auto role = requireField<SemanticRole>(value.field("role"));
    if (!status || !shortLabel || !role) return false;
    auto const expected = gitTreeAffordance(*status);
    if (*shortLabel != expected.shortLabel || *role != expected.role) {
        return false;
    }
    out.emplace(std::move(expected));
    return true;
}

ProtocolValue toValue(TreeNode const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("parent_id", toValue(value.parentId));
    fields.emplace_back("label", toValue(value.label));
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("icon", toValue(value.icon));
    fields.emplace_back("commands", toValue(value.commands));
    fields.emplace_back("git_status", toValue(value.gitStatus));
    fields.emplace_back("workspace_path", toValue(value.workspacePath));
    if (value.sourceLine) {
        fields.emplace_back("source_line", toValue(*value.sourceLine));
    } else {
        fields.emplace_back("source_line", ProtocolValue::makeNull());
    }
    fields.emplace_back("expandable", toValue(value.expandable));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeNode>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<TreeNodeId>(value.field("id"));
    auto label = requireField<std::string>(value.field("label"));
    auto kind = requireField<TreeNodeKind>(value.field("kind"));
    auto commands = requireField<std::vector<TreeNodeCommand>>(value.field("commands"));
    auto expandable = requireField<bool>(value.field("expandable"));
    if (!id || !label || !kind || !commands || !expandable) return false;
    std::optional<TreeNodeId> parentId;
    if (!decodeOptionalField(value.field("parent_id"), parentId)) return false;
    std::optional<std::string> icon;
    if (!decodeOptionalField(value.field("icon"), icon)) return false;
    std::optional<GitTreeAffordance> gitStatus;
    if (!decodeOptionalField(value.field("git_status"), gitStatus)) return false;
    std::optional<std::string> workspacePath;
    if (!decodeOptionalField(value.field("workspace_path"), workspacePath)) return false;
    std::optional<std::uint32_t> sourceLine;
    if (!decodeOptionalField(value.field("source_line"), sourceLine)) return false;
    out.emplace(TreeNode{*id, std::move(parentId), *label, *kind, std::move(icon),
                         *commands, std::move(gitStatus), std::move(workspacePath),
                         std::move(sourceLine), *expandable});
    return true;
}

ProtocolValue toValue(TreeNodeView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("node", toValue(value.node));
    fields.emplace_back("depth", toValue(static_cast<std::uint64_t>(value.depth)));
    fields.emplace_back("expanded", toValue(value.expanded));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeNodeView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto node = requireField<TreeNode>(value.field("node"));
    auto depth = requireField<std::uint64_t>(value.field("depth"));
    auto expanded = requireField<bool>(value.field("expanded"));
    if (!node || !depth || !expanded) return false;
    out.emplace(TreeNodeView{*node, static_cast<std::size_t>(*depth), *expanded});
    return true;
}

ProtocolValue toValue(TreeProviderView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("provider_id", toValue(value.providerId));
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("nodes", toValue(value.nodes));
    fields.emplace_back("selected", toValue(value.selected));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeProviderView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto providerId = requireField<TreeProviderId>(value.field("provider_id"));
    auto kind = requireField<TreeProviderKind>(value.field("kind"));
    auto nodes = requireField<std::vector<TreeNodeView>>(value.field("nodes"));
    std::optional<TreeNodeId> selected;
    if (!providerId || !kind || !nodes) return false;
    if (!decodeOptionalField(value.field("selected"), selected)) return false;
    out.emplace(TreeProviderView{*providerId, *kind, *nodes, selected});
    return true;
}

ProtocolValue toValue(TreeWindow const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("first_visible", toValue(value.firstVisible));
    fields.emplace_back("scrollbar", toValue(value.scrollbar));
    fields.emplace_back("visible_node_ids", toValue(value.visibleNodeIds));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeWindow>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto firstVisible = requireField<std::uint32_t>(value.field("first_visible"));
    auto scrollbar = requireField<ScrollbarMetrics>(value.field("scrollbar"));
    auto visibleNodeIds =
        requireField<std::vector<TreeNodeId>>(value.field("visible_node_ids"));
    if (!firstVisible || !scrollbar || !visibleNodeIds) return false;
    out.emplace(TreeWindow{*firstVisible, *scrollbar,
                           std::move(*visibleNodeIds)});
    return true;
}

ProtocolValue toValue(TreeViewState const& value) {
    if (!isValidTreeViewState(value)) {
        throw std::invalid_argument("cannot encode an invalid TreeViewState");
    }
    auto providers = value.providers;
    if (value.activeBinding) {
        const auto active = std::find_if(
            providers.begin(), providers.end(), [&](const TreeProviderView& provider) {
                return provider.providerId == value.activeBinding->id &&
                       provider.kind == value.activeBinding->kind;
            });
        std::rotate(providers.begin(), active, std::next(active));
    }
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("providers", toValue(providers));
    fields.emplace_back("active_binding", toValue(value.activeBinding));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<TreeRevision>(value.field("revision"));
    auto providers = requireField<std::vector<TreeProviderView>>(value.field("providers"));
    if (!revision || !providers) return false;
    std::optional<TreeProviderBinding> activeBinding;
    const bool hasActiveBinding = value.field("active_binding") != nullptr;
    if (!decodeOptionalField(value.field("active_binding"), activeBinding)) return false;
    if (!hasActiveBinding && !providers->empty()) {
        activeBinding = TreeProviderBinding{providers->front().providerId,
                                            providers->front().kind};
    }
    TreeViewState decoded{*revision, std::move(*providers),
                          std::move(activeBinding)};
    if (!isValidTreeViewState(decoded)) return false;
    out.emplace(std::move(decoded));
    return true;
}

ProtocolValue toValue(TreeProviderDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("provider_id", toValue(value.providerId));
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("remove_provider", toValue(value.removeProvider));
    fields.emplace_back("start", toValue(static_cast<std::uint64_t>(value.start)));
    fields.emplace_back("erase_count", toValue(static_cast<std::uint64_t>(value.eraseCount)));
    fields.emplace_back("insert", toValue(value.insert));
    fields.emplace_back("selected", toValue(value.selected));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeProviderDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto providerId = requireField<TreeProviderId>(value.field("provider_id"));
    auto kind = requireField<TreeProviderKind>(value.field("kind"));
    auto removeProvider = requireField<bool>(value.field("remove_provider"));
    auto start = requireField<std::uint64_t>(value.field("start"));
    auto eraseCount = requireField<std::uint64_t>(value.field("erase_count"));
    auto insert = requireField<std::vector<TreeNodeView>>(value.field("insert"));
    if (!providerId || !kind || !removeProvider || !start || !eraseCount || !insert) {
        return false;
    }
    std::optional<TreeNodeId> selected;
    if (!decodeOptionalField(value.field("selected"), selected)) return false;
    out.emplace(TreeProviderDelta{*providerId, *kind, *removeProvider,
                                  static_cast<std::size_t>(*start),
                                  static_cast<std::size_t>(*eraseCount), *insert,
                                  selected});
    return true;
}

ProtocolValue toValue(TreeDelta const& value) {
    auto providerOrder = value.providerOrder;
    if (value.activeBinding) {
        const auto active =
            std::ranges::find(providerOrder, value.activeBinding->id);
        if (active == providerOrder.end()) {
            throw std::invalid_argument(
                "cannot encode a TreeDelta whose active binding is absent");
        }
        std::rotate(providerOrder.begin(), active, std::next(active));
    }
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(value.baseRevision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("snapshot_required", toValue(value.snapshotRequired));
    fields.emplace_back("providers", toValue(value.providers));
    fields.emplace_back("provider_order", toValue(providerOrder));
    if (value.activeBinding || providerOrder.empty()) {
        fields.emplace_back("active_binding", toValue(value.activeBinding));
    }
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto baseRevision = requireField<TreeRevision>(value.field("base_revision"));
    auto revision = requireField<TreeRevision>(value.field("revision"));
    auto snapshotRequired = requireField<bool>(value.field("snapshot_required"));
    auto providers = requireField<std::vector<TreeProviderDelta>>(value.field("providers"));
    auto providerOrder =
        requireField<std::vector<TreeProviderId>>(value.field("provider_order"));
    if (!baseRevision || !revision || !snapshotRequired || !providers ||
        !providerOrder) return false;
    std::optional<TreeProviderBinding> activeBinding;
    if (!decodeOptionalField(value.field("active_binding"), activeBinding)) return false;
    out.emplace(TreeDelta{*baseRevision, *revision, *snapshotRequired, *providers,
                          *providerOrder, std::move(activeBinding)});
    return true;
}


ProtocolValue toValue(SyntaxRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("begin", toValue(value.begin));
    fields.emplace_back("end", toValue(value.end));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxRange>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto begin = requireField<ByteOffset>(value.field("begin"));
    auto end = requireField<ByteOffset>(value.field("end"));
    if (!begin || !end) return false;
    out.emplace(SyntaxRange{*begin, *end});
    return true;
}

ProtocolValue toValue(SyntaxSpan const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("begin", toValue(value.begin));
    fields.emplace_back("end", toValue(value.end));
    fields.emplace_back("scope", toValue(value.scope));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxSpan>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto begin = requireField<ByteOffset>(value.field("begin"));
    auto end = requireField<ByteOffset>(value.field("end"));
    auto scope = requireField<SyntaxScope>(value.field("scope"));
    if (!begin || !end || !scope) return false;
    out.emplace(SyntaxSpan{*begin, *end, *scope});
    return true;
}

ProtocolValue toValue(SyntaxBracketPair const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("open", toValue(value.open));
    fields.emplace_back("close", toValue(value.close));
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("depth", toValue(value.depth));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxBracketPair>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto open = requireField<ByteOffset>(value.field("open"));
    auto close = requireField<ByteOffset>(value.field("close"));
    auto kind = requireField<BracketKind>(value.field("kind"));
    auto depth = requireField<std::uint32_t>(value.field("depth"));
    if (!open || !close || !kind || !depth) return false;
    out.emplace(SyntaxBracketPair{*open, *close, *kind, *depth});
    return true;
}

ProtocolValue toValue(UnmatchedBracket const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("offset", toValue(value.offset));
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("role", toValue(value.role));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<UnmatchedBracket>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto offset = requireField<ByteOffset>(value.field("offset"));
    auto kind = requireField<BracketKind>(value.field("kind"));
    auto role = requireField<BracketRole>(value.field("role"));
    if (!offset || !kind || !role) return false;
    out.emplace(UnmatchedBracket{*offset, *kind, *role});
    return true;
}

ProtocolValue toValue(CommentToken const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("range", toValue(value.range));
    fields.emplace_back("role", toValue(value.role));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<CommentToken>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto range = requireField<SyntaxRange>(value.field("range"));
    auto role = requireField<CommentTokenRole>(value.field("role"));
    if (!range || !role) return false;
    out.emplace(CommentToken{*range, *role});
    return true;
}

ProtocolValue toValue(CommentRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("range", toValue(value.range));
    fields.emplace_back("kind", toValue(value.kind));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<CommentRange>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto range = requireField<SyntaxRange>(value.field("range"));
    auto kind = requireField<CommentKind>(value.field("kind"));
    if (!range || !kind) return false;
    out.emplace(CommentRange{*range, *kind});
    return true;
}

ProtocolValue toValue(LineIndentation const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("line", toValue(value.line));
    fields.emplace_back("line_start", toValue(value.lineStart));
    fields.emplace_back("content_start", toValue(value.contentStart));
    fields.emplace_back("spaces", toValue(value.spaces));
    fields.emplace_back("tabs", toValue(value.tabs));
    fields.emplace_back("columns", toValue(value.columns));
    fields.emplace_back("blank", toValue(value.blank));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LineIndentation>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto line = requireField<LineIndex>(value.field("line"));
    auto lineStart = requireField<ByteOffset>(value.field("line_start"));
    auto contentStart = requireField<ByteOffset>(value.field("content_start"));
    auto spaces = requireField<std::uint32_t>(value.field("spaces"));
    auto tabs = requireField<std::uint32_t>(value.field("tabs"));
    auto columns = requireField<std::uint32_t>(value.field("columns"));
    auto blank = requireField<bool>(value.field("blank"));
    if (!line || !lineStart || !contentStart || !spaces || !tabs || !columns || !blank) {
        return false;
    }
    out.emplace(LineIndentation{*line, *lineStart, *contentStart, *spaces, *tabs,
                                *columns, *blank});
    return true;
}

ProtocolValue toValue(SyntaxViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision()));
    fields.emplace_back("language", toValue(value.language()));
    fields.emplace_back("text_bytes", toValue(value.textBytes()));
    fields.emplace_back("spans", toValue(value.spans()));
    fields.emplace_back("bracket_pairs", toValue(value.bracketPairs()));
    fields.emplace_back("unmatched_brackets", toValue(value.unmatchedBrackets()));
    fields.emplace_back("comment_tokens", toValue(value.commentTokens()));
    fields.emplace_back("comment_ranges", toValue(value.commentRanges()));
    fields.emplace_back("indentation", toValue(value.indentation()));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto language = requireField<LanguageId>(value.field("language"));
    auto textBytes = requireField<std::uint64_t>(value.field("text_bytes"));
    auto spans = requireField<std::vector<SyntaxSpan>>(value.field("spans"));
    auto bracketPairs = requireField<std::vector<SyntaxBracketPair>>(value.field("bracket_pairs"));
    auto unmatchedBrackets =
        requireField<std::vector<UnmatchedBracket>>(value.field("unmatched_brackets"));
    auto commentTokens = requireField<std::vector<CommentToken>>(value.field("comment_tokens"));
    auto commentRanges = requireField<std::vector<CommentRange>>(value.field("comment_ranges"));
    auto indentation = requireField<std::vector<LineIndentation>>(value.field("indentation"));
    if (!revision || !language || !textBytes || !spans || !bracketPairs ||
        !unmatchedBrackets || !commentTokens || !commentRanges || !indentation) {
        return false;
    }
    out.emplace(*revision, *language, *textBytes, *spans, *bracketPairs,
               *unmatchedBrackets, *commentTokens, *commentRanges, *indentation);
    return true;
}

ProtocolValue toValue(SyntaxDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(value.baseRevision()));
    fields.emplace_back("revision", toValue(value.revision()));
    fields.emplace_back("language", toValue(value.language()));
    fields.emplace_back("text_bytes", toValue(value.textBytes()));
    fields.emplace_back("spans", toValue(value.spans()));
    fields.emplace_back("bracket_pairs", toValue(value.bracketPairs()));
    fields.emplace_back("unmatched_brackets", toValue(value.unmatchedBrackets()));
    fields.emplace_back("comment_tokens", toValue(value.commentTokens()));
    fields.emplace_back("comment_ranges", toValue(value.commentRanges()));
    fields.emplace_back("indentation", toValue(value.indentation()));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto baseRevision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    if (!baseRevision || !revision) return false;
    std::optional<LanguageId> language;
    if (!decodeOptionalField(value.field("language"), language)) return false;
    std::optional<std::uint64_t> textBytes;
    if (!decodeOptionalField(value.field("text_bytes"), textBytes)) return false;
    std::optional<std::vector<SyntaxSpan>> spans;
    if (!decodeOptionalField(value.field("spans"), spans)) return false;
    std::optional<std::vector<SyntaxBracketPair>> bracketPairs;
    if (!decodeOptionalField(value.field("bracket_pairs"), bracketPairs)) return false;
    std::optional<std::vector<UnmatchedBracket>> unmatchedBrackets;
    if (!decodeOptionalField(value.field("unmatched_brackets"), unmatchedBrackets)) {
        return false;
    }
    std::optional<std::vector<CommentToken>> commentTokens;
    if (!decodeOptionalField(value.field("comment_tokens"), commentTokens)) return false;
    std::optional<std::vector<CommentRange>> commentRanges;
    if (!decodeOptionalField(value.field("comment_ranges"), commentRanges)) return false;
    std::optional<std::vector<LineIndentation>> indentation;
    if (!decodeOptionalField(value.field("indentation"), indentation)) return false;
    out.emplace(*baseRevision, *revision, std::move(language), std::move(textBytes),
               std::move(spans), std::move(bracketPairs), std::move(unmatchedBrackets),
               std::move(commentTokens), std::move(commentRanges), std::move(indentation));
    return true;
}


ProtocolValue toValue(LspPosition const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("line", toValue(value.line));
    fields.emplace_back("character", toValue(value.character));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspPosition>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto line = requireField<std::uint64_t>(value.field("line"));
    auto character = requireField<std::uint64_t>(value.field("character"));
    if (!line || !character) return false;
    out.emplace(LspPosition{*line, *character});
    return true;
}

ProtocolValue toValue(LspRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("start", toValue(value.start));
    fields.emplace_back("end", toValue(value.end));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspRange>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto start = requireField<LspPosition>(value.field("start"));
    auto end = requireField<LspPosition>(value.field("end"));
    if (!start || !end) return false;
    out.emplace(LspRange{*start, *end});
    return true;
}

ProtocolValue toValue(LspDiagnostic const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("range", toValue(value.range));
    fields.emplace_back("severity", toValue(value.severity));
    fields.emplace_back("code", toValue(value.code));
    fields.emplace_back("message", toValue(value.message));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspDiagnostic>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto range = requireField<LspRange>(value.field("range"));
    auto code = requireField<std::string>(value.field("code"));
    auto message = requireField<std::string>(value.field("message"));
    if (!range || !code || !message) return false;
    LspDiagnostic result;
    result.range = *range;
    if (!decodeOptionalField(value.field("severity"), result.severity)) return false;
    result.code = *code;
    result.message = *message;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(LspDocumentDiagnostics const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("uri", toValue(value.uri));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("diagnostics", toValue(value.diagnostics));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspDocumentDiagnostics>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto uri = requireField<std::string>(value.field("uri"));
    auto revision = requireField<Revision>(value.field("revision"));
    auto diagnostics = requireField<std::vector<LspDiagnostic>>(value.field("diagnostics"));
    if (!uri || !revision || !diagnostics) return false;
    out.emplace(LspDocumentDiagnostics{*uri, *revision, *diagnostics});
    return true;
}

ProtocolValue toValue(LspSyncViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("documents", toValue(value.documents));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspSyncViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto documents = requireField<std::vector<LspDocumentDiagnostics>>(value.field("documents"));
    if (!revision || !documents) return false;
    out.emplace(LspSyncViewState{*revision, *documents});
    return true;
}

ProtocolValue toValue(LspSyncDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(value.baseRevision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("state", toValue(value.state));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspSyncDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto baseRevision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    if (!baseRevision || !revision) return false;
    LspSyncDelta result;
    result.baseRevision = *baseRevision;
    result.revision = *revision;
    if (!decodeOptionalField(value.field("state"), result.state)) return false;
    out.emplace(std::move(result));
    return true;
}


ProtocolValue toValue(LspCompletionItem const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("label", toValue(value.label));
    fields.emplace_back("detail", toValue(value.detail));
    fields.emplace_back("sort_text", toValue(value.sortText));
    fields.emplace_back("insert_text", toValue(value.insertText));
    fields.emplace_back("replacement_range", toValue(value.replacementRange));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspCompletionItem>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto label = requireField<std::string>(value.field("label"));
    auto detail = requireField<std::string>(value.field("detail"));
    auto sortText = requireField<std::string>(value.field("sort_text"));
    auto insertText = requireField<std::string>(value.field("insert_text"));
    if (!label || !detail || !sortText || !insertText) return false;
    LspCompletionItem result;
    result.label = *label;
    result.detail = *detail;
    result.sortText = *sortText;
    result.insertText = *insertText;
    if (!decodeOptionalField(value.field("replacement_range"), result.replacementRange)) {
        return false;
    }
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(LspCompletionViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("visible", toValue(value.visible));
    fields.emplace_back("loading", toValue(value.loading));
    fields.emplace_back("items", toValue(value.items));
    if (value.selectedIndex) {
        fields.emplace_back("selected_index",
                            toValue(static_cast<std::uint64_t>(*value.selectedIndex)));
    } else {
        fields.emplace_back("selected_index", ProtocolValue::makeNull());
    }
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspCompletionViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto visible = requireField<bool>(value.field("visible"));
    auto loading = requireField<bool>(value.field("loading"));
    auto items = requireField<std::vector<LspCompletionItem>>(value.field("items"));
    if (!visible || !loading || !items) return false;
    LspCompletionViewState result;
    result.visible = *visible;
    result.loading = *loading;
    result.items = *items;
    std::optional<std::uint64_t> selectedIndex;
    if (!decodeOptionalField(value.field("selected_index"), selectedIndex)) return false;
    if (selectedIndex) result.selectedIndex = static_cast<std::size_t>(*selectedIndex);
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(LspHover const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("contents", toValue(value.contents));
    fields.emplace_back("range", toValue(value.range));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspHover>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto contents = requireField<std::string>(value.field("contents"));
    if (!contents) return false;
    LspHover result;
    result.contents = *contents;
    if (!decodeOptionalField(value.field("range"), result.range)) return false;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(LspNavigationTarget const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("uri", toValue(value.uri));
    fields.emplace_back("range", toValue(value.range));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspNavigationTarget>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto uri = requireField<std::string>(value.field("uri"));
    auto range = requireField<LspRange>(value.field("range"));
    if (!uri || !range) return false;
    out.emplace(LspNavigationTarget{*uri, *range});
    return true;
}

ProtocolValue toValue(LspNavigationViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("targets", toValue(value.targets));
    if (value.selectedIndex) {
        fields.emplace_back("selected_index",
                            toValue(static_cast<std::uint64_t>(*value.selectedIndex)));
    } else {
        fields.emplace_back("selected_index", ProtocolValue::makeNull());
    }
    fields.emplace_back("user_navigation", toValue(value.userNavigation));
    fields.emplace_back("reveal_primary_caret", toValue(value.revealPrimaryCaret));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspNavigationViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto targets = requireField<std::vector<LspNavigationTarget>>(value.field("targets"));
    auto userNavigation = requireField<bool>(value.field("user_navigation"));
    auto revealPrimaryCaret = requireField<bool>(value.field("reveal_primary_caret"));
    if (!targets || !userNavigation || !revealPrimaryCaret) return false;
    LspNavigationViewState result;
    result.targets = *targets;
    std::optional<std::uint64_t> selectedIndex;
    if (!decodeOptionalField(value.field("selected_index"), selectedIndex)) return false;
    if (selectedIndex) result.selectedIndex = static_cast<std::size_t>(*selectedIndex);
    result.userNavigation = *userNavigation;
    result.revealPrimaryCaret = *revealPrimaryCaret;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(LspFeatureViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("completion", toValue(value.completion));
    fields.emplace_back("hover", toValue(value.hover));
    fields.emplace_back("navigation", toValue(value.navigation));
    fields.emplace_back("status", toValue(value.status));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspFeatureViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto completion = requireField<LspCompletionViewState>(value.field("completion"));
    auto navigation = requireField<LspNavigationViewState>(value.field("navigation"));
    auto status = requireField<std::string>(value.field("status"));
    if (!revision || !completion || !navigation || !status) return false;
    LspFeatureViewState result;
    result.revision = *revision;
    result.completion = *completion;
    if (!decodeOptionalField(value.field("hover"), result.hover)) return false;
    result.navigation = *navigation;
    result.status = *status;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(LspFeatureDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(value.baseRevision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("state", toValue(value.state));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<LspFeatureDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto baseRevision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    if (!baseRevision || !revision) return false;
    LspFeatureDelta result;
    result.baseRevision = *baseRevision;
    result.revision = *revision;
    if (!decodeOptionalField(value.field("state"), result.state)) return false;
    out.emplace(std::move(result));
    return true;
}


ProtocolValue toValue(SrgbColor const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("red", toValue(value.red));
    fields.emplace_back("green", toValue(value.green));
    fields.emplace_back("blue", toValue(value.blue));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SrgbColor>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto red = requireField<std::uint8_t>(value.field("red"));
    auto green = requireField<std::uint8_t>(value.field("green"));
    auto blue = requireField<std::uint8_t>(value.field("blue"));
    if (!red || !green || !blue) return false;
    out.emplace(
        SrgbColor::fromSerializedChannels(*red, *green, *blue));
    return true;
}

ProtocolValue toValue(ThemeSnapshot const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("role_colors", toValue(value.roleColors));
    fields.emplace_back("syntax_colors", toValue(value.syntaxColors));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ThemeSnapshot>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto roleColors = requireField<std::array<SrgbColor, kSemanticRoleCount>>(
        value.field("role_colors"));
    auto syntaxColors = requireField<std::array<SrgbColor, kSyntaxScopeCount>>(
        value.field("syntax_colors"));
    if (!roleColors || !syntaxColors) {
        return false;
    }
    out.emplace(ThemeSnapshot{*roleColors, *syntaxColors});
    return true;
}

// Style's glyph tables are Style-internal structs, so their fields are encoded
// flat here rather than through per-struct codecs nothing else would use.
ProtocolValue toValue(Style const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("scrollbar_gutter", toValue(value.scrollbar.gutter));
    fields.emplace_back("scrollbar_track", toValue(value.scrollbar.track));
    fields.emplace_back("scrollbar_single", toValue(value.scrollbar.single));
    fields.emplace_back("scrollbar_top", toValue(value.scrollbar.top));
    fields.emplace_back("scrollbar_body", toValue(value.scrollbar.body));
    fields.emplace_back("scrollbar_bottom", toValue(value.scrollbar.bottom));
    fields.emplace_back("tree_expanded", toValue(value.tree.expanded));
    fields.emplace_back("tree_collapsed", toValue(value.tree.collapsed));
    fields.emplace_back("tree_indent", toValue(value.tree.indentPerDepth));
    fields.emplace_back("tab_dirty_suffix", toValue(value.tab.dirtySuffix));
    fields.emplace_back("tab_live_diff_prefix", toValue(value.tab.liveDiffPrefix));
    fields.emplace_back("tab_read_only_suffix", toValue(value.tab.readOnlySuffix));
    fields.emplace_back("tab_left_edge", toValue(value.tab.leftEdge));
    fields.emplace_back("tab_right_edge", toValue(value.tab.rightEdge));
    fields.emplace_back("tab_separator", toValue(value.tab.separator));
    fields.emplace_back("toggle_checked", toValue(value.toggle.checked));
    fields.emplace_back("toggle_unchecked", toValue(value.toggle.unchecked));
    fields.emplace_back("truncation", toValue(value.truncation));
    fields.emplace_back("input_line_sigil", toValue(value.inputLineSigil));
    fields.emplace_back("unrenderable", toValue(value.unrenderable));
    fields.emplace_back("prompt_label_separator",
                        toValue(value.promptLabelSeparator));
    fields.emplace_back("cwd_prefix", toValue(value.cwdPrefix));
    fields.emplace_back("dim_minimum_columns",
                        toValue(value.dimensions.minimumColumns));
    fields.emplace_back("dim_minimum_rows",
                        toValue(value.dimensions.minimumRows));
    fields.emplace_back("dim_panel_target_width",
                        toValue(value.dimensions.panelTargetWidth));
    fields.emplace_back("dim_panel_minimum_width",
                        toValue(value.dimensions.panelMinimumWidth));
    fields.emplace_back("dim_editor_minimum_width",
                        toValue(value.dimensions.editorMinimumWidth));
    fields.emplace_back("dim_scrollbar_gutter_width",
                        toValue(value.dimensions.scrollbarGutterWidth));
    fields.emplace_back("dim_header_height",
                        toValue(value.dimensions.headerHeight));
    fields.emplace_back("dim_tab_bar_height",
                        toValue(value.dimensions.tabBarHeight));
    fields.emplace_back("dim_footer_height",
                        toValue(value.dimensions.footerHeight));
    fields.emplace_back("dim_label_padding",
                        toValue(value.dimensions.labelPadding));
    fields.emplace_back("dim_input_line_separator",
                        toValue(value.dimensions.inputLineSeparator));
    fields.emplace_back("dim_input_line_query_budget",
                        toValue(value.dimensions.inputLineQueryBudget));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<Style>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto gutter = requireField<std::string>(value.field("scrollbar_gutter"));
    auto track = requireField<std::string>(value.field("scrollbar_track"));
    auto single = requireField<std::string>(value.field("scrollbar_single"));
    auto top = requireField<std::string>(value.field("scrollbar_top"));
    auto body = requireField<std::string>(value.field("scrollbar_body"));
    auto bottom = requireField<std::string>(value.field("scrollbar_bottom"));
    auto expanded = requireField<std::string>(value.field("tree_expanded"));
    auto collapsed = requireField<std::string>(value.field("tree_collapsed"));
    auto indent = requireField<int>(value.field("tree_indent"));
    auto dirtySuffix = requireField<std::string>(value.field("tab_dirty_suffix"));
    auto liveDiffPrefix =
        requireField<std::string>(value.field("tab_live_diff_prefix"));
    auto readOnlySuffix =
        requireField<std::string>(value.field("tab_read_only_suffix"));
    auto tabLeftEdge = requireField<std::string>(value.field("tab_left_edge"));
    auto tabRightEdge = requireField<std::string>(value.field("tab_right_edge"));
    auto tabSeparator = requireField<std::string>(value.field("tab_separator"));
    auto checked = requireField<std::string>(value.field("toggle_checked"));
    auto unchecked = requireField<std::string>(value.field("toggle_unchecked"));
    auto truncation = requireField<std::string>(value.field("truncation"));
    auto sigil = requireField<std::string>(value.field("input_line_sigil"));
    auto unrenderable = requireField<std::string>(value.field("unrenderable"));
    auto separator =
        requireField<std::string>(value.field("prompt_label_separator"));
    auto cwdPrefix = requireField<std::string>(value.field("cwd_prefix"));
    auto minCols = requireField<int>(value.field("dim_minimum_columns"));
    auto minRows = requireField<int>(value.field("dim_minimum_rows"));
    auto panelTarget = requireField<int>(value.field("dim_panel_target_width"));
    auto panelMin = requireField<int>(value.field("dim_panel_minimum_width"));
    auto editorMin = requireField<int>(value.field("dim_editor_minimum_width"));
    auto gutterWidth =
        requireField<int>(value.field("dim_scrollbar_gutter_width"));
    auto headerHeight = requireField<int>(value.field("dim_header_height"));
    auto tabBarHeight = requireField<int>(value.field("dim_tab_bar_height"));
    auto footerHeight = requireField<int>(value.field("dim_footer_height"));
    auto labelPadding = requireField<int>(value.field("dim_label_padding"));
    auto separatorWidth =
        requireField<int>(value.field("dim_input_line_separator"));
    auto queryBudget =
        requireField<int>(value.field("dim_input_line_query_budget"));
    if (!gutter || !track || !single || !top || !body || !bottom || !expanded ||
        !collapsed || !indent || !dirtySuffix || !liveDiffPrefix ||
        !readOnlySuffix || !checked ||
        !tabLeftEdge || !tabRightEdge || !tabSeparator ||
        !unchecked || !truncation || !sigil || !unrenderable || !separator ||
        !cwdPrefix ||
        !minCols || !minRows || !panelTarget || !panelMin || !editorMin ||
        !gutterWidth || !headerHeight || !tabBarHeight || !footerHeight ||
        !labelPadding || !separatorWidth || !queryBudget) {
        return false;
    }
    Style style;
    style.scrollbar = {*gutter, *track, *single, *top, *body, *bottom};
    style.tree = {*expanded, *collapsed, *indent};
    style.tab = {*dirtySuffix, *liveDiffPrefix, *readOnlySuffix,
                 *tabLeftEdge, *tabRightEdge, *tabSeparator};
    style.toggle = {*checked, *unchecked};
    style.truncation = *truncation;
    style.inputLineSigil = *sigil;
    style.unrenderable = *unrenderable;
    style.promptLabelSeparator = *separator;
    style.cwdPrefix = *cwdPrefix;
    style.dimensions = {*minCols,      *minRows,      *panelTarget,
                        *panelMin,     *editorMin,    *gutterWidth,
                        *headerHeight, *tabBarHeight, *footerHeight,
                        *labelPadding, *separatorWidth, *queryBudget};
    out.emplace(std::move(style));
    return true;
}


ProtocolValue toValue(SessionTopology const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("active_workspace", toValue(value.activeWorkspace));
    fields.emplace_back("active_view", toValue(value.activeView));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SessionTopology>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    SessionTopology result;
    if (!decodeOptionalField(value.field("active_workspace"), result.activeWorkspace)) {
        return false;
    }
    if (!decodeOptionalField(value.field("active_view"), result.activeView)) {
        return false;
    }
    out.emplace(std::move(result));
    return true;
}


ProtocolValue toValue(ClientSnapshotState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("client_id", toValue(value.clientId));
    fields.emplace_back("view_id", toValue(value.viewId));
    fields.emplace_back("capabilities", toValue(value.capabilities));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ClientSnapshotState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto clientId = requireField<ClientId>(value.field("client_id"));
    auto viewId = requireField<ViewId>(value.field("view_id"));
    auto capabilities = requireField<std::vector<CapabilityId>>(value.field("capabilities"));
    if (!clientId || !viewId || !capabilities) return false;
    out.emplace(ClientSnapshotState{*clientId, *viewId, *capabilities});
    return true;
}

ProtocolValue toValue(PresentationSnapshot const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("viewport", toValue(value.viewport));
    fields.emplace_back("style", toValue(value.style));
    fields.emplace_back("prompt", toValue(value.prompt));
    fields.emplace_back("shell", toValue(value.shell));
    fields.emplace_back("selection_nav", toValue(value.selectionNav));
    fields.emplace_back("tree_windows", toValue(value.treeWindows));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PresentationSnapshot>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto viewport = requireField<ViewportViewState>(value.field("viewport"));
    auto style = requireField<Style>(value.field("style"));
    auto shell = requireField<ShellViewState>(value.field("shell"));
    auto selectionNav = requireField<SelectionNavigation>(value.field("selection_nav"));
    auto treeWindows = requireField<std::vector<TreeWindow>>(value.field("tree_windows"));
    if (!viewport || !style || !shell || !selectionNav || !treeWindows) return false;
    std::optional<PromptViewState> prompt;
    if (!decodeOptionalField(value.field("prompt"), prompt)) return false;
    out.emplace(PresentationSnapshot{*viewport, std::move(*style),
                                     std::move(prompt), std::move(*shell),
                                     *selectionNav, std::move(*treeWindows)});
    return true;
}

ProtocolValue encodeUiFrameVersion(UiFrameVersion version) {
    return ProtocolValue::makeObject(
        {{"generation", ProtocolValue::makeUint(version.generation.value())},
         {"presence_basis",
          ProtocolValue::makeUint(version.presenceBasis.value())}});
}

std::optional<UiFrameVersion> decodeUiFrameVersion(
    const ProtocolValue& value) {
    const auto* generation = value.field("generation");
    const auto* presenceBasis = value.field("presence_basis");
    if (!value.asObject() || !generation || !generation->asUint() ||
        !presenceBasis || !presenceBasis->asUint()) {
        return std::nullopt;
    }
    return UiFrameVersion{Generation{*generation->asUint()},
                          PresenceBasis{*presenceBasis->asUint()}};
}

ProtocolValue encodeUiFrame(const UiFrame& frame) {
    return ProtocolValue::makeObject(
        {{"version", encodeUiFrameVersion(frame.version())},
         {"schema", encodeUiSchema(frame.schema())},
         {"state", encodeUiState(frame.state())},
         {"presence", encodeUiPresence(frame.presence())}});
}

std::optional<UiFrame> decodeUiFrame(const ProtocolValue& value) {
    if (!value.asObject()) return std::nullopt;
    const auto version =
        value.field("version")
            ? decodeUiFrameVersion(*value.field("version"))
            : std::nullopt;
    const auto schema =
        value.field("schema") ? decodeUiSchema(*value.field("schema"))
                              : std::nullopt;
    const auto state =
        value.field("state") ? decodeUiState(*value.field("state"))
                             : std::nullopt;
    const auto presence =
        value.field("presence")
            ? decodeUiPresence(*value.field("presence"))
            : std::nullopt;
    if (!version || !schema || !state || !presence) return std::nullopt;
    auto frame = UiFrame::create(*schema, *state, *presence);
    if (!frame || frame->version() != *version) return std::nullopt;
    return frame;
}

ProtocolValue encodeUiFrameDelta(const UiFrameDelta& delta) {
    std::vector<ProtocolValue::Field> fields{
        {"base", encodeUiFrameVersion(delta.base())},
        {"target", encodeUiFrameVersion(delta.target())}};
    if (const auto* replacement =
            std::get_if<UiFrameReplacement>(&delta.body())) {
        fields.emplace_back("kind", ProtocolValue::makeText("replacement"));
        fields.emplace_back("frame", encodeUiFrame(replacement->frame));
        return ProtocolValue::makeObject(std::move(fields));
    }
    const auto* changes = std::get_if<UiFrameChanges>(&delta.body());
    if (!changes) {
        throw std::invalid_argument(
            "legacy UI frame changes cannot be encoded");
    }
    UiStateSection stateChanges{
        delta.target().generation, changes->state,
        changes->focusPathChanged ? changes->focusPath : std::nullopt};
    UiPresenceSection presenceChanges{delta.target().generation,
                                      delta.target().presenceBasis,
                                      changes->presence};
    fields.emplace_back("kind", ProtocolValue::makeText("changes"));
    fields.emplace_back("state", encodeUiState(stateChanges));
    fields.emplace_back("presence", encodeUiPresence(presenceChanges));
    return ProtocolValue::makeObject(std::move(fields));
}

std::optional<UiFrameDelta> decodeUiFrameDelta(const ProtocolValue& value) {
    if (!value.asObject()) return std::nullopt;
    const auto base =
        value.field("base") ? decodeUiFrameVersion(*value.field("base"))
                            : std::nullopt;
    const auto target =
        value.field("target") ? decodeUiFrameVersion(*value.field("target"))
                              : std::nullopt;
    const auto* kindField = value.field("kind");
    if (!base || !target || !kindField || !kindField->asText()) {
        return std::nullopt;
    }
    if (*kindField->asText() == "replacement") {
        const auto frame =
            value.field("frame") ? decodeUiFrame(*value.field("frame"))
                                 : std::nullopt;
        if (!frame || frame->version() != *target) return std::nullopt;
        return UiFrameDelta::replacement(*base, *frame);
    }
    if (*kindField->asText() != "changes") return std::nullopt;
    const auto state =
        value.field("state") ? decodeUiState(*value.field("state"))
                             : std::nullopt;
    const auto presence =
        value.field("presence")
            ? decodeUiPresence(*value.field("presence"))
            : std::nullopt;
    if (!state || !presence || state->generation != target->generation ||
        presence->generation != target->generation ||
        presence->basis != target->presenceBasis) {
        return std::nullopt;
    }
    UiFrameChanges changes;
    changes.state = state->nodes;
    changes.presence = presence->nodes;
    changes.focusPathChanged = state->focusPath.has_value();
    changes.focusPath = state->focusPath;
    return UiFrameDelta::changes(*base, *target, std::move(changes));
}

void annotateLegacyFocusHosts(UiNode& node) {
    const std::string_view id = node.id.value();
    // Preceding schemas had no focus metadata; codec-known host identity wins
    // over any incidental value carried by that frozen representation.
    if (id == kEditorNodeId) {
        node.focusContext = FocusTarget::Editor;
    } else if (id == kPanelNodeId) {
        node.focusContext = FocusTarget::Panel;
    } else if (id == kHeaderPromptInputNodeId ||
               id == kFooterPromptNodeId) {
        node.focusContext = FocusTarget::Prompt;
    } else if (id == kExternalModNodeId) {
        node.focusContext = FocusTarget::ExternalModification;
    }
    if (auto* container = std::get_if<UiContainer>(&node.content)) {
        for (auto& child : container->children) {
            annotateLegacyFocusHosts(child);
        }
    }
}

constexpr auto kSemanticSessionFields = std::to_array<std::string_view>({
    "document", "selection", "history", "clipboard", "prompt_status", "search",
    "find_replace", "settings", "keymap", "text_encoding", "tabs", "diff",
    "external_modification", "follow_edits", "tree", "syntax", "lsp_sync",
    "lsp_features", "theme", "focus", "palette", "ui_frame",
    "prompt_view", "notice_view", "watcher_available",
    "external_focus_held",
});

constexpr auto kSemanticSessionDeltaFields = std::to_array<std::string_view>({
    "document", "document_caret", "selection", "history", "clipboard",
    "prompt_status", "search", "find_replace", "settings", "keymap",
    "text_encoding", "tabs", "diff", "external_modification", "follow_edits",
    "tree", "syntax", "lsp_sync", "lsp_features", "theme", "focus", "palette",
    "ui_frame_delta", "prompt_view", "notice_view",
    "watcher_available", "external_focus_held",
});

template <typename Record>
void appendRecordsInSchemaOrder(
    UiNode const& node, std::vector<Record>& records,
    std::unordered_map<std::string, std::size_t> const& indexes,
    std::vector<Record>& ordered) {
    ordered.push_back(std::move(records[indexes.at(node.id.value())]));
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children) {
            appendRecordsInSchemaOrder(child, records, indexes, ordered);
        }
    }
}

template <typename Record>
void canonicalizeUiRecordOrder(UiSchema const& schema,
                               std::vector<Record>& records) {
    const auto ids = uiSchemaNodeIds(schema);
    if (records.size() != ids.size()) return;
    std::unordered_map<std::string, std::size_t> indexes;
    indexes.reserve(records.size());
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (!ids.contains(records[index].id) ||
            !indexes.emplace(records[index].id.value(), index).second) {
            return;
        }
    }
    std::vector<Record> ordered;
    ordered.reserve(records.size());
    appendRecordsInSchemaOrder(schema.root, records, indexes, ordered);
    records = std::move(ordered);
}

template <typename Record>
std::optional<std::array<std::size_t, 3>> legacyTreeRecordIndexes(
    const std::vector<Record>& records) {
    std::array<std::size_t, 3> indexes{};
    constexpr std::array ids{kFileTreeNodeId, kGitStatusNodeId, kSymbolsNodeId};
    for (std::size_t wanted = 0; wanted < ids.size(); ++wanted) {
        const auto found = std::find_if(
            records.begin(), records.end(), [&](const Record& record) {
                return record.id.value() == ids[wanted];
            });
        if (found == records.end()) return std::nullopt;
        indexes[wanted] =
            static_cast<std::size_t>(std::distance(records.begin(), found));
    }
    return indexes;
}

void normalizeLegacyTreeRecords(UiPresenceSection& presence) {
    const auto indexes = legacyTreeRecordIndexes(presence.nodes);
    if (!indexes) return;
    std::erase_if(presence.nodes, [](const UiPresenceRecord& record) {
        return record.id.value() == kFileTreeNodeId ||
               record.id.value() == kGitStatusNodeId ||
               record.id.value() == kSymbolsNodeId;
    });
    presence.nodes.push_back(
        UiPresenceRecord{UiNodeId{std::string{kTreeNodeId}}, true});
}

void normalizeLegacyTreeRecords(UiStateSection& state) {
    const auto indexes = legacyTreeRecordIndexes(state.nodes);
    if (!indexes) return;
    std::erase_if(state.nodes, [](const UiNodeState& record) {
        return record.id.value() == kFileTreeNodeId ||
               record.id.value() == kGitStatusNodeId ||
               record.id.value() == kSymbolsNodeId;
    });
    state.nodes.push_back(
        UiNodeState{UiNodeId{std::string{kTreeNodeId}}, std::nullopt});
    if (state.focusPath) {
        for (auto& id : *state.focusPath) {
            if (id.value() == kFileTreeNodeId ||
                id.value() == kGitStatusNodeId ||
                id.value() == kSymbolsNodeId) {
                id = UiNodeId{std::string{kTreeNodeId}};
            }

        }
    }
}

ProtocolValue toValue(SessionSnapshotSections const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back(kSemanticSessionFields[0], toValue(value.document));
    fields.emplace_back(kSemanticSessionFields[1], toValue(value.selection));
    fields.emplace_back(kSemanticSessionFields[2], toValue(value.history));
    fields.emplace_back(kSemanticSessionFields[3], toValue(value.clipboard));
    fields.emplace_back(kSemanticSessionFields[4], toValue(value.promptStatus));
    fields.emplace_back(kSemanticSessionFields[5], toValue(value.search));
    fields.emplace_back(kSemanticSessionFields[6], toValue(value.findReplace));
    fields.emplace_back(kSemanticSessionFields[7], toValue(value.settings));
    fields.emplace_back(kSemanticSessionFields[8], toValue(value.keymap));
    fields.emplace_back(kSemanticSessionFields[9], toValue(value.textEncoding));
    fields.emplace_back(kSemanticSessionFields[10], toValue(value.tabs));
    fields.emplace_back(kSemanticSessionFields[11], toValue(value.diff));
    fields.emplace_back(kSemanticSessionFields[12],
                        toValue(value.externalModification));
    fields.emplace_back(kSemanticSessionFields[13], toValue(value.followEdits));
    fields.emplace_back(kSemanticSessionFields[14], toValue(value.tree));
    fields.emplace_back(kSemanticSessionFields[15], toValue(value.syntax));
    fields.emplace_back(kSemanticSessionFields[16], toValue(value.lspSync));
    fields.emplace_back(kSemanticSessionFields[17], toValue(value.lspFeatures));
    fields.emplace_back(kSemanticSessionFields[18], toValue(value.theme));
    fields.emplace_back(kSemanticSessionFields[19],
                        toValue(value.uiFrame.legacyFocus()));
    fields.emplace_back(kSemanticSessionFields[20], encodePalette(value.palette));
    fields.emplace_back(kSemanticSessionFields[21],
                        encodeUiFrame(value.uiFrame));
    const auto promptView =
        detail::legacyPromptView(value.promptStatus, value.uiFrame);
    if (!promptView.valid) {
        throw std::logic_error{
            "cannot derive legacy prompt view from UI frame"};
    }
    fields.emplace_back(kSemanticSessionFields[22], promptView.view
                                           ? toValue(*promptView.view)
                                           : ProtocolValue::makeNull());
    // Additive: the semantic draft-conflict notice section. Null when the active
    // document has no unresolved conflict; a decoder that predates this field simply
    // ignores it, and a frame that omits it decodes to no notice.
    fields.emplace_back(kSemanticSessionFields[23], value.noticeView
                                           ? toValue(*value.noticeView)
                                           : ProtocolValue::makeNull());
    // Additive: whether the session watches for external modification (Decision
    // 13). A decoder that predates this field ignores it; an absent field decodes
    // to available (true), so an old peer is never shown as unwatched.
    fields.emplace_back(kSemanticSessionFields[24],
                        toValue(value.watcherAvailable));
    // Additive: whether the external-modification bar is the EFFECTIVE (top)
    // focus, not merely present on the capture stack. A Prompt captured above the
    // external capture makes this false while the legacy `focus` field publishes
    // Prompt, so a new client reconstructs "effective => ExternalModification,
    // else legacy focus" unambiguously. A decoder that predates this field
    // ignores it; an absent field decodes to false, so the legacy `focus` field
    // alone reconstructs focus for an old peer.
    fields.emplace_back(kSemanticSessionFields[25],
                        toValue(value.uiFrame.effectiveFocus() ==
                                FocusTarget::ExternalModification));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SessionSnapshotSections>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto document = requireField<DocumentViewState>(value.field("document"));
    auto selection = requireField<SelectionSet>(value.field("selection"));
    auto history = requireField<HistoryViewState>(value.field("history"));
    auto clipboard = requireField<ClipboardViewState>(value.field("clipboard"));
    auto promptStatus = requireField<PromptStatusViewState>(value.field("prompt_status"));
    auto search = requireField<SearchViewState>(value.field("search"));
    auto findReplace = requireField<FindReplaceViewState>(value.field("find_replace"));
    auto settings = requireField<SettingsViewState>(value.field("settings"));
    auto keymap = requireField<KeymapViewState>(value.field("keymap"));
    auto textEncoding = requireField<TextEncodingViewState>(value.field("text_encoding"));
    auto tabs = requireField<TabViewState>(value.field("tabs"));
    auto diff = requireField<DiffViewState>(value.field("diff"));
    auto externalModification =
        requireField<ExternalModificationViewState>(value.field("external_modification"));
    auto followEdits = requireField<FollowEditsViewState>(value.field("follow_edits"));
    auto tree = requireField<TreeViewState>(value.field("tree"));
    auto syntax = requireField<SyntaxViewState>(value.field("syntax"));
    auto lspSync = requireField<LspSyncViewState>(value.field("lsp_sync"));
    auto lspFeatures = requireField<LspFeatureViewState>(value.field("lsp_features"));
    auto theme = requireField<ThemeSnapshot>(value.field("theme"));
    auto focus = requireField<FocusTarget>(value.field("focus"));
    auto palette = value.field("palette")
        ? decodePalette(*value.field("palette"))
        : std::optional<PaletteViewState>{};
    std::optional<UiFrame> uiFrame;
    if (const ProtocolValue* frameField = value.field("ui_frame")) {
        uiFrame = decodeUiFrame(*frameField);
        if (!uiFrame) return false;
    }
    std::optional<UiSchema> ui;
    if (const ProtocolValue* uiField = value.field("ui")) {
        ui = decodeUiSchema(*uiField);
        if (!ui) return false;
        annotateLegacyFocusHosts(ui->root);
    }
    std::optional<UiStateSection> uiState;
    if (const ProtocolValue* uiStateField = value.field("ui_state")) {
        uiState = decodeUiState(*uiStateField);
        if (!uiState) return false;
    }
    std::optional<UiPresenceSection> uiPresence;
    if (const ProtocolValue* uiPresenceField = value.field("ui_presence")) {
        uiPresence = decodeUiPresence(*uiPresenceField);
        if (!uiPresence) return false;
    }
    // Additive: absent OR null decodes to no footer prompt; present-but-malformed
    // fails loud.
    std::optional<PromptView> promptView;
    if (const ProtocolValue* promptViewField = value.field("prompt_view")) {
        if (promptViewField->kind() != ProtocolValue::Kind::NullValue) {
            if (!decodePresent(*promptViewField, promptView)) return false;
        }
    }
    // Additive: absent OR null decodes to no notice; present-but-malformed fails loud.
    std::optional<NoticeView> noticeView;
    if (const ProtocolValue* noticeViewField = value.field("notice_view")) {
        if (noticeViewField->kind() != ProtocolValue::Kind::NullValue) {
            if (!decodePresent(*noticeViewField, noticeView)) return false;
        }
    }
    // Additive: an absent watcher-availability field decodes to available (true), so
    // a frame from a peer that predates it is never shown as unwatched; a
    // present-but-malformed field fails loud.
    bool watcherAvailable = true;
    if (const ProtocolValue* watcherField = value.field("watcher_available")) {
        auto decoded = requireField<bool>(watcherField);
        if (!decoded) return false;
        watcherAvailable = *decoded;
    }
    // Additive: an absent external-focus-held field decodes to false, so a frame
    // from a peer that predates it never spuriously holds external focus; a
    // present-but-malformed field fails loud.
    bool externalFocusHeld = false;
    if (const ProtocolValue* heldField = value.field("external_focus_held")) {
        auto decoded = requireField<bool>(heldField);
        if (!decoded) return false;
        externalFocusHeld = *decoded;
    }
    const bool hasLegacyUi = ui || uiState || uiPresence;
    if (uiFrame && hasLegacyUi) return false;
    if (hasLegacyUi && (!ui || !uiState || !uiPresence)) return false;
    if (ui && uiState && uiPresence) {
        auto validated = ValidatedSchema::validate(*ui);
        if (!validated.ok()) return false;
        if (uiSchemaNodeIds(*ui).contains(
                UiNodeId{std::string{kTreeNodeId}})) {
            normalizeLegacyTreeRecords(*uiPresence);
            normalizeLegacyTreeRecords(*uiState);
        }
        if (!uiPresenceCorrespondsToSchema(*uiPresence, validated.schema()))
            return false;
        canonicalizeUiRecordOrder(*ui, uiPresence->nodes);
        canonicalizeUiRecordOrder(*ui, uiState->nodes);
    }
    if (!document || !selection || !history || !clipboard || !promptStatus || !search ||
        !findReplace || !settings || !keymap || !textEncoding || !tabs || !diff ||
        !externalModification || !followEdits || !tree || !syntax || !lspSync ||
        !lspFeatures || !theme || !focus) {
        return false;
    }
    if (!palette) return false;
    if (uiFrame &&
        (uiFrame->legacyFocus() != *focus ||
         (uiFrame->effectiveFocus() == FocusTarget::ExternalModification) !=
             externalFocusHeld)) {
        return false;
    }
    if (!uiFrame && ui && uiState && uiPresence) {
        if (!uiState->focusPath) {
            uiState->focusPath = detail::legacyFocusPath(
                *ui, *uiPresence, *focus, externalFocusHeld);
            if (!uiState->focusPath) return false;
        }
        uiFrame = UiFrame::create(std::move(*ui), std::move(*uiState),
                                  std::move(*uiPresence));
        if (!uiFrame) return false;
    }
    if (uiFrame && value.field("prompt_view")) {
        const auto derived = detail::legacyPromptView(*promptStatus, *uiFrame);
        if (!derived.valid || derived.view != promptView) return false;
    }
    out.emplace(SessionSnapshotSections{
        *document, *selection, *history, *clipboard, *promptStatus, *search,
        *findReplace, *settings, *keymap, *textEncoding, *tabs, *diff,
        *externalModification, *followEdits, *tree, std::move(*syntax), *lspSync,
        *lspFeatures, *theme});
    out->palette = std::move(*palette);
    if (uiFrame) out->uiFrame = std::move(*uiFrame);
    out->noticeView = std::move(noticeView);
    out->watcherAvailable = watcherAvailable;
    return true;
}


ProtocolValue toValue(TextInputArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("text", toValue(value.text));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TextInputArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto text = requireField<std::string>(value.field("text"));
    if (!text) return false;
    out.emplace(TextInputArguments{*text});
    return true;
}

ProtocolValue toValue(PaletteExecuteArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("command_id", toValue(value.commandId));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PaletteExecuteArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto commandId = requireField<std::string>(value.field("command_id"));
    if (!commandId) return false;
    out.emplace(PaletteExecuteArguments{*commandId});
    return true;
}

ProtocolValue toValue(PickerSubmitArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("mode", toValue(value.activation.mode));
    fields.emplace_back("activation_id",
                        toValue(value.activation.id.value()));
    fields.emplace_back("candidate_id", toValue(value.candidateId));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value,
                   std::optional<PickerSubmitArguments>& out) {
    if (!value.asObject()) return false;
    auto mode = requireField<SearchMode>(value.field("mode"));
    auto activationId =
        requireField<std::uint64_t>(value.field("activation_id"));
    auto candidateId =
        requireField<std::string>(value.field("candidate_id"));
    if (!mode || !activationId || *activationId == 0 || !candidateId) {
        return false;
    }
    out.emplace(PickerSubmitArguments{
        PickerActivation{*mode, PickerActivationId{*activationId}},
        *candidateId});
    return true;
}

ProtocolValue toValue(TreeSelectArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("node_id", toValue(value.nodeId));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<TreeSelectArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto nodeId = requireField<TreeNodeId>(value.field("node_id"));
    if (!nodeId) return false;
    out.emplace(TreeSelectArguments{*nodeId});
    return true;
}

ProtocolValue toValue(ExternalActionInvocation const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("file_id", toValue(value.fileId));
    fields.emplace_back(
        "action", ProtocolValue::makeUint(
                      static_cast<std::uint8_t>(value.action)));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value,
                   std::optional<ExternalActionInvocation>& out) {
    auto const* object = value.asObject();
    if (!object || object->size() != 2 || !value.field("file_id") ||
        !value.field("action")) {
        return false;
    }
    auto fileId = requireField<DiffFileId>(value.field("file_id"));
    auto action = requireField<std::uint8_t>(value.field("action"));
    if (!fileId || !action ||
        *action > static_cast<std::uint8_t>(ExternalAction::OpenDiff)) {
        return false;
    }
    out.emplace(ExternalActionInvocation{
        *fileId, static_cast<ExternalAction>(*action)});
    return true;
}

ProtocolValue toValue(FindQueryArguments const& value) {    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("query", toValue(value.query));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<FindQueryArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto query = requireField<std::string>(value.field("query"));
    if (!query) return false;
    out.emplace(FindQueryArguments{*query});
    return true;
}

ProtocolValue toValue(PromptValueArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("index", toValue(static_cast<std::uint64_t>(value.index)));
    fields.emplace_back("value", toValue(value.value));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptValueArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto index = requireField<std::uint64_t>(value.field("index"));
    auto text = requireField<std::string>(value.field("value"));
    if (!index || !text) return false;
    out.emplace(PromptValueArguments{static_cast<std::size_t>(*index), *text});
    return true;
}

ProtocolValue toValue(PromptFocusArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("control_id", toValue(value.controlId));
    return ProtocolValue::makeObject(std::move(fields));
}

ProtocolValue toValue(UiNodeActivationArguments const& value) {
    return ProtocolValue::makeObject(
        {{"generation", ProtocolValue::makeUint(value.generation.value())},
         {"node_id", ProtocolValue::makeText(value.nodeId.value())}});
}

bool decodePresent(ProtocolValue const& value,
                   std::optional<UiNodeActivationArguments>& out) {
    if (!value.asObject()) return false;
                   const auto* generation = value.field("generation");
                   const auto* nodeId = value.field("node_id");
                   if (!generation || !generation->asUint() || !nodeId ||
                       !nodeId->asText()) {
                       return false;
                   }
                   out.emplace(UiNodeActivationArguments{Generation{*generation->asUint()},
                                                         UiNodeId{*nodeId->asText()}});
    return true;
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptFocusArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto controlId = requireField<std::string>(value.field("control_id"));
    if (!controlId || controlId->empty()) return false;
    out.emplace(PromptFocusArguments{std::move(*controlId)});
    return true;
}

ProtocolValue toValue(SelectionCommandArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("position", toValue(value.position));
    fields.emplace_back("selection", toValue(value.selection));
    fields.emplace_back("selections", toValue(value.selections));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SelectionCommandArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    SelectionCommandArguments result;
    if (!decodeOptionalField(value.field("position"), result.position)) return false;
    if (!decodeOptionalField(value.field("selection"), result.selection)) return false;
    std::optional<std::vector<Selection>> selections;
    if (!decodeOptionalField(value.field("selections"), selections)) return false;
    if (selections) result.selections = std::move(*selections);
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(ScrollLinesArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("rows", toValue(value.rows));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ScrollLinesArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto rows = requireField<std::int64_t>(value.field("rows"));
    if (!rows) return false;
    out.emplace(ScrollLinesArguments{*rows});
    return true;
}

ProtocolValue toValue(ScrollPagesArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("pages", toValue(value.pages));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ScrollPagesArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto pages = requireField<std::int64_t>(value.field("pages"));
    if (!pages) return false;
    out.emplace(ScrollPagesArguments{*pages});
    return true;
}

ProtocolValue toValue(ScrollFractionArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("numerator", toValue(value.numerator));
    fields.emplace_back("denominator", toValue(value.denominator));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ScrollFractionArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto numerator = requireField<std::uint32_t>(value.field("numerator"));
    auto denominator = requireField<std::uint32_t>(value.field("denominator"));
    if (!numerator || !denominator) return false;
    out.emplace(*numerator, *denominator);
    return true;
}

ProtocolValue toValue(DroppedContentArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("bytes", toValue(value.bytes));
    fields.emplace_back("suggested_label", toValue(value.suggestedLabel));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<DroppedContentArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto bytes = requireField<std::vector<std::uint8_t>>(value.field("bytes"));
    auto suggestedLabel = requireField<std::string>(value.field("suggested_label"));
    if (!bytes || !suggestedLabel) return false;
    out.emplace(DroppedContentArguments{*bytes, *suggestedLabel});
    return true;
}

ProtocolValue toValue(ReopenWithEncodingArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("encoding", toValue(value.encoding));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ReopenWithEncodingArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto encoding = requireField<TextEncoding>(value.field("encoding"));
    if (!encoding) return false;
    out.emplace(ReopenWithEncodingArguments{*encoding});
    return true;
}

ProtocolValue toValue(SetEncodingArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("encoding", toValue(value.encoding));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SetEncodingArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto encoding = requireField<TextEncoding>(value.field("encoding"));
    if (!encoding) return false;
    out.emplace(SetEncodingArguments{*encoding});
    return true;
}

ProtocolValue toValue(SetLineEndingArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("line_ending", toValue(value.lineEnding));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SetLineEndingArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto lineEnding = requireField<LineEnding>(value.field("line_ending"));
    if (!lineEnding) return false;
    out.emplace(SetLineEndingArguments{*lineEnding});
    return true;
}

ProtocolValue toValue(SetFinalNewlineArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("final_newline", toValue(value.finalNewline));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SetFinalNewlineArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto finalNewline = requireField<bool>(value.field("final_newline"));
    if (!finalNewline) return false;
    out.emplace(SetFinalNewlineArguments{*finalNewline});
    return true;
}

ProtocolValue toValue(SettingSetArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("scope", toValue(value.scope));
    fields.emplace_back("key", toValue(value.key));
    fields.emplace_back("value", toValue(value.value));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SettingSetArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto scope = requireField<SettingScope>(value.field("scope"));
    auto key = requireField<SettingKey>(value.field("key"));
    auto settingValue = requireField<SettingValue>(value.field("value"));
    if (!scope || !key || !settingValue) return false;
    out.emplace(SettingSetArguments{*scope, *key, *settingValue});
    return true;
}

ProtocolValue toValue(SettingResetArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("scope", toValue(value.scope));
    fields.emplace_back("key", toValue(value.key));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SettingResetArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto scope = requireField<SettingScope>(value.field("scope"));
    auto key = requireField<SettingKey>(value.field("key"));
    if (!scope || !key) return false;
    out.emplace(SettingResetArguments{*scope, *key});
    return true;
}

ProtocolValue toValue(SettingResetScopeArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("scope", toValue(value.scope));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<SettingResetScopeArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto scope = requireField<SettingScope>(value.field("scope"));
    if (!scope) return false;
    out.emplace(SettingResetScopeArguments{*scope});
    return true;
}

ProtocolValue toValue(WorkspaceReplaceArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("request", toValue(value.request));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<WorkspaceReplaceArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto request = requireField<FindRequest>(value.field("request"));
    auto replacement = requireField<std::string>(value.field("replacement"));
    if (!request || !replacement) return false;
    out.emplace(WorkspaceReplaceArguments{*request, *replacement});
    return true;
}

ProtocolValue toValue(ThemeSectionDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ThemeSectionDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    std::optional<ThemeSnapshot> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(ThemeSectionDelta{std::move(replacement)});
    return true;
}

ProtocolValue toValue(StyleSectionDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<StyleSectionDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    std::optional<Style> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(StyleSectionDelta{std::move(replacement)});
    return true;
}

ProtocolValue toValue(ShellSectionDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ShellSectionDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    std::optional<ShellViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(ShellSectionDelta{std::move(replacement)});
    return true;
}

}  // namespace



namespace {

CommandArgumentCodec makeNoneCodec() {
    return CommandArgumentCodec{
        [](std::any const&) { return ProtocolValue::makeNull(); },
        [](ProtocolValue const& value) -> std::optional<std::any> {
            if (value.kind() != ProtocolValue::Kind::NullValue) {
                return std::nullopt;
            }
            return std::any{};
        }};
}

template <typename Arguments>
CommandArgumentCodec makeTypedCodec() {
    return CommandArgumentCodec{
        [](std::any const& payload) {
            return toValue(std::any_cast<Arguments const&>(payload));
        },
        [](ProtocolValue const& value) -> std::optional<std::any> {
            std::optional<Arguments> decoded;
            if (!fromValue(value, decoded) || !decoded.has_value()) {
                return std::nullopt;
            }
            return std::any{std::move(*decoded)};
        }};
}

template <typename Arguments>
CommandArgumentCodec makeOptionalTypedCodec() {
    return CommandArgumentCodec{
        [](std::any const& payload) {
            if (!payload.has_value()) return ProtocolValue::makeNull();
            return toValue(std::any_cast<Arguments const&>(payload));
        },
        [](ProtocolValue const& value) -> std::optional<std::any> {
            if (value.kind() == ProtocolValue::Kind::NullValue) {
                return std::any{};
            }
            std::optional<Arguments> decoded;
            if (!fromValue(value, decoded) || !decoded.has_value()) {
                return std::nullopt;
            }
            return std::any{std::move(*decoded)};
        }};
}

CommandArgumentCodec makeWorkspaceApplyCodec() {
    return CommandArgumentCodec{
        [](std::any const& payload) {
            if (!payload.has_value()) return ProtocolValue::makeNull();
            return toValue(std::any_cast<WorkspaceReplacePreview const&>(payload));
        },
        [](ProtocolValue const& value) -> std::optional<std::any> {
            if (value.kind() == ProtocolValue::Kind::NullValue) {
                return std::any{};
            }
            std::optional<WorkspaceReplacePreview> decoded;
            if (!fromValue(value, decoded) || !decoded.has_value()) {
                return std::nullopt;
            }
            return std::any{std::move(*decoded)};
        }};
}

}  // namespace

namespace {

// How a command's arguments travel, keyed by the TYPE the handler consumes.
//
// This replaced a switch over an authored ArgumentKind enum, which itself
// replaced a 20-branch if-chain on command ids whose final `else` silently gave
// any unlisted command the no-argument codec.  Keying on the type closes the
// remaining gap: the type comes from the handler that consumes it, so a
// command's codec and its handler cannot disagree about what the payload is.
//
// The catalog says which type each command uses; this says how each type
// travels.  Neither restates the other.
std::unordered_map<std::type_index, CommandArgumentCodec> const&
argumentCodecsByType() {
    static auto const codecs = [] {
        std::unordered_map<std::type_index, CommandArgumentCodec> table;
        table.emplace(typeid(TextInputArguments), makeTypedCodec<TextInputArguments>());
        table.emplace(typeid(SelectionCommandArguments), makeTypedCodec<SelectionCommandArguments>());
        table.emplace(typeid(ScrollLinesArguments), makeTypedCodec<ScrollLinesArguments>());
        table.emplace(typeid(ScrollPagesArguments), makeTypedCodec<ScrollPagesArguments>());
        table.emplace(typeid(ScrollFractionArguments), makeTypedCodec<ScrollFractionArguments>());
        table.emplace(typeid(DroppedContentArguments), makeTypedCodec<DroppedContentArguments>());
        table.emplace(typeid(ReopenWithEncodingArguments), makeTypedCodec<ReopenWithEncodingArguments>());
        table.emplace(typeid(SetEncodingArguments), makeTypedCodec<SetEncodingArguments>());
        table.emplace(typeid(SetLineEndingArguments), makeTypedCodec<SetLineEndingArguments>());
        table.emplace(typeid(SetFinalNewlineArguments), makeTypedCodec<SetFinalNewlineArguments>());
        table.emplace(typeid(SettingSetArguments), makeTypedCodec<SettingSetArguments>());
        table.emplace(typeid(SettingResetArguments), makeTypedCodec<SettingResetArguments>());
        table.emplace(typeid(SettingResetScopeArguments), makeTypedCodec<SettingResetScopeArguments>());
        table.emplace(typeid(WorkspaceReplaceArguments), makeTypedCodec<WorkspaceReplaceArguments>());
        table.emplace(typeid(WorkspaceReplacePreview), makeWorkspaceApplyCodec());
        table.emplace(typeid(PaletteExecuteArguments), makeTypedCodec<PaletteExecuteArguments>());
        table.emplace(typeid(PickerSubmitArguments), makeTypedCodec<PickerSubmitArguments>());
        table.emplace(typeid(TreeSelectArguments), makeTypedCodec<TreeSelectArguments>());
        table.emplace(typeid(ExternalActionInvocation), makeTypedCodec<ExternalActionInvocation>());
        table.emplace(typeid(FindQueryArguments), makeTypedCodec<FindQueryArguments>());
        table.emplace(typeid(PromptValueArguments), makeTypedCodec<PromptValueArguments>());
        table.emplace(typeid(PromptFocusArguments), makeTypedCodec<PromptFocusArguments>());
        table.emplace(typeid(UiNodeActivationArguments),
                      makeTypedCodec<UiNodeActivationArguments>());
        table.emplace(typeid(TabId), makeOptionalTypedCodec<TabId>());
        return table;
    }();
    return codecs;
}

// The codec a registered command's arguments travel by, or nullptr when the
// command is unknown.  Resolved per call against the LIVE catalog, so a command
// registered a moment ago is already carryable and nothing can go stale.
CommandArgumentCodec const* codecForCommand(CommandCatalog const& catalog,
                                            std::string_view commandId) {
    auto const* command = catalog.find(commandId);
    if (command == nullptr) return nullptr;
    // An in-process-only payload has no wire representation, so the command
    // carries nothing across the protocol -- the same as taking no arguments.
    if (!command->argument.type.has_value() || !command->argument.wire) {
        static CommandArgumentCodec const none = makeNoneCodec();
        return &none;
    }
    auto const& codecs = argumentCodecsByType();
    auto const found = codecs.find(*command->argument.type);
    return found == codecs.end() ? nullptr : &found->second;
}

}  // namespace


struct CommandArgumentCodecRegistry::Impl {
    std::shared_ptr<CommandCatalog const> catalog;
};

CommandArgumentCodecRegistry::CommandArgumentCodecRegistry(
    std::shared_ptr<CommandCatalog const> catalog)
    : impl_{std::make_unique<Impl>(std::move(catalog))} {
    if (!impl_->catalog) {
        throw std::invalid_argument{"command argument codecs require a catalog"};
    }
}

CommandArgumentCodecRegistry::~CommandArgumentCodecRegistry() = default;
CommandArgumentCodecRegistry::CommandArgumentCodecRegistry(
    CommandArgumentCodecRegistry&&) noexcept = default;
CommandArgumentCodecRegistry& CommandArgumentCodecRegistry::operator=(
    CommandArgumentCodecRegistry&&) noexcept = default;

bool CommandArgumentCodecRegistry::contains(std::string_view commandId) const {
    return codecForCommand(*impl_->catalog, commandId) != nullptr;
}

ProtocolValue CommandArgumentCodecRegistry::encodeArgument(
    std::string_view commandId, std::any const& payload) const {
    auto const* codec = codecForCommand(*impl_->catalog, commandId);
    if (codec == nullptr) {
        throw std::invalid_argument{"unknown command ID: " + std::string{commandId}};
    }
    return codec->encode(payload);
}

std::optional<std::any> CommandArgumentCodecRegistry::decodeArgument(
    std::string_view commandId, ProtocolValue const& value) const {
    auto const* codec = codecForCommand(*impl_->catalog, commandId);
    if (codec == nullptr) return std::nullopt;
    return codec->decode(value);
}

std::string ProtocolCodec::encodeCommandRequest(ClientCommand const& command,
                                   CommandArgumentCodecRegistry const& registry) const {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(std::string{command.id.name()}));
    fields.emplace_back("base_revision", toValue(command.baseRevision));
    fields.emplace_back(
        "payload", registry.encodeArgument(command.id.name(), command.payload));
    return encodeMessage(ProtocolMessageKind::CommandRequest,
                          ProtocolValue::makeObject(std::move(fields)));
}

DecodeCommandRequestResult ProtocolCodec::decodeCommandRequest(
    std::string_view bytes, CommandArgumentCodecRegistry const& registry,
    ProtocolLimits limits) const {
    auto decoded =
        decodeMessage(bytes, ProtocolMessageKind::CommandRequest, limits);
    if (decoded.error != ProtocolError::None) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    auto const& payload = *decoded.payload;
    if (!payload.asObject()) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "command request payload is not an object"};
    }
    auto id = requireField<std::string>(payload.field("id"));
    auto baseRevision = requireField<Revision>(payload.field("base_revision"));
    if (!id || !baseRevision) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "command request payload is malformed"};
    }
    if (!registry.contains(*id)) {
        return {ProtocolError::UnsupportedCommand, std::nullopt,
                "command request references an unknown command id"};
    }
    auto const* payloadField = payload.field("payload");
    if (payloadField == nullptr) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "command request is missing its payload field"};
    }
    auto argument = registry.decodeArgument(*id, *payloadField);
    if (!argument) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "command request payload does not match its command id"};
    }
    return {ProtocolError::None,
            ClientCommand{std::move(*id), *baseRevision,
                          std::move(*argument)},
            {}};
}

namespace {

template <typename Enum>
std::optional<Enum> viewEnumField(ProtocolValue const* value, Enum maximum) {
    const auto raw = requireField<std::uint8_t>(value);
    if (!raw ||
        *raw > static_cast<std::uint8_t>(maximum)) {
        return std::nullopt;
    }
    return static_cast<Enum>(*raw);
}

ProtocolValue viewActionValue(ViewAction const& action) {
    std::vector<ProtocolValue::Field> fields;
    std::visit(
        [&](auto const& typed) {
            using Action = std::decay_t<decltype(typed)>;
            if constexpr (std::same_as<Action, ViewScrollLines>) {
                fields.emplace_back("kind", toValue(ViewActionKind::ScrollLines));
                fields.emplace_back("target", toValue(typed.target));
                fields.emplace_back("rows", toValue(typed.rows));
            } else if constexpr (std::same_as<Action, ViewScrollPages>) {
                fields.emplace_back("kind", toValue(ViewActionKind::ScrollPages));
                fields.emplace_back("pages", toValue(typed.pages));
            } else if constexpr (std::same_as<Action, ViewScrollFraction>) {
                fields.emplace_back("kind",
                                    toValue(ViewActionKind::ScrollFraction));
                fields.emplace_back("target", toValue(typed.target));
                fields.emplace_back("numerator", toValue(typed.numerator));
                fields.emplace_back("denominator", toValue(typed.denominator));
            } else if constexpr (std::same_as<Action, MoveVisualSelection>) {
                fields.emplace_back(
                    "kind", toValue(ViewActionKind::MoveVisualSelection));
                fields.emplace_back("direction", toValue(typed.direction));
                fields.emplace_back("extend", toValue(typed.extend));
            } else if constexpr (std::same_as<Action, RevealSelection>) {
                fields.emplace_back("kind",
                                    toValue(ViewActionKind::RevealSelection));
            } else if constexpr (std::same_as<Action, CenterSelection>) {
                fields.emplace_back("kind",
                                    toValue(ViewActionKind::CenterSelection));
            } else if constexpr (std::same_as<Action, SplitPane>) {
                fields.emplace_back("kind", toValue(ViewActionKind::SplitPane));
                fields.emplace_back("axis", toValue(typed.axis));
            } else if constexpr (std::same_as<Action, ClosePane>) {
                fields.emplace_back("kind", toValue(ViewActionKind::ClosePane));
            } else if constexpr (std::same_as<Action, CyclePane>) {
                fields.emplace_back("kind", toValue(ViewActionKind::CyclePane));
                fields.emplace_back("direction", toValue(typed.direction));
            } else if constexpr (std::same_as<Action, FocusPane>) {
                fields.emplace_back("kind", toValue(ViewActionKind::FocusPane));
                fields.emplace_back("direction", toValue(typed.direction));
            } else if constexpr (std::same_as<Action, ContinuePointerEdge>) {
                fields.emplace_back(
                    "kind", toValue(ViewActionKind::ContinuePointerEdge));
                fields.emplace_back("direction", toValue(typed.direction));
            }
        },
        action);
    return ProtocolValue::makeObject(std::move(fields));
}

std::optional<ViewAction> viewActionFromValue(ProtocolValue const& value) {
    if (!value.asObject()) return std::nullopt;
    const auto kind = viewEnumField(
        value.field("kind"), ViewActionKind::ContinuePointerEdge);
    if (!kind) {
        return std::nullopt;
    }
    switch (*kind) {
    case ViewActionKind::ScrollLines: {
        const auto target = viewEnumField(
            value.field("target"), ViewScrollTarget::Tree);
        const auto rows = requireField<std::int64_t>(value.field("rows"));
        if (!target || !rows || *rows == 0)
            return std::nullopt;
        return ViewScrollLines{*target, *rows};
    }
    case ViewActionKind::ScrollPages: {
        const auto pages = requireField<std::int64_t>(value.field("pages"));
        if (!pages || *pages == 0) return std::nullopt;
        return ViewScrollPages{*pages};
    }
    case ViewActionKind::ScrollFraction: {
        const auto target = viewEnumField(
            value.field("target"), ViewScrollTarget::Tree);
        const auto numerator =
            requireField<std::uint32_t>(value.field("numerator"));
        const auto denominator =
            requireField<std::uint32_t>(value.field("denominator"));
        if (!target || !numerator || !denominator || *denominator == 0 ||
            *numerator > *denominator) {
            return std::nullopt;
        }
        return ViewScrollFraction{*target, *numerator, *denominator};
    }
    case ViewActionKind::MoveVisualSelection: {
        const auto direction = viewEnumField(
            value.field("direction"), VisualSelectionDirection::PageDown);
        const auto extend = requireField<bool>(value.field("extend"));
        if (!direction || !extend) {
            return std::nullopt;
        }
        return MoveVisualSelection{*direction, *extend};
    }
    case ViewActionKind::RevealSelection:
        return RevealSelection{};
    case ViewActionKind::CenterSelection:
        return CenterSelection{};
    case ViewActionKind::SplitPane: {
        const auto axis =
            viewEnumField(value.field("axis"), SplitAxis::Vertical);
        if (!axis) return std::nullopt;
        return SplitPane{*axis};
    }
    case ViewActionKind::ClosePane:
        return ClosePane{};
    case ViewActionKind::CyclePane: {
        const auto direction = viewEnumField(
            value.field("direction"), PaneCycleDirection::Previous);
        if (!direction)
            return std::nullopt;
        return CyclePane{*direction};
    }
    case ViewActionKind::FocusPane: {
        const auto direction =
            viewEnumField(value.field("direction"), PaneDirection::Down);
        if (!direction)
            return std::nullopt;
        return FocusPane{*direction};
    }
    case ViewActionKind::ContinuePointerEdge: {
        const auto direction = viewEnumField(
            value.field("direction"), PointerEdgeDirection::After);
        if (!direction)
            return std::nullopt;
        return ContinuePointerEdge{*direction};
    }
    }
    return std::nullopt;
}

ProtocolValue viewActionRequestValue(ViewActionRequest const& request) {
    return ProtocolValue::makeObject(
        {{"view_id", toValue(request.viewId)},
         {"semantic_revision", toValue(request.semanticRevision)},
         {"action", viewActionValue(request.action)}});
}

std::optional<ViewActionRequest> viewActionRequestFromValue(
    ProtocolValue const& value) {
    if (!value.asObject()) return std::nullopt;
    const auto viewId = requireField<ViewId>(value.field("view_id"));
    const auto revision =
        requireField<Revision>(value.field("semantic_revision"));
    const auto* actionField = value.field("action");
    if (!viewId || !revision || !actionField) return std::nullopt;
    auto action = viewActionFromValue(*actionField);
    if (!action) return std::nullopt;
    return ViewActionRequest{*viewId, *revision, std::move(*action)};
}

ProtocolValue commandResultValue(CommandResult const& result) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back(
        "error", ProtocolValue::makeUint(
                    static_cast<std::uint8_t>(result.error)));
    fields.emplace_back("revision", toValue(result.revision));
    fields.emplace_back("message", toValue(result.message));
    // Additive: the dispatch effects a host reads for per-drain snapshot
    // coalescing. Round-tripped rather than dropped so the decoded result is a
    // faithful copy; absent on an old peer's message, they default to false.
    fields.emplace_back(
        "routingChanged",
        ProtocolValue::makeUint(result.effects.routingChanged ? 1u : 0u));
    fields.emplace_back(
        "geometryChanged",
        ProtocolValue::makeUint(result.effects.geometryChanged ? 1u : 0u));
    if (result.viewAction) {
        fields.emplace_back("view_action",
                            viewActionRequestValue(*result.viewAction));
    }
    return ProtocolValue::makeObject(std::move(fields));
}

std::optional<CommandResult> commandResultFromValue(
    ProtocolValue const& payload) {
    if (!payload.asObject()) {
        return std::nullopt;
    }
    auto error = requireField<std::uint8_t>(payload.field("error"));
    auto revision = requireField<Revision>(payload.field("revision"));
    auto message = requireField<std::string>(payload.field("message"));
    if (!error || *error > static_cast<std::uint8_t>(
                              CommandError::RevisionExhausted) ||
        !revision || !message) {
        return std::nullopt;
    }
    // Additive effects fields: absent on an old peer, so default to false; but a
    // PRESENT field must be a valid boolean 0/1 -- a malformed or out-of-range
    // value is a corrupt message, not a silent false.
    CommandResult result{static_cast<CommandError>(*error), *revision,
                         std::move(*message)};
    if (auto const* routingField = payload.field("routingChanged")) {
        auto routing = requireField<std::uint8_t>(routingField);
        if (!routing || *routing > 1) {
            return std::nullopt;
        }
        result.effects.routingChanged = *routing != 0;
    }
    if (auto const* geometryField = payload.field("geometryChanged")) {
        auto geometry = requireField<std::uint8_t>(geometryField);
        if (!geometry || *geometry > 1) {
            return std::nullopt;
        }
        result.effects.geometryChanged = *geometry != 0;
    }
    if (auto const* actionField = payload.field("view_action")) {
        result.viewAction = viewActionRequestFromValue(*actionField);
        if (!result.viewAction || result.error != CommandError::None) {
            return std::nullopt;
        }
    }
    return result;
}

}  // namespace

std::string ProtocolCodec::encodeCommandResult(CommandResult const& result) const {
    return encodeMessage(ProtocolMessageKind::CommandResult,
                         commandResultValue(result));
}

DecodeCommandResultResult ProtocolCodec::decodeCommandResult(
    std::string_view bytes, ProtocolLimits limits) const {
    auto decoded =
        decodeMessage(bytes, ProtocolMessageKind::CommandResult, limits);
    if (decoded.error != ProtocolError::None) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    auto result = commandResultFromValue(*decoded.payload);
    if (!result) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "command result payload is malformed"};
    }
    return {ProtocolError::None, std::move(result), {}};
}

std::string ProtocolCodec::encodeClientInput(
    ClientInput const& input) const {
    std::vector<ProtocolValue::Field> fields;
    std::visit(
        [&](auto const& semantic) {
            using Input = std::decay_t<decltype(semantic)>;
            if constexpr (std::same_as<Input, ClientKeyInput>) {
                fields.emplace_back("kind", toValue(ClientInputKind::Key));
                fields.emplace_back(
                    "stroke", semantic.stroke.code == KeyCode::None
                                  ? ProtocolValue::makeNull()
                                  : toValue(semantic.stroke));
                fields.emplace_back("committed_text",
                                    toValue(semantic.committedText));
            } else if constexpr (std::same_as<Input, ScrollLinesInput>) {
                fields.emplace_back("kind",
                                    toValue(ClientInputKind::ScrollLines));
                fields.emplace_back("basis_revision",
                                    toValue(semantic.basis.observedRevision));
                fields.emplace_back("target", toValue(semantic.target));
                fields.emplace_back("rows", toValue(semantic.rows));
            } else if constexpr (std::same_as<Input, ScrollFractionInput>) {
                fields.emplace_back("kind",
                                    toValue(ClientInputKind::ScrollFraction));
                fields.emplace_back("basis_revision",
                                    toValue(semantic.basis.observedRevision));
                fields.emplace_back("target", toValue(semantic.target));
                fields.emplace_back("numerator", toValue(semantic.numerator));
                fields.emplace_back("denominator",
                                    toValue(semantic.denominator));
            } else if constexpr (std::same_as<Input,
                                              ViewNavigationInput>) {
                fields.emplace_back("kind",
                                    toValue(ClientInputKind::ViewNavigation));
                fields.emplace_back("basis_revision",
                                    toValue(semantic.basis.observedRevision));
            } else if constexpr (std::same_as<Input,
                                              ResolvedPaneFocusInput>) {
                fields.emplace_back(
                    "kind", toValue(ClientInputKind::ResolvedPaneFocus));
                fields.emplace_back("basis_revision",
                                    toValue(semantic.basis.observedRevision));
            } else if constexpr (std::same_as<Input,
                                               ResolvedSelectionInput>) {
                fields.emplace_back(
                    "kind", toValue(ClientInputKind::ResolvedSelection));
                fields.emplace_back("basis_revision",
                                    toValue(semantic.basis.observedRevision));
                fields.emplace_back("active_tab",
                                    toValue(semantic.activeTab));
                fields.emplace_back("document_revision",
                                    toValue(semantic.documentRevision));
                fields.emplace_back("selections",
                                    toValue(semantic.selections));
            } else {
                auto addPointer = [&](ClientInputKind kind) {
                    fields.emplace_back("kind", toValue(kind));
                    fields.emplace_back("button", toValue(semantic.button));
                    fields.emplace_back("phase", toValue(semantic.phase));
                };
                if constexpr (std::same_as<Input, TabPointerInput>) {
                    addPointer(ClientInputKind::Tab);
                    fields.emplace_back("basis_revision",
                                        toValue(semantic.basis.observedRevision));
                    fields.emplace_back("tab_id", toValue(semantic.tabId));
                } else if constexpr (std::same_as<Input, TreePointerInput>) {
                    addPointer(ClientInputKind::Tree);
                    fields.emplace_back("basis_revision",
                                        toValue(semantic.basis.observedRevision));
                    fields.emplace_back("node_id", toValue(semantic.nodeId));
                } else if constexpr (std::same_as<Input,
                                                  PickerPointerInput>) {
                    addPointer(ClientInputKind::Picker);
                    fields.emplace_back("picker_mode",
                                        toValue(semantic.activation.mode));
                    fields.emplace_back(
                        "activation_id",
                        ProtocolValue::makeUint(semantic.activation.id.value()));
                    fields.emplace_back("candidate_id",
                                        toValue(semantic.candidateId));
                } else if constexpr (std::same_as<
                                         Input, PromptControlPointerInput>) {
                    addPointer(ClientInputKind::PromptControl);
                    fields.emplace_back("basis_revision",
                                        toValue(semantic.basis.observedRevision));
                    fields.emplace_back("control_id",
                                        toValue(semantic.controlId));
                } else if constexpr (std::same_as<
                                         Input, ExternalActionPointerInput>) {
                    addPointer(ClientInputKind::ExternalAction);
                    fields.emplace_back("basis_revision",
                                        toValue(semantic.basis.observedRevision));
                    fields.emplace_back("invocation",
                                        toValue(semantic.invocation));
                } else if constexpr (std::same_as<
                                         Input, StatusActionPointerInput>) {
                    addPointer(ClientInputKind::StatusAction);
                    fields.emplace_back("basis_revision",
                                        toValue(semantic.basis.observedRevision));
                    fields.emplace_back("invocation",
                                        toValue(semantic.invocation));
                } else if constexpr (std::same_as<
                                         Input, PublishedUiActionPointerInput>) {
                    addPointer(ClientInputKind::PublishedUiAction);
                    fields.emplace_back("basis_revision",
                                        toValue(semantic.basis.observedRevision));
                    fields.emplace_back(
                        "schema_generation",
                        ProtocolValue::makeUint(
                            semantic.schemaGeneration.value()));
                    fields.emplace_back(
                        "node_id",
                        ProtocolValue::makeText(semantic.nodeId.value()));
                } else if constexpr (std::same_as<
                                         Input, NoticeActionPointerInput>) {
                    addPointer(ClientInputKind::NoticeAction);
                    fields.emplace_back("basis_revision",
                                        toValue(semantic.basis.observedRevision));
                    fields.emplace_back("action_id", toValue(semantic.actionId));
                } else if constexpr (std::same_as<
                                         Input, DocumentPointerInput>) {
                    addPointer(ClientInputKind::Document);
                    fields.emplace_back("basis_revision",
                                        toValue(semantic.basis.observedRevision));
                    fields.emplace_back("position",
                                        toValue(semantic.position));
                    fields.emplace_back("additive",
                                        toValue(semantic.additive));
                    fields.emplace_back("select_word",
                                        toValue(semantic.selectWord));
                    fields.emplace_back("edge", toValue(semantic.edge));
                }
            }
        },
        input);
    return encodeMessage(ProtocolMessageKind::ClientInput,
                         ProtocolValue::makeObject(std::move(fields)));
}

DecodeClientInputResult ProtocolCodec::decodeClientInput(
    std::string_view bytes, ProtocolLimits limits) const {
    auto decoded =
        decodeMessage(bytes, ProtocolMessageKind::ClientInput, limits);
    if (decoded.error != ProtocolError::None) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    auto const& payload = *decoded.payload;
    auto kind = requireField<ClientInputKind>(payload.field("kind"));
    if (!payload.asObject() || !kind) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "client input payload is malformed"};
    }
    const auto hasExactly = [&](std::initializer_list<std::string_view> names) {
        const auto* fields = payload.asObject();
        if (fields == nullptr || fields->size() != names.size()) return false;
        return std::all_of(
            names.begin(), names.end(),
            [&](std::string_view name) { return payload.field(name) != nullptr; });
    };
    if (*kind == ClientInputKind::Key) {
        if (!hasExactly({"kind", "stroke", "committed_text"})) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client key input fields are malformed"};
        }
        auto const* strokeField = payload.field("stroke");
        auto text = requireField<std::string>(payload.field("committed_text"));
        if (strokeField == nullptr || !text) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                   "client key input payload is malformed"};
        }
        KeyStroke stroke;
        if (strokeField->kind() != ProtocolValue::Kind::NullValue) {
            auto decodedStroke = requireField<KeyStroke>(strokeField);
            if (!decodedStroke) {
                return {ProtocolError::MalformedMessage, std::nullopt,
                       "client input stroke is malformed"};
            }
            stroke = *decodedStroke;
        }
        return {ProtocolError::None,
                ClientInput{ClientKeyInput{stroke, std::move(*text)}}, {}};
    }
    if (*kind == ClientInputKind::ScrollLines) {
        if (!hasExactly(
                {"kind", "basis_revision", "target", "rows"})) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client line-scroll input fields are malformed"};
        }
        auto basis =
            requireField<Revision>(payload.field("basis_revision"));
        auto target =
            requireField<SemanticScrollTarget>(payload.field("target"));
        auto rows = requireField<std::int64_t>(payload.field("rows"));
        if (!basis || !target || !rows || *rows == 0) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client line-scroll input is malformed"};
        }
        return {ProtocolError::None,
                ClientInput{ScrollLinesInput{{*basis}, *target, *rows}}, {}};
    }
    if (*kind == ClientInputKind::ScrollFraction) {
        if (!hasExactly({"kind", "basis_revision", "target", "numerator",
                         "denominator"})) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client fraction-scroll input fields are malformed"};
        }
        auto basis =
            requireField<Revision>(payload.field("basis_revision"));
        auto target =
            requireField<SemanticScrollTarget>(payload.field("target"));
        auto numerator =
            requireField<std::uint32_t>(payload.field("numerator"));
        auto denominator =
            requireField<std::uint32_t>(payload.field("denominator"));
        if (!basis || !target || !numerator || !denominator ||
            *denominator == 0 || *numerator > *denominator) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client fraction-scroll input is malformed"};
        }
        return {
            ProtocolError::None,
            ClientInput{ScrollFractionInput{
                {*basis}, *target, *numerator, *denominator}},
            {}};
    }
    if (*kind == ClientInputKind::ViewNavigation) {
        if (!hasExactly({"kind", "basis_revision"})) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client view-navigation input fields are malformed"};
        }
        auto basis =
            requireField<Revision>(payload.field("basis_revision"));
        if (!basis) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client view-navigation input is malformed"};
        }
        return {ProtocolError::None,
                ClientInput{ViewNavigationInput{{*basis}}}, {}};
    }
    if (*kind == ClientInputKind::ResolvedPaneFocus) {
        if (!hasExactly({"kind", "basis_revision"})) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client resolved-pane-focus input fields are malformed"};
        }
        auto basis =
            requireField<Revision>(payload.field("basis_revision"));
        if (!basis) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client resolved-pane-focus input is malformed"};
        }
        return {ProtocolError::None,
                ClientInput{ResolvedPaneFocusInput{{*basis}}}, {}};
    }
    if (*kind == ClientInputKind::ResolvedSelection) {
        if (!hasExactly({"kind", "basis_revision", "active_tab",
                        "document_revision", "selections"})) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                   "client resolved-selection input fields are malformed"};
        }
        auto basis =
            requireField<Revision>(payload.field("basis_revision"));
        auto activeTab =
            requireField<TabId>(payload.field("active_tab"));
        auto documentRevision =
            requireField<Revision>(payload.field("document_revision"));
        auto selections =
            requireField<std::vector<ResolvedSelectionRange>>(
                payload.field("selections"));
        if (!basis || !activeTab || !documentRevision || !selections ||
            selections->empty()) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                   "client resolved-selection input is malformed"};
        }
        return {
            ProtocolError::None,
            ClientInput{ResolvedSelectionInput{
                {*basis}, *activeTab, *documentRevision,
                std::move(*selections)}},
            {}};
    }
    auto button =
        requireField<InputPointerButton>(payload.field("button"));
    auto phase = requireField<InputPointerPhase>(payload.field("phase"));
    if (!button || !phase) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "client pointer input gesture is malformed"};
    }
    if (*kind == ClientInputKind::Picker) {
        if (!hasExactly({"kind", "button", "phase", "picker_mode",
                         "activation_id", "candidate_id"})) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client picker input fields are malformed"};
        }
        auto mode = requireField<SearchMode>(payload.field("picker_mode"));
        auto activationId =
            requireField<std::uint64_t>(payload.field("activation_id"));
        auto candidate =
            requireField<std::string>(payload.field("candidate_id"));
        if (!mode || !activationId || *activationId == 0 || !candidate ||
            candidate->empty()) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                   "client picker input target is malformed"};
        }
        return {
            ProtocolError::None,
            ClientInput{PickerPointerInput{
                PickerActivation{*mode, PickerActivationId{*activationId}},
                std::move(*candidate), *button, *phase}},
            {}};
    }
    auto basis =
        requireField<Revision>(payload.field("basis_revision"));
    if (!basis) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "client pointer input basis is malformed"};
    }
    const SemanticInputBasis semanticBasis{*basis};
    if (*kind == ClientInputKind::Tab) {
        if (!hasExactly(
                {"kind", "button", "phase", "basis_revision", "tab_id"})) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client tab input fields are malformed"};
        }
        auto id = requireField<TabId>(payload.field("tab_id"));
        if (id) {
            return {ProtocolError::None,
                   ClientInput{TabPointerInput{
                       semanticBasis, *id, *button, *phase}},
                   {}};
        }
    } else if (*kind == ClientInputKind::Tree) {
        if (!hasExactly(
                {"kind", "button", "phase", "basis_revision", "node_id"})) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client tree input fields are malformed"};
        }
        auto id = requireField<TreeNodeId>(payload.field("node_id"));
        if (id) {
            return {ProtocolError::None,
                   ClientInput{TreePointerInput{
                       semanticBasis, std::move(*id), *button, *phase}},
                   {}};
        }
    } else if (*kind == ClientInputKind::PromptControl) {
        if (!hasExactly({"kind", "button", "phase", "basis_revision",
                         "control_id"})) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client prompt-control input fields are malformed"};
        }
        auto id = requireField<std::string>(payload.field("control_id"));
        if (id && !id->empty()) {
            return {ProtocolError::None,
                   ClientInput{PromptControlPointerInput{
                       semanticBasis, std::move(*id), *button, *phase}},
                   {}};
        }
    } else if (*kind == ClientInputKind::ExternalAction) {
        if (!hasExactly({"kind", "button", "phase", "basis_revision",
                         "invocation"})) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client external-action input fields are malformed"};
        }
        auto invocation = requireField<ExternalActionInvocation>(
            payload.field("invocation"));
        if (invocation) {
            return {ProtocolError::None,
                   ClientInput{ExternalActionPointerInput{
                       semanticBasis, std::move(*invocation), *button, *phase}},
                   {}};
        }
    } else if (*kind == ClientInputKind::StatusAction) {
        if (!hasExactly({"kind", "button", "phase", "basis_revision",
                         "invocation"})) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client status-action input fields are malformed"};
        }
        auto invocation = requireField<StatusActionInvocation>(
            payload.field("invocation"));
        if (invocation) {
            return {ProtocolError::None,
                   ClientInput{StatusActionPointerInput{
                       semanticBasis, std::move(*invocation), *button, *phase}},
                   {}};
        }
    } else if (*kind == ClientInputKind::PublishedUiAction) {
        if (!hasExactly({"kind", "button", "phase", "basis_revision",
                         "schema_generation", "node_id"})) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client UI-action input fields are malformed"};
        }
        auto generation =
            requireField<std::uint64_t>(payload.field("schema_generation"));
        auto id = requireField<std::string>(payload.field("node_id"));
        if (generation && id && !id->empty()) {
            return {ProtocolError::None,
                   ClientInput{PublishedUiActionPointerInput{
                       semanticBasis, Generation{*generation},
                       UiNodeId{std::move(*id)}, *button, *phase}},
                   {}};
        }
    } else if (*kind == ClientInputKind::NoticeAction) {
        if (!hasExactly({"kind", "button", "phase", "basis_revision",
                         "action_id"})) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client notice-action input fields are malformed"};
        }
        auto id = requireField<std::string>(payload.field("action_id"));
        if (id && !id->empty()) {
            return {ProtocolError::None,
                   ClientInput{NoticeActionPointerInput{
                       semanticBasis, std::move(*id), *button, *phase}},
                   {}};
        }
    } else if (*kind == ClientInputKind::Document) {
        if (!hasExactly({"kind", "button", "phase", "basis_revision",
                         "position", "additive", "select_word", "edge"})) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client document input fields are malformed"};
        }
        std::optional<ByteOffset> position;
        auto const* positionField = payload.field("position");
        if (positionField == nullptr) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client document input target is malformed"};
        }
        if (positionField->kind() != ProtocolValue::Kind::NullValue) {
            auto decodedPosition = requireField<ByteOffset>(positionField);
            if (!decodedPosition) {
                return {ProtocolError::MalformedMessage, std::nullopt,
                        "client document input target is malformed"};
            }
            position = *decodedPosition;
        }
        auto additive = requireField<bool>(payload.field("additive"));
        auto selectWord = requireField<bool>(payload.field("select_word"));
        auto edge =
            requireField<DocumentPointerEdge>(payload.field("edge"));
        const auto hasEdge = edge && *edge != DocumentPointerEdge::None;
        if (!additive || !selectWord || !edge ||
            (*phase == InputPointerPhase::Press && (!position || hasEdge)) ||
            (*phase == InputPointerPhase::Move &&
             (position.has_value() == hasEdge)) ||
            ((*phase == InputPointerPhase::Release ||
              *phase == InputPointerPhase::Cancel) &&
             (position || hasEdge)) ||
            (*phase != InputPointerPhase::Press &&
             (*selectWord || *additive))) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client document input gesture is malformed"};
        }
        return {ProtocolError::None,
                ClientInput{DocumentPointerInput{
                    semanticBasis, position, *additive, *selectWord,
                    *button, *phase, *edge}},
                {}};
    }
    return {ProtocolError::MalformedMessage, std::nullopt,
            "client pointer input target is malformed"};
}

std::string ProtocolCodec::encodeClientInputResult(
    ClientInputResult const& result) const {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back(
        "outcome", ProtocolValue::makeUint(
                       static_cast<std::uint8_t>(result.outcome)));
    if (result.clientOwned) {
        std::vector<ProtocolValue::Field> owned;
        owned.emplace_back(
            "kind", ProtocolValue::makeUint(
                        static_cast<std::uint8_t>(result.clientOwned->kind)));
        owned.emplace_back("text", toValue(result.clientOwned->text));
        fields.emplace_back("client_owned",
                            ProtocolValue::makeObject(std::move(owned)));
    } else {
        fields.emplace_back("client_owned", ProtocolValue::makeNull());
    }
    fields.emplace_back(
        "command", result.command ? commandResultValue(*result.command)
                                   : ProtocolValue::makeNull());
    if (result.pickerActivation) {
        fields.emplace_back(
            "picker_activation",
            ProtocolValue::makeObject(
                {{"mode", ProtocolValue::makeUint(static_cast<std::uint8_t>(
                              result.pickerActivation->mode))},
                 {"activation_id",
                  ProtocolValue::makeUint(
                      result.pickerActivation->id.value())}}));
    } else {
        fields.emplace_back("picker_activation", ProtocolValue::makeNull());
    }
    return encodeMessage(ProtocolMessageKind::ClientInputResult,
                         ProtocolValue::makeObject(std::move(fields)));
}

DecodeClientInputResultResult ProtocolCodec::decodeClientInputResult(
    std::string_view bytes, ProtocolLimits limits) const {
    auto decoded = decodeMessage(
        bytes, ProtocolMessageKind::ClientInputResult, limits);
    if (decoded.error != ProtocolError::None) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    auto const& payload = *decoded.payload;
    auto outcome = requireField<std::uint8_t>(payload.field("outcome"));
    if (!payload.asObject() || !outcome ||
        *outcome > static_cast<std::uint8_t>(ClientInputOutcome::ViewOwned)) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "client input result payload is malformed"};
    }
    ClientInputResult result{static_cast<ClientInputOutcome>(*outcome),
                             std::nullopt, std::nullopt, std::nullopt};
    auto const* ownedField = payload.field("client_owned");
    if (ownedField == nullptr) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "client input result is missing client_owned"};
    }
    if (ownedField->kind() != ProtocolValue::Kind::NullValue) {
        auto kind = requireField<std::uint8_t>(ownedField->field("kind"));
        auto text = requireField<std::string>(ownedField->field("text"));
        if (!ownedField->asObject() || !kind || !text ||
            *kind > static_cast<std::uint8_t>(
                        ClientOwnedInputKind::Submit)) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client input result client_owned is malformed"};
        }
        result.clientOwned = ClientOwnedInput{
            static_cast<ClientOwnedInputKind>(*kind), std::move(*text)};
    }
    auto const* activationField = payload.field("picker_activation");
    if (activationField == nullptr) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "client input result is missing picker_activation"};
    }
    if (activationField->kind() != ProtocolValue::Kind::NullValue) {
        auto mode = requireField<SearchMode>(activationField->field("mode"));
        auto id =
            requireField<std::uint64_t>(activationField->field("activation_id"));
        if (!activationField->asObject() || !mode || !id || *id == 0 ||
            (*mode != SearchMode::Command && *mode != SearchMode::File)) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client input result picker activation is malformed"};
        }
        result.pickerActivation =
            PickerActivation{*mode, PickerActivationId{*id}};
    }
    auto const* commandField = payload.field("command");
    if (commandField == nullptr) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "client input result is missing command"};
    }
    if (commandField->kind() != ProtocolValue::Kind::NullValue) {
        result.command = commandResultFromValue(*commandField);
        if (!result.command) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client input result command is malformed"};
        }
    }
    const bool commandRequiresView =
        result.command && result.command->viewAction;
    const bool validShape =
        (result.outcome == ClientInputOutcome::ViewOwned &&
         commandRequiresView) ||
        (result.outcome != ClientInputOutcome::ViewOwned &&
         !commandRequiresView);
    if (!validShape) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "client input result outcome does not match its view action"};
    }
    return {ProtocolError::None, std::move(result), {}};
}

std::string ProtocolCodec::encodeSessionSnapshot(SessionSnapshot const& snapshot) const {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(snapshot.revision()));
    fields.emplace_back("topology", toValue(snapshot.topology()));
    fields.emplace_back("client", toValue(snapshot.client()));
    fields.emplace_back("sections", toValue(snapshot.sections()));
    fields.emplace_back(
        "presentation",
        toValue(std::optional<PresentationSnapshot>{}));
    return encodeMessage(ProtocolMessageKind::SessionSnapshot,
                          ProtocolValue::makeObject(std::move(fields)));
}

DecodeSessionSnapshotResult ProtocolCodec::decodeSessionSnapshot(std::string_view bytes,
                                                    ProtocolLimits limits) const {
    auto decoded =
        decodeMessage(bytes, ProtocolMessageKind::SessionSnapshot, limits);
    if (decoded.error != ProtocolError::None) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    auto const& payload = *decoded.payload;
    if (!payload.asObject()) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "session snapshot payload is not an object"};
    }
    auto revision = requireField<Revision>(payload.field("revision"));
    auto topology = requireField<SessionTopology>(payload.field("topology"));
    auto client = requireField<ClientSnapshotState>(payload.field("client"));
    auto sections =
        requireField<SessionSnapshotSections>(payload.field("sections"));
    std::optional<PresentationSnapshot> presentation;
    if (!decodeOptionalField(payload.field("presentation"), presentation)) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "session snapshot presentation is malformed"};
    }
    (void)presentation;
    if (!revision || !topology || !client || !sections) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "session snapshot payload is malformed"};
    }
    return {ProtocolError::None,
            SessionSnapshot{*revision, std::move(*topology),
                            std::move(*client), std::move(*sections)},
            {}};
}

std::string ProtocolCodec::encodeLegacyPresentationSnapshot(
    LegacyPresentationSnapshot const& snapshot) const {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(snapshot.semantic().revision()));
    fields.emplace_back("topology", toValue(snapshot.semantic().topology()));
    fields.emplace_back("client", toValue(snapshot.semantic().client()));
    fields.emplace_back("sections", toValue(snapshot.semantic().sections()));
    fields.emplace_back(
        "presentation",
        toValue(std::optional<PresentationSnapshot>{
            snapshot.presentation()}));
    return encodeMessage(ProtocolMessageKind::SessionSnapshot,
                         ProtocolValue::makeObject(std::move(fields)));
}

DecodeLegacyPresentationSnapshotResult
ProtocolCodec::decodeLegacyPresentationSnapshot(
    std::string_view bytes, ProtocolLimits limits) const {
    auto decoded =
        decodeMessage(bytes, ProtocolMessageKind::SessionSnapshot, limits);
    if (decoded.error != ProtocolError::None) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    auto const& payload = *decoded.payload;
    if (!payload.asObject()) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "session snapshot payload is not an object"};
    }
    auto revision = requireField<Revision>(payload.field("revision"));
    auto topology = requireField<SessionTopology>(payload.field("topology"));
    auto client = requireField<ClientSnapshotState>(payload.field("client"));
    auto sections =
        requireField<SessionSnapshotSections>(payload.field("sections"));
    std::optional<PresentationSnapshot> presentation;
    if (!decodeOptionalField(payload.field("presentation"), presentation) ||
        !presentation || !revision || !topology || !client || !sections) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "legacy presentation snapshot payload is malformed"};
    }
    return {
        ProtocolError::None,
        LegacyPresentationSnapshot{
            SessionSnapshot{*revision, std::move(*topology),
                            std::move(*client), std::move(*sections)},
            std::move(*presentation)},
        {}};
}

std::string ProtocolCodec::encodeSessionDelta(SessionDelta const& delta) const {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(delta.baseRevision()));
    fields.emplace_back("revision", toValue(delta.revision()));
    fields.emplace_back("client_id", toValue(delta.clientId()));
    fields.emplace_back("view_id", toValue(delta.viewId()));
    fields.emplace_back("capabilities", toValue(delta.capabilities()));
    fields.emplace_back("topology", toValue(delta.topology()));
    fields.emplace_back(kSemanticSessionDeltaFields[0], toValue(delta.document()));
    fields.emplace_back(kSemanticSessionDeltaFields[1], toValue(delta.documentCaret()));
    fields.emplace_back(kSemanticSessionDeltaFields[2], toValue(delta.selection()));
    fields.emplace_back(kSemanticSessionDeltaFields[3], toValue(delta.history()));
    fields.emplace_back(kSemanticSessionDeltaFields[4], toValue(delta.clipboard()));
    fields.emplace_back(kSemanticSessionDeltaFields[5], toValue(delta.promptStatus()));
    fields.emplace_back(kSemanticSessionDeltaFields[6], toValue(delta.search()));
    fields.emplace_back(kSemanticSessionDeltaFields[7], toValue(delta.findReplace()));
    fields.emplace_back(kSemanticSessionDeltaFields[8], toValue(delta.settings()));
    fields.emplace_back(kSemanticSessionDeltaFields[9], toValue(delta.keymap()));
    fields.emplace_back(kSemanticSessionDeltaFields[10], toValue(delta.textEncoding()));
    fields.emplace_back(kSemanticSessionDeltaFields[11], toValue(delta.tabs()));
    fields.emplace_back(kSemanticSessionDeltaFields[12], toValue(delta.diff()));
    fields.emplace_back(kSemanticSessionDeltaFields[13],
                        toValue(delta.externalModification()));
    fields.emplace_back(kSemanticSessionDeltaFields[14], toValue(delta.followEdits()));
    fields.emplace_back(kSemanticSessionDeltaFields[15], toValue(delta.tree()));
    fields.emplace_back(kSemanticSessionDeltaFields[16], toValue(delta.syntax()));
    fields.emplace_back(kSemanticSessionDeltaFields[17], toValue(delta.lspSync()));
    fields.emplace_back(kSemanticSessionDeltaFields[18], toValue(delta.lspFeatures()));
    fields.emplace_back(kSemanticSessionDeltaFields[19], toValue(delta.theme()));
    fields.emplace_back("style", toValue(StyleSectionDelta{}));
    fields.emplace_back("shell", toValue(ShellSectionDelta{}));
    fields.emplace_back("viewport",
                        toValue(ViewportDelta{false, std::nullopt}));
    fields.emplace_back(kSemanticSessionDeltaFields[20],
                        toValue(delta.legacyFocus()));
    fields.emplace_back("selection_nav",
                        toValue(SelectionNavigationDelta{}));
    fields.emplace_back("prompt_projection",
                        toValue(PromptProjectionDelta{}));
    fields.emplace_back("tree_windows", toValue(TreeWindowsDelta{}));
    fields.emplace_back(kSemanticSessionDeltaFields[22],
                        encodeUiFrameDelta(delta.uiFrameDelta()));
    fields.emplace_back(kSemanticSessionDeltaFields[21],
                        delta.palette().replacement
                            ? encodePalette(*delta.palette().replacement)
                            : ProtocolValue::makeNull());
    fields.emplace_back(kSemanticSessionDeltaFields[23],
                        toValue(delta.legacyPromptView()));
    fields.emplace_back(kSemanticSessionDeltaFields[24], toValue(delta.noticeView()));
    // Additive: present only when watcher availability flipped (Decision 13). An
    // absent field means "unchanged" for a peer that predates it.
    fields.emplace_back(kSemanticSessionDeltaFields[25],
                        toValue(delta.watcherAvailable()));
    // Additive: present only when the external-focus-held state flipped. An absent
    // field means "unchanged" for a peer that predates it.
    fields.emplace_back(kSemanticSessionDeltaFields[26],
                        toValue(delta.legacyExternalFocusHeld()));
    return encodeMessage(ProtocolMessageKind::SessionDelta,
                          ProtocolValue::makeObject(std::move(fields)));
}

DecodeSessionDeltaResult ProtocolCodec::decodeSessionDelta(std::string_view bytes,
                                              ProtocolLimits limits) const {
    auto decoded =
        decodeMessage(bytes, ProtocolMessageKind::SessionDelta, limits);
    if (decoded.error != ProtocolError::None) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    auto const& payload = *decoded.payload;
    if (!payload.asObject()) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "session delta payload is not an object"};
    }

    auto baseRevision = requireField<Revision>(payload.field("base_revision"));
    auto revision = requireField<Revision>(payload.field("revision"));
    auto clientId = requireField<ClientId>(payload.field("client_id"));
    auto viewId = requireField<ViewId>(payload.field("view_id"));
    auto capabilities =
        requireField<std::vector<CapabilityId>>(payload.field("capabilities"));

    std::optional<SessionTopology> topology;
    std::optional<DocumentDelta> document;
    std::optional<ByteOffset> documentCaret;
    std::optional<TextEncodingDelta> textEncoding;
    std::optional<FocusTarget> focus;
    std::optional<bool> watcherAvailable;
    std::optional<bool> externalFocusHeld;
    bool const optionalOk =
        decodeOptionalField(payload.field("topology"), topology) &&
        decodeOptionalField(payload.field("document"), document) &&
        decodeOptionalField(payload.field("document_caret"), documentCaret) &&
        decodeOptionalField(payload.field("focus"), focus) &&
        decodeOptionalField(payload.field("watcher_available"), watcherAvailable) &&
        decodeOptionalField(payload.field("external_focus_held"), externalFocusHeld) &&
        decodeOptionalField(payload.field("text_encoding"), textEncoding);

    auto selection = requireField<SelectionSetDelta>(payload.field("selection"));
    auto history = requireField<HistoryDelta>(payload.field("history"));
    auto clipboard = requireField<ClipboardDelta>(payload.field("clipboard"));
    auto promptStatus =
        requireField<PromptStatusDelta>(payload.field("prompt_status"));
    auto search = requireField<SearchDelta>(payload.field("search"));
    auto findReplace =
        requireField<FindReplaceDelta>(payload.field("find_replace"));
    auto settings =
        requireField<SettingsSectionDelta>(payload.field("settings"));
    auto keymap = requireField<KeymapDelta>(payload.field("keymap"));
    auto tabs = requireField<TabDelta>(payload.field("tabs"));
    auto diff = requireField<DiffDelta>(payload.field("diff"));
    auto externalModification = requireField<ExternalModificationDelta>(
        payload.field("external_modification"));
    auto followEdits =
        requireField<FollowEditsDelta>(payload.field("follow_edits"));
    auto tree = requireField<TreeDelta>(payload.field("tree"));
    auto syntax = requireField<SyntaxDelta>(payload.field("syntax"));
    auto lspSync = requireField<LspSyncDelta>(payload.field("lsp_sync"));
    auto lspFeatures =
        requireField<LspFeatureDelta>(payload.field("lsp_features"));
    auto theme = requireField<ThemeSectionDelta>(payload.field("theme"));
    auto style = requireField<StyleSectionDelta>(payload.field("style"));
    auto shell = requireField<ShellSectionDelta>(payload.field("shell"));
    auto viewport = requireField<ViewportDelta>(payload.field("viewport"));
    auto selectionNav = requireField<SelectionNavigationDelta>(payload.field("selection_nav"));
    auto promptProjection = requireField<PromptProjectionDelta>(payload.field("prompt_projection"));
    auto treeWindows = requireField<TreeWindowsDelta>(payload.field("tree_windows"));
    const ProtocolValue* frameDeltaField = payload.field("ui_frame_delta");
    const ProtocolValue* uiField = payload.field("ui");
    const ProtocolValue* uiStateField = payload.field("ui_state");
    const ProtocolValue* uiPresenceField = payload.field("ui_presence");
    if (frameDeltaField && (uiField || uiStateField || uiPresenceField)) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "session delta mixes UI frame representations"};
    }
    std::optional<UiFrameDelta> uiFrameDelta;
    if (frameDeltaField) {
        uiFrameDelta = decodeUiFrameDelta(*frameDeltaField);
        if (!uiFrameDelta) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "session delta UI frame is malformed"};
        }
    }
    LegacyUiFrameChanges legacyUi;
    if (uiField) {
        if (uiField->kind() != ProtocolValue::Kind::NullValue) {
            auto ui = decodeUiSchema(*uiField);
            if (!ui) {
                return {ProtocolError::MalformedMessage, std::nullopt,
                        "session delta payload is malformed"};
            }
            annotateLegacyFocusHosts(ui->root);
            legacyUi.schema = std::move(*ui);
        }
    }
    if (uiStateField) {
        if (uiStateField->kind() != ProtocolValue::Kind::NullValue) {
            auto uiState = decodeUiState(*uiStateField);
            if (!uiState) {
                return {ProtocolError::MalformedMessage, std::nullopt,
                        "session delta payload is malformed"};
            }
            legacyUi.state = std::move(*uiState);
        }
    }
    if (uiPresenceField) {
        if (uiPresenceField->kind() != ProtocolValue::Kind::NullValue) {
            auto uiPresence = decodeUiPresence(*uiPresenceField);
            if (!uiPresence) {
                return {ProtocolError::MalformedMessage, std::nullopt,
                        "session delta payload is malformed"};
            }
            legacyUi.presence = std::move(*uiPresence);
        }
    }
    if (legacyUi.state) {
        normalizeLegacyTreeRecords(*legacyUi.state);
    }
    if (legacyUi.presence) {
        normalizeLegacyTreeRecords(*legacyUi.presence);
    }
    if (!uiFrameDelta) {
        if (legacyUi.schema) {
            if (legacyUi.state) {
                canonicalizeUiRecordOrder(*legacyUi.schema,
                                          legacyUi.state->nodes);
            }
            if (legacyUi.presence) {
                canonicalizeUiRecordOrder(*legacyUi.schema,
                                          legacyUi.presence->nodes);
            }
        }
        uiFrameDelta =
            UiFrameDelta::legacyChanges(std::move(legacyUi));
    }
    PaletteSectionDelta paletteDelta;
    if (const ProtocolValue* paletteField = payload.field("palette")) {
        if (paletteField->kind() != ProtocolValue::Kind::NullValue) {
            auto palette = decodePalette(*paletteField);
            if (!palette) {
                return {ProtocolError::MalformedMessage, std::nullopt,
                        "session delta payload is malformed"};
            }
            paletteDelta.replacement = std::move(*palette);
        }
    }
    // Additive: an absent prompt_view field means "unchanged" (changed=false), so
    // a delta from a peer that predates the field never spuriously closes the
    // prompt; a present-but-malformed field fails loud.
    LegacyPromptViewDelta promptViewDelta;
    if (const ProtocolValue* promptViewField = payload.field("prompt_view")) {
        std::optional<LegacyPromptViewDelta> decoded;
        if (!decodePresent(*promptViewField, decoded)) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "session delta payload is malformed"};
        }
        promptViewDelta = std::move(*decoded);
    }
    // Additive: an absent notice_view field means "unchanged" (changed=false), so a
    // delta from a peer that predates the field never spuriously clears the notice; a
    // present-but-malformed field fails loud.
    NoticeViewSectionDelta noticeViewDelta;
    if (const ProtocolValue* noticeViewField = payload.field("notice_view")) {
        std::optional<NoticeViewSectionDelta> decoded;
        if (!decodePresent(*noticeViewField, decoded)) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "session delta payload is malformed"};
        }
        noticeViewDelta = std::move(*decoded);
    }

    if (!optionalOk || !baseRevision || !revision || !clientId || !viewId ||
        !capabilities || !selection || !history || !clipboard ||
        !promptStatus || !search || !findReplace || !settings || !keymap ||
        !tabs || !diff || !externalModification || !followEdits || !tree ||
        !syntax || !lspSync || !lspFeatures || !theme || !style || !shell ||
        !viewport || !selectionNav || !promptProjection || !treeWindows) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "session delta payload is malformed"};
    }

    return {ProtocolError::None,
            SessionSnapshotCodec{}.decodeWire(
                *baseRevision, *revision, *clientId, *viewId,
                std::move(*capabilities), std::move(topology),
                std::move(document), std::move(documentCaret),
                std::move(*selection), std::move(*history),
                std::move(*clipboard), std::move(*promptStatus),
                std::move(*search), std::move(*findReplace),
                std::move(*settings), std::move(*keymap),
                std::move(textEncoding), std::move(*tabs), std::move(*diff),
                std::move(*externalModification), std::move(*followEdits),
                std::move(*tree), std::move(*syntax), std::move(*lspSync),
                std::move(*lspFeatures), std::move(*theme),
                std::move(*style),
                std::move(*shell), std::move(*viewport),
                std::move(*uiFrameDelta), std::move(focus),
                std::move(*selectionNav), std::move(*promptProjection),
                std::move(*treeWindows), std::move(paletteDelta),
                std::move(promptViewDelta),
                std::move(noticeViewDelta), watcherAvailable, externalFocusHeld),
            {}};
}

std::vector<std::string> styleWireFieldNames() {
    auto const encoded = toValue(Style{});
    std::vector<std::string> names;
    if (auto const* object = encoded.asObject()) {
        names.reserve(object->size());
        for (auto const& [key, _] : *object) names.push_back(key);
    }
    return names;
}

std::vector<std::string> semanticSessionWireFieldNames() {
    std::vector<std::string> names;
    names.reserve(kSemanticSessionFields.size());
    for (auto const name : kSemanticSessionFields) names.emplace_back(name);
    return names;
}

std::vector<std::string> semanticSessionDeltaWireFieldNames() {
    std::vector<std::string> names;
    names.reserve(kSemanticSessionDeltaFields.size());
    for (auto const name : kSemanticSessionDeltaFields) names.emplace_back(name);
    return names;
}

}  // namespace ssg
