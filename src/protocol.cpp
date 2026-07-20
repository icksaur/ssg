#include <ssg/protocol.h>

#include <ssg/document.h>
#include <ssg/editor_session_assembly.h>
#include <ssg/file_commands.h>
#include <ssg/find_replace.h>
#include <ssg/input.h>
#include <ssg/selection.h>
#include <ssg/settings.h>
#include <ssg/text_encoding.h>
#include <ssg/text_input_commands.h>

#include <any>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
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

constexpr std::string_view prefix = "SSG1";

std::string hexEncode(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::optional<std::string> hexDecode(std::string_view text) {
    if ((text.size() & 1U) != 0) return std::nullopt;
    std::string result(text.size() / 2, '\0');
    for (std::size_t i = 0; i < text.size(); i += 2) {
        int const high = hexValue(text[i]);
        int const low = hexValue(text[i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        result[i / 2] = static_cast<char>((high << 4) | low);
    }
    return result;
}

bool validUtf8(std::string_view text) {
    for (std::size_t index = 0; index < text.size();) {
        auto const first = static_cast<unsigned char>(text[index]);
        if (first == 0) return false;
        std::size_t length = 0;
        std::uint32_t value = 0;
        if (first <= 0x7f) {
            length = 1;
            value = first;
        } else if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
            value = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
            value = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
            value = first & 0x07;
        } else {
            return false;
        }
        if (index + length > text.size()) return false;
        for (std::size_t tail = 1; tail < length; ++tail) {
            auto const byte =
                static_cast<unsigned char>(text[index + tail]);
            if ((byte & 0xc0) != 0x80) return false;
            value = (value << 6) | (byte & 0x3f);
        }
        if ((length == 3 && value < 0x800) ||
            (length == 4 && value < 0x10000) ||
            (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) {
            return false;
        }
        index += length;
    }
    return true;
}

template <class Integer>
bool parseInteger(std::string_view text, Integer& value) {
    if (text.empty()) return false;
    auto const result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} &&
           result.ptr == text.data() + text.size();
}

std::vector<std::string_view> fields(std::string_view message) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (start <= message.size()) {
        auto const end = message.find(' ', start);
        result.push_back(message.substr(
            start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

DecodeInsertResult decodeFailure(ProtocolError error, std::string message) {
    return {error, std::nullopt, std::move(message)};
}

DocumentPosition caretPosition(std::size_t offset) {
    return {ByteOffset{offset}, LineIndex{0}, CellIndex{offset}};
}

std::string_view protocolErrorName(ProtocolError error) {
    switch (error) {
        case ProtocolError::None: return "none";
        case ProtocolError::MessageTooLarge: return "message_too_large";
        case ProtocolError::MalformedMessage: return "malformed_message";
        case ProtocolError::UnsupportedVersion: return "unsupported_version";
        case ProtocolError::UnsupportedCommand: return "unsupported_command";
        case ProtocolError::InsertTooLarge: return "insert_too_large";
    }
    return "malformed_message";
}

std::string_view commandErrorName(CommandError error) {
    switch (error) {
        case CommandError::None: return "none";
        case CommandError::UnknownClient: return "unknown_client";
        case CommandError::UnknownCommand: return "unknown_command";
        case CommandError::StaleRevision: return "stale_revision";
        case CommandError::CapabilityDenied: return "capability_denied";
        case CommandError::HandlerFailed: return "handler_failed";
        case CommandError::RevisionExhausted: return "revision_exhausted";
    }
    return "handler_failed";
}

ProtocolError parseProtocolError(std::string_view value) {
    for (auto error : {ProtocolError::None, ProtocolError::MessageTooLarge,
                       ProtocolError::MalformedMessage,
                       ProtocolError::UnsupportedVersion,
                       ProtocolError::UnsupportedCommand,
                       ProtocolError::InsertTooLarge}) {
        if (protocolErrorName(error) == value) return error;
    }
    throw std::invalid_argument{"unknown protocol error"};
}

CommandError parseCommandError(std::string_view value) {
    for (auto error : {CommandError::None, CommandError::UnknownClient,
                       CommandError::UnknownCommand,
                       CommandError::StaleRevision,
                       CommandError::CapabilityDenied,
                       CommandError::HandlerFailed,
                       CommandError::RevisionExhausted}) {
        if (commandErrorName(error) == value) return error;
    }
    throw std::invalid_argument{"unknown command error"};
}

}  // namespace

std::string encodeInsertRequest(InsertRequest const& request) {
    return std::string{prefix} + " INSERT " +
           std::to_string(request.base_revision.value()) + " " +
           hexEncode(request.text);
}

DecodeInsertResult decodeInsertRequest(std::string_view message,
                                         ProtocolLimits limits) {
    if (message.size() > limits.max_message_bytes) {
        return decodeFailure(ProtocolError::MessageTooLarge,
                              "message exceeds configured byte limit");
    }
    auto const parts = fields(message);
    if (parts.size() != 4) {
        return decodeFailure(ProtocolError::MalformedMessage,
                              "insert message must contain four fields");
    }
    if (parts[0] != prefix) {
        return decodeFailure(ProtocolError::UnsupportedVersion,
                              "unsupported protocol version");
    }
    if (parts[1] != "INSERT") {
        return decodeFailure(ProtocolError::UnsupportedCommand,
                              "only text.insert is supported");
    }
    std::uint64_t revision = 0;
    if (!parseInteger(parts[2], revision) || revision == 0) {
        return decodeFailure(ProtocolError::MalformedMessage,
                              "base revision must be a positive integer");
    }
    auto text = hexDecode(parts[3]);
    if (!text) {
        return decodeFailure(ProtocolError::MalformedMessage,
                              "insert payload must be hexadecimal");
    }
    if (text->size() > limits.max_insert_bytes) {
        return decodeFailure(ProtocolError::InsertTooLarge,
                              "insert exceeds configured byte limit");
    }
    if (!validUtf8(*text)) {
        return decodeFailure(ProtocolError::MalformedMessage,
                              "insert payload must be valid non-NUL UTF-8");
    }
    return {ProtocolError::None,
            InsertRequest{Revision{revision}, std::move(*text)}, {}};
}

std::string encodeSliceResponse(SliceResponse const& response) {
    std::string encoded =
        std::string{prefix} + " RESPONSE " +
        std::string{protocolErrorName(response.protocol_error)} + " " +
        std::string{commandErrorName(response.command_error)} + " " +
        std::to_string(response.snapshot.revision.value()) + " " +
        std::to_string(response.snapshot.caret.value()) + " " +
        hexEncode(response.snapshot.text) + " " +
        hexEncode(response.message) + " ";
    if (!response.delta) {
        return encoded + "-";
    }
    auto const& delta = *response.delta;
    return encoded + std::to_string(delta.base_revision.value()) + "," +
           std::to_string(delta.revision.value()) + "," +
           std::to_string(delta.start.value()) + "," +
           std::to_string(delta.erased_bytes) + "," +
           hexEncode(delta.inserted_text);
}

SliceResponse decodeSliceResponse(std::string_view message,
                                    ProtocolLimits limits) {
    if (message.size() > limits.max_message_bytes) {
        throw std::invalid_argument{"response exceeds configured byte limit"};
    }
    auto const parts = fields(message);
    if (parts.size() != 9 || parts[0] != prefix || parts[1] != "RESPONSE") {
        throw std::invalid_argument{"malformed slice response"};
    }
    std::uint64_t revision = 0;
    std::uint64_t caret = 0;
    if (!parseInteger(parts[4], revision) ||
        !parseInteger(parts[5], caret)) {
        throw std::invalid_argument{"malformed response revision or caret"};
    }
    auto text = hexDecode(parts[6]);
    auto error_message = hexDecode(parts[7]);
    if (!text || !error_message) {
        throw std::invalid_argument{"malformed response hexadecimal field"};
    }
    std::optional<DocumentDelta> delta;
    if (parts[8] != "-") {
        std::vector<std::string_view> values;
        std::size_t start = 0;
        while (start <= parts[8].size()) {
            auto const end = parts[8].find(',', start);
            values.push_back(parts[8].substr(
                start, end == std::string_view::npos ? end : end - start));
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        std::uint64_t base = 0, next = 0, offset = 0, erased = 0;
        if (values.size() != 5 || !parseInteger(values[0], base) ||
            !parseInteger(values[1], next) ||
            !parseInteger(values[2], offset) ||
            !parseInteger(values[3], erased)) {
            throw std::invalid_argument{"malformed response delta"};
        }
        auto inserted = hexDecode(values[4]);
        if (!inserted) throw std::invalid_argument{"malformed delta payload"};
        delta = DocumentDelta{Revision{base}, Revision{next},
                              ByteOffset{offset}, erased,
                              std::move(*inserted)};
    }
    return {parseProtocolError(parts[2]), parseCommandError(parts[3]),
            {Revision{revision}, std::move(*text), ByteOffset{caret}},
            std::move(delta), std::move(*error_message)};
}

struct CoreEditorSlice::Impl {
    Impl()
        : document{},
          selections{std::vector<Selection>{
              Selection{caretPosition(0), caretPosition(0)}}},
          session{CommandRegistry{std::vector<CommandSet>{CommandSet{{
              CommandRegistration{
                  {"text.insert", CommandEffect::Mutation, {}},
                  [this](CommandContext&, std::any const& payload) {
                      auto const* arguments =
                          std::any_cast<TextInputArguments>(&payload);
                      if (arguments == nullptr) {
                          return CommandHandlerResult::failure(
                              "text.insert payload has the wrong type");
                      }
                      if (arguments->text.empty()) {
                          return CommandHandlerResult::failure(
                              "text.insert payload must not be empty");
                      }
                      auto result = applyTextInput(
                          document.snapshot(), selections,
                          {IndentStyle::Spaces, 4, true, LineEnding::Lf},
                          TextInputCommand::Insert, *arguments);
                      if (!result.accepted() || !result.transaction ||
                          !result.selections) {
                          return CommandHandlerResult::failure(result.message);
                      }
                      auto applied = document.apply(*result.transaction);
                      if (!applied.accepted()) {
                          return CommandHandlerResult::failure(applied.message);
                      }
                      selections = std::move(*result.selections);
                      return CommandHandlerResult::success();
                  }}}}}}} {}

    DocumentViewState snapshotUnlocked() const {
        auto document_snapshot = document.snapshot();
        return {document_snapshot.revision, std::move(document_snapshot.text),
                selections.primary().active.byte_offset};
    }

    mutable std::mutex mutex;
    Document document;
    SelectionSet selections;
    EditorSession session;
};

CoreEditorSlice::CoreEditorSlice() : impl_{std::make_unique<Impl>()} {}
CoreEditorSlice::~CoreEditorSlice() = default;

bool CoreEditorSlice::attach(InvocationPrincipal principal) {
    std::lock_guard lock{impl_->mutex};
    return impl_->session.attach(std::move(principal), ViewId{1}).accepted();
}

bool CoreEditorSlice::detach(ClientId client_id) {
    std::lock_guard lock{impl_->mutex};
    return impl_->session.detach(client_id);
}

SliceResponse CoreEditorSlice::execute(ClientId client_id,
                                       InsertRequest const& request) {
    std::lock_guard lock{impl_->mutex};
    auto const before = impl_->snapshotUnlocked();
    auto result = impl_->session.dispatch(
        client_id,
        ClientCommand{"text.insert", request.base_revision,
                      TextInputArguments{request.text}});
    auto const after = impl_->snapshotUnlocked();
    return {ProtocolError::None, result.error, after,
            result.accepted() ? deriveDocumentDelta(before, after)
                              : std::nullopt,
            std::move(result.message)};
}

DocumentViewState CoreEditorSlice::snapshot() const {
    std::lock_guard lock{impl_->mutex};
    return impl_->snapshotUnlocked();
}


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
    if (depth > limits.max_value_depth) {
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
            if (length > limits.max_text_bytes) {
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
            if (length > limits.max_bytes_length) {
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
            if (count > limits.max_collection_length) {
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
            if (count > limits.max_collection_length) {
                return ValueReadStatus::BoundsExceeded;
            }
            ProtocolValue::Object fields;
            fields.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                std::uint32_t key_length = 0;
                if (!reader.readU32(key_length)) {
                    return ValueReadStatus::Truncated;
                }
                if (key_length > limits.max_text_bytes) {
                    return ValueReadStatus::BoundsExceeded;
                }
                std::string_view key_bytes;
                if (!reader.readBytes(key_length, key_bytes)) {
                    return ValueReadStatus::Truncated;
                }
                ProtocolValue field_value;
                auto status = readValue(reader, field_value, depth + 1, limits);
                if (status != ValueReadStatus::Ok) {
                    return status;
                }
                fields.emplace_back(std::string{key_bytes}, std::move(field_value));
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


constexpr std::uint8_t kProtocolWireVersion = 1;

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
                              ProtocolMessageKind expected_kind,
                              ProtocolLimits const& limits) {
    if (bytes.size() > limits.max_message_bytes) {
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
    std::uint8_t kind_byte = 0;
    if (!reader.readU8(kind_byte)) {
        return {ProtocolError::TruncatedMessage, std::nullopt,
                "message is missing its kind byte"};
    }
    if (kind_byte != static_cast<std::uint8_t>(expected_kind)) {
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
// SessionDelta, ClipboardRequest/Response, and StatusActionInvocation has a
// toValue()/decode_present() pair, plumbed through one generic fromValue<T>
// entry point defined once below.
//
// Rationale for the decode_present() split: several domain types (DiffFileId,
// TreeNodeId, TreeProviderId, LanguageId, TreeRevision, ViewportDimensions,
// ScrollFractionArguments, SelectionSet and therefore SelectionViewState,
// JournalDocumentKey, and every aggregate embedding one of these) have no
// default constructor, so a generic "T out{}; decode into out" pattern does
// not compile. Every decode_present() overload instead receives a
// std::optional<T>& and constructs the result in place via out.emplace(...)
// from already-decoded parts -- never a bare default-constructed T. The
// single generic fromValue<T>() wrapper interprets a wire null as "field is
// legitimately absent" (out.reset(); return true) before delegating to
// decode_present() for the non-null case. This one signature serves both:
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
// empirically), so every concrete/generic toValue()/decode_present()
// overload is forward-declared before fromValue<T>, toValue<optional<T>>,
// toValue<vector<T>>, and toValue<array<T,N>> -- the generic templates
// that call them -- are *defined*. Forward declarations are added to this
// block as new leaf/composite types are introduced further down the file.
namespace {

ProtocolValue toValue(bool value);
bool decode_present(ProtocolValue const& value, std::optional<bool>& out);
ProtocolValue toValue(std::uint8_t value);
bool decode_present(ProtocolValue const& value, std::optional<std::uint8_t>& out);
ProtocolValue toValue(std::uint32_t value);
bool decode_present(ProtocolValue const& value, std::optional<std::uint32_t>& out);
ProtocolValue toValue(std::uint64_t value);
bool decode_present(ProtocolValue const& value, std::optional<std::uint64_t>& out);
ProtocolValue toValue(std::int64_t value);
bool decode_present(ProtocolValue const& value, std::optional<std::int64_t>& out);
ProtocolValue toValue(int value);
bool decode_present(ProtocolValue const& value, std::optional<int>& out);
ProtocolValue toValue(std::string const& value);
bool decode_present(ProtocolValue const& value, std::optional<std::string>& out);
ProtocolValue toValue(std::vector<std::uint8_t> const& value);
bool decode_present(ProtocolValue const& value,
                    std::optional<std::vector<std::uint8_t>>& out);
ProtocolValue toValue(std::filesystem::path const& value);
bool decode_present(ProtocolValue const& value,
                    std::optional<std::filesystem::path>& out);

template <typename Enum, typename = std::enable_if_t<std::is_enum_v<Enum>>>
ProtocolValue toValue(Enum value) {
    return ProtocolValue::makeUint(static_cast<std::uint64_t>(
        static_cast<std::underlying_type_t<Enum>>(value)));
}

ProtocolValue toValue(bool value) { return ProtocolValue::makeBool(value); }
bool decode_present(ProtocolValue const& value, std::optional<bool>& out) {
    auto decoded = value.asBool();
    if (!decoded) return false;
    out = *decoded;
    return true;
}

ProtocolValue toValue(std::uint8_t value) {
    return ProtocolValue::makeUint(value);
}
bool decode_present(ProtocolValue const& value, std::optional<std::uint8_t>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<std::uint32_t>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<std::uint64_t>& out) {
    auto decoded = value.asUint();
    if (!decoded) return false;
    out = *decoded;
    return true;
}

ProtocolValue toValue(std::int64_t value) {
    return ProtocolValue::makeInt(value);
}
bool decode_present(ProtocolValue const& value, std::optional<std::int64_t>& out) {
    auto decoded = value.asInt();
    if (!decoded) return false;
    out = *decoded;
    return true;
}

ProtocolValue toValue(int value) {
    return ProtocolValue::makeInt(static_cast<std::int64_t>(value));
}
bool decode_present(ProtocolValue const& value, std::optional<int>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<std::string>& out) {
    auto const* decoded = value.asText();
    if (!decoded) return false;
    out = *decoded;
    return true;
}

ProtocolValue toValue(std::vector<std::uint8_t> const& value) {
    return ProtocolValue::makeBytes(value);
}
bool decode_present(ProtocolValue const& value,
                    std::optional<std::vector<std::uint8_t>>& out) {
    auto const* decoded = value.asBytes();
    if (!decoded) return false;
    out = *decoded;
    return true;
}

ProtocolValue toValue(std::filesystem::path const& value) {
    return ProtocolValue::makeText(value.string());
}
bool decode_present(ProtocolValue const& value,
                    std::optional<std::filesystem::path>& out) {
    auto const* decoded = value.asText();
    if (!decoded) return false;
    out = std::filesystem::path{*decoded};
    return true;
}

// (toValue(Enum) is served generically above; only decode_present needs a
// forward declaration per enum, each implemented via decode_enum().)
bool decode_present(ProtocolValue const& value, std::optional<DocumentMode>& out);
bool decode_present(ProtocolValue const& value, std::optional<ClipboardRequestKind>& out);
bool decode_present(ProtocolValue const& value, std::optional<ClipboardResponseStatus>& out);
bool decode_present(ProtocolValue const& value, std::optional<StatusPriority>& out);
bool decode_present(ProtocolValue const& value, std::optional<PromptKind>& out);
bool decode_present(ProtocolValue const& value, std::optional<PromptControlKind>& out);
bool decode_present(ProtocolValue const& value, std::optional<SearchMode>& out);
bool decode_present(ProtocolValue const& value, std::optional<FindReplaceError>& out);
bool decode_present(ProtocolValue const& value, std::optional<SettingScope>& out);
bool decode_present(ProtocolValue const& value, std::optional<SettingKey>& out);
bool decode_present(ProtocolValue const& value, std::optional<TextEncoding>& out);
bool decode_present(ProtocolValue const& value, std::optional<IndentStyle>& out);
bool decode_present(ProtocolValue const& value, std::optional<LineEnding>& out);
bool decode_present(ProtocolValue const& value, std::optional<TabKind>& out);
bool decode_present(ProtocolValue const& value, std::optional<TabRecoveryBadge>& out);
bool decode_present(ProtocolValue const& value, std::optional<JournalDocumentKeyKind>& out);
bool decode_present(ProtocolValue const& value, std::optional<DiffLineKind>& out);
bool decode_present(ProtocolValue const& value, std::optional<ExternalAction>& out);
bool decode_present(ProtocolValue const& value, std::optional<ExternalDocumentStatus>& out);
bool decode_present(ProtocolValue const& value, std::optional<FollowMode>& out);
bool decode_present(ProtocolValue const& value, std::optional<TreeProviderKind>& out);
bool decode_present(ProtocolValue const& value, std::optional<TreeNodeKind>& out);
bool decode_present(ProtocolValue const& value, std::optional<GitTreeStatus>& out);
bool decode_present(ProtocolValue const& value, std::optional<SyntaxScope>& out);
bool decode_present(ProtocolValue const& value, std::optional<BracketKind>& out);
bool decode_present(ProtocolValue const& value, std::optional<BracketRole>& out);
bool decode_present(ProtocolValue const& value, std::optional<CommentKind>& out);
bool decode_present(ProtocolValue const& value, std::optional<CommentTokenRole>& out);
bool decode_present(ProtocolValue const& value, std::optional<LspDiagnosticSeverity>& out);
bool decode_present(ProtocolValue const& value, std::optional<ShellNodeKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<FocusTarget>& out);
bool decode_present(ProtocolValue const& value, std::optional<SemanticRole>& out);

ProtocolValue toValue(Revision const& value);
bool decode_present(ProtocolValue const& value, std::optional<Revision>& out);
ProtocolValue toValue(ByteOffset const& value);
bool decode_present(ProtocolValue const& value, std::optional<ByteOffset>& out);
ProtocolValue toValue(LineIndex const& value);
bool decode_present(ProtocolValue const& value, std::optional<LineIndex>& out);
ProtocolValue toValue(CellIndex const& value);
bool decode_present(ProtocolValue const& value, std::optional<CellIndex>& out);
ProtocolValue toValue(ClientId const& value);
bool decode_present(ProtocolValue const& value, std::optional<ClientId>& out);
ProtocolValue toValue(ViewId const& value);
bool decode_present(ProtocolValue const& value, std::optional<ViewId>& out);
ProtocolValue toValue(WorkspaceId const& value);
bool decode_present(ProtocolValue const& value, std::optional<WorkspaceId>& out);
ProtocolValue toValue(CapabilityId const& value);
bool decode_present(ProtocolValue const& value, std::optional<CapabilityId>& out);
ProtocolValue toValue(StatusId const& value);
bool decode_present(ProtocolValue const& value, std::optional<StatusId>& out);
ProtocolValue toValue(TabId const& value);
bool decode_present(ProtocolValue const& value, std::optional<TabId>& out);
ProtocolValue toValue(FileDocumentId const& value);
bool decode_present(ProtocolValue const& value, std::optional<FileDocumentId>& out);
ProtocolValue toValue(DiffFileId const& value);
bool decode_present(ProtocolValue const& value, std::optional<DiffFileId>& out);
ProtocolValue toValue(PaneId const& value);
bool decode_present(ProtocolValue const& value, std::optional<PaneId>& out);
ProtocolValue toValue(TreeProviderId const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeProviderId>& out);
ProtocolValue toValue(TreeNodeId const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeNodeId>& out);
ProtocolValue toValue(TreeRevision const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeRevision>& out);
ProtocolValue toValue(LanguageId const& value);
bool decode_present(ProtocolValue const& value, std::optional<LanguageId>& out);
ProtocolValue toValue(UntitledDocumentId const& value);
bool decode_present(ProtocolValue const& value, std::optional<UntitledDocumentId>& out);
ProtocolValue toValue(JournalDocumentKey const& value);
bool decode_present(ProtocolValue const& value, std::optional<JournalDocumentKey>& out);

ProtocolValue toValue(DocumentPosition const& value);
bool decode_present(ProtocolValue const& value, std::optional<DocumentPosition>& out);
ProtocolValue toValue(DocumentViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<DocumentViewState>& out);
ProtocolValue toValue(DocumentDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<DocumentDelta>& out);
ProtocolValue toValue(Selection const& value);
bool decode_present(ProtocolValue const& value, std::optional<Selection>& out);
ProtocolValue toValue(SelectionSet const& value);
bool decode_present(ProtocolValue const& value, std::optional<SelectionSet>& out);
ProtocolValue toValue(SelectionViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<SelectionViewState>& out);
ProtocolValue toValue(SelectionViewDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<SelectionViewDelta>& out);
ProtocolValue toValue(HistoryViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<HistoryViewState>& out);
ProtocolValue toValue(HistoryDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<HistoryDelta>& out);
ProtocolValue toValue(ClipboardRequest const& value);
bool decode_present(ProtocolValue const& value, std::optional<ClipboardRequest>& out);
ProtocolValue toValue(ClipboardResponse const& value);
bool decode_present(ProtocolValue const& value, std::optional<ClipboardResponse>& out);
ProtocolValue toValue(ClipboardViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<ClipboardViewState>& out);
ProtocolValue toValue(ClipboardDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<ClipboardDelta>& out);
ProtocolValue toValue(StatusAction const& value);
bool decode_present(ProtocolValue const& value, std::optional<StatusAction>& out);
ProtocolValue toValue(StatusItemView const& value);
bool decode_present(ProtocolValue const& value, std::optional<StatusItemView>& out);
ProtocolValue toValue(StatusViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<StatusViewState>& out);
ProtocolValue toValue(StatusActionInvocation const& value);
bool decode_present(ProtocolValue const& value, std::optional<StatusActionInvocation>& out);
ProtocolValue toValue(Rect const& value);
bool decode_present(ProtocolValue const& value, std::optional<Rect>& out);
ProtocolValue toValue(PromptControlView const& value);
bool decode_present(ProtocolValue const& value, std::optional<PromptControlView>& out);
ProtocolValue toValue(PromptViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<PromptViewState>& out);
ProtocolValue toValue(PromptStatusViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<PromptStatusViewState>& out);
ProtocolValue toValue(PromptStatusDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<PromptStatusDelta>& out);
ProtocolValue toValue(SearchResult const& value);
bool decode_present(ProtocolValue const& value, std::optional<SearchResult>& out);
ProtocolValue toValue(SearchViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<SearchViewState>& out);
ProtocolValue toValue(SearchDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<SearchDelta>& out);
ProtocolValue toValue(ByteRange const& value);
bool decode_present(ProtocolValue const& value, std::optional<ByteRange>& out);
ProtocolValue toValue(FindOptions const& value);
bool decode_present(ProtocolValue const& value, std::optional<FindOptions>& out);
ProtocolValue toValue(FindRequest const& value);
bool decode_present(ProtocolValue const& value, std::optional<FindRequest>& out);
ProtocolValue toValue(FindMatch const& value);
bool decode_present(ProtocolValue const& value, std::optional<FindMatch>& out);
ProtocolValue toValue(WorkspaceFileReplacement const& value);
bool decode_present(ProtocolValue const& value, std::optional<WorkspaceFileReplacement>& out);
ProtocolValue toValue(WorkspaceReplacePreview const& value);
bool decode_present(ProtocolValue const& value, std::optional<WorkspaceReplacePreview>& out);
ProtocolValue toValue(FindReplaceViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<FindReplaceViewState>& out);
ProtocolValue toValue(FindReplaceDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<FindReplaceDelta>& out);
ProtocolValue toValue(SettingValue const& value);
bool decode_present(ProtocolValue const& value, std::optional<SettingValue>& out);
ProtocolValue toValue(EffectiveSetting const& value);
bool decode_present(ProtocolValue const& value, std::optional<EffectiveSetting>& out);
ProtocolValue toValue(SettingViewEntry const& value);
bool decode_present(ProtocolValue const& value, std::optional<SettingViewEntry>& out);
ProtocolValue toValue(SettingsViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<SettingsViewState>& out);
ProtocolValue toValue(SettingsDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<SettingsDelta>& out);
ProtocolValue toValue(SettingsSectionDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<SettingsSectionDelta>& out);
ProtocolValue toValue(KeyStroke const& value);
bool decode_present(ProtocolValue const& value, std::optional<KeyStroke>& out);
ProtocolValue toValue(KeyBinding const& value);
bool decode_present(ProtocolValue const& value, std::optional<KeyBinding>& out);
ProtocolValue toValue(KeymapViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<KeymapViewState>& out);
ProtocolValue toValue(KeymapDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<KeymapDelta>& out);
ProtocolValue toValue(TextEncodingStatus const& value);
bool decode_present(ProtocolValue const& value, std::optional<TextEncodingStatus>& out);
ProtocolValue toValue(TextEncodingViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<TextEncodingViewState>& out);
ProtocolValue toValue(TextEncodingDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<TextEncodingDelta>& out);
ProtocolValue toValue(TabState const& value);
bool decode_present(ProtocolValue const& value, std::optional<TabState>& out);
ProtocolValue toValue(TabViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<TabViewState>& out);
ProtocolValue toValue(TabDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<TabDelta>& out);
ProtocolValue toValue(DiffHunk const& value);
bool decode_present(ProtocolValue const& value, std::optional<DiffHunk>& out);
ProtocolValue toValue(DiffLineChange const& value);
bool decode_present(ProtocolValue const& value, std::optional<DiffLineChange>& out);
ProtocolValue toValue(DiffFileView const& value);
bool decode_present(ProtocolValue const& value, std::optional<DiffFileView>& out);
ProtocolValue toValue(DiffViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<DiffViewState>& out);
ProtocolValue toValue(DiffDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<DiffDelta>& out);
ProtocolValue toValue(ExternalDocumentView const& value);
bool decode_present(ProtocolValue const& value, std::optional<ExternalDocumentView>& out);
ProtocolValue toValue(ExternalModificationViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<ExternalModificationViewState>& out);
ProtocolValue toValue(ExternalModificationDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<ExternalModificationDelta>& out);
ProtocolValue toValue(ViewportDimensions const& value);
bool decode_present(ProtocolValue const& value, std::optional<ViewportDimensions>& out);
ProtocolValue toValue(FollowScrollOffset const& value);
bool decode_present(ProtocolValue const& value, std::optional<FollowScrollOffset>& out);
ProtocolValue toValue(FollowTarget const& value);
bool decode_present(ProtocolValue const& value, std::optional<FollowTarget>& out);
ProtocolValue toValue(FollowClientView const& value);
bool decode_present(ProtocolValue const& value, std::optional<FollowClientView>& out);
ProtocolValue toValue(FollowEditsViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<FollowEditsViewState>& out);
ProtocolValue toValue(FollowEditsDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<FollowEditsDelta>& out);
ProtocolValue toValue(TreeNodeCommand const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeNodeCommand>& out);
ProtocolValue toValue(TreeNode const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeNode>& out);
ProtocolValue toValue(TreeNodeView const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeNodeView>& out);
ProtocolValue toValue(TreeProviderView const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeProviderView>& out);
ProtocolValue toValue(TreeViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeViewState>& out);
ProtocolValue toValue(TreeProviderDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeProviderDelta>& out);
ProtocolValue toValue(TreeDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeDelta>& out);
ProtocolValue toValue(SyntaxSpan const& value);
bool decode_present(ProtocolValue const& value, std::optional<SyntaxSpan>& out);
ProtocolValue toValue(SyntaxBracketPair const& value);
bool decode_present(ProtocolValue const& value, std::optional<SyntaxBracketPair>& out);
ProtocolValue toValue(UnmatchedBracket const& value);
bool decode_present(ProtocolValue const& value, std::optional<UnmatchedBracket>& out);
ProtocolValue toValue(SyntaxRange const& value);
bool decode_present(ProtocolValue const& value, std::optional<SyntaxRange>& out);
ProtocolValue toValue(CommentToken const& value);
bool decode_present(ProtocolValue const& value, std::optional<CommentToken>& out);
ProtocolValue toValue(CommentRange const& value);
bool decode_present(ProtocolValue const& value, std::optional<CommentRange>& out);
ProtocolValue toValue(LineIndentation const& value);
bool decode_present(ProtocolValue const& value, std::optional<LineIndentation>& out);
ProtocolValue toValue(SyntaxViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<SyntaxViewState>& out);
ProtocolValue toValue(SyntaxDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<SyntaxDelta>& out);
ProtocolValue toValue(LspPosition const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspPosition>& out);
ProtocolValue toValue(LspRange const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspRange>& out);
ProtocolValue toValue(LspDiagnostic const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspDiagnostic>& out);
ProtocolValue toValue(LspDocumentDiagnostics const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspDocumentDiagnostics>& out);
ProtocolValue toValue(LspSyncViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspSyncViewState>& out);
ProtocolValue toValue(LspSyncDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspSyncDelta>& out);
ProtocolValue toValue(LspCompletionItem const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspCompletionItem>& out);
ProtocolValue toValue(LspCompletionViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspCompletionViewState>& out);
ProtocolValue toValue(LspHover const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspHover>& out);
ProtocolValue toValue(LspNavigationTarget const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspNavigationTarget>& out);
ProtocolValue toValue(LspNavigationViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspNavigationViewState>& out);
ProtocolValue toValue(LspFeatureViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspFeatureViewState>& out);
ProtocolValue toValue(LspFeatureDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspFeatureDelta>& out);
ProtocolValue toValue(SrgbColor const& value);
bool decode_present(ProtocolValue const& value, std::optional<SrgbColor>& out);
ProtocolValue toValue(ThemeSnapshot const& value);
bool decode_present(ProtocolValue const& value, std::optional<ThemeSnapshot>& out);
ProtocolValue toValue(ThemeSectionDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<ThemeSectionDelta>& out);
ProtocolValue toValue(GridSize const& value);
bool decode_present(ProtocolValue const& value, std::optional<GridSize>& out);
ProtocolValue toValue(AccessibilityNode const& value);
bool decode_present(ProtocolValue const& value, std::optional<AccessibilityNode>& out);
ProtocolValue toValue(PaneGeometry const& value);
bool decode_present(ProtocolValue const& value, std::optional<PaneGeometry>& out);
ProtocolValue toValue(TabHit const& value);
bool decode_present(ProtocolValue const& value, std::optional<TabHit>& out);
ProtocolValue toValue(ShellViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<ShellViewState>& out);
ProtocolValue toValue(ShellSectionDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<ShellSectionDelta>& out);
ProtocolValue toValue(VisualRow const& value);
bool decode_present(ProtocolValue const& value, std::optional<VisualRow>& out);
ProtocolValue toValue(CellHitTarget const& value);
bool decode_present(ProtocolValue const& value, std::optional<CellHitTarget>& out);
ProtocolValue toValue(ScrollbarMetrics const& value);
bool decode_present(ProtocolValue const& value, std::optional<ScrollbarMetrics>& out);
ProtocolValue toValue(ViewportViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<ViewportViewState>& out);
ProtocolValue toValue(ViewportDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<ViewportDelta>& out);
ProtocolValue toValue(SessionTopology const& value);
bool decode_present(ProtocolValue const& value, std::optional<SessionTopology>& out);
ProtocolValue toValue(ClientSnapshotState const& value);
bool decode_present(ProtocolValue const& value, std::optional<ClientSnapshotState>& out);
ProtocolValue toValue(SessionSnapshotSections const& value);
bool decode_present(ProtocolValue const& value, std::optional<SessionSnapshotSections>& out);

ProtocolValue toValue(TextInputArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<TextInputArguments>& out);
ProtocolValue toValue(PaletteExecuteArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<PaletteExecuteArguments>& out);
ProtocolValue toValue(TreeSelectArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeSelectArguments>& out);
ProtocolValue toValue(FindQueryArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<FindQueryArguments>& out);
ProtocolValue toValue(SelectionCommandArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<SelectionCommandArguments>& out);
ProtocolValue toValue(ScrollLinesArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<ScrollLinesArguments>& out);
ProtocolValue toValue(ScrollPagesArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<ScrollPagesArguments>& out);
ProtocolValue toValue(ScrollFractionArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<ScrollFractionArguments>& out);
ProtocolValue toValue(DroppedContentArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<DroppedContentArguments>& out);
ProtocolValue toValue(ReopenWithEncodingArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<ReopenWithEncodingArguments>& out);
ProtocolValue toValue(SetEncodingArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<SetEncodingArguments>& out);
ProtocolValue toValue(SetLineEndingArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<SetLineEndingArguments>& out);
ProtocolValue toValue(SetFinalNewlineArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<SetFinalNewlineArguments>& out);
ProtocolValue toValue(SettingSetArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<SettingSetArguments>& out);
ProtocolValue toValue(SettingResetArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<SettingResetArguments>& out);
ProtocolValue toValue(SettingResetScopeArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<SettingResetScopeArguments>& out);
ProtocolValue toValue(WorkspaceReplaceArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<WorkspaceReplaceArguments>& out);

template <typename T>
ProtocolValue toValue(std::optional<T> const& value);
template <typename T>
ProtocolValue toValue(std::vector<T> const& value);
template <typename T, std::size_t N>
ProtocolValue toValue(std::array<T, N> const& value);

template <typename T>
[[nodiscard]] bool fromValue(ProtocolValue const& value, std::optional<T>& out);
template <typename T>
bool decode_present(ProtocolValue const& value, std::optional<std::vector<T>>& out);
template <typename T, std::size_t N>
bool decode_present(ProtocolValue const& value, std::optional<std::array<T, N>>& out);

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
        return decode_present(value, out);
    } catch (std::invalid_argument const&) {
        // Domain constructors enforce invariants for trusted in-process
        // callers. Invalid wire values are ordinary decode failures, not
        // exceptions escaping into the transport.
        out.reset();
        return false;
    }
}

template <typename T>
bool decode_present(ProtocolValue const& value, std::optional<std::vector<T>>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<std::array<T, N>>& out) {
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
[[nodiscard]] bool decodeOptionalField(ProtocolValue const* field_value,
                                         std::optional<T>& out) {
    if (field_value == nullptr) {
        out.reset();
        return true;
    }
    return fromValue(*field_value, out);
}

template <typename Enum, std::size_t N>
[[nodiscard]] bool decodeEnum(ProtocolValue const& value, std::optional<Enum>& out,
                               std::array<Enum, N> const& valid_values) {
    auto raw = value.asUint();
    if (!raw) {
        return false;
    }
    for (Enum candidate : valid_values) {
        if (static_cast<std::uint64_t>(
                static_cast<std::underlying_type_t<Enum>>(candidate)) ==
            *raw) {
            out.emplace(candidate);
            return true;
        }
    }
    return false;
}


// Enum decode_present() definitions, each delegating to decode_enum() with
// the closed set of valid values for that enum.

bool decode_present(ProtocolValue const& value, std::optional<DocumentMode>& out) {
    static constexpr std::array values{DocumentMode::Edit, DocumentMode::ReadOnly,
                                       DocumentMode::Diff};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<ClipboardRequestKind>& out) {
    static constexpr std::array values{ClipboardRequestKind::Write,
                                       ClipboardRequestKind::Read};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<ClipboardResponseStatus>& out) {
    static constexpr std::array values{
        ClipboardResponseStatus::Success, ClipboardResponseStatus::Denied,
        ClipboardResponseStatus::Unavailable, ClipboardResponseStatus::Disconnected};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<StatusPriority>& out) {
    static constexpr std::array values{StatusPriority::Error, StatusPriority::Warning,
                                       StatusPriority::Information, StatusPriority::Progress};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<PromptKind>& out) {
    static constexpr std::array values{PromptKind::Path, PromptKind::Find,
                                       PromptKind::Replace, PromptKind::Settings,
                                       PromptKind::CommandArgument,
                                       PromptKind::Palette};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<PromptControlKind>& out) {
    static constexpr std::array values{PromptControlKind::Input, PromptControlKind::Toggle,
                                       PromptControlKind::Count};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<SearchMode>& out) {
    static constexpr std::array values{SearchMode::File, SearchMode::Line,
                                       SearchMode::Symbol, SearchMode::Text,
                                       SearchMode::Command};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<FindReplaceError>& out) {
    static constexpr std::array values{
        FindReplaceError::None, FindReplaceError::InvalidPattern,
        FindReplaceError::InvalidUtf8, FindReplaceError::InvalidSelection,
        FindReplaceError::BudgetExhausted, FindReplaceError::Cancelled,
        FindReplaceError::NoMatch, FindReplaceError::StaleRevision,
        FindReplaceError::DocumentRejected, FindReplaceError::WorkspaceRejected,
        FindReplaceError::RecoveryRejected};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<SettingScope>& out) {
    static constexpr std::array values{SettingScope::Defaults, SettingScope::User,
                                       SettingScope::Workspace, SettingScope::Language,
                                       SettingScope::Document};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<SettingKey>& out) {
    static constexpr std::array values{
        SettingKey::IndentWidth, SettingKey::IndentStyle, SettingKey::IndentDetection,
        SettingKey::AutoIndent, SettingKey::LineEnding, SettingKey::FinalNewline,
        SettingKey::Encoding, SettingKey::WordWrap, SettingKey::Theme, SettingKey::Keymap,
        SettingKey::SearchCaseSensitive, SettingKey::SearchWholeWord,
        SettingKey::SearchRegularExpression, SettingKey::UndoByteBudget,
        SettingKey::RecoveryByteBudget, SettingKey::TypingCoalescingMs};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<TextEncoding>& out) {
    static constexpr std::array values{TextEncoding::Utf8, TextEncoding::Utf8Bom,
                                       TextEncoding::Utf16le, TextEncoding::Utf16be,
                                       TextEncoding::Windows1252, TextEncoding::Iso88591};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<IndentStyle>& out) {
    static constexpr std::array values{IndentStyle::Spaces, IndentStyle::Tabs};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<LineEnding>& out) {
    static constexpr std::array values{LineEnding::Lf, LineEnding::Crlf, LineEnding::Cr,
                                       LineEnding::Mixed};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<TabKind>& out) {
    static constexpr std::array values{TabKind::Document, TabKind::LiveDiff,
                                       TabKind::ReadOnlyOutput, TabKind::SearchResults,
                                       TabKind::TreeView};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<TabRecoveryBadge>& out) {
    static constexpr std::array values{TabRecoveryBadge::None, TabRecoveryBadge::Pending,
                                       TabRecoveryBadge::Durable, TabRecoveryBadge::Failed};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<JournalDocumentKeyKind>& out) {
    static constexpr std::array values{JournalDocumentKeyKind::Saved,
                                       JournalDocumentKeyKind::Untitled};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<DiffLineKind>& out) {
    static constexpr std::array values{DiffLineKind::Added, DiffLineKind::Removed,
                                       DiffLineKind::Modified};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<ExternalAction>& out) {
    static constexpr std::array values{ExternalAction::Reload, ExternalAction::KeepBuffer,
                                       ExternalAction::OpenDiff};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<ExternalDocumentStatus>& out) {
    static constexpr std::array values{ExternalDocumentStatus::ExternallyModified,
                                       ExternalDocumentStatus::ExternallyRemoved};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<FollowMode>& out) {
    static constexpr std::array values{FollowMode::Following, FollowMode::Paused};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<TreeProviderKind>& out) {
    static constexpr std::array values{TreeProviderKind::Filesystem, TreeProviderKind::Git,
                                       TreeProviderKind::Symbols};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<TreeNodeKind>& out) {
    static constexpr std::array values{TreeNodeKind::Root, TreeNodeKind::Directory,
                                       TreeNodeKind::File, TreeNodeKind::Symlink,
                                       TreeNodeKind::GitEntry, TreeNodeKind::Symbol};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<GitTreeStatus>& out) {
    static constexpr std::array values{GitTreeStatus::Added, GitTreeStatus::Modified,
                                       GitTreeStatus::Deleted, GitTreeStatus::Renamed,
                                       GitTreeStatus::Untracked};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<SyntaxScope>& out) {
    return decodeEnum(value, out, all_syntax_scopes);
}

bool decode_present(ProtocolValue const& value, std::optional<BracketKind>& out) {
    static constexpr std::array values{BracketKind::Round, BracketKind::Square,
                                       BracketKind::Curly};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<BracketRole>& out) {
    static constexpr std::array values{BracketRole::Open, BracketRole::Close};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<CommentKind>& out) {
    static constexpr std::array values{CommentKind::Line, CommentKind::Block};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<CommentTokenRole>& out) {
    static constexpr std::array values{CommentTokenRole::Line, CommentTokenRole::BlockOpen,
                                       CommentTokenRole::BlockClose};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<LspDiagnosticSeverity>& out) {
    static constexpr std::array values{
        LspDiagnosticSeverity::Error, LspDiagnosticSeverity::Warning,
        LspDiagnosticSeverity::Information, LspDiagnosticSeverity::Hint};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<ShellNodeKind>& out) {
    static constexpr std::array values{
        ShellNodeKind::Header, ShellNodeKind::HeaderField, ShellNodeKind::Footer,
        ShellNodeKind::FooterField, ShellNodeKind::FooterAction, ShellNodeKind::TabBar,
        ShellNodeKind::Tab, ShellNodeKind::Panel, ShellNodeKind::PanelProvider,
        ShellNodeKind::Pane, ShellNodeKind::Scrollbar, ShellNodeKind::PromptReservation,
        ShellNodeKind::EmptyState};
    return decodeEnum(value, out, values);
}

bool decodePresent(ProtocolValue const& value, std::optional<FocusTarget>& out) {
    static constexpr std::array values{FocusTarget::Editor, FocusTarget::Panel,
                                       FocusTarget::Prompt};
    return decodeEnum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<SemanticRole>& out) {
    return decodeEnum(value, out, all_semantic_roles);
}

// Strong-id toValue()/decode_present() definitions.

ProtocolValue toValue(Revision const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<Revision>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(ByteOffset const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<ByteOffset>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(LineIndex const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<LineIndex>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(CellIndex const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<CellIndex>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(ClientId const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<ClientId>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(ViewId const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<ViewId>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(WorkspaceId const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<WorkspaceId>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(CapabilityId const& value) {
    return ProtocolValue::makeText(std::string{value.value()});
}
bool decode_present(ProtocolValue const& value, std::optional<CapabilityId>& out) {
    auto const* text = value.asText();
    if (!text) return false;
    out.emplace(*text);
    return true;
}

ProtocolValue toValue(StatusId const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<StatusId>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(TabId const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<TabId>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(FileDocumentId const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<FileDocumentId>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(DiffFileId const& value) {
    return ProtocolValue::makeText(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<DiffFileId>& out) {
    auto const* text = value.asText();
    if (!text) return false;
    out.emplace(*text);
    return true;
}

ProtocolValue toValue(PaneId const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<PaneId>& out) {
    auto raw = value.asUint();
    if (!raw || *raw > std::numeric_limits<std::uint32_t>::max()) return false;
    out.emplace(static_cast<std::uint32_t>(*raw));
    return true;
}

ProtocolValue toValue(TreeProviderId const& value) {
    return ProtocolValue::makeText(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<TreeProviderId>& out) {
    auto const* text = value.asText();
    if (!text) return false;
    out.emplace(*text);
    return true;
}

ProtocolValue toValue(TreeNodeId const& value) {
    return ProtocolValue::makeText(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<TreeNodeId>& out) {
    auto const* text = value.asText();
    if (!text) return false;
    out.emplace(*text);
    return true;
}

ProtocolValue toValue(TreeRevision const& value) {
    return ProtocolValue::makeUint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<TreeRevision>& out) {
    auto raw = value.asUint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue toValue(LanguageId const& value) {
    return ProtocolValue::makeText(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<LanguageId>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<UntitledDocumentId>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<JournalDocumentKey>& out) {
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
// Every composite decode_present() below starts by rejecting a non-object
// wire value outright: value.field() already returns nullptr for every key
// when the value is not an object, which require_field() and
// decode_optional_field() both turn into "field absent" -- but a struct
// whose fields are *all* domain-optional (e.g. SessionTopology) would then
// wrongly decode a malformed non-object value (an array, a bare integer) as
// "every field absent" instead of rejecting it. The explicit as_object()
// check below closes that gap uniformly.

ProtocolValue toValue(DocumentPosition const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("byte_offset", toValue(value.byte_offset));
    fields.emplace_back("line", toValue(value.line));
    fields.emplace_back("cell", toValue(value.cell));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DocumentPosition>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto byte_offset = requireField<ByteOffset>(value.field("byte_offset"));
    auto line = requireField<LineIndex>(value.field("line"));
    auto cell = requireField<CellIndex>(value.field("cell"));
    if (!byte_offset || !line || !cell) return false;
    out.emplace(DocumentPosition{*byte_offset, *line, *cell});
    return true;
}

ProtocolValue toValue(DocumentViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("text", toValue(value.text));
    fields.emplace_back("caret", toValue(value.caret));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DocumentViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto text = requireField<std::string>(value.field("text"));
    auto caret = requireField<ByteOffset>(value.field("caret"));
    if (!revision || !text || !caret) return false;
    out.emplace(DocumentViewState{*revision, *text, *caret});
    return true;
}

ProtocolValue toValue(DocumentDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(value.base_revision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("start", toValue(value.start));
    fields.emplace_back("erased_bytes", toValue(value.erased_bytes));
    fields.emplace_back("inserted_text", toValue(value.inserted_text));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DocumentDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto base_revision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    auto start = requireField<ByteOffset>(value.field("start"));
    auto erased_bytes = requireField<std::uint64_t>(value.field("erased_bytes"));
    auto inserted_text = requireField<std::string>(value.field("inserted_text"));
    if (!base_revision || !revision || !start || !erased_bytes || !inserted_text) {
        return false;
    }
    out.emplace(DocumentDelta{*base_revision, *revision, *start, *erased_bytes,
                              *inserted_text});
    return true;
}

ProtocolValue toValue(Selection const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("anchor", toValue(value.anchor));
    fields.emplace_back("active", toValue(value.active));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<Selection>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<SelectionSet>& out) {
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
    fields.emplace_back("first_visual_row", toValue(value.first_visual_row));
    fields.emplace_back("first_visual_column", toValue(value.first_visual_column));
    fields.emplace_back("desired_cell", toValue(value.desired_cell));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SelectionViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto selections = requireField<SelectionSet>(value.field("selections"));
    auto first_visual_row =
        requireField<std::uint32_t>(value.field("first_visual_row"));
    auto first_visual_column =
        requireField<std::uint32_t>(value.field("first_visual_column"));
    if (!selections || !first_visual_row || !first_visual_column) return false;
    std::optional<CellIndex> desired_cell;
    if (!decodeOptionalField(value.field("desired_cell"), desired_cell)) return false;
    out.emplace(SelectionViewState{*selections, *first_visual_row,
                                   *first_visual_column, desired_cell});
    return true;
}

ProtocolValue toValue(SelectionViewDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SelectionViewDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<SelectionViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(SelectionViewDelta{*changed, std::move(replacement)});
    return true;
}

ProtocolValue toValue(HistoryViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("can_undo", toValue(value.can_undo));
    fields.emplace_back("can_redo", toValue(value.can_redo));
    fields.emplace_back("retained_bytes", toValue(value.retained_bytes));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<HistoryViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto can_undo = requireField<bool>(value.field("can_undo"));
    auto can_redo = requireField<bool>(value.field("can_redo"));
    auto retained_bytes = requireField<std::uint64_t>(value.field("retained_bytes"));
    if (!can_undo || !can_redo || !retained_bytes) return false;
    out.emplace(HistoryViewState{*can_undo, *can_redo, *retained_bytes});
    return true;
}

ProtocolValue toValue(HistoryDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<HistoryDelta>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<Rect>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<GridSize>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto columns = requireField<int>(value.field("columns"));
    auto rows = requireField<int>(value.field("rows"));
    if (!columns || !rows) return false;
    out.emplace(GridSize{*columns, *rows});
    return true;
}

ProtocolValue toValue(ShellLabel const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessible_label));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<ShellLabel>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto accessible_label = requireField<std::string>(value.field("accessible_label"));
    if (!id || !accessible_label) return false;
    out.emplace(ShellLabel{*id, *accessible_label});
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
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<AccessibilityNode>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto kind = requireField<ShellNodeKind>(value.field("kind"));
    auto id = requireField<std::string>(value.field("id"));
    auto label = requireField<std::string>(value.field("label"));
    auto rect = requireField<Rect>(value.field("rect"));
    auto role = requireField<SemanticRole>(value.field("role"));
    auto content = requireField<std::string>(value.field("content"));
    if (!kind || !id || !label || !rect || !role || !content) return false;
    out.emplace(AccessibilityNode{*kind, *id, *label, *rect, *role, *content});
    return true;
}

ProtocolValue toValue(PaneGeometry const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("frame", toValue(value.frame));
    fields.emplace_back("content", toValue(value.content));
    fields.emplace_back("scrollbar", toValue(value.scrollbar));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<PaneGeometry>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<PaneId>(value.field("id"));
    auto frame = requireField<Rect>(value.field("frame"));
    auto content = requireField<Rect>(value.field("content"));
    auto scrollbar = requireField<Rect>(value.field("scrollbar"));
    if (!id || !frame || !content || !scrollbar) return false;
    out.emplace(PaneGeometry{*id, *frame, *content, *scrollbar});
    return true;
}

ProtocolValue toValue(TabHit const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("rect", toValue(value.rect));
    fields.emplace_back("index", toValue(value.index));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TabHit>& out) {
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
    fields.emplace_back("tab_bar", toValue(value.tab_bar));
    fields.emplace_back("panel", toValue(value.panel));
    fields.emplace_back("panel_scrollbar", toValue(value.panel_scrollbar));
    fields.emplace_back("prompt", toValue(value.prompt));
    fields.emplace_back("panes", toValue(value.panes));
    fields.emplace_back("tab_hits", toValue(value.tab_hits));
    fields.emplace_back("accessibility_nodes", toValue(value.accessibility_nodes));
    fields.emplace_back("focus", toValue(value.focus));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ShellViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto viewport = requireField<GridSize>(value.field("viewport"));
    auto panes = requireField<std::vector<PaneGeometry>>(value.field("panes"));
    auto tab_hits = requireField<std::vector<TabHit>>(value.field("tab_hits"));
    auto accessibility_nodes =
        requireField<std::vector<AccessibilityNode>>(value.field("accessibility_nodes"));
    if (!viewport || !panes || !tab_hits || !accessibility_nodes) return false;
    ShellViewState result;
    result.viewport = *viewport;
    if (auto const* focus_field = value.field("focus")) {
        std::optional<FocusTarget> focus;
        if (!decodePresent(*focus_field, focus) || !focus) return false;
        result.focus = *focus;
    }
    if (!decodeOptionalField(value.field("header"), result.header)) return false;
    if (!decodeOptionalField(value.field("footer"), result.footer)) return false;
    if (!decodeOptionalField(value.field("tab_bar"), result.tab_bar)) return false;
    if (!decodeOptionalField(value.field("panel"), result.panel)) return false;
    if (!decodeOptionalField(value.field("panel_scrollbar"), result.panel_scrollbar)) return false;
    if (!decodeOptionalField(value.field("prompt"), result.prompt)) return false;
    result.panes = *panes;
    result.tab_hits = *tab_hits;
    result.accessibility_nodes = *accessibility_nodes;
    out.emplace(std::move(result));
    return true;
}


ProtocolValue toValue(PromptInput const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessible_label));
    fields.emplace_back("value", toValue(value.value));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptInput>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto accessible_label = requireField<std::string>(value.field("accessible_label"));
    auto text_value = requireField<std::string>(value.field("value"));
    if (!id || !accessible_label || !text_value) return false;
    out.emplace(PromptInput{*id, *accessible_label, *text_value});
    return true;
}

ProtocolValue toValue(PromptToggle const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessible_label));
    fields.emplace_back("value", toValue(value.value));
    fields.emplace_back("width", toValue(value.width));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptToggle>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto accessible_label = requireField<std::string>(value.field("accessible_label"));
    auto toggle_value = requireField<bool>(value.field("value"));
    auto width = requireField<int>(value.field("width"));
    if (!id || !accessible_label || !toggle_value || !width) return false;
    out.emplace(PromptToggle{*id, *accessible_label, *toggle_value, *width});
    return true;
}

ProtocolValue toValue(PromptMatchCount const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessible_label));
    fields.emplace_back("value", toValue(value.value));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decodePresent(ProtocolValue const& value, std::optional<PromptMatchCount>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto accessible_label = requireField<std::string>(value.field("accessible_label"));
    auto text_value = requireField<std::string>(value.field("value"));
    if (!id || !accessible_label || !text_value) return false;
    out.emplace(PromptMatchCount{*id, *accessible_label, *text_value});
    return true;
}

ProtocolValue toValue(PromptControlView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessible_label));
    fields.emplace_back("value", toValue(value.value));
    fields.emplace_back("checked", toValue(value.checked));
    fields.emplace_back("rect", toValue(value.rect));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<PromptControlView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto kind = requireField<PromptControlKind>(value.field("kind"));
    auto id = requireField<std::string>(value.field("id"));
    auto accessible_label = requireField<std::string>(value.field("accessible_label"));
    auto text_value = requireField<std::string>(value.field("value"));
    auto checked = requireField<bool>(value.field("checked"));
    auto rect = requireField<Rect>(value.field("rect"));
    if (!kind || !id || !accessible_label || !text_value || !checked || !rect) {
        return false;
    }
    out.emplace(PromptControlView{*kind, *id, *accessible_label, *text_value,
                                  *checked, *rect});
    return true;
}

ProtocolValue toValue(PromptViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("accessible_label", toValue(value.accessible_label));
    fields.emplace_back("rect", toValue(value.rect));
    fields.emplace_back("controls", toValue(value.controls));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<PromptViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto kind = requireField<PromptKind>(value.field("kind"));
    auto accessible_label = requireField<std::string>(value.field("accessible_label"));
    auto rect = requireField<Rect>(value.field("rect"));
    auto controls = requireField<std::vector<PromptControlView>>(value.field("controls"));
    if (!kind || !accessible_label || !rect || !controls) return false;
    out.emplace(PromptViewState{*kind, *accessible_label, *rect, *controls});
    return true;
}


ProtocolValue toValue(StatusAction const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("accessible_label", toValue(value.accessible_label));
    fields.emplace_back("command_id", toValue(value.command_id));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<StatusAction>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto accessible_label = requireField<std::string>(value.field("accessible_label"));
    auto command_id = requireField<std::string>(value.field("command_id"));
    if (!id || !accessible_label || !command_id) return false;
    out.emplace(StatusAction{*id, *accessible_label, *command_id});
    return true;
}

ProtocolValue toValue(StatusItemView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("priority", toValue(value.priority));
    fields.emplace_back("generation", toValue(value.generation));
    fields.emplace_back("accessible_label", toValue(value.accessible_label));
    fields.emplace_back("actions", toValue(value.actions));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<StatusItemView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<StatusId>(value.field("id"));
    auto priority = requireField<StatusPriority>(value.field("priority"));
    auto generation = requireField<std::uint64_t>(value.field("generation"));
    auto accessible_label = requireField<std::string>(value.field("accessible_label"));
    auto actions = requireField<std::vector<StatusAction>>(value.field("actions"));
    if (!id || !priority || !generation || !accessible_label || !actions) {
        return false;
    }
    out.emplace(StatusItemView{*id, *priority, *generation, *accessible_label,
                               *actions});
    return true;
}

ProtocolValue toValue(StatusViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("items", toValue(value.items));
    fields.emplace_back("selected", toValue(static_cast<std::uint64_t>(value.selected)));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<StatusViewState>& out) {
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
    fields.emplace_back("status_id", toValue(value.status_id));
    fields.emplace_back("action_id", toValue(value.action_id));
    fields.emplace_back("generation", toValue(value.generation));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<StatusActionInvocation>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto status_id = requireField<StatusId>(value.field("status_id"));
    auto action_id = requireField<std::string>(value.field("action_id"));
    auto generation = requireField<std::uint64_t>(value.field("generation"));
    if (!status_id || !action_id || !generation) return false;
    out.emplace(StatusActionInvocation{*status_id, *action_id, *generation});
    return true;
}

ProtocolValue toValue(PromptStatusViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("prompt", toValue(value.prompt));
    fields.emplace_back("status", toValue(value.status));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<PromptStatusViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto status = requireField<StatusViewState>(value.field("status"));
    if (!status) return false;
    std::optional<PromptViewState> prompt;
    if (!decodeOptionalField(value.field("prompt"), prompt)) return false;
    out.emplace(PromptStatusViewState{std::move(prompt), *status});
    return true;
}

ProtocolValue toValue(PromptStatusDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<PromptStatusDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<PromptStatusViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(PromptStatusDelta{*changed, std::move(replacement)});
    return true;
}


ProtocolValue toValue(ClipboardRequest const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("request_revision", toValue(value.request_revision));
    fields.emplace_back("text", toValue(value.text));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ClipboardRequest>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::uint64_t>(value.field("id"));
    auto kind = requireField<ClipboardRequestKind>(value.field("kind"));
    auto request_revision = requireField<Revision>(value.field("request_revision"));
    auto text = requireField<std::string>(value.field("text"));
    if (!id || !kind || !request_revision || !text) return false;
    out.emplace(ClipboardRequest{*id, *kind, *request_revision, *text});
    return true;
}

ProtocolValue toValue(ClipboardResponse const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("request_revision", toValue(value.request_revision));
    fields.emplace_back("observed_document_revision",
                        toValue(value.observed_document_revision));
    fields.emplace_back("status", toValue(value.status));
    fields.emplace_back("text", toValue(value.text));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ClipboardResponse>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::uint64_t>(value.field("id"));
    auto request_revision = requireField<Revision>(value.field("request_revision"));
    auto observed_document_revision =
        requireField<Revision>(value.field("observed_document_revision"));
    auto status = requireField<ClipboardResponseStatus>(value.field("status"));
    auto text = requireField<std::string>(value.field("text"));
    if (!id || !request_revision || !observed_document_revision || !status || !text) {
        return false;
    }
    out.emplace(ClipboardResponse{*id, *request_revision,
                                  *observed_document_revision, *status, *text});
    return true;
}

ProtocolValue toValue(ClipboardViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("fragments", toValue(value.fragments));
    fields.emplace_back("plain_text", toValue(value.plain_text));
    fields.emplace_back("pending_read", toValue(value.pending_read));
    fields.emplace_back("pending_write", toValue(value.pending_write));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ClipboardViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto fragments = requireField<std::vector<std::string>>(value.field("fragments"));
    auto plain_text = requireField<std::string>(value.field("plain_text"));
    if (!fragments || !plain_text) return false;
    ClipboardViewState result;
    result.fragments = *fragments;
    result.plain_text = *plain_text;
    if (!decodeOptionalField(value.field("pending_read"), result.pending_read)) {
        return false;
    }
    if (!decodeOptionalField(value.field("pending_write"), result.pending_write)) {
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
bool decode_present(ProtocolValue const& value, std::optional<ClipboardDelta>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<SearchResult>& out) {
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
    fields.emplace_back("palette_open", toValue(value.palette_open));
    fields.emplace_back("query", toValue(value.query));
    fields.emplace_back("mode", toValue(value.mode));
    fields.emplace_back("results", toValue(value.results));
    if (value.selected_index) {
        fields.emplace_back("selected_index",
                            toValue(static_cast<std::uint64_t>(*value.selected_index)));
    } else {
        fields.emplace_back("selected_index", ProtocolValue::makeNull());
    }
    fields.emplace_back("search_generation", toValue(value.search_generation));
    fields.emplace_back("searching", toValue(value.searching));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SearchViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto palette_open = requireField<bool>(value.field("palette_open"));
    auto query = requireField<std::string>(value.field("query"));
    auto mode = requireField<SearchMode>(value.field("mode"));
    auto results = requireField<std::vector<SearchResult>>(value.field("results"));
    auto search_generation = requireField<std::uint64_t>(value.field("search_generation"));
    auto searching = requireField<bool>(value.field("searching"));
    if (!revision || !palette_open || !query || !mode || !results ||
        !search_generation || !searching) {
        return false;
    }
    SearchViewState result;
    result.revision = *revision;
    result.palette_open = *palette_open;
    result.query = *query;
    result.mode = *mode;
    result.results = *results;
    std::optional<std::uint64_t> selected_index;
    if (!decodeOptionalField(value.field("selected_index"), selected_index)) {
        return false;
    }
    if (selected_index) {
        result.selected_index = static_cast<std::size_t>(*selected_index);
    }
    result.search_generation = *search_generation;
    result.searching = *searching;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(SearchDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(value.base_revision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("state", toValue(value.state));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SearchDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto base_revision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    if (!base_revision || !revision) return false;
    SearchDelta result;
    result.base_revision = *base_revision;
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
bool decode_present(ProtocolValue const& value, std::optional<ByteRange>& out) {
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
    fields.emplace_back("case_sensitive", toValue(value.case_sensitive));
    fields.emplace_back("whole_word", toValue(value.whole_word));
    fields.emplace_back("regex", toValue(value.regex));
    fields.emplace_back("selection_only", toValue(value.selection_only));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FindOptions>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto case_sensitive = requireField<bool>(value.field("case_sensitive"));
    auto whole_word = requireField<bool>(value.field("whole_word"));
    auto regex = requireField<bool>(value.field("regex"));
    auto selection_only = requireField<bool>(value.field("selection_only"));
    if (!case_sensitive || !whole_word || !regex || !selection_only) return false;
    out.emplace(FindOptions{*case_sensitive, *whole_word, *regex, *selection_only});
    return true;
}

ProtocolValue toValue(FindRequest const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("query", toValue(value.query));
    fields.emplace_back("options", toValue(value.options));
    fields.emplace_back("selection", toValue(value.selection));
    fields.emplace_back("work_budget", toValue(value.work_budget));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FindRequest>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto query = requireField<std::string>(value.field("query"));
    auto options = requireField<FindOptions>(value.field("options"));
    std::optional<ByteRange> selection;
    auto work_budget = requireField<std::uint64_t>(value.field("work_budget"));
    if (!query || !options ||
        !decodeOptionalField(value.field("selection"), selection) ||
        !work_budget) {
        return false;
    }
    out.emplace(FindRequest{*query, *options, std::move(selection),
                            *work_budget, nullptr});
    return true;
}

ProtocolValue toValue(FindMatch const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("begin", toValue(value.begin));
    fields.emplace_back("end", toValue(value.end));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FindMatch>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<WorkspaceFileReplacement>& out) {
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
    fields.emplace_back("source_revision", toValue(value.source_revision));
    fields.emplace_back("query", toValue(value.query));
    fields.emplace_back("replacement", toValue(value.replacement));
    fields.emplace_back("options", toValue(value.options));
    fields.emplace_back("changes", toValue(value.changes));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<WorkspaceReplacePreview>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto source_revision = requireField<Revision>(value.field("source_revision"));
    auto query = requireField<std::string>(value.field("query"));
    auto replacement = requireField<std::string>(value.field("replacement"));
    auto options = requireField<FindOptions>(value.field("options"));
    auto changes = requireField<std::vector<WorkspaceFileReplacement>>(value.field("changes"));
    if (!source_revision || !query || !replacement || !options || !changes) return false;
    out.emplace(WorkspaceReplacePreview{*source_revision, *query,
                                        *replacement, *options, *changes});
    return true;
}

ProtocolValue toValue(FindReplaceViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("generation", toValue(value.generation));
    fields.emplace_back("open", toValue(value.open));
    fields.emplace_back("replace_mode", toValue(value.replace_mode));
    fields.emplace_back("source_revision", toValue(value.source_revision));
    fields.emplace_back("query", toValue(value.query));
    fields.emplace_back("replacement", toValue(value.replacement));
    fields.emplace_back("options", toValue(value.options));
    fields.emplace_back("matches", toValue(value.matches));
    if (value.active_match) {
        fields.emplace_back("active_match",
                            toValue(static_cast<std::uint64_t>(*value.active_match)));
    } else {
        fields.emplace_back("active_match", ProtocolValue::makeNull());
    }
    fields.emplace_back("error", toValue(value.error));
    fields.emplace_back("message", toValue(value.message));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FindReplaceViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto generation = requireField<std::uint64_t>(value.field("generation"));
    auto open = requireField<bool>(value.field("open"));
    auto replace_mode = requireField<bool>(value.field("replace_mode"));
    auto source_revision = requireField<Revision>(value.field("source_revision"));
    auto query = requireField<std::string>(value.field("query"));
    auto replacement = requireField<std::string>(value.field("replacement"));
    auto options = requireField<FindOptions>(value.field("options"));
    auto matches = requireField<std::vector<FindMatch>>(value.field("matches"));
    auto error = requireField<FindReplaceError>(value.field("error"));
    auto message = requireField<std::string>(value.field("message"));
    if (!generation || !open || !replace_mode || !source_revision || !query ||
        !replacement || !options || !matches || !error || !message) {
        return false;
    }
    FindReplaceViewState result;
    result.generation = *generation;
    result.open = *open;
    result.replace_mode = *replace_mode;
    result.source_revision = *source_revision;
    result.query = *query;
    result.replacement = *replacement;
    result.options = *options;
    result.matches = *matches;
    std::optional<std::uint64_t> active_match;
    if (!decodeOptionalField(value.field("active_match"), active_match)) {
        return false;
    }
    if (active_match) result.active_match = static_cast<std::size_t>(*active_match);
    result.error = *error;
    result.message = *message;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(FindReplaceDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("base_generation", toValue(value.base_generation));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FindReplaceDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changed = requireField<bool>(value.field("changed"));
    auto base_generation = requireField<std::uint64_t>(value.field("base_generation"));
    if (!changed || !base_generation) return false;
    FindReplaceDelta result;
    result.changed = *changed;
    result.base_generation = *base_generation;
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
bool decode_present(ProtocolValue const& value, std::optional<SettingValue>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto index = requireField<std::uint64_t>(value.field("index"));
    if (!index) return false;
    auto const* alt_value = value.field("value");
    if (!alt_value) return false;
    switch (*index) {
        case 0: {
            auto decoded = requireField<bool>(alt_value);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<0>, *decoded});
            return true;
        }
        case 1: {
            auto decoded = requireField<std::uint32_t>(alt_value);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<1>, *decoded});
            return true;
        }
        case 2: {
            auto decoded = requireField<std::uint64_t>(alt_value);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<2>, *decoded});
            return true;
        }
        case 3: {
            auto decoded = requireField<IndentStyle>(alt_value);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<3>, *decoded});
            return true;
        }
        case 4: {
            auto decoded = requireField<LineEnding>(alt_value);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<4>, *decoded});
            return true;
        }
        case 5: {
            auto decoded = requireField<TextEncoding>(alt_value);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<5>, *decoded});
            return true;
        }
        case 6: {
            auto decoded = requireField<std::string>(alt_value);
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
bool decode_present(ProtocolValue const& value, std::optional<EffectiveSetting>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto setting_value = requireField<SettingValue>(value.field("value"));
    auto source = requireField<SettingScope>(value.field("source"));
    if (!setting_value || !source) return false;
    out.emplace(EffectiveSetting{*setting_value, *source});
    return true;
}

ProtocolValue toValue(SettingViewEntry const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("key", toValue(value.key));
    fields.emplace_back("effective", toValue(value.effective));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SettingViewEntry>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<SettingsViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto entries =
        requireField<std::array<SettingViewEntry, setting_key_count>>(
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
bool decode_present(ProtocolValue const& value, std::optional<SettingsDelta>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<SettingsSectionDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto changes = requireField<std::vector<SettingsDelta>>(value.field("changes"));
    if (!changes) return false;
    out.emplace(SettingsSectionDelta{*changes});
    return true;
}


ProtocolValue toValue(KeyStroke const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("code", toValue(value.code));
    fields.emplace_back("control", toValue(value.control));
    fields.emplace_back("alt", toValue(value.alt));
    fields.emplace_back("meta", toValue(value.meta));
    fields.emplace_back("shift", toValue(value.shift));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<KeyStroke>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto code = requireField<std::string>(value.field("code"));
    auto control = requireField<bool>(value.field("control"));
    auto alt = requireField<bool>(value.field("alt"));
    auto meta = requireField<bool>(value.field("meta"));
    auto shift = requireField<bool>(value.field("shift"));
    if (!code || !control || !alt || !meta || !shift) return false;
    out.emplace(KeyStroke{*code, *control, *alt, *meta, *shift});
    return true;
}

ProtocolValue toValue(KeyBinding const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("sequence", toValue(value.sequence));
    fields.emplace_back("command_id", toValue(value.command_id));
    fields.emplace_back("context", toValue(value.context));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<KeyBinding>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto sequence = requireField<std::vector<KeyStroke>>(value.field("sequence"));
    auto command_id = requireField<std::string>(value.field("command_id"));
    auto context = requireField<std::string>(value.field("context"));
    if (!sequence || !command_id || !context) return false;
    out.emplace(KeyBinding{*sequence, *command_id, *context});
    return true;
}

ProtocolValue toValue(KeymapViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("name", toValue(value.name));
    fields.emplace_back("bindings", toValue(value.bindings));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<KeymapViewState>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<KeymapDelta>& out) {
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
    fields.emplace_back("line_ending", toValue(value.line_ending));
    fields.emplace_back("had_bom", toValue(value.had_bom));
    fields.emplace_back("final_newline", toValue(value.final_newline));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TextEncodingStatus>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto encoding = requireField<TextEncoding>(value.field("encoding"));
    auto line_ending = requireField<LineEnding>(value.field("line_ending"));
    auto had_bom = requireField<bool>(value.field("had_bom"));
    auto final_newline = requireField<bool>(value.field("final_newline"));
    if (!encoding || !line_ending || !had_bom || !final_newline) return false;
    out.emplace(TextEncodingStatus{*encoding, *line_ending, *had_bom, *final_newline});
    return true;
}

ProtocolValue toValue(TextEncodingViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("status", toValue(value.status));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TextEncodingViewState>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<TextEncodingDelta>& out) {
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
    fields.emplace_back("document_key", toValue(value.document_key));
    fields.emplace_back("content_identity", toValue(value.content_identity));
    fields.emplace_back("label", toValue(value.label));
    fields.emplace_back("mode", toValue(value.mode));
    fields.emplace_back("dirty", toValue(value.dirty));
    fields.emplace_back("recovery", toValue(value.recovery));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TabState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<TabId>(value.field("id"));
    auto kind = requireField<TabKind>(value.field("kind"));
    auto content_identity = requireField<std::string>(value.field("content_identity"));
    auto label = requireField<std::string>(value.field("label"));
    auto mode = requireField<DocumentMode>(value.field("mode"));
    auto dirty = requireField<bool>(value.field("dirty"));
    auto recovery = requireField<TabRecoveryBadge>(value.field("recovery"));
    if (!id || !kind || !content_identity || !label || !mode || !dirty || !recovery) {
        return false;
    }
    TabState result;
    result.id = *id;
    result.kind = *kind;
    if (!decodeOptionalField(value.field("document"), result.document)) return false;
    if (!decodeOptionalField(value.field("document_key"), result.document_key)) {
        return false;
    }
    result.content_identity = *content_identity;
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
bool decode_present(ProtocolValue const& value, std::optional<TabViewState>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<TabDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    TabDelta result;
    if (!decodeOptionalField(value.field("state"), result.state)) return false;
    out.emplace(std::move(result));
    return true;
}


ProtocolValue toValue(DiffLineChange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", toValue(value.kind));
    if (value.baseline_line) {
        fields.emplace_back("baseline_line",
                            toValue(static_cast<std::uint64_t>(*value.baseline_line)));
    } else {
        fields.emplace_back("baseline_line", ProtocolValue::makeNull());
    }
    if (value.target_line) {
        fields.emplace_back("target_line",
                            toValue(static_cast<std::uint64_t>(*value.target_line)));
    } else {
        fields.emplace_back("target_line", ProtocolValue::makeNull());
    }
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DiffLineChange>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto kind = requireField<DiffLineKind>(value.field("kind"));
    if (!kind) return false;
    DiffLineChange result;
    result.kind = *kind;
    std::optional<std::uint64_t> baseline_line;
    if (!decodeOptionalField(value.field("baseline_line"), baseline_line)) return false;
    if (baseline_line) result.baseline_line = static_cast<std::size_t>(*baseline_line);
    std::optional<std::uint64_t> target_line;
    if (!decodeOptionalField(value.field("target_line"), target_line)) return false;
    if (target_line) result.target_line = static_cast<std::size_t>(*target_line);
    out.emplace(result);
    return true;
}

ProtocolValue toValue(DiffHunk const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("baseline_start",
                        toValue(static_cast<std::uint64_t>(value.baseline_start)));
    fields.emplace_back("target_start",
                        toValue(static_cast<std::uint64_t>(value.target_start)));
    fields.emplace_back("baseline_lines", toValue(value.baseline_lines));
    fields.emplace_back("target_lines", toValue(value.target_lines));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DiffHunk>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto baseline_start = requireField<std::uint64_t>(value.field("baseline_start"));
    auto target_start = requireField<std::uint64_t>(value.field("target_start"));
    auto baseline_lines = requireField<std::vector<std::string>>(value.field("baseline_lines"));
    auto target_lines = requireField<std::vector<std::string>>(value.field("target_lines"));
    if (!baseline_start || !target_start || !baseline_lines || !target_lines) {
        return false;
    }
    DiffHunk result;
    result.baseline_start = static_cast<std::size_t>(*baseline_start);
    result.target_start = static_cast<std::size_t>(*target_start);
    result.baseline_lines = *baseline_lines;
    result.target_lines = *target_lines;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(DiffFileView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("path", toValue(value.path));
    fields.emplace_back("previous_path", toValue(value.previous_path));
    fields.emplace_back("deleted", toValue(value.deleted));
    fields.emplace_back("baseline_identity", toValue(value.baseline_identity));
    fields.emplace_back("current_content", toValue(value.current_content));
    fields.emplace_back("hunks", toValue(value.hunks));
    fields.emplace_back("changed_lines", toValue(value.changed_lines));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DiffFileView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<DiffFileId>(value.field("id"));
    auto path = requireField<std::filesystem::path>(value.field("path"));
    auto deleted = requireField<bool>(value.field("deleted"));
    auto baseline_identity = requireField<std::string>(value.field("baseline_identity"));
    auto current_content = requireField<std::string>(value.field("current_content"));
    auto hunks = requireField<std::vector<DiffHunk>>(value.field("hunks"));
    auto changed_lines = requireField<std::vector<DiffLineChange>>(value.field("changed_lines"));
    if (!id || !path || !deleted || !baseline_identity || !current_content || !hunks ||
        !changed_lines) {
        return false;
    }
    std::optional<std::filesystem::path> previous_path;
    if (!decodeOptionalField(value.field("previous_path"), previous_path)) {
        return false;
    }
    out.emplace(DiffFileView{*id, *path, std::move(previous_path), *deleted,
                            *baseline_identity, *current_content, *hunks,
                            *changed_lines});
    return true;
}

ProtocolValue toValue(DiffViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("files", toValue(value.files));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DiffViewState>& out) {
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
    fields.emplace_back("base_revision", toValue(value.base_revision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("upserted", toValue(value.upserted));
    fields.emplace_back("removed", toValue(value.removed));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DiffDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto base_revision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    auto upserted = requireField<std::vector<DiffFileView>>(value.field("upserted"));
    auto removed = requireField<std::vector<DiffFileId>>(value.field("removed"));
    if (!base_revision || !revision || !upserted || !removed) return false;
    out.emplace(DiffDelta{*base_revision, *revision, *upserted, *removed});
    return true;
}


ProtocolValue toValue(ExternalDocumentView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("path", toValue(value.path));
    fields.emplace_back("status", toValue(value.status));
    fields.emplace_back("accessible_status", toValue(value.accessible_status));
    fields.emplace_back("actions", toValue(value.actions));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ExternalDocumentView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<DiffFileId>(value.field("id"));
    auto path = requireField<std::filesystem::path>(value.field("path"));
    auto status = requireField<ExternalDocumentStatus>(value.field("status"));
    auto accessible_status = requireField<std::string>(value.field("accessible_status"));
    auto actions = requireField<std::vector<ExternalAction>>(value.field("actions"));
    if (!id || !path || !status || !accessible_status || !actions) return false;
    out.emplace(ExternalDocumentView{*id, *path, *status, *accessible_status, *actions});
    return true;
}

ProtocolValue toValue(ExternalModificationViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("files", toValue(value.files));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ExternalModificationViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto files = requireField<std::vector<ExternalDocumentView>>(value.field("files"));
    if (!revision || !files) return false;
    out.emplace(ExternalModificationViewState{*revision, *files});
    return true;
}

ProtocolValue toValue(ExternalModificationDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(value.base_revision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("upserted", toValue(value.upserted));
    fields.emplace_back("removed", toValue(value.removed));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ExternalModificationDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto base_revision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    auto upserted = requireField<std::vector<ExternalDocumentView>>(value.field("upserted"));
    auto removed = requireField<std::vector<DiffFileId>>(value.field("removed"));
    if (!base_revision || !revision || !upserted || !removed) return false;
    out.emplace(ExternalModificationDelta{*base_revision, *revision, *upserted, *removed});
    return true;
}


ProtocolValue toValue(ViewportDimensions const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("columns", toValue(value.columns));
    fields.emplace_back("rows", toValue(value.rows));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ViewportDimensions>& out) {
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
    fields.emplace_back("logical_line", toValue(value.logical_line));
    fields.emplace_back("first_span", toValue(value.first_span));
    fields.emplace_back("span_count", toValue(value.span_count));
    fields.emplace_back("start_cell", toValue(value.start_cell));
    fields.emplace_back("content_cells", toValue(value.content_cells));
    fields.emplace_back("visible_cells", toValue(value.visible_cells));
    fields.emplace_back("end_byte_offset", toValue(value.end_byte_offset));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<VisualRow>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto logical_line = requireField<std::uint32_t>(value.field("logical_line"));
    auto first_span = requireField<std::uint32_t>(value.field("first_span"));
    auto span_count = requireField<std::uint32_t>(value.field("span_count"));
    auto start_cell = requireField<CellIndex>(value.field("start_cell"));
    auto content_cells = requireField<std::uint32_t>(value.field("content_cells"));
    auto visible_cells = requireField<std::uint32_t>(value.field("visible_cells"));
    auto end_byte_offset = requireField<std::uint32_t>(value.field("end_byte_offset"));
    if (!logical_line || !first_span || !span_count || !start_cell || !content_cells ||
        !visible_cells || !end_byte_offset) {
        return false;
    }
    out.emplace(VisualRow{*logical_line, *first_span, *span_count, *start_cell,
                          *content_cells, *visible_cells, *end_byte_offset});
    return true;
}

ProtocolValue toValue(CellHitTarget const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("viewport_row", toValue(value.viewport_row));
    fields.emplace_back("viewport_column", toValue(value.viewport_column));
    fields.emplace_back("logical_line", toValue(value.logical_line));
    fields.emplace_back("cell", toValue(value.cell));
    fields.emplace_back("byte_offset", toValue(value.byte_offset));
    fields.emplace_back("byte_len", toValue(value.byte_len));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<CellHitTarget>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto viewport_row = requireField<std::uint32_t>(value.field("viewport_row"));
    auto viewport_column = requireField<std::uint32_t>(value.field("viewport_column"));
    auto logical_line = requireField<std::uint32_t>(value.field("logical_line"));
    auto cell = requireField<CellIndex>(value.field("cell"));
    auto byte_offset = requireField<std::uint32_t>(value.field("byte_offset"));
    auto byte_len = requireField<std::uint32_t>(value.field("byte_len"));
    if (!viewport_row || !viewport_column || !logical_line || !cell || !byte_offset ||
        !byte_len) {
        return false;
    }
    out.emplace(CellHitTarget{*viewport_row, *viewport_column, *logical_line, *cell,
                              *byte_offset, *byte_len});
    return true;
}

ProtocolValue toValue(ScrollbarMetrics const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("total_rows", toValue(value.total_rows));
    fields.emplace_back("viewport_rows", toValue(value.viewport_rows));
    fields.emplace_back("first_row", toValue(value.first_row));
    fields.emplace_back("maximum_first_row", toValue(value.maximum_first_row));
    fields.emplace_back("thumb_start", toValue(value.thumb_start));
    fields.emplace_back("thumb_size", toValue(value.thumb_size));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ScrollbarMetrics>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto total_rows = requireField<std::uint32_t>(value.field("total_rows"));
    auto viewport_rows = requireField<std::uint32_t>(value.field("viewport_rows"));
    auto first_row = requireField<std::uint32_t>(value.field("first_row"));
    auto maximum_first_row = requireField<std::uint32_t>(value.field("maximum_first_row"));
    auto thumb_start = requireField<std::uint32_t>(value.field("thumb_start"));
    auto thumb_size = requireField<std::uint32_t>(value.field("thumb_size"));
    if (!total_rows || !viewport_rows || !first_row || !maximum_first_row || !thumb_start ||
        !thumb_size) {
        return false;
    }
    out.emplace(ScrollbarMetrics{*total_rows, *viewport_rows, *first_row,
                                 *maximum_first_row, *thumb_start, *thumb_size});
    return true;
}

ProtocolValue toValue(ViewportViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("dimensions", toValue(value.dimensions));
    fields.emplace_back("first_visual_row", toValue(value.first_visual_row));
    fields.emplace_back("first_visual_column", toValue(value.first_visual_column));
    fields.emplace_back("total_visual_rows", toValue(value.total_visual_rows));
    fields.emplace_back("visible_rows", toValue(value.visible_rows));
    fields.emplace_back("hit_targets", toValue(value.hit_targets));
    fields.emplace_back("scrollbar", toValue(value.scrollbar));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ViewportViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto dimensions = requireField<ViewportDimensions>(value.field("dimensions"));
    auto first_visual_row = requireField<std::uint32_t>(value.field("first_visual_row"));
    auto first_visual_column = requireField<std::uint32_t>(value.field("first_visual_column"));
    auto total_visual_rows = requireField<std::uint32_t>(value.field("total_visual_rows"));
    auto visible_rows = requireField<std::vector<VisualRow>>(value.field("visible_rows"));
    auto hit_targets = requireField<std::vector<CellHitTarget>>(value.field("hit_targets"));
    auto scrollbar = requireField<ScrollbarMetrics>(value.field("scrollbar"));
    if (!dimensions || !first_visual_row || !first_visual_column ||
        !total_visual_rows || !visible_rows || !hit_targets || !scrollbar) {
        return false;
    }
    out.emplace(ViewportViewState{*dimensions, *first_visual_row,
                                  *first_visual_column, *total_visual_rows,
                                  *visible_rows, *hit_targets, *scrollbar});
    return true;
}

ProtocolValue toValue(ViewportDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", toValue(value.changed));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ViewportDelta>& out) {
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
    fields.emplace_back("first_row", toValue(value.first_row));
    fields.emplace_back("first_column", toValue(value.first_column));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FollowScrollOffset>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto first_row = requireField<std::uint64_t>(value.field("first_row"));
    auto first_column = requireField<std::uint64_t>(value.field("first_column"));
    if (!first_row || !first_column) return false;
    out.emplace(FollowScrollOffset{*first_row, *first_column});
    return true;
}

ProtocolValue toValue(FollowTarget const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("path", toValue(value.path));
    fields.emplace_back("deleted", toValue(value.deleted));
    fields.emplace_back("newest_hunk_line",
                        toValue(static_cast<std::uint64_t>(value.newest_hunk_line)));
    fields.emplace_back("source_revision", toValue(value.source_revision));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FollowTarget>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<DiffFileId>(value.field("id"));
    auto path = requireField<std::filesystem::path>(value.field("path"));
    auto deleted = requireField<bool>(value.field("deleted"));
    auto newest_hunk_line = requireField<std::uint64_t>(value.field("newest_hunk_line"));
    auto source_revision = requireField<Revision>(value.field("source_revision"));
    if (!id || !path || !deleted || !newest_hunk_line || !source_revision) return false;
    out.emplace(FollowTarget{*id, *path, *deleted,
                             static_cast<std::size_t>(*newest_hunk_line),
                             *source_revision});
    return true;
}

ProtocolValue toValue(FollowClientView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("client", toValue(value.client));
    fields.emplace_back("dimensions", toValue(value.dimensions));
    fields.emplace_back("offset", toValue(value.offset));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FollowClientView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto client = requireField<ClientId>(value.field("client"));
    auto dimensions = requireField<ViewportDimensions>(value.field("dimensions"));
    auto offset = requireField<FollowScrollOffset>(value.field("offset"));
    if (!client || !dimensions || !offset) return false;
    out.emplace(FollowClientView{*client, *dimensions, *offset});
    return true;
}

ProtocolValue toValue(FollowEditsViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("generation", toValue(value.generation));
    fields.emplace_back("mode", toValue(value.mode));
    fields.emplace_back("active_pane", toValue(value.active_pane));
    fields.emplace_back("active_target", toValue(value.active_target));
    fields.emplace_back("queued_targets", toValue(value.queued_targets));
    fields.emplace_back("clients", toValue(value.clients));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FollowEditsViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto generation = requireField<std::uint64_t>(value.field("generation"));
    auto mode = requireField<FollowMode>(value.field("mode"));
    auto active_pane = requireField<PaneId>(value.field("active_pane"));
    auto queued_targets = requireField<std::vector<FollowTarget>>(value.field("queued_targets"));
    auto clients = requireField<std::vector<FollowClientView>>(value.field("clients"));
    if (!generation || !mode || !active_pane || !queued_targets || !clients) return false;
    FollowEditsViewState result;
    result.generation = *generation;
    result.mode = *mode;
    result.active_pane = *active_pane;
    if (!decodeOptionalField(value.field("active_target"), result.active_target)) {
        return false;
    }
    result.queued_targets = *queued_targets;
    result.clients = *clients;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(FollowEditsDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_generation", toValue(value.base_generation));
    fields.emplace_back("generation", toValue(value.generation));
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FollowEditsDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto base_generation = requireField<std::uint64_t>(value.field("base_generation"));
    auto generation = requireField<std::uint64_t>(value.field("generation"));
    if (!base_generation || !generation) return false;
    FollowEditsDelta result;
    result.base_generation = *base_generation;
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
bool decode_present(ProtocolValue const& value, std::optional<TreeNodeCommand>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<std::string>(value.field("id"));
    auto label = requireField<std::string>(value.field("label"));
    if (!id || !label) return false;
    out.emplace(TreeNodeCommand{*id, *label});
    return true;
}

ProtocolValue toValue(TreeNode const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(value.id));
    fields.emplace_back("parent_id", toValue(value.parent_id));
    fields.emplace_back("label", toValue(value.label));
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("icon", toValue(value.icon));
    fields.emplace_back("commands", toValue(value.commands));
    fields.emplace_back("git_status", toValue(value.git_status));
    fields.emplace_back("workspace_path", toValue(value.workspace_path));
    if (value.source_line) {
        fields.emplace_back("source_line", toValue(*value.source_line));
    } else {
        fields.emplace_back("source_line", ProtocolValue::makeNull());
    }
    fields.emplace_back("expandable", toValue(value.expandable));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TreeNode>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto id = requireField<TreeNodeId>(value.field("id"));
    auto label = requireField<std::string>(value.field("label"));
    auto kind = requireField<TreeNodeKind>(value.field("kind"));
    auto commands = requireField<std::vector<TreeNodeCommand>>(value.field("commands"));
    auto expandable = requireField<bool>(value.field("expandable"));
    if (!id || !label || !kind || !commands || !expandable) return false;
    std::optional<TreeNodeId> parent_id;
    if (!decodeOptionalField(value.field("parent_id"), parent_id)) return false;
    std::optional<std::string> icon;
    if (!decodeOptionalField(value.field("icon"), icon)) return false;
    std::optional<GitTreeStatus> git_status;
    if (!decodeOptionalField(value.field("git_status"), git_status)) return false;
    std::optional<std::string> workspace_path;
    if (!decodeOptionalField(value.field("workspace_path"), workspace_path)) return false;
    std::optional<std::uint32_t> source_line;
    if (!decodeOptionalField(value.field("source_line"), source_line)) return false;
    out.emplace(TreeNode{*id, std::move(parent_id), *label, *kind, std::move(icon),
                         *commands, std::move(git_status), std::move(workspace_path),
                         std::move(source_line), *expandable});
    return true;
}

ProtocolValue toValue(TreeNodeView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("node", toValue(value.node));
    fields.emplace_back("depth", toValue(static_cast<std::uint64_t>(value.depth)));
    fields.emplace_back("expanded", toValue(value.expanded));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TreeNodeView>& out) {
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
    fields.emplace_back("provider_id", toValue(value.provider_id));
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("nodes", toValue(value.nodes));
    fields.emplace_back("selected", toValue(value.selected));
    fields.emplace_back("first_visible", toValue(value.first_visible));
    fields.emplace_back("scrollbar", toValue(value.scrollbar));
    fields.emplace_back("visible_node_ids", toValue(value.visible_node_ids));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TreeProviderView>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto provider_id = requireField<TreeProviderId>(value.field("provider_id"));
    auto kind = requireField<TreeProviderKind>(value.field("kind"));
    auto nodes = requireField<std::vector<TreeNodeView>>(value.field("nodes"));
    std::optional<TreeNodeId> selected;
    if (!provider_id || !kind || !nodes) return false;
    if (!decodeOptionalField(value.field("selected"), selected)) return false;
    auto first_visible = requireField<std::uint32_t>(value.field("first_visible"));
    auto scrollbar = requireField<ScrollbarMetrics>(value.field("scrollbar"));
    auto visible_node_ids =
        requireField<std::vector<TreeNodeId>>(value.field("visible_node_ids"));
    if (!first_visible || !scrollbar || !visible_node_ids) return false;
    out.emplace(TreeProviderView{*provider_id, *kind, *nodes, selected,
                                 *first_visible, *scrollbar,
                                 std::move(*visible_node_ids)});
    return true;
}

ProtocolValue toValue(TreeViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("providers", toValue(value.providers));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TreeViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<TreeRevision>(value.field("revision"));
    auto providers = requireField<std::vector<TreeProviderView>>(value.field("providers"));
    if (!revision || !providers) return false;
    out.emplace(TreeViewState{*revision, *providers});
    return true;
}

ProtocolValue toValue(TreeProviderDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("provider_id", toValue(value.provider_id));
    fields.emplace_back("kind", toValue(value.kind));
    fields.emplace_back("remove_provider", toValue(value.remove_provider));
    fields.emplace_back("start", toValue(static_cast<std::uint64_t>(value.start)));
    fields.emplace_back("erase_count", toValue(static_cast<std::uint64_t>(value.erase_count)));
    fields.emplace_back("insert", toValue(value.insert));
    fields.emplace_back("selected", toValue(value.selected));
    fields.emplace_back("first_visible", toValue(value.first_visible));
    fields.emplace_back("scrollbar", toValue(value.scrollbar));
    fields.emplace_back("visible_node_ids", toValue(value.visible_node_ids));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TreeProviderDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto provider_id = requireField<TreeProviderId>(value.field("provider_id"));
    auto kind = requireField<TreeProviderKind>(value.field("kind"));
    auto remove_provider = requireField<bool>(value.field("remove_provider"));
    auto start = requireField<std::uint64_t>(value.field("start"));
    auto erase_count = requireField<std::uint64_t>(value.field("erase_count"));
    auto insert = requireField<std::vector<TreeNodeView>>(value.field("insert"));
    if (!provider_id || !kind || !remove_provider || !start || !erase_count || !insert) {
        return false;
    }
    std::optional<TreeNodeId> selected;
    if (!decodeOptionalField(value.field("selected"), selected)) return false;
    auto first_visible = requireField<std::uint32_t>(value.field("first_visible"));
    auto scrollbar = requireField<ScrollbarMetrics>(value.field("scrollbar"));
    auto visible_node_ids =
        requireField<std::vector<TreeNodeId>>(value.field("visible_node_ids"));
    if (!first_visible || !scrollbar || !visible_node_ids) return false;
    out.emplace(TreeProviderDelta{*provider_id, *kind, *remove_provider,
                                  static_cast<std::size_t>(*start),
                                  static_cast<std::size_t>(*erase_count), *insert,
                                  selected, *first_visible, *scrollbar,
                                  std::move(*visible_node_ids)});
    return true;
}

ProtocolValue toValue(TreeDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(value.base_revision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("snapshot_required", toValue(value.snapshot_required));
    fields.emplace_back("providers", toValue(value.providers));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TreeDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto base_revision = requireField<TreeRevision>(value.field("base_revision"));
    auto revision = requireField<TreeRevision>(value.field("revision"));
    auto snapshot_required = requireField<bool>(value.field("snapshot_required"));
    auto providers = requireField<std::vector<TreeProviderDelta>>(value.field("providers"));
    if (!base_revision || !revision || !snapshot_required || !providers) return false;
    out.emplace(TreeDelta{*base_revision, *revision, *snapshot_required, *providers});
    return true;
}


ProtocolValue toValue(SyntaxRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("begin", toValue(value.begin));
    fields.emplace_back("end", toValue(value.end));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SyntaxRange>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<SyntaxSpan>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<SyntaxBracketPair>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<UnmatchedBracket>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<CommentToken>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<CommentRange>& out) {
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
    fields.emplace_back("line_start", toValue(value.line_start));
    fields.emplace_back("content_start", toValue(value.content_start));
    fields.emplace_back("spaces", toValue(value.spaces));
    fields.emplace_back("tabs", toValue(value.tabs));
    fields.emplace_back("columns", toValue(value.columns));
    fields.emplace_back("blank", toValue(value.blank));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LineIndentation>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto line = requireField<LineIndex>(value.field("line"));
    auto line_start = requireField<ByteOffset>(value.field("line_start"));
    auto content_start = requireField<ByteOffset>(value.field("content_start"));
    auto spaces = requireField<std::uint32_t>(value.field("spaces"));
    auto tabs = requireField<std::uint32_t>(value.field("tabs"));
    auto columns = requireField<std::uint32_t>(value.field("columns"));
    auto blank = requireField<bool>(value.field("blank"));
    if (!line || !line_start || !content_start || !spaces || !tabs || !columns || !blank) {
        return false;
    }
    out.emplace(LineIndentation{*line, *line_start, *content_start, *spaces, *tabs,
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
bool decode_present(ProtocolValue const& value, std::optional<SyntaxViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto revision = requireField<Revision>(value.field("revision"));
    auto language = requireField<LanguageId>(value.field("language"));
    auto text_bytes = requireField<std::uint64_t>(value.field("text_bytes"));
    auto spans = requireField<std::vector<SyntaxSpan>>(value.field("spans"));
    auto bracket_pairs = requireField<std::vector<SyntaxBracketPair>>(value.field("bracket_pairs"));
    auto unmatched_brackets =
        requireField<std::vector<UnmatchedBracket>>(value.field("unmatched_brackets"));
    auto comment_tokens = requireField<std::vector<CommentToken>>(value.field("comment_tokens"));
    auto comment_ranges = requireField<std::vector<CommentRange>>(value.field("comment_ranges"));
    auto indentation = requireField<std::vector<LineIndentation>>(value.field("indentation"));
    if (!revision || !language || !text_bytes || !spans || !bracket_pairs ||
        !unmatched_brackets || !comment_tokens || !comment_ranges || !indentation) {
        return false;
    }
    out.emplace(*revision, *language, *text_bytes, *spans, *bracket_pairs,
               *unmatched_brackets, *comment_tokens, *comment_ranges, *indentation);
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
bool decode_present(ProtocolValue const& value, std::optional<SyntaxDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto base_revision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    if (!base_revision || !revision) return false;
    std::optional<LanguageId> language;
    if (!decodeOptionalField(value.field("language"), language)) return false;
    std::optional<std::uint64_t> text_bytes;
    if (!decodeOptionalField(value.field("text_bytes"), text_bytes)) return false;
    std::optional<std::vector<SyntaxSpan>> spans;
    if (!decodeOptionalField(value.field("spans"), spans)) return false;
    std::optional<std::vector<SyntaxBracketPair>> bracket_pairs;
    if (!decodeOptionalField(value.field("bracket_pairs"), bracket_pairs)) return false;
    std::optional<std::vector<UnmatchedBracket>> unmatched_brackets;
    if (!decodeOptionalField(value.field("unmatched_brackets"), unmatched_brackets)) {
        return false;
    }
    std::optional<std::vector<CommentToken>> comment_tokens;
    if (!decodeOptionalField(value.field("comment_tokens"), comment_tokens)) return false;
    std::optional<std::vector<CommentRange>> comment_ranges;
    if (!decodeOptionalField(value.field("comment_ranges"), comment_ranges)) return false;
    std::optional<std::vector<LineIndentation>> indentation;
    if (!decodeOptionalField(value.field("indentation"), indentation)) return false;
    out.emplace(*base_revision, *revision, std::move(language), std::move(text_bytes),
               std::move(spans), std::move(bracket_pairs), std::move(unmatched_brackets),
               std::move(comment_tokens), std::move(comment_ranges), std::move(indentation));
    return true;
}


ProtocolValue toValue(LspPosition const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("line", toValue(value.line));
    fields.emplace_back("character", toValue(value.character));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspPosition>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<LspRange>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<LspDiagnostic>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<LspDocumentDiagnostics>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<LspSyncViewState>& out) {
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
    fields.emplace_back("base_revision", toValue(value.base_revision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("state", toValue(value.state));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspSyncDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto base_revision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    if (!base_revision || !revision) return false;
    LspSyncDelta result;
    result.base_revision = *base_revision;
    result.revision = *revision;
    if (!decodeOptionalField(value.field("state"), result.state)) return false;
    out.emplace(std::move(result));
    return true;
}


ProtocolValue toValue(LspCompletionItem const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("label", toValue(value.label));
    fields.emplace_back("detail", toValue(value.detail));
    fields.emplace_back("sort_text", toValue(value.sort_text));
    fields.emplace_back("insert_text", toValue(value.insert_text));
    fields.emplace_back("replacement_range", toValue(value.replacement_range));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspCompletionItem>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto label = requireField<std::string>(value.field("label"));
    auto detail = requireField<std::string>(value.field("detail"));
    auto sort_text = requireField<std::string>(value.field("sort_text"));
    auto insert_text = requireField<std::string>(value.field("insert_text"));
    if (!label || !detail || !sort_text || !insert_text) return false;
    LspCompletionItem result;
    result.label = *label;
    result.detail = *detail;
    result.sort_text = *sort_text;
    result.insert_text = *insert_text;
    if (!decodeOptionalField(value.field("replacement_range"), result.replacement_range)) {
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
    if (value.selected_index) {
        fields.emplace_back("selected_index",
                            toValue(static_cast<std::uint64_t>(*value.selected_index)));
    } else {
        fields.emplace_back("selected_index", ProtocolValue::makeNull());
    }
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspCompletionViewState>& out) {
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
    std::optional<std::uint64_t> selected_index;
    if (!decodeOptionalField(value.field("selected_index"), selected_index)) return false;
    if (selected_index) result.selected_index = static_cast<std::size_t>(*selected_index);
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(LspHover const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("contents", toValue(value.contents));
    fields.emplace_back("range", toValue(value.range));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspHover>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<LspNavigationTarget>& out) {
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
    if (value.selected_index) {
        fields.emplace_back("selected_index",
                            toValue(static_cast<std::uint64_t>(*value.selected_index)));
    } else {
        fields.emplace_back("selected_index", ProtocolValue::makeNull());
    }
    fields.emplace_back("user_navigation", toValue(value.user_navigation));
    fields.emplace_back("reveal_primary_caret", toValue(value.reveal_primary_caret));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspNavigationViewState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto targets = requireField<std::vector<LspNavigationTarget>>(value.field("targets"));
    auto user_navigation = requireField<bool>(value.field("user_navigation"));
    auto reveal_primary_caret = requireField<bool>(value.field("reveal_primary_caret"));
    if (!targets || !user_navigation || !reveal_primary_caret) return false;
    LspNavigationViewState result;
    result.targets = *targets;
    std::optional<std::uint64_t> selected_index;
    if (!decodeOptionalField(value.field("selected_index"), selected_index)) return false;
    if (selected_index) result.selected_index = static_cast<std::size_t>(*selected_index);
    result.user_navigation = *user_navigation;
    result.reveal_primary_caret = *reveal_primary_caret;
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
bool decode_present(ProtocolValue const& value, std::optional<LspFeatureViewState>& out) {
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
    fields.emplace_back("base_revision", toValue(value.base_revision));
    fields.emplace_back("revision", toValue(value.revision));
    fields.emplace_back("state", toValue(value.state));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspFeatureDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto base_revision = requireField<Revision>(value.field("base_revision"));
    auto revision = requireField<Revision>(value.field("revision"));
    if (!base_revision || !revision) return false;
    LspFeatureDelta result;
    result.base_revision = *base_revision;
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
bool decode_present(ProtocolValue const& value, std::optional<SrgbColor>& out) {
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
    fields.emplace_back("palette", toValue(value.palette));
    fields.emplace_back("semantic_indices", toValue(value.semantic_indices));
    fields.emplace_back("syntax_indices", toValue(value.syntax_indices));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ThemeSnapshot>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto palette =
        requireField<std::array<SrgbColor, theme_palette_size>>(value.field("palette"));
    auto semantic_indices = requireField<std::array<std::uint8_t, semantic_role_count>>(
        value.field("semantic_indices"));
    auto syntax_indices = requireField<std::array<std::uint8_t, syntax_scope_count>>(
        value.field("syntax_indices"));
    if (!palette || !semantic_indices || !syntax_indices) return false;
    out.emplace(ThemeSnapshot{*palette, *semantic_indices, *syntax_indices});
    return true;
}


ProtocolValue toValue(SessionTopology const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("active_workspace", toValue(value.active_workspace));
    fields.emplace_back("active_view", toValue(value.active_view));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SessionTopology>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    SessionTopology result;
    if (!decodeOptionalField(value.field("active_workspace"), result.active_workspace)) {
        return false;
    }
    if (!decodeOptionalField(value.field("active_view"), result.active_view)) {
        return false;
    }
    out.emplace(std::move(result));
    return true;
}


ProtocolValue toValue(ClientSnapshotState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("client_id", toValue(value.client_id));
    fields.emplace_back("view_id", toValue(value.view_id));
    fields.emplace_back("capabilities", toValue(value.capabilities));
    fields.emplace_back("viewport", toValue(value.viewport));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ClientSnapshotState>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto client_id = requireField<ClientId>(value.field("client_id"));
    auto view_id = requireField<ViewId>(value.field("view_id"));
    auto capabilities = requireField<std::vector<CapabilityId>>(value.field("capabilities"));
    auto viewport = requireField<ViewportViewState>(value.field("viewport"));
    if (!client_id || !view_id || !capabilities || !viewport) return false;
    out.emplace(ClientSnapshotState{*client_id, *view_id, *capabilities, *viewport});
    return true;
}

ProtocolValue toValue(SessionSnapshotSections const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("document", toValue(value.document));
    fields.emplace_back("selection", toValue(value.selection));
    fields.emplace_back("history", toValue(value.history));
    fields.emplace_back("clipboard", toValue(value.clipboard));
    fields.emplace_back("prompt_status", toValue(value.prompt_status));
    fields.emplace_back("search", toValue(value.search));
    fields.emplace_back("find_replace", toValue(value.find_replace));
    fields.emplace_back("settings", toValue(value.settings));
    fields.emplace_back("keymap", toValue(value.keymap));
    fields.emplace_back("text_encoding", toValue(value.text_encoding));
    fields.emplace_back("tabs", toValue(value.tabs));
    fields.emplace_back("diff", toValue(value.diff));
    fields.emplace_back("external_modification", toValue(value.external_modification));
    fields.emplace_back("follow_edits", toValue(value.follow_edits));
    fields.emplace_back("tree", toValue(value.tree));
    fields.emplace_back("syntax", toValue(value.syntax));
    fields.emplace_back("lsp_sync", toValue(value.lsp_sync));
    fields.emplace_back("lsp_features", toValue(value.lsp_features));
    fields.emplace_back("theme", toValue(value.theme));
    fields.emplace_back("shell", toValue(value.shell));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SessionSnapshotSections>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto document = requireField<DocumentViewState>(value.field("document"));
    auto selection = requireField<SelectionViewState>(value.field("selection"));
    auto history = requireField<HistoryViewState>(value.field("history"));
    auto clipboard = requireField<ClipboardViewState>(value.field("clipboard"));
    auto prompt_status = requireField<PromptStatusViewState>(value.field("prompt_status"));
    auto search = requireField<SearchViewState>(value.field("search"));
    auto find_replace = requireField<FindReplaceViewState>(value.field("find_replace"));
    auto settings = requireField<SettingsViewState>(value.field("settings"));
    auto keymap = requireField<KeymapViewState>(value.field("keymap"));
    auto text_encoding = requireField<TextEncodingViewState>(value.field("text_encoding"));
    auto tabs = requireField<TabViewState>(value.field("tabs"));
    auto diff = requireField<DiffViewState>(value.field("diff"));
    auto external_modification =
        requireField<ExternalModificationViewState>(value.field("external_modification"));
    auto follow_edits = requireField<FollowEditsViewState>(value.field("follow_edits"));
    auto tree = requireField<TreeViewState>(value.field("tree"));
    auto syntax = requireField<SyntaxViewState>(value.field("syntax"));
    auto lsp_sync = requireField<LspSyncViewState>(value.field("lsp_sync"));
    auto lsp_features = requireField<LspFeatureViewState>(value.field("lsp_features"));
    auto theme = requireField<ThemeSnapshot>(value.field("theme"));
    auto shell = requireField<ShellViewState>(value.field("shell"));
    if (!document || !selection || !history || !clipboard || !prompt_status || !search ||
        !find_replace || !settings || !keymap || !text_encoding || !tabs || !diff ||
        !external_modification || !follow_edits || !tree || !syntax || !lsp_sync ||
        !lsp_features || !theme || !shell) {
        return false;
    }
    out.emplace(SessionSnapshotSections{
        *document, *selection, *history, *clipboard, *prompt_status, *search,
        *find_replace, *settings, *keymap, *text_encoding, *tabs, *diff,
        *external_modification, *follow_edits, *tree, std::move(*syntax), *lsp_sync,
        *lsp_features, *theme, *shell});
    return true;
}


ProtocolValue toValue(TextInputArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("text", toValue(value.text));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TextInputArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto text = requireField<std::string>(value.field("text"));
    if (!text) return false;
    out.emplace(TextInputArguments{*text});
    return true;
}

ProtocolValue toValue(PaletteExecuteArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("command_id", toValue(value.command_id));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<PaletteExecuteArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto command_id = requireField<std::string>(value.field("command_id"));
    if (!command_id) return false;
    out.emplace(PaletteExecuteArguments{*command_id});
    return true;
}

ProtocolValue toValue(TreeSelectArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("node_id", toValue(value.node_id));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TreeSelectArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto node_id = requireField<TreeNodeId>(value.field("node_id"));
    if (!node_id) return false;
    out.emplace(TreeSelectArguments{*node_id});
    return true;
}

ProtocolValue toValue(FindQueryArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("query", toValue(value.query));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FindQueryArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto query = requireField<std::string>(value.field("query"));
    if (!query) return false;
    out.emplace(FindQueryArguments{*query});
    return true;
}

ProtocolValue toValue(SelectionCommandArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("position", toValue(value.position));
    fields.emplace_back("selection", toValue(value.selection));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SelectionCommandArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    SelectionCommandArguments result;
    if (!decodeOptionalField(value.field("position"), result.position)) return false;
    if (!decodeOptionalField(value.field("selection"), result.selection)) return false;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue toValue(ScrollLinesArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("rows", toValue(value.rows));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ScrollLinesArguments>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<ScrollPagesArguments>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<ScrollFractionArguments>& out) {
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
    fields.emplace_back("suggested_label", toValue(value.suggested_label));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DroppedContentArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto bytes = requireField<std::vector<std::uint8_t>>(value.field("bytes"));
    auto suggested_label = requireField<std::string>(value.field("suggested_label"));
    if (!bytes || !suggested_label) return false;
    out.emplace(DroppedContentArguments{*bytes, *suggested_label});
    return true;
}

ProtocolValue toValue(ReopenWithEncodingArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("encoding", toValue(value.encoding));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ReopenWithEncodingArguments>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<SetEncodingArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto encoding = requireField<TextEncoding>(value.field("encoding"));
    if (!encoding) return false;
    out.emplace(SetEncodingArguments{*encoding});
    return true;
}

ProtocolValue toValue(SetLineEndingArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("line_ending", toValue(value.line_ending));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SetLineEndingArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto line_ending = requireField<LineEnding>(value.field("line_ending"));
    if (!line_ending) return false;
    out.emplace(SetLineEndingArguments{*line_ending});
    return true;
}

ProtocolValue toValue(SetFinalNewlineArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("final_newline", toValue(value.final_newline));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SetFinalNewlineArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto final_newline = requireField<bool>(value.field("final_newline"));
    if (!final_newline) return false;
    out.emplace(SetFinalNewlineArguments{*final_newline});
    return true;
}

ProtocolValue toValue(SettingSetArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("scope", toValue(value.scope));
    fields.emplace_back("key", toValue(value.key));
    fields.emplace_back("value", toValue(value.value));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SettingSetArguments>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    auto scope = requireField<SettingScope>(value.field("scope"));
    auto key = requireField<SettingKey>(value.field("key"));
    auto setting_value = requireField<SettingValue>(value.field("value"));
    if (!scope || !key || !setting_value) return false;
    out.emplace(SettingSetArguments{*scope, *key, *setting_value});
    return true;
}

ProtocolValue toValue(SettingResetArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("scope", toValue(value.scope));
    fields.emplace_back("key", toValue(value.key));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SettingResetArguments>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<SettingResetScopeArguments>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<WorkspaceReplaceArguments>& out) {
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
bool decode_present(ProtocolValue const& value, std::optional<ThemeSectionDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    std::optional<ThemeSnapshot> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(ThemeSectionDelta{std::move(replacement)});
    return true;
}

ProtocolValue toValue(ShellSectionDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("replacement", toValue(value.replacement));
    return ProtocolValue::makeObject(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ShellSectionDelta>& out) {
    auto const* object = value.asObject();
    if (!object) return false;
    std::optional<ShellViewState> replacement;
    if (!decodeOptionalField(value.field("replacement"), replacement)) return false;
    out.emplace(ShellSectionDelta{std::move(replacement)});
    return true;
}

}  // namespace


struct CommandArgumentCodecRegistry::Impl {
    std::unordered_map<std::string, CommandArgumentCodec> codecs;
};

CommandArgumentCodecRegistry::CommandArgumentCodecRegistry(
    std::vector<std::pair<std::string, CommandArgumentCodec>> entries)
    : impl_{std::make_unique<Impl>()} {
    auto const descriptors = p0CommandDescriptors();

    std::unordered_map<std::string, CommandArgumentCodec> codecs;
    codecs.reserve(entries.size());
    for (auto& entry : entries) {
        if (!codecs.emplace(std::move(entry.first), std::move(entry.second))
                 .second) {
            throw std::invalid_argument{
                "duplicate command argument codec entry: " + entry.first};
        }
    }
    if (codecs.size() != descriptors.size()) {
        throw std::invalid_argument{
            "command argument codec registry does not cover exactly the P0 "
            "command catalog"};
    }
    for (auto const& descriptor : descriptors) {
        if (codecs.find(descriptor.id) == codecs.end()) {
            throw std::invalid_argument{
                "command argument codec registry is missing command: " +
                descriptor.id};
        }
    }
    impl_->codecs = std::move(codecs);
}

CommandArgumentCodecRegistry::~CommandArgumentCodecRegistry() = default;
CommandArgumentCodecRegistry::CommandArgumentCodecRegistry(
    CommandArgumentCodecRegistry&&) noexcept = default;
CommandArgumentCodecRegistry& CommandArgumentCodecRegistry::operator=(
    CommandArgumentCodecRegistry&&) noexcept = default;

bool CommandArgumentCodecRegistry::contains(
    std::string_view command_id) const {
    return impl_->codecs.find(std::string{command_id}) !=
           impl_->codecs.end();
}

ProtocolValue CommandArgumentCodecRegistry::encodeArgument(
    std::string_view command_id, std::any const& payload) const {
    auto found = impl_->codecs.find(std::string{command_id});
    if (found == impl_->codecs.end()) {
        throw std::invalid_argument{"unknown command id: " +
                                    std::string{command_id}};
    }
    return found->second.encode(payload);
}

std::optional<std::any> CommandArgumentCodecRegistry::decodeArgument(
    std::string_view command_id, ProtocolValue const& value) const {
    auto found = impl_->codecs.find(std::string{command_id});
    if (found == impl_->codecs.end()) {
        throw std::invalid_argument{"unknown command id: " +
                                    std::string{command_id}};
    }
    return found->second.decode(value);
}

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

CommandArgumentCodecRegistry buildCommandArgumentCodecRegistry() {
    auto const text_input_commands = textInputCommandSet();
    std::unordered_set<std::string> text_input_ids;
    for (auto const& descriptor : text_input_commands.descriptors()) {
        text_input_ids.emplace(descriptor.id);
    }
    auto const selection_commands = selectionNavigationCommandSet();
    std::unordered_set<std::string> selection_ids;
    for (auto const& descriptor : selection_commands.descriptors()) {
        selection_ids.emplace(descriptor.id);
    }

    auto const none_codec = makeNoneCodec();
    auto const text_input_codec = makeTypedCodec<TextInputArguments>();
    auto const palette_execute_codec = makeTypedCodec<PaletteExecuteArguments>();
    auto const tree_select_codec = makeTypedCodec<TreeSelectArguments>();
    auto const find_query_codec = makeTypedCodec<FindQueryArguments>();
    auto const selection_codec =
        makeTypedCodec<SelectionCommandArguments>();
    auto const scroll_lines_codec = makeTypedCodec<ScrollLinesArguments>();
    auto const scroll_pages_codec = makeTypedCodec<ScrollPagesArguments>();
    auto const scroll_fraction_codec =
        makeTypedCodec<ScrollFractionArguments>();
    auto const dropped_content_codec =
        makeTypedCodec<DroppedContentArguments>();
    auto const reopen_with_encoding_codec =
        makeTypedCodec<ReopenWithEncodingArguments>();
    auto const set_encoding_codec = makeTypedCodec<SetEncodingArguments>();
    auto const set_line_ending_codec =
        makeTypedCodec<SetLineEndingArguments>();
    auto const set_final_newline_codec =
        makeTypedCodec<SetFinalNewlineArguments>();
    auto const setting_set_codec = makeTypedCodec<SettingSetArguments>();
    auto const setting_reset_codec = makeTypedCodec<SettingResetArguments>();
    auto const setting_reset_scope_codec =
        makeTypedCodec<SettingResetScopeArguments>();
    auto const workspace_replace_codec =
        makeTypedCodec<WorkspaceReplaceArguments>();
    auto const workspace_apply_codec = makeWorkspaceApplyCodec();

    std::vector<std::pair<std::string, CommandArgumentCodec>> entries;
    for (auto const& descriptor : p0CommandDescriptors()) {
        if (descriptor.id == "view.scroll_lines") {
            entries.emplace_back(descriptor.id, scroll_lines_codec);
        } else if (descriptor.id == "tree.scroll") {
            entries.emplace_back(descriptor.id, scroll_lines_codec);
        } else if (descriptor.id == "view.scroll_pages") {
            entries.emplace_back(descriptor.id, scroll_pages_codec);
        } else if (descriptor.id == "view.scroll_to_fraction") {
            entries.emplace_back(descriptor.id, scroll_fraction_codec);
        } else if (descriptor.id == "file.open_dropped_content") {
            entries.emplace_back(descriptor.id, dropped_content_codec);
        } else if (descriptor.id == "file.reopen_with_encoding") {
            entries.emplace_back(descriptor.id, reopen_with_encoding_codec);
        } else if (descriptor.id == "file.set_encoding") {
            entries.emplace_back(descriptor.id, set_encoding_codec);
        } else if (descriptor.id == "file.set_line_ending") {
            entries.emplace_back(descriptor.id, set_line_ending_codec);
        } else if (descriptor.id == "file.set_final_newline") {
            entries.emplace_back(descriptor.id, set_final_newline_codec);
        } else if (descriptor.id == "settings.set") {
            entries.emplace_back(descriptor.id, setting_set_codec);
        } else if (descriptor.id == "settings.reset") {
            entries.emplace_back(descriptor.id, setting_reset_codec);
        } else if (descriptor.id == "settings.reset_scope") {
            entries.emplace_back(descriptor.id, setting_reset_scope_codec);
        } else if (descriptor.id == "replace.workspace_preview") {
            entries.emplace_back(descriptor.id, workspace_replace_codec);
        } else if (descriptor.id == "replace.workspace_apply") {
            entries.emplace_back(descriptor.id, workspace_apply_codec);
        } else if (descriptor.id == "palette.execute") {
            entries.emplace_back(descriptor.id, palette_execute_codec);
        } else if (descriptor.id == "tree.select") {
            entries.emplace_back(descriptor.id, tree_select_codec);
        } else if (descriptor.id == "find.update_query") {
            entries.emplace_back(descriptor.id, find_query_codec);
        } else if (descriptor.id == "replace.update_replacement") {
            entries.emplace_back(descriptor.id, find_query_codec);
        } else if (text_input_ids.contains(descriptor.id)) {
            entries.emplace_back(descriptor.id, text_input_codec);
        } else if (selection_ids.contains(descriptor.id)) {
            entries.emplace_back(descriptor.id, selection_codec);
        } else {
            entries.emplace_back(descriptor.id, none_codec);
        }
    }
    return CommandArgumentCodecRegistry{std::move(entries)};
}


std::string encodeCommandRequest(ClientCommand const& command,
                                   CommandArgumentCodecRegistry const& registry) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", toValue(command.id));
    fields.emplace_back("base_revision", toValue(command.base_revision));
    fields.emplace_back("payload",
                        registry.encodeArgument(command.id, command.payload));
    return encodeMessage(ProtocolMessageKind::CommandRequest,
                          ProtocolValue::makeObject(std::move(fields)));
}

DecodeCommandRequestResult decodeCommandRequest(
    std::string_view bytes, CommandArgumentCodecRegistry const& registry,
    ProtocolLimits limits) {
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
    auto base_revision = requireField<Revision>(payload.field("base_revision"));
    if (!id || !base_revision) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "command request payload is malformed"};
    }
    if (!registry.contains(*id)) {
        return {ProtocolError::UnsupportedCommand, std::nullopt,
                "command request references an unknown command id"};
    }
    auto const* payload_field = payload.field("payload");
    if (payload_field == nullptr) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "command request is missing its payload field"};
    }
    auto argument = registry.decodeArgument(*id, *payload_field);
    if (!argument) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "command request payload does not match its command id"};
    }
    return {ProtocolError::None,
            ClientCommand{std::move(*id), *base_revision,
                          std::move(*argument)},
            {}};
}

std::string encodeCommandResult(CommandResult const& result) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back(
        "error", ProtocolValue::makeUint(
                    static_cast<std::uint8_t>(result.error)));
    fields.emplace_back("revision", toValue(result.revision));
    fields.emplace_back("message", toValue(result.message));
    return encodeMessage(ProtocolMessageKind::CommandResult,
                         ProtocolValue::makeObject(std::move(fields)));
}

DecodeCommandResultResult decodeCommandResult(std::string_view bytes,
                                               ProtocolLimits limits) {
    auto decoded =
        decodeMessage(bytes, ProtocolMessageKind::CommandResult, limits);
    if (decoded.error != ProtocolError::None) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    auto const& payload = *decoded.payload;
    if (!payload.asObject()) {
        return {ProtocolError::MalformedMessage, std::nullopt,
               "command result payload is not an object"};
    }
    auto error = requireField<std::uint8_t>(payload.field("error"));
    auto revision = requireField<Revision>(payload.field("revision"));
    auto message = requireField<std::string>(payload.field("message"));
    if (!error || *error > static_cast<std::uint8_t>(
                              CommandError::RevisionExhausted) ||
        !revision || !message) {
        return {ProtocolError::MalformedMessage, std::nullopt,
               "command result payload is malformed"};
    }
    return {ProtocolError::None,
            CommandResult{static_cast<CommandError>(*error), *revision,
                         std::move(*message)},
            {}};
}

std::string encodeSessionSnapshot(SessionSnapshot const& snapshot) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", toValue(snapshot.revision()));
    fields.emplace_back("topology", toValue(snapshot.topology()));
    fields.emplace_back("client", toValue(snapshot.client()));
    fields.emplace_back("sections", toValue(snapshot.sections()));
    return encodeMessage(ProtocolMessageKind::SessionSnapshot,
                          ProtocolValue::makeObject(std::move(fields)));
}

DecodeSessionSnapshotResult decodeSessionSnapshot(std::string_view bytes,
                                                    ProtocolLimits limits) {
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
    if (!revision || !topology || !client || !sections) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "session snapshot payload is malformed"};
    }
    return {ProtocolError::None,
            SessionSnapshot{*revision, std::move(*topology),
                            std::move(*client), std::move(*sections)},
            {}};
}

std::string encodeSessionDelta(SessionDelta const& delta) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", toValue(delta.baseRevision()));
    fields.emplace_back("revision", toValue(delta.revision()));
    fields.emplace_back("client_id", toValue(delta.clientId()));
    fields.emplace_back("view_id", toValue(delta.viewId()));
    fields.emplace_back("capabilities", toValue(delta.capabilities()));
    fields.emplace_back("topology", toValue(delta.topology()));
    fields.emplace_back("document", toValue(delta.document()));
    fields.emplace_back("document_caret", toValue(delta.documentCaret()));
    fields.emplace_back("selection", toValue(delta.selection()));
    fields.emplace_back("history", toValue(delta.history()));
    fields.emplace_back("clipboard", toValue(delta.clipboard()));
    fields.emplace_back("prompt_status", toValue(delta.promptStatus()));
    fields.emplace_back("search", toValue(delta.search()));
    fields.emplace_back("find_replace", toValue(delta.findReplace()));
    fields.emplace_back("settings", toValue(delta.settings()));
    fields.emplace_back("keymap", toValue(delta.keymap()));
    fields.emplace_back("text_encoding", toValue(delta.textEncoding()));
    fields.emplace_back("tabs", toValue(delta.tabs()));
    fields.emplace_back("diff", toValue(delta.diff()));
    fields.emplace_back("external_modification",
                        toValue(delta.externalModification()));
    fields.emplace_back("follow_edits", toValue(delta.followEdits()));
    fields.emplace_back("tree", toValue(delta.tree()));
    fields.emplace_back("syntax", toValue(delta.syntax()));
    fields.emplace_back("lsp_sync", toValue(delta.lspSync()));
    fields.emplace_back("lsp_features", toValue(delta.lspFeatures()));
    fields.emplace_back("theme", toValue(delta.theme()));
    fields.emplace_back("shell", toValue(delta.shell()));
    fields.emplace_back("viewport", toValue(delta.viewport()));
    return encodeMessage(ProtocolMessageKind::SessionDelta,
                          ProtocolValue::makeObject(std::move(fields)));
}

DecodeSessionDeltaResult decodeSessionDelta(std::string_view bytes,
                                              ProtocolLimits limits) {
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

    auto base_revision = requireField<Revision>(payload.field("base_revision"));
    auto revision = requireField<Revision>(payload.field("revision"));
    auto client_id = requireField<ClientId>(payload.field("client_id"));
    auto view_id = requireField<ViewId>(payload.field("view_id"));
    auto capabilities =
        requireField<std::vector<CapabilityId>>(payload.field("capabilities"));

    std::optional<SessionTopology> topology;
    std::optional<DocumentDelta> document;
    std::optional<ByteOffset> document_caret;
    std::optional<TextEncodingDelta> text_encoding;
    bool const optional_ok =
        decodeOptionalField(payload.field("topology"), topology) &&
        decodeOptionalField(payload.field("document"), document) &&
        decodeOptionalField(payload.field("document_caret"), document_caret) &&
        decodeOptionalField(payload.field("text_encoding"), text_encoding);

    auto selection = requireField<SelectionViewDelta>(payload.field("selection"));
    auto history = requireField<HistoryDelta>(payload.field("history"));
    auto clipboard = requireField<ClipboardDelta>(payload.field("clipboard"));
    auto prompt_status =
        requireField<PromptStatusDelta>(payload.field("prompt_status"));
    auto search = requireField<SearchDelta>(payload.field("search"));
    auto find_replace =
        requireField<FindReplaceDelta>(payload.field("find_replace"));
    auto settings =
        requireField<SettingsSectionDelta>(payload.field("settings"));
    auto keymap = requireField<KeymapDelta>(payload.field("keymap"));
    auto tabs = requireField<TabDelta>(payload.field("tabs"));
    auto diff = requireField<DiffDelta>(payload.field("diff"));
    auto external_modification = requireField<ExternalModificationDelta>(
        payload.field("external_modification"));
    auto follow_edits =
        requireField<FollowEditsDelta>(payload.field("follow_edits"));
    auto tree = requireField<TreeDelta>(payload.field("tree"));
    auto syntax = requireField<SyntaxDelta>(payload.field("syntax"));
    auto lsp_sync = requireField<LspSyncDelta>(payload.field("lsp_sync"));
    auto lsp_features =
        requireField<LspFeatureDelta>(payload.field("lsp_features"));
    auto theme = requireField<ThemeSectionDelta>(payload.field("theme"));
    auto shell = requireField<ShellSectionDelta>(payload.field("shell"));
    auto viewport = requireField<ViewportDelta>(payload.field("viewport"));

    if (!optional_ok || !base_revision || !revision || !client_id || !view_id ||
        !capabilities || !selection || !history || !clipboard ||
        !prompt_status || !search || !find_replace || !settings || !keymap ||
        !tabs || !diff || !external_modification || !follow_edits || !tree ||
        !syntax || !lsp_sync || !lsp_features || !theme || !shell ||
        !viewport) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "session delta payload is malformed"};
    }

    return {ProtocolError::None,
            decodeWireSessionDelta(
                *base_revision, *revision, *client_id, *view_id,
                std::move(*capabilities), std::move(topology),
                std::move(document), std::move(document_caret),
                std::move(*selection), std::move(*history),
                std::move(*clipboard), std::move(*prompt_status),
                std::move(*search), std::move(*find_replace),
                std::move(*settings), std::move(*keymap),
                std::move(text_encoding), std::move(*tabs), std::move(*diff),
                std::move(*external_modification), std::move(*follow_edits),
                std::move(*tree), std::move(*syntax), std::move(*lsp_sync),
                std::move(*lsp_features), std::move(*theme),
                std::move(*shell), std::move(*viewport)),
            {}};
}

std::string encodeClipboardRequest(ClipboardRequest const& request) {
    return encodeMessage(ProtocolMessageKind::ClipboardRequest,
                          toValue(request));
}

DecodeClipboardRequestResult decodeClipboardRequest(std::string_view bytes,
                                                      ProtocolLimits limits) {
    auto decoded =
        decodeMessage(bytes, ProtocolMessageKind::ClipboardRequest, limits);
    if (decoded.error != ProtocolError::None) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    std::optional<ClipboardRequest> request;
    if (!fromValue(*decoded.payload, request) || !request.has_value()) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "clipboard request payload is malformed"};
    }
    return {ProtocolError::None, std::move(request), {}};
}

std::string encodeClipboardResponse(ClipboardResponse const& response) {
    return encodeMessage(ProtocolMessageKind::ClipboardResponse,
                          toValue(response));
}

DecodeClipboardResponseResult decodeClipboardResponse(
    std::string_view bytes, ProtocolLimits limits) {
    auto decoded =
        decodeMessage(bytes, ProtocolMessageKind::ClipboardResponse, limits);
    if (decoded.error != ProtocolError::None) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    std::optional<ClipboardResponse> response;
    if (!fromValue(*decoded.payload, response) || !response.has_value()) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "clipboard response payload is malformed"};
    }
    return {ProtocolError::None, std::move(response), {}};
}

std::string encodeStatusActionInvocation(
    StatusActionInvocation const& invocation) {
    return encodeMessage(ProtocolMessageKind::StatusActionInvocation,
                          toValue(invocation));
}

DecodeStatusActionInvocationResult decodeStatusActionInvocation(
    std::string_view bytes, ProtocolLimits limits) {
    auto decoded = decodeMessage(
        bytes, ProtocolMessageKind::StatusActionInvocation, limits);
    if (decoded.error != ProtocolError::None) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    std::optional<StatusActionInvocation> invocation;
    if (!fromValue(*decoded.payload, invocation) || !invocation.has_value()) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "status action invocation payload is malformed"};
    }
    return {ProtocolError::None, std::move(invocation), {}};
}

// Binary-frame envelope: [u8 version][u8 kind][u64 request_id][u32
// declared_length][declared_length bytes]. No trailing bytes are permitted.

std::string encodeBinaryFrame(BinaryFrame const& frame) {
    std::string out;
    writeU8(out, frame.version);
    writeU8(out, static_cast<std::uint8_t>(frame.kind));
    writeU64(out, frame.request_id);
    writeU32(out, static_cast<std::uint32_t>(frame.bytes.size()));
    writeRawBytes(out, frame.bytes);
    return out;
}

DecodeBinaryFrameResult decodeBinaryFrame(std::string_view bytes,
                                            ProtocolLimits limits) {
    if (bytes.size() > limits.max_binary_frame_bytes) {
        return {ProtocolError::BinaryFrameTooLarge, std::nullopt,
                "binary frame exceeds the configured byte limit"};
    }
    ByteReader reader{bytes};
    std::uint8_t version = 0;
    if (!reader.readU8(version)) {
        return {ProtocolError::TruncatedMessage, std::nullopt,
                "binary frame is missing its version byte"};
    }
    std::uint8_t kind_byte = 0;
    if (!reader.readU8(kind_byte)) {
        return {ProtocolError::TruncatedMessage, std::nullopt,
                "binary frame is missing its kind byte"};
    }
    if (kind_byte != static_cast<std::uint8_t>(BinaryPayloadKind::DroppedContent)) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "binary frame declares an unsupported payload kind"};
    }
    std::uint64_t request_id = 0;
    if (!reader.readU64(request_id)) {
        return {ProtocolError::TruncatedMessage, std::nullopt,
                "binary frame is missing its request id"};
    }
    std::uint32_t declared_length = 0;
    if (!reader.readU32(declared_length)) {
        return {ProtocolError::TruncatedMessage, std::nullopt,
                "binary frame is missing its length prefix"};
    }
    if (declared_length > limits.max_binary_frame_bytes) {
        return {ProtocolError::BinaryFrameTooLarge, std::nullopt,
                "binary frame payload exceeds the configured byte limit"};
    }
    std::string_view raw_payload;
    if (!reader.readBytes(declared_length, raw_payload)) {
        return {ProtocolError::TruncatedMessage, std::nullopt,
                "binary frame payload is truncated"};
    }
    if (!reader.atEnd()) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "binary frame has unexpected trailing bytes"};
    }
    std::vector<std::uint8_t> owned_bytes{raw_payload.begin(), raw_payload.end()};
    return {ProtocolError::None,
            BinaryFrame{version, static_cast<BinaryPayloadKind>(kind_byte),
                       request_id, std::move(owned_bytes)},
            {}};
}

}  // namespace ssg
