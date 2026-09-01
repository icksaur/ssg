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
#include <ssg/detail/generated/wire_schema.h>

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

#include "protocol/codec_detail.h"

namespace ssg {

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

namespace protocol_detail {

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

namespace protocol_detail {
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
// Enum decodePresent() definitions, each delegating to decode_enum() with
// the closed set of valid values for that enum.

bool decodePresent(ProtocolValue const& value, std::optional<DocumentMode>& out) {
    return decodeEnum(
        value, out, detail::generated::kDocumentModeWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<StatusPriority>& out) {
    return decodeEnum(
        value, out, detail::generated::kStatusPriorityWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<PromptKind>& out) {
    return decodeEnum(value, out, detail::generated::kPromptKindWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<PromptControlKind>& out) {
    return decodeEnum(
        value, out, detail::generated::kPromptControlKindWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<SearchMode>& out) {
    return decodeEnum(value, out, detail::generated::kSearchModeWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<FindReplaceError>& out) {
    return decodeEnum(
        value, out, detail::generated::kFindReplaceErrorWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<SettingScope>& out) {
    return decodeEnum(value, out, detail::generated::kSettingScopeWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<SettingKey>& out) {
    return decodeEnum(value, out, detail::generated::kSettingKeyWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<TextEncoding>& out) {
    return decodeEnum(value, out, detail::generated::kTextEncodingWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<IndentStyle>& out) {
    return decodeEnum(value, out, detail::generated::kIndentStyleWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<LineEnding>& out) {
    return decodeEnum(value, out, detail::generated::kLineEndingWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<TabKind>& out) {
    return decodeEnum(value, out, detail::generated::kTabKindWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<TabRecoveryBadge>& out) {
    return decodeEnum(
        value, out, detail::generated::kTabRecoveryBadgeWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<JournalDocumentKeyKind>& out) {
    return decodeEnum(
        value, out, detail::generated::kJournalDocumentKeyKindWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<DiffLineKind>& out) {
    return decodeEnum(value, out, detail::generated::kDiffLineKindWireValues);
}
bool decodePresent(ProtocolValue const& value, std::optional<DiffFileStatus>& out) {
    return decodeEnum(value, out, detail::generated::kDiffFileStatusWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<ExternalAction>& out) {
    return decodeEnum(value, out, detail::generated::kExternalActionWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<ExternalDocumentStatus>& out) {
    return decodeEnum(
        value, out, detail::generated::kExternalDocumentStatusWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<FollowMode>& out) {
    return decodeEnum(value, out, detail::generated::kFollowModeWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<TreeProviderKind>& out) {
    return decodeEnum(
        value, out, detail::generated::kTreeProviderKindWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<TreeNodeKind>& out) {
    return decodeEnum(value, out, detail::generated::kTreeNodeKindWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<GitTreeStatus>& out) {
    return decodeEnum(value, out, detail::generated::kGitTreeStatusWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<SyntaxScope>& out) {
    return decodeEnum(value, out, detail::generated::kSyntaxScopeWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<BracketKind>& out) {
    return decodeEnum(value, out, detail::generated::kBracketKindWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<BracketRole>& out) {
    return decodeEnum(value, out, detail::generated::kBracketRoleWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<CommentKind>& out) {
    return decodeEnum(value, out, detail::generated::kCommentKindWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<CommentTokenRole>& out) {
    return decodeEnum(
        value, out, detail::generated::kCommentTokenRoleWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<LspDiagnosticSeverity>& out) {
    return decodeEnum(
        value, out, detail::generated::kLspDiagnosticSeverityWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<FocusTarget>& out) {
    static constexpr std::array values{FocusTarget::Editor, FocusTarget::Panel,
                                       FocusTarget::Prompt};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<SemanticRole>& out) {
    return decodeEnum(value, out, detail::generated::kSemanticRoleWireValues);
}

bool decodePresent(ProtocolValue const& value, std::optional<ClientInputKind>& out) {
    return decodeEnum(
        value, out, detail::generated::kClientInputKindWireValues);
}

bool decodePresent(ProtocolValue const& value,
                   std::optional<InputPointerButton>& out) {
    return decodeEnum(
        value, out, detail::generated::kInputPointerButtonWireValues);
}

bool decodePresent(ProtocolValue const& value,
                   std::optional<InputPointerPhase>& out) {
    return decodeEnum(
        value, out, detail::generated::kInputPointerPhaseWireValues);
}

bool decodePresent(ProtocolValue const& value,
                   std::optional<DocumentPointerEdge>& out) {
    return decodeEnum(
        value, out, detail::generated::kDocumentPointerEdgeWireValues);
}

bool decodePresent(ProtocolValue const& value,
                   std::optional<SemanticScrollTarget>& out) {
    return decodeEnum(
        value, out, detail::generated::kSemanticScrollTargetWireValues);
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
    if (!detail::generated::validateExternalActionInvocationWire(value)) {
        return false;
    }
    auto fileId = requireField<DiffFileId>(value.field("file_id"));
    const auto action =
        static_cast<ExternalAction>(*value.field("action")->asUint());
    if (!fileId) return false;
    out.emplace(ExternalActionInvocation{
        *fileId, action});
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
}  // namespace protocol_detail

using namespace protocol_detail;


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
Enum validatedEnumField(ProtocolValue const* value) {
    return static_cast<Enum>(*value->asUint());
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
    if (!detail::generated::validateViewActionWire(value)) {
        return std::nullopt;
    }
    const auto kind = validatedEnumField<ViewActionKind>(value.field("kind"));
    switch (kind) {
    case ViewActionKind::ScrollLines: {
        const auto target =
            validatedEnumField<ViewScrollTarget>(value.field("target"));
        const auto rows = requireField<std::int64_t>(value.field("rows"));
        if (!rows || *rows == 0)
            return std::nullopt;
        return ViewScrollLines{target, *rows};
    }
    case ViewActionKind::ScrollPages: {
        const auto pages = requireField<std::int64_t>(value.field("pages"));
        if (!pages || *pages == 0) return std::nullopt;
        return ViewScrollPages{*pages};
    }
    case ViewActionKind::ScrollFraction: {
        const auto target =
            validatedEnumField<ViewScrollTarget>(value.field("target"));
        const auto numerator =
            requireField<std::uint32_t>(value.field("numerator"));
        const auto denominator =
            requireField<std::uint32_t>(value.field("denominator"));
        if (!numerator || !denominator || *denominator == 0 ||
            *numerator > *denominator) {
            return std::nullopt;
        }
        return ViewScrollFraction{target, *numerator, *denominator};
    }
    case ViewActionKind::MoveVisualSelection: {
        const auto direction =
            validatedEnumField<VisualSelectionDirection>(
                value.field("direction"));
        const auto extend = requireField<bool>(value.field("extend"));
        if (!extend) {
            return std::nullopt;
        }
        return MoveVisualSelection{direction, *extend};
    }
    case ViewActionKind::RevealSelection:
        return RevealSelection{};
    case ViewActionKind::CenterSelection:
        return CenterSelection{};
    case ViewActionKind::SplitPane: {
        return SplitPane{
            validatedEnumField<SplitAxis>(value.field("axis"))};
    }
    case ViewActionKind::ClosePane:
        return ClosePane{};
    case ViewActionKind::CyclePane: {
        return CyclePane{
            validatedEnumField<PaneCycleDirection>(
                value.field("direction"))};
    }
    case ViewActionKind::FocusPane: {
        return FocusPane{
            validatedEnumField<PaneDirection>(value.field("direction"))};
    }
    case ViewActionKind::ContinuePointerEdge: {
        return ContinuePointerEdge{
            validatedEnumField<PointerEdgeDirection>(
                value.field("direction"))};
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
    if (!detail::generated::validateViewActionRequestWire(value)) {
        return std::nullopt;
    }
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
    if (!detail::generated::validateCommandResultWire(payload)) {
        return std::nullopt;
    }
    const auto error =
        validatedEnumField<CommandError>(payload.field("error"));
    auto revision = requireField<Revision>(payload.field("revision"));
    auto message = requireField<std::string>(payload.field("message"));
    if (!revision || !message) {
        return std::nullopt;
    }
    // Additive effects fields: absent on an old peer, so default to false; but a
    // PRESENT field must be a valid boolean 0/1 -- a malformed or out-of-range
    // value is a corrupt message, not a silent false.
    CommandResult result{error, *revision, std::move(*message)};
    if (auto const* routingField = payload.field("routingChanged")) {
        result.effects.routingChanged = *routingField->asUint() != 0;
    }
    if (auto const* geometryField = payload.field("geometryChanged")) {
        result.effects.geometryChanged = *geometryField->asUint() != 0;
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
                auto addPointer = [&](std::uint64_t kind) {
                    fields.emplace_back("kind", ProtocolValue::makeUint(kind));
                    fields.emplace_back("button", toValue(semantic.button));
                    fields.emplace_back("phase", toValue(semantic.phase));
                };
                if constexpr (std::same_as<Input, TabPointerInput>) {
                    addPointer(static_cast<std::uint64_t>(
                        ClientInputKind::Tab));
                    fields.emplace_back("basis_revision",
                                        toValue(semantic.basis.observedRevision));
                    fields.emplace_back("tab_id", toValue(semantic.tabId));
                } else if constexpr (std::same_as<Input, TreePointerInput>) {
                    addPointer(static_cast<std::uint64_t>(
                        ClientInputKind::Tree));
                    fields.emplace_back("basis_revision",
                                        toValue(semantic.basis.observedRevision));
                    fields.emplace_back("node_id", toValue(semantic.nodeId));
                } else if constexpr (std::same_as<Input,
                                                  PickerPointerInput>) {
                    addPointer(static_cast<std::uint64_t>(
                        ClientInputKind::Picker));
                    fields.emplace_back("picker_mode",
                                        toValue(semantic.activation.mode));
                    fields.emplace_back(
                        "activation_id",
                        ProtocolValue::makeUint(semantic.activation.id.value()));
                    fields.emplace_back("candidate_id",
                                        toValue(semantic.candidateId));
                } else if constexpr (std::same_as<
                                         Input, ExternalActionPointerInput>) {
                    addPointer(static_cast<std::uint64_t>(
                        ClientInputKind::ExternalAction));
                    fields.emplace_back("basis_revision",
                                        toValue(semantic.basis.observedRevision));
                    fields.emplace_back("invocation",
                                        toValue(semantic.invocation));
                } else if constexpr (std::same_as<
                                         Input, NoticeActionPointerInput>) {
                    addPointer(static_cast<std::uint64_t>(
                        ClientInputKind::NoticeAction));
                    fields.emplace_back("basis_revision",
                                        toValue(semantic.basis.observedRevision));
                    fields.emplace_back("action_id", toValue(semantic.actionId));
                } else if constexpr (std::same_as<
                                         Input, DocumentPointerInput>) {
                    addPointer(static_cast<std::uint64_t>(
                        ClientInputKind::Document));
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
    if (!detail::generated::validateClientInputWire(payload)) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "client input payload is malformed"};
    }
    const auto kind = requireField<std::uint64_t>(payload.field("kind"));
    if (!kind) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "client input kind is malformed"};
    }
    const auto isCurrentKind = [&](ClientInputKind expected) {
        return *kind == static_cast<std::uint64_t>(expected);
    };
    if (isCurrentKind(ClientInputKind::Key)) {
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
    if (isCurrentKind(ClientInputKind::ScrollLines)) {
        auto basis =
            requireField<Revision>(payload.field("basis_revision"));
        const auto target = validatedEnumField<SemanticScrollTarget>(
            payload.field("target"));
        auto rows = requireField<std::int64_t>(payload.field("rows"));
        if (!basis || !rows || *rows == 0) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client line-scroll input is malformed"};
        }
        return {ProtocolError::None,
                ClientInput{ScrollLinesInput{{*basis}, target, *rows}}, {}};
    }
    if (isCurrentKind(ClientInputKind::ScrollFraction)) {
        auto basis =
            requireField<Revision>(payload.field("basis_revision"));
        const auto target = validatedEnumField<SemanticScrollTarget>(
            payload.field("target"));
        auto numerator =
            requireField<std::uint32_t>(payload.field("numerator"));
        auto denominator =
            requireField<std::uint32_t>(payload.field("denominator"));
        if (!basis || !numerator || !denominator ||
            *denominator == 0 || *numerator > *denominator) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client fraction-scroll input is malformed"};
        }
        return {
            ProtocolError::None,
            ClientInput{ScrollFractionInput{
                {*basis}, target, *numerator, *denominator}},
            {}};
    }
    if (isCurrentKind(ClientInputKind::ViewNavigation)) {
        auto basis =
            requireField<Revision>(payload.field("basis_revision"));
        if (!basis) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client view-navigation input is malformed"};
        }
        return {ProtocolError::None,
                ClientInput{ViewNavigationInput{{*basis}}}, {}};
    }
    if (isCurrentKind(ClientInputKind::ResolvedPaneFocus)) {
        auto basis =
            requireField<Revision>(payload.field("basis_revision"));
        if (!basis) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client resolved-pane-focus input is malformed"};
        }
        return {ProtocolError::None,
                ClientInput{ResolvedPaneFocusInput{{*basis}}}, {}};
    }
    if (isCurrentKind(ClientInputKind::ResolvedSelection)) {
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
    const auto button = validatedEnumField<InputPointerButton>(
        payload.field("button"));
    const auto phase =
        validatedEnumField<InputPointerPhase>(payload.field("phase"));
    if (isCurrentKind(ClientInputKind::Picker)) {
        const auto mode =
            validatedEnumField<SearchMode>(payload.field("picker_mode"));
        auto activationId =
            requireField<std::uint64_t>(payload.field("activation_id"));
        auto candidate =
            requireField<std::string>(payload.field("candidate_id"));
        if (!activationId || *activationId == 0 || !candidate ||
            candidate->empty()) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                   "client picker input target is malformed"};
        }
        return {
            ProtocolError::None,
            ClientInput{PickerPointerInput{
                PickerActivation{mode, PickerActivationId{*activationId}},
                std::move(*candidate), button, phase}},
            {}};
    }
    auto basis =
        requireField<Revision>(payload.field("basis_revision"));
    if (!basis) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "client pointer input basis is malformed"};
    }
    const SemanticInputBasis semanticBasis{*basis};
    if (isCurrentKind(ClientInputKind::Tab)) {
        auto id = requireField<TabId>(payload.field("tab_id"));
        if (id) {
            return {ProtocolError::None,
                   ClientInput{TabPointerInput{
                       semanticBasis, *id, button, phase}},
                   {}};
        }
    } else if (isCurrentKind(ClientInputKind::Tree)) {
        auto id = requireField<TreeNodeId>(payload.field("node_id"));
        if (id) {
            return {ProtocolError::None,
                   ClientInput{TreePointerInput{
                       semanticBasis, std::move(*id), button, phase}},
                   {}};
        }
    } else if (isCurrentKind(ClientInputKind::ExternalAction)) {
        auto invocation = requireField<ExternalActionInvocation>(
            payload.field("invocation"));
        if (invocation) {
            return {ProtocolError::None,
                   ClientInput{ExternalActionPointerInput{
                       semanticBasis, std::move(*invocation), button, phase}},
                   {}};
        }
    } else if (isCurrentKind(ClientInputKind::NoticeAction)) {
        auto id = requireField<std::string>(payload.field("action_id"));
        if (id && !id->empty()) {
            return {ProtocolError::None,
                   ClientInput{NoticeActionPointerInput{
                       semanticBasis, std::move(*id), button, phase}},
                   {}};
        }
    } else if (isCurrentKind(ClientInputKind::Document)) {
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
        const auto edge = validatedEnumField<DocumentPointerEdge>(
            payload.field("edge"));
        const auto hasEdge = edge != DocumentPointerEdge::None;
        if (!additive || !selectWord ||
            (phase == InputPointerPhase::Press && (!position || hasEdge)) ||
            (phase == InputPointerPhase::Move &&
             (position.has_value() == hasEdge)) ||
            ((phase == InputPointerPhase::Release ||
              phase == InputPointerPhase::Cancel) &&
             (position || hasEdge)) ||
            (phase != InputPointerPhase::Press &&
             (*selectWord || *additive))) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client document input gesture is malformed"};
        }
        return {ProtocolError::None,
                ClientInput{DocumentPointerInput{
                    semanticBasis, position, *additive, *selectWord,
                    button, phase, edge}},
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
    if (!detail::generated::validateClientInputResultWire(payload)) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "client input result payload is malformed"};
    }
    const auto outcome =
        validatedEnumField<ClientInputOutcome>(payload.field("outcome"));
    ClientInputResult result{outcome,
                             std::nullopt, std::nullopt, std::nullopt};
    auto const* ownedField = payload.field("client_owned");
    if (ownedField->kind() != ProtocolValue::Kind::NullValue) {
        const auto kind = validatedEnumField<ClientOwnedInputKind>(
            ownedField->field("kind"));
        auto text = requireField<std::string>(ownedField->field("text"));
        if (!text) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client input result client_owned is malformed"};
        }
        result.clientOwned = ClientOwnedInput{kind, std::move(*text)};
    }
    auto const* activationField = payload.field("picker_activation");
    if (activationField->kind() != ProtocolValue::Kind::NullValue) {
        const auto mode =
            validatedEnumField<SearchMode>(activationField->field("mode"));
        auto id =
            requireField<std::uint64_t>(activationField->field("activation_id"));
        if (!id || *id == 0 ||
            (mode != SearchMode::Command && mode != SearchMode::File)) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "client input result picker activation is malformed"};
        }
        result.pickerActivation =
            PickerActivation{mode, PickerActivationId{*id}};
    }
    auto const* commandField = payload.field("command");
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
    names.reserve(detail::generated::kSemanticSnapshotFields.size());
    for (auto const name : detail::generated::kSemanticSnapshotFields) {
        names.emplace_back(name);
    }
    return names;
}

std::vector<std::string> semanticSessionDeltaWireFieldNames() {
    std::vector<std::string> names;
    names.reserve(detail::generated::kSemanticDeltaFields.size());
    for (auto const name : detail::generated::kSemanticDeltaFields) {
        names.emplace_back(name);
    }
    return names;
}

}  // namespace ssg
