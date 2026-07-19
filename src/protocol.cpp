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

std::string hex_encode(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::optional<std::string> hex_decode(std::string_view text) {
    if ((text.size() & 1U) != 0) return std::nullopt;
    std::string result(text.size() / 2, '\0');
    for (std::size_t i = 0; i < text.size(); i += 2) {
        int const high = hex_value(text[i]);
        int const low = hex_value(text[i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        result[i / 2] = static_cast<char>((high << 4) | low);
    }
    return result;
}

bool valid_utf8(std::string_view text) {
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
bool parse_integer(std::string_view text, Integer& value) {
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

DecodeInsertResult decode_failure(ProtocolError error, std::string message) {
    return {error, std::nullopt, std::move(message)};
}

DocumentPosition caret_position(std::size_t offset) {
    return {ByteOffset{offset}, LineIndex{0}, CellIndex{offset}};
}

std::string_view protocol_error_name(ProtocolError error) {
    switch (error) {
        case ProtocolError::none: return "none";
        case ProtocolError::message_too_large: return "message_too_large";
        case ProtocolError::malformed_message: return "malformed_message";
        case ProtocolError::unsupported_version: return "unsupported_version";
        case ProtocolError::unsupported_command: return "unsupported_command";
        case ProtocolError::insert_too_large: return "insert_too_large";
    }
    return "malformed_message";
}

std::string_view command_error_name(CommandError error) {
    switch (error) {
        case CommandError::none: return "none";
        case CommandError::unknown_client: return "unknown_client";
        case CommandError::unknown_command: return "unknown_command";
        case CommandError::stale_revision: return "stale_revision";
        case CommandError::capability_denied: return "capability_denied";
        case CommandError::handler_failed: return "handler_failed";
        case CommandError::revision_exhausted: return "revision_exhausted";
    }
    return "handler_failed";
}

ProtocolError parse_protocol_error(std::string_view value) {
    for (auto error : {ProtocolError::none, ProtocolError::message_too_large,
                       ProtocolError::malformed_message,
                       ProtocolError::unsupported_version,
                       ProtocolError::unsupported_command,
                       ProtocolError::insert_too_large}) {
        if (protocol_error_name(error) == value) return error;
    }
    throw std::invalid_argument{"unknown protocol error"};
}

CommandError parse_command_error(std::string_view value) {
    for (auto error : {CommandError::none, CommandError::unknown_client,
                       CommandError::unknown_command,
                       CommandError::stale_revision,
                       CommandError::capability_denied,
                       CommandError::handler_failed,
                       CommandError::revision_exhausted}) {
        if (command_error_name(error) == value) return error;
    }
    throw std::invalid_argument{"unknown command error"};
}

}  // namespace

std::string encode_insert_request(InsertRequest const& request) {
    return std::string{prefix} + " INSERT " +
           std::to_string(request.base_revision.value()) + " " +
           hex_encode(request.text);
}

DecodeInsertResult decode_insert_request(std::string_view message,
                                         ProtocolLimits limits) {
    if (message.size() > limits.max_message_bytes) {
        return decode_failure(ProtocolError::message_too_large,
                              "message exceeds configured byte limit");
    }
    auto const parts = fields(message);
    if (parts.size() != 4) {
        return decode_failure(ProtocolError::malformed_message,
                              "insert message must contain four fields");
    }
    if (parts[0] != prefix) {
        return decode_failure(ProtocolError::unsupported_version,
                              "unsupported protocol version");
    }
    if (parts[1] != "INSERT") {
        return decode_failure(ProtocolError::unsupported_command,
                              "only text.insert is supported");
    }
    std::uint64_t revision = 0;
    if (!parse_integer(parts[2], revision) || revision == 0) {
        return decode_failure(ProtocolError::malformed_message,
                              "base revision must be a positive integer");
    }
    auto text = hex_decode(parts[3]);
    if (!text) {
        return decode_failure(ProtocolError::malformed_message,
                              "insert payload must be hexadecimal");
    }
    if (text->size() > limits.max_insert_bytes) {
        return decode_failure(ProtocolError::insert_too_large,
                              "insert exceeds configured byte limit");
    }
    if (!valid_utf8(*text)) {
        return decode_failure(ProtocolError::malformed_message,
                              "insert payload must be valid non-NUL UTF-8");
    }
    return {ProtocolError::none,
            InsertRequest{Revision{revision}, std::move(*text)}, {}};
}

std::string encode_slice_response(SliceResponse const& response) {
    std::string encoded =
        std::string{prefix} + " RESPONSE " +
        std::string{protocol_error_name(response.protocol_error)} + " " +
        std::string{command_error_name(response.command_error)} + " " +
        std::to_string(response.snapshot.revision.value()) + " " +
        std::to_string(response.snapshot.caret.value()) + " " +
        hex_encode(response.snapshot.text) + " " +
        hex_encode(response.message) + " ";
    if (!response.delta) {
        return encoded + "-";
    }
    auto const& delta = *response.delta;
    return encoded + std::to_string(delta.base_revision.value()) + "," +
           std::to_string(delta.revision.value()) + "," +
           std::to_string(delta.start.value()) + "," +
           std::to_string(delta.erased_bytes) + "," +
           hex_encode(delta.inserted_text);
}

SliceResponse decode_slice_response(std::string_view message,
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
    if (!parse_integer(parts[4], revision) ||
        !parse_integer(parts[5], caret)) {
        throw std::invalid_argument{"malformed response revision or caret"};
    }
    auto text = hex_decode(parts[6]);
    auto error_message = hex_decode(parts[7]);
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
        if (values.size() != 5 || !parse_integer(values[0], base) ||
            !parse_integer(values[1], next) ||
            !parse_integer(values[2], offset) ||
            !parse_integer(values[3], erased)) {
            throw std::invalid_argument{"malformed response delta"};
        }
        auto inserted = hex_decode(values[4]);
        if (!inserted) throw std::invalid_argument{"malformed delta payload"};
        delta = DocumentDelta{Revision{base}, Revision{next},
                              ByteOffset{offset}, erased,
                              std::move(*inserted)};
    }
    return {parse_protocol_error(parts[2]), parse_command_error(parts[3]),
            {Revision{revision}, std::move(*text), ByteOffset{caret}},
            std::move(delta), std::move(*error_message)};
}

struct CoreEditorSlice::Impl {
    Impl()
        : document{},
          selections{std::vector<Selection>{
              Selection{caret_position(0), caret_position(0)}}},
          session{CommandRegistry{std::vector<CommandSet>{CommandSet{{
              CommandRegistration{
                  {"text.insert", CommandEffect::mutation, {}},
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
                      auto result = apply_text_input(
                          document.snapshot(), selections,
                          {IndentStyle::spaces, 4, true, LineEnding::lf},
                          TextInputCommand::insert, *arguments);
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

    DocumentViewState snapshot_unlocked() const {
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
    auto const before = impl_->snapshot_unlocked();
    auto result = impl_->session.dispatch(
        client_id,
        ClientCommand{"text.insert", request.base_revision,
                      TextInputArguments{request.text}});
    auto const after = impl_->snapshot_unlocked();
    return {ProtocolError::none, result.error, after,
            result.accepted() ? derive_document_delta(before, after)
                              : std::nullopt,
            std::move(result.message)};
}

DocumentViewState CoreEditorSlice::snapshot() const {
    std::lock_guard lock{impl_->mutex};
    return impl_->snapshot_unlocked();
}

// ---------------------------------------------------------------------------
// ProtocolValue: bounded, versioned wire value tree.

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

ProtocolValue ProtocolValue::make_null() { return ProtocolValue{}; }

ProtocolValue ProtocolValue::make_bool(bool value) {
    return ProtocolValue{std::make_shared<Storage>(Storage{Storage::Value{value}})};
}

ProtocolValue ProtocolValue::make_int(std::int64_t value) {
    return ProtocolValue{std::make_shared<Storage>(Storage{Storage::Value{value}})};
}

ProtocolValue ProtocolValue::make_uint(std::uint64_t value) {
    return ProtocolValue{std::make_shared<Storage>(Storage{Storage::Value{value}})};
}

ProtocolValue ProtocolValue::make_text(std::string value) {
    return ProtocolValue{
        std::make_shared<Storage>(Storage{Storage::Value{std::move(value)}})};
}

ProtocolValue ProtocolValue::make_bytes(std::vector<std::uint8_t> value) {
    return ProtocolValue{
        std::make_shared<Storage>(Storage{Storage::Value{std::move(value)}})};
}

ProtocolValue ProtocolValue::make_array(Array items) {
    return ProtocolValue{
        std::make_shared<Storage>(Storage{Storage::Value{std::move(items)}})};
}

ProtocolValue ProtocolValue::make_object(Object fields) {
    return ProtocolValue{
        std::make_shared<Storage>(Storage{Storage::Value{std::move(fields)}})};
}

ProtocolValue::Kind ProtocolValue::kind() const noexcept {
    switch (storage_->value.index()) {
        case 0:
            return Kind::null_value;
        case 1:
            return Kind::boolean;
        case 2:
            return Kind::integer;
        case 3:
            return Kind::unsigned_integer;
        case 4:
            return Kind::text;
        case 5:
            return Kind::bytes;
        case 6:
            return Kind::array;
        case 7:
            return Kind::object;
        default:
            return Kind::null_value;
    }
}

std::optional<bool> ProtocolValue::as_bool() const {
    if (auto const* value = std::get_if<bool>(&storage_->value)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::int64_t> ProtocolValue::as_int() const {
    if (auto const* value = std::get_if<std::int64_t>(&storage_->value)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> ProtocolValue::as_uint() const {
    if (auto const* value = std::get_if<std::uint64_t>(&storage_->value)) {
        return *value;
    }
    return std::nullopt;
}

std::string const* ProtocolValue::as_text() const {
    return std::get_if<std::string>(&storage_->value);
}

std::vector<std::uint8_t> const* ProtocolValue::as_bytes() const {
    return std::get_if<std::vector<std::uint8_t>>(&storage_->value);
}

ProtocolValue::Array const* ProtocolValue::as_array() const {
    return std::get_if<ProtocolValue::Array>(&storage_->value);
}

ProtocolValue::Object const* ProtocolValue::as_object() const {
    return std::get_if<ProtocolValue::Object>(&storage_->value);
}

ProtocolValue const* ProtocolValue::field(std::string_view key) const {
    auto const* object = as_object();
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

// ---------------------------------------------------------------------------
// Binary wire encoding for a ProtocolValue tree.
//
// Layout: [u8 tag][tag-specific payload]. null: nothing; boolean: 1 byte;
// integer/unsigned_integer: 8 bytes little-endian; text/bytes: u32 length +
// raw bytes; array: u32 count + N values; object: u32 count + N x (u16 key
// length + key bytes + value). Top-level messages wrap this in
// [u8 wire_version][u8 message_kind][payload].

void write_u8(std::string& out, std::uint8_t value) {
    out.push_back(static_cast<char>(value));
}

void write_u32(std::string& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

void write_u64(std::string& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

void write_i64(std::string& out, std::int64_t value) {
    write_u64(out, static_cast<std::uint64_t>(value));
}

void write_raw_bytes(std::string& out, std::vector<std::uint8_t> const& bytes) {
    out.append(reinterpret_cast<char const*>(bytes.data()), bytes.size());
}

class ByteReader {
public:
    explicit ByteReader(std::string_view data) : data_{data} {}

    [[nodiscard]] bool read_u8(std::uint8_t& out) {
        if (pos_ + 1 > data_.size()) {
            return false;
        }
        out = static_cast<std::uint8_t>(data_[pos_]);
        pos_ += 1;
        return true;
    }

    [[nodiscard]] bool read_u32(std::uint32_t& out) {
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

    [[nodiscard]] bool read_u64(std::uint64_t& out) {
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

    [[nodiscard]] bool read_i64(std::int64_t& out) {
        std::uint64_t raw = 0;
        if (!read_u64(raw)) {
            return false;
        }
        out = static_cast<std::int64_t>(raw);
        return true;
    }

    [[nodiscard]] bool read_bytes(std::size_t length, std::string_view& out) {
        if (pos_ + length > data_.size()) {
            return false;
        }
        out = data_.substr(pos_, length);
        pos_ += length;
        return true;
    }

    [[nodiscard]] bool at_end() const noexcept { return pos_ == data_.size(); }

private:
    std::string_view data_;
    std::size_t pos_ = 0;
};

enum class ValueReadStatus : std::uint8_t { ok, truncated, malformed, bounds_exceeded };

void write_value(std::string& out, ProtocolValue const& value) {
    switch (value.kind()) {
        case ProtocolValue::Kind::null_value:
            write_u8(out, 0);
            break;
        case ProtocolValue::Kind::boolean:
            write_u8(out, 1);
            write_u8(out, *value.as_bool() ? 1 : 0);
            break;
        case ProtocolValue::Kind::integer:
            write_u8(out, 2);
            write_i64(out, *value.as_int());
            break;
        case ProtocolValue::Kind::unsigned_integer:
            write_u8(out, 3);
            write_u64(out, *value.as_uint());
            break;
        case ProtocolValue::Kind::text: {
            write_u8(out, 4);
            auto const& text = *value.as_text();
            write_u32(out, static_cast<std::uint32_t>(text.size()));
            out.append(text);
            break;
        }
        case ProtocolValue::Kind::bytes: {
            write_u8(out, 5);
            auto const& bytes = *value.as_bytes();
            write_u32(out, static_cast<std::uint32_t>(bytes.size()));
            write_raw_bytes(out, bytes);
            break;
        }
        case ProtocolValue::Kind::array: {
            write_u8(out, 6);
            auto const& items = *value.as_array();
            write_u32(out, static_cast<std::uint32_t>(items.size()));
            for (auto const& item : items) {
                write_value(out, item);
            }
            break;
        }
        case ProtocolValue::Kind::object: {
            write_u8(out, 7);
            auto const& fields = *value.as_object();
            write_u32(out, static_cast<std::uint32_t>(fields.size()));
            for (auto const& [key, field_value] : fields) {
                write_u32(out, static_cast<std::uint32_t>(key.size()));
                out.append(key);
                write_value(out, field_value);
            }
            break;
        }
    }
}

ValueReadStatus read_value(ByteReader& reader, ProtocolValue& out,
                           std::size_t depth, ProtocolLimits const& limits) {
    if (depth > limits.max_value_depth) {
        return ValueReadStatus::bounds_exceeded;
    }
    std::uint8_t tag = 0;
    if (!reader.read_u8(tag)) {
        return ValueReadStatus::truncated;
    }
    switch (tag) {
        case 0:
            out = ProtocolValue::make_null();
            return ValueReadStatus::ok;
        case 1: {
            std::uint8_t raw = 0;
            if (!reader.read_u8(raw)) {
                return ValueReadStatus::truncated;
            }
            if (raw > 1) {
                return ValueReadStatus::malformed;
            }
            out = ProtocolValue::make_bool(raw != 0);
            return ValueReadStatus::ok;
        }
        case 2: {
            std::int64_t value = 0;
            if (!reader.read_i64(value)) {
                return ValueReadStatus::truncated;
            }
            out = ProtocolValue::make_int(value);
            return ValueReadStatus::ok;
        }
        case 3: {
            std::uint64_t value = 0;
            if (!reader.read_u64(value)) {
                return ValueReadStatus::truncated;
            }
            out = ProtocolValue::make_uint(value);
            return ValueReadStatus::ok;
        }
        case 4: {
            std::uint32_t length = 0;
            if (!reader.read_u32(length)) {
                return ValueReadStatus::truncated;
            }
            if (length > limits.max_text_bytes) {
                return ValueReadStatus::bounds_exceeded;
            }
            std::string_view bytes;
            if (!reader.read_bytes(length, bytes)) {
                return ValueReadStatus::truncated;
            }
            out = ProtocolValue::make_text(std::string{bytes});
            return ValueReadStatus::ok;
        }
        case 5: {
            std::uint32_t length = 0;
            if (!reader.read_u32(length)) {
                return ValueReadStatus::truncated;
            }
            if (length > limits.max_bytes_length) {
                return ValueReadStatus::bounds_exceeded;
            }
            std::string_view bytes;
            if (!reader.read_bytes(length, bytes)) {
                return ValueReadStatus::truncated;
            }
            out = ProtocolValue::make_bytes(
                std::vector<std::uint8_t>{bytes.begin(), bytes.end()});
            return ValueReadStatus::ok;
        }
        case 6: {
            std::uint32_t count = 0;
            if (!reader.read_u32(count)) {
                return ValueReadStatus::truncated;
            }
            if (count > limits.max_collection_length) {
                return ValueReadStatus::bounds_exceeded;
            }
            ProtocolValue::Array items;
            items.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                ProtocolValue item;
                auto status = read_value(reader, item, depth + 1, limits);
                if (status != ValueReadStatus::ok) {
                    return status;
                }
                items.push_back(std::move(item));
            }
            out = ProtocolValue::make_array(std::move(items));
            return ValueReadStatus::ok;
        }
        case 7: {
            std::uint32_t count = 0;
            if (!reader.read_u32(count)) {
                return ValueReadStatus::truncated;
            }
            if (count > limits.max_collection_length) {
                return ValueReadStatus::bounds_exceeded;
            }
            ProtocolValue::Object fields;
            fields.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                std::uint32_t key_length = 0;
                if (!reader.read_u32(key_length)) {
                    return ValueReadStatus::truncated;
                }
                if (key_length > limits.max_text_bytes) {
                    return ValueReadStatus::bounds_exceeded;
                }
                std::string_view key_bytes;
                if (!reader.read_bytes(key_length, key_bytes)) {
                    return ValueReadStatus::truncated;
                }
                ProtocolValue field_value;
                auto status = read_value(reader, field_value, depth + 1, limits);
                if (status != ValueReadStatus::ok) {
                    return status;
                }
                fields.emplace_back(std::string{key_bytes}, std::move(field_value));
            }
            out = ProtocolValue::make_object(std::move(fields));
            return ValueReadStatus::ok;
        }
        default:
            return ValueReadStatus::malformed;
    }
}

ProtocolError to_protocol_error(ValueReadStatus status) {
    switch (status) {
        case ValueReadStatus::ok:
            return ProtocolError::none;
        case ValueReadStatus::truncated:
            return ProtocolError::truncated_message;
        case ValueReadStatus::malformed:
            return ProtocolError::malformed_message;
        case ValueReadStatus::bounds_exceeded:
            return ProtocolError::value_bounds_exceeded;
    }
    return ProtocolError::malformed_message;
}

// ---------------------------------------------------------------------------
// Top-level message envelope: [u8 wire_version][u8 message_kind][payload].

constexpr std::uint8_t kProtocolWireVersion = 1;

std::string encode_message(ProtocolMessageKind kind, ProtocolValue const& payload) {
    std::string out;
    write_u8(out, kProtocolWireVersion);
    write_u8(out, static_cast<std::uint8_t>(kind));
    write_value(out, payload);
    return out;
}

struct DecodedMessage {
    ProtocolError error;
    std::optional<ProtocolValue> payload;
    std::string message;
};

DecodedMessage decode_message(std::string_view bytes,
                              ProtocolMessageKind expected_kind,
                              ProtocolLimits const& limits) {
    if (bytes.size() > limits.max_message_bytes) {
        return {ProtocolError::message_too_large, std::nullopt,
                "message exceeds the configured byte limit"};
    }
    ByteReader reader{bytes};
    std::uint8_t version = 0;
    if (!reader.read_u8(version)) {
        return {ProtocolError::truncated_message, std::nullopt,
                "message is missing its version byte"};
    }
    if (version != kProtocolWireVersion) {
        return {ProtocolError::unsupported_version, std::nullopt,
                "message declares an unsupported protocol version"};
    }
    std::uint8_t kind_byte = 0;
    if (!reader.read_u8(kind_byte)) {
        return {ProtocolError::truncated_message, std::nullopt,
                "message is missing its kind byte"};
    }
    if (kind_byte != static_cast<std::uint8_t>(expected_kind)) {
        return {ProtocolError::unsupported_message_kind, std::nullopt,
                "message kind does not match the requested decoder"};
    }
    ProtocolValue payload;
    auto status = read_value(reader, payload, 0, limits);
    if (status != ValueReadStatus::ok) {
        return {to_protocol_error(status), std::nullopt,
                "message payload is malformed"};
    }
    if (!reader.at_end()) {
        return {ProtocolError::malformed_message, std::nullopt,
                "message has unexpected trailing bytes"};
    }
    return {ProtocolError::none, std::move(payload), {}};
}

}  // namespace

// ---------------------------------------------------------------------------
// Domain-type <-> ProtocolValue bridge.
//
// Every leaf/composite type reachable from SessionSnapshotSections,
// SessionDelta, ClipboardRequest/Response, and StatusActionInvocation has a
// to_value()/decode_present() pair, plumbed through one generic from_value<T>
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
// single generic from_value<T>() wrapper interprets a wire null as "field is
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
// empirically), so every concrete/generic to_value()/decode_present()
// overload is forward-declared before from_value<T>, to_value<optional<T>>,
// to_value<vector<T>>, and to_value<array<T,N>> -- the generic templates
// that call them -- are *defined*. Forward declarations are added to this
// block as new leaf/composite types are introduced further down the file.
namespace {

// -- Forward declarations: primitives -------------------------------------
ProtocolValue to_value(bool value);
bool decode_present(ProtocolValue const& value, std::optional<bool>& out);
ProtocolValue to_value(std::uint8_t value);
bool decode_present(ProtocolValue const& value, std::optional<std::uint8_t>& out);
ProtocolValue to_value(std::uint32_t value);
bool decode_present(ProtocolValue const& value, std::optional<std::uint32_t>& out);
ProtocolValue to_value(std::uint64_t value);
bool decode_present(ProtocolValue const& value, std::optional<std::uint64_t>& out);
ProtocolValue to_value(std::int64_t value);
bool decode_present(ProtocolValue const& value, std::optional<std::int64_t>& out);
ProtocolValue to_value(int value);
bool decode_present(ProtocolValue const& value, std::optional<int>& out);
ProtocolValue to_value(std::string const& value);
bool decode_present(ProtocolValue const& value, std::optional<std::string>& out);
ProtocolValue to_value(std::vector<std::uint8_t> const& value);
bool decode_present(ProtocolValue const& value,
                    std::optional<std::vector<std::uint8_t>>& out);
ProtocolValue to_value(std::filesystem::path const& value);
bool decode_present(ProtocolValue const& value,
                    std::optional<std::filesystem::path>& out);

template <typename Enum, typename = std::enable_if_t<std::is_enum_v<Enum>>>
ProtocolValue to_value(Enum value) {
    return ProtocolValue::make_uint(static_cast<std::uint64_t>(
        static_cast<std::underlying_type_t<Enum>>(value)));
}

// -- Definitions: primitives -----------------------------------------------
ProtocolValue to_value(bool value) { return ProtocolValue::make_bool(value); }
bool decode_present(ProtocolValue const& value, std::optional<bool>& out) {
    auto decoded = value.as_bool();
    if (!decoded) return false;
    out = *decoded;
    return true;
}

ProtocolValue to_value(std::uint8_t value) {
    return ProtocolValue::make_uint(value);
}
bool decode_present(ProtocolValue const& value, std::optional<std::uint8_t>& out) {
    auto decoded = value.as_uint();
    if (!decoded || *decoded > std::numeric_limits<std::uint8_t>::max()) {
        return false;
    }
    out = static_cast<std::uint8_t>(*decoded);
    return true;
}

ProtocolValue to_value(std::uint32_t value) {
    return ProtocolValue::make_uint(value);
}
bool decode_present(ProtocolValue const& value, std::optional<std::uint32_t>& out) {
    auto decoded = value.as_uint();
    if (!decoded || *decoded > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    out = static_cast<std::uint32_t>(*decoded);
    return true;
}

ProtocolValue to_value(std::uint64_t value) {
    return ProtocolValue::make_uint(value);
}
bool decode_present(ProtocolValue const& value, std::optional<std::uint64_t>& out) {
    auto decoded = value.as_uint();
    if (!decoded) return false;
    out = *decoded;
    return true;
}

ProtocolValue to_value(std::int64_t value) {
    return ProtocolValue::make_int(value);
}
bool decode_present(ProtocolValue const& value, std::optional<std::int64_t>& out) {
    auto decoded = value.as_int();
    if (!decoded) return false;
    out = *decoded;
    return true;
}

ProtocolValue to_value(int value) {
    return ProtocolValue::make_int(static_cast<std::int64_t>(value));
}
bool decode_present(ProtocolValue const& value, std::optional<int>& out) {
    auto decoded = value.as_int();
    if (!decoded || *decoded < std::numeric_limits<int>::min() ||
        *decoded > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(*decoded);
    return true;
}

ProtocolValue to_value(std::string const& value) {
    return ProtocolValue::make_text(value);
}
bool decode_present(ProtocolValue const& value, std::optional<std::string>& out) {
    auto const* decoded = value.as_text();
    if (!decoded) return false;
    out = *decoded;
    return true;
}

ProtocolValue to_value(std::vector<std::uint8_t> const& value) {
    return ProtocolValue::make_bytes(value);
}
bool decode_present(ProtocolValue const& value,
                    std::optional<std::vector<std::uint8_t>>& out) {
    auto const* decoded = value.as_bytes();
    if (!decoded) return false;
    out = *decoded;
    return true;
}

ProtocolValue to_value(std::filesystem::path const& value) {
    return ProtocolValue::make_text(value.string());
}
bool decode_present(ProtocolValue const& value,
                    std::optional<std::filesystem::path>& out) {
    auto const* decoded = value.as_text();
    if (!decoded) return false;
    out = std::filesystem::path{*decoded};
    return true;
}

// -- Forward declarations: enums ------------------------------------------
// (to_value(Enum) is served generically above; only decode_present needs a
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
bool decode_present(ProtocolValue const& value, std::optional<FocusTarget>& out);
bool decode_present(ProtocolValue const& value, std::optional<SemanticRole>& out);

// -- Forward declarations: strong ids -------------------------------------
ProtocolValue to_value(Revision const& value);
bool decode_present(ProtocolValue const& value, std::optional<Revision>& out);
ProtocolValue to_value(ByteOffset const& value);
bool decode_present(ProtocolValue const& value, std::optional<ByteOffset>& out);
ProtocolValue to_value(LineIndex const& value);
bool decode_present(ProtocolValue const& value, std::optional<LineIndex>& out);
ProtocolValue to_value(CellIndex const& value);
bool decode_present(ProtocolValue const& value, std::optional<CellIndex>& out);
ProtocolValue to_value(ClientId const& value);
bool decode_present(ProtocolValue const& value, std::optional<ClientId>& out);
ProtocolValue to_value(ViewId const& value);
bool decode_present(ProtocolValue const& value, std::optional<ViewId>& out);
ProtocolValue to_value(WorkspaceId const& value);
bool decode_present(ProtocolValue const& value, std::optional<WorkspaceId>& out);
ProtocolValue to_value(CapabilityId const& value);
bool decode_present(ProtocolValue const& value, std::optional<CapabilityId>& out);
ProtocolValue to_value(StatusId const& value);
bool decode_present(ProtocolValue const& value, std::optional<StatusId>& out);
ProtocolValue to_value(TabId const& value);
bool decode_present(ProtocolValue const& value, std::optional<TabId>& out);
ProtocolValue to_value(FileDocumentId const& value);
bool decode_present(ProtocolValue const& value, std::optional<FileDocumentId>& out);
ProtocolValue to_value(DiffFileId const& value);
bool decode_present(ProtocolValue const& value, std::optional<DiffFileId>& out);
ProtocolValue to_value(PaneId const& value);
bool decode_present(ProtocolValue const& value, std::optional<PaneId>& out);
ProtocolValue to_value(TreeProviderId const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeProviderId>& out);
ProtocolValue to_value(TreeNodeId const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeNodeId>& out);
ProtocolValue to_value(TreeRevision const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeRevision>& out);
ProtocolValue to_value(LanguageId const& value);
bool decode_present(ProtocolValue const& value, std::optional<LanguageId>& out);
ProtocolValue to_value(UntitledDocumentId const& value);
bool decode_present(ProtocolValue const& value, std::optional<UntitledDocumentId>& out);
ProtocolValue to_value(JournalDocumentKey const& value);
bool decode_present(ProtocolValue const& value, std::optional<JournalDocumentKey>& out);

// -- Forward declarations: composite types ---------------------------------
ProtocolValue to_value(DocumentPosition const& value);
bool decode_present(ProtocolValue const& value, std::optional<DocumentPosition>& out);
ProtocolValue to_value(DocumentViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<DocumentViewState>& out);
ProtocolValue to_value(DocumentDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<DocumentDelta>& out);
ProtocolValue to_value(Selection const& value);
bool decode_present(ProtocolValue const& value, std::optional<Selection>& out);
ProtocolValue to_value(SelectionSet const& value);
bool decode_present(ProtocolValue const& value, std::optional<SelectionSet>& out);
ProtocolValue to_value(SelectionViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<SelectionViewState>& out);
ProtocolValue to_value(SelectionViewDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<SelectionViewDelta>& out);
ProtocolValue to_value(HistoryViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<HistoryViewState>& out);
ProtocolValue to_value(HistoryDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<HistoryDelta>& out);
ProtocolValue to_value(ClipboardRequest const& value);
bool decode_present(ProtocolValue const& value, std::optional<ClipboardRequest>& out);
ProtocolValue to_value(ClipboardResponse const& value);
bool decode_present(ProtocolValue const& value, std::optional<ClipboardResponse>& out);
ProtocolValue to_value(ClipboardViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<ClipboardViewState>& out);
ProtocolValue to_value(ClipboardDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<ClipboardDelta>& out);
ProtocolValue to_value(StatusAction const& value);
bool decode_present(ProtocolValue const& value, std::optional<StatusAction>& out);
ProtocolValue to_value(StatusItemView const& value);
bool decode_present(ProtocolValue const& value, std::optional<StatusItemView>& out);
ProtocolValue to_value(StatusViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<StatusViewState>& out);
ProtocolValue to_value(StatusActionInvocation const& value);
bool decode_present(ProtocolValue const& value, std::optional<StatusActionInvocation>& out);
ProtocolValue to_value(Rect const& value);
bool decode_present(ProtocolValue const& value, std::optional<Rect>& out);
ProtocolValue to_value(PromptControlView const& value);
bool decode_present(ProtocolValue const& value, std::optional<PromptControlView>& out);
ProtocolValue to_value(PromptViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<PromptViewState>& out);
ProtocolValue to_value(PromptStatusViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<PromptStatusViewState>& out);
ProtocolValue to_value(PromptStatusDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<PromptStatusDelta>& out);
ProtocolValue to_value(SearchResult const& value);
bool decode_present(ProtocolValue const& value, std::optional<SearchResult>& out);
ProtocolValue to_value(SearchViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<SearchViewState>& out);
ProtocolValue to_value(SearchDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<SearchDelta>& out);
ProtocolValue to_value(ByteRange const& value);
bool decode_present(ProtocolValue const& value, std::optional<ByteRange>& out);
ProtocolValue to_value(FindOptions const& value);
bool decode_present(ProtocolValue const& value, std::optional<FindOptions>& out);
ProtocolValue to_value(FindRequest const& value);
bool decode_present(ProtocolValue const& value, std::optional<FindRequest>& out);
ProtocolValue to_value(FindMatch const& value);
bool decode_present(ProtocolValue const& value, std::optional<FindMatch>& out);
ProtocolValue to_value(WorkspaceFileReplacement const& value);
bool decode_present(ProtocolValue const& value, std::optional<WorkspaceFileReplacement>& out);
ProtocolValue to_value(WorkspaceReplacePreview const& value);
bool decode_present(ProtocolValue const& value, std::optional<WorkspaceReplacePreview>& out);
ProtocolValue to_value(FindReplaceViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<FindReplaceViewState>& out);
ProtocolValue to_value(FindReplaceDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<FindReplaceDelta>& out);
ProtocolValue to_value(SettingValue const& value);
bool decode_present(ProtocolValue const& value, std::optional<SettingValue>& out);
ProtocolValue to_value(EffectiveSetting const& value);
bool decode_present(ProtocolValue const& value, std::optional<EffectiveSetting>& out);
ProtocolValue to_value(SettingViewEntry const& value);
bool decode_present(ProtocolValue const& value, std::optional<SettingViewEntry>& out);
ProtocolValue to_value(SettingsViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<SettingsViewState>& out);
ProtocolValue to_value(SettingsDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<SettingsDelta>& out);
ProtocolValue to_value(SettingsSectionDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<SettingsSectionDelta>& out);
ProtocolValue to_value(KeyStroke const& value);
bool decode_present(ProtocolValue const& value, std::optional<KeyStroke>& out);
ProtocolValue to_value(KeyBinding const& value);
bool decode_present(ProtocolValue const& value, std::optional<KeyBinding>& out);
ProtocolValue to_value(KeymapViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<KeymapViewState>& out);
ProtocolValue to_value(KeymapDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<KeymapDelta>& out);
ProtocolValue to_value(TextEncodingStatus const& value);
bool decode_present(ProtocolValue const& value, std::optional<TextEncodingStatus>& out);
ProtocolValue to_value(TextEncodingViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<TextEncodingViewState>& out);
ProtocolValue to_value(TextEncodingDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<TextEncodingDelta>& out);
ProtocolValue to_value(TabState const& value);
bool decode_present(ProtocolValue const& value, std::optional<TabState>& out);
ProtocolValue to_value(TabViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<TabViewState>& out);
ProtocolValue to_value(TabDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<TabDelta>& out);
ProtocolValue to_value(DiffHunk const& value);
bool decode_present(ProtocolValue const& value, std::optional<DiffHunk>& out);
ProtocolValue to_value(DiffLineChange const& value);
bool decode_present(ProtocolValue const& value, std::optional<DiffLineChange>& out);
ProtocolValue to_value(DiffFileView const& value);
bool decode_present(ProtocolValue const& value, std::optional<DiffFileView>& out);
ProtocolValue to_value(DiffViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<DiffViewState>& out);
ProtocolValue to_value(DiffDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<DiffDelta>& out);
ProtocolValue to_value(ExternalDocumentView const& value);
bool decode_present(ProtocolValue const& value, std::optional<ExternalDocumentView>& out);
ProtocolValue to_value(ExternalModificationViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<ExternalModificationViewState>& out);
ProtocolValue to_value(ExternalModificationDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<ExternalModificationDelta>& out);
ProtocolValue to_value(ViewportDimensions const& value);
bool decode_present(ProtocolValue const& value, std::optional<ViewportDimensions>& out);
ProtocolValue to_value(FollowScrollOffset const& value);
bool decode_present(ProtocolValue const& value, std::optional<FollowScrollOffset>& out);
ProtocolValue to_value(FollowTarget const& value);
bool decode_present(ProtocolValue const& value, std::optional<FollowTarget>& out);
ProtocolValue to_value(FollowClientView const& value);
bool decode_present(ProtocolValue const& value, std::optional<FollowClientView>& out);
ProtocolValue to_value(FollowEditsViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<FollowEditsViewState>& out);
ProtocolValue to_value(FollowEditsDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<FollowEditsDelta>& out);
ProtocolValue to_value(TreeNodeCommand const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeNodeCommand>& out);
ProtocolValue to_value(TreeNode const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeNode>& out);
ProtocolValue to_value(TreeNodeView const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeNodeView>& out);
ProtocolValue to_value(TreeProviderView const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeProviderView>& out);
ProtocolValue to_value(TreeViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeViewState>& out);
ProtocolValue to_value(TreeProviderDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeProviderDelta>& out);
ProtocolValue to_value(TreeDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeDelta>& out);
ProtocolValue to_value(SyntaxSpan const& value);
bool decode_present(ProtocolValue const& value, std::optional<SyntaxSpan>& out);
ProtocolValue to_value(SyntaxBracketPair const& value);
bool decode_present(ProtocolValue const& value, std::optional<SyntaxBracketPair>& out);
ProtocolValue to_value(UnmatchedBracket const& value);
bool decode_present(ProtocolValue const& value, std::optional<UnmatchedBracket>& out);
ProtocolValue to_value(SyntaxRange const& value);
bool decode_present(ProtocolValue const& value, std::optional<SyntaxRange>& out);
ProtocolValue to_value(CommentToken const& value);
bool decode_present(ProtocolValue const& value, std::optional<CommentToken>& out);
ProtocolValue to_value(CommentRange const& value);
bool decode_present(ProtocolValue const& value, std::optional<CommentRange>& out);
ProtocolValue to_value(LineIndentation const& value);
bool decode_present(ProtocolValue const& value, std::optional<LineIndentation>& out);
ProtocolValue to_value(SyntaxViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<SyntaxViewState>& out);
ProtocolValue to_value(SyntaxDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<SyntaxDelta>& out);
ProtocolValue to_value(LspPosition const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspPosition>& out);
ProtocolValue to_value(LspRange const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspRange>& out);
ProtocolValue to_value(LspDiagnostic const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspDiagnostic>& out);
ProtocolValue to_value(LspDocumentDiagnostics const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspDocumentDiagnostics>& out);
ProtocolValue to_value(LspSyncViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspSyncViewState>& out);
ProtocolValue to_value(LspSyncDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspSyncDelta>& out);
ProtocolValue to_value(LspCompletionItem const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspCompletionItem>& out);
ProtocolValue to_value(LspCompletionViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspCompletionViewState>& out);
ProtocolValue to_value(LspHover const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspHover>& out);
ProtocolValue to_value(LspNavigationTarget const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspNavigationTarget>& out);
ProtocolValue to_value(LspNavigationViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspNavigationViewState>& out);
ProtocolValue to_value(LspFeatureViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspFeatureViewState>& out);
ProtocolValue to_value(LspFeatureDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<LspFeatureDelta>& out);
ProtocolValue to_value(SrgbColor const& value);
bool decode_present(ProtocolValue const& value, std::optional<SrgbColor>& out);
ProtocolValue to_value(ThemeSnapshot const& value);
bool decode_present(ProtocolValue const& value, std::optional<ThemeSnapshot>& out);
ProtocolValue to_value(ThemeSectionDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<ThemeSectionDelta>& out);
ProtocolValue to_value(GridSize const& value);
bool decode_present(ProtocolValue const& value, std::optional<GridSize>& out);
ProtocolValue to_value(AccessibilityNode const& value);
bool decode_present(ProtocolValue const& value, std::optional<AccessibilityNode>& out);
ProtocolValue to_value(PaneGeometry const& value);
bool decode_present(ProtocolValue const& value, std::optional<PaneGeometry>& out);
ProtocolValue to_value(TabHit const& value);
bool decode_present(ProtocolValue const& value, std::optional<TabHit>& out);
ProtocolValue to_value(ShellViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<ShellViewState>& out);
ProtocolValue to_value(ShellSectionDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<ShellSectionDelta>& out);
ProtocolValue to_value(VisualRow const& value);
bool decode_present(ProtocolValue const& value, std::optional<VisualRow>& out);
ProtocolValue to_value(CellHitTarget const& value);
bool decode_present(ProtocolValue const& value, std::optional<CellHitTarget>& out);
ProtocolValue to_value(ScrollbarMetrics const& value);
bool decode_present(ProtocolValue const& value, std::optional<ScrollbarMetrics>& out);
ProtocolValue to_value(ViewportViewState const& value);
bool decode_present(ProtocolValue const& value, std::optional<ViewportViewState>& out);
ProtocolValue to_value(ViewportDelta const& value);
bool decode_present(ProtocolValue const& value, std::optional<ViewportDelta>& out);
ProtocolValue to_value(SessionTopology const& value);
bool decode_present(ProtocolValue const& value, std::optional<SessionTopology>& out);
ProtocolValue to_value(ClientSnapshotState const& value);
bool decode_present(ProtocolValue const& value, std::optional<ClientSnapshotState>& out);
ProtocolValue to_value(SessionSnapshotSections const& value);
bool decode_present(ProtocolValue const& value, std::optional<SessionSnapshotSections>& out);

// -- Forward declarations: command argument payload types -------------------
ProtocolValue to_value(TextInputArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<TextInputArguments>& out);
ProtocolValue to_value(PaletteExecuteArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<PaletteExecuteArguments>& out);
ProtocolValue to_value(TreeSelectArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<TreeSelectArguments>& out);
ProtocolValue to_value(FindQueryArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<FindQueryArguments>& out);
ProtocolValue to_value(SelectionCommandArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<SelectionCommandArguments>& out);
ProtocolValue to_value(ScrollLinesArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<ScrollLinesArguments>& out);
ProtocolValue to_value(ScrollPagesArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<ScrollPagesArguments>& out);
ProtocolValue to_value(ScrollFractionArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<ScrollFractionArguments>& out);
ProtocolValue to_value(DroppedContentArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<DroppedContentArguments>& out);
ProtocolValue to_value(ReopenWithEncodingArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<ReopenWithEncodingArguments>& out);
ProtocolValue to_value(SetEncodingArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<SetEncodingArguments>& out);
ProtocolValue to_value(SetLineEndingArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<SetLineEndingArguments>& out);
ProtocolValue to_value(SetFinalNewlineArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<SetFinalNewlineArguments>& out);
ProtocolValue to_value(SettingSetArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<SettingSetArguments>& out);
ProtocolValue to_value(SettingResetArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<SettingResetArguments>& out);
ProtocolValue to_value(SettingResetScopeArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<SettingResetScopeArguments>& out);
ProtocolValue to_value(WorkspaceReplaceArguments const& value);
bool decode_present(ProtocolValue const& value, std::optional<WorkspaceReplaceArguments>& out);

// -- Forward declarations: generic container shapes ------------------------
template <typename T>
ProtocolValue to_value(std::optional<T> const& value);
template <typename T>
ProtocolValue to_value(std::vector<T> const& value);
template <typename T, std::size_t N>
ProtocolValue to_value(std::array<T, N> const& value);

template <typename T>
[[nodiscard]] bool from_value(ProtocolValue const& value, std::optional<T>& out);
template <typename T>
bool decode_present(ProtocolValue const& value, std::optional<std::vector<T>>& out);
template <typename T, std::size_t N>
bool decode_present(ProtocolValue const& value, std::optional<std::array<T, N>>& out);

// -- Generic entry points (definitions) -------------------------------------
template <typename T>
ProtocolValue to_value(std::optional<T> const& value) {
    if (!value) {
        return ProtocolValue::make_null();
    }
    return to_value(*value);
}

template <typename T>
ProtocolValue to_value(std::vector<T> const& value) {
    std::vector<ProtocolValue> items;
    items.reserve(value.size());
    for (auto const& item : value) {
        items.push_back(to_value(item));
    }
    return ProtocolValue::make_array(std::move(items));
}

template <typename T, std::size_t N>
ProtocolValue to_value(std::array<T, N> const& value) {
    std::vector<ProtocolValue> items;
    items.reserve(N);
    for (auto const& item : value) {
        items.push_back(to_value(item));
    }
    return ProtocolValue::make_array(std::move(items));
}

template <typename T>
[[nodiscard]] bool from_value(ProtocolValue const& value, std::optional<T>& out) {
    if (value.kind() == ProtocolValue::Kind::null_value) {
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
    auto const* items = value.as_array();
    if (items == nullptr) {
        return false;
    }
    std::vector<T> result;
    result.reserve(items->size());
    for (auto const& item : *items) {
        std::optional<T> decoded;
        if (!from_value(item, decoded) || !decoded.has_value()) {
            return false;
        }
        result.push_back(std::move(*decoded));
    }
    out.emplace(std::move(result));
    return true;
}

template <typename T, std::size_t N>
bool decode_present(ProtocolValue const& value, std::optional<std::array<T, N>>& out) {
    auto const* items = value.as_array();
    if (items == nullptr || items->size() != N) {
        return false;
    }
    std::array<T, N> result{};
    for (std::size_t i = 0; i < N; ++i) {
        std::optional<T> decoded;
        if (!from_value((*items)[i], decoded) || !decoded.has_value()) {
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
[[nodiscard]] std::optional<T> require_field(ProtocolValue const* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    std::optional<T> decoded;
    if (!from_value(*value, decoded)) {
        return std::nullopt;
    }
    return decoded;
}

// Decodes a domain-optional field: a null field pointer (absent from the
// wire object) is a legitimate absence (out.reset(), success); a present
// field delegates to from_value(), which itself treats an explicit wire null
// as absence and rejects malformed data. Unlike require_field(), a
// genuinely-absent field is not an error here.
template <typename T>
[[nodiscard]] bool decode_optional_field(ProtocolValue const* field_value,
                                         std::optional<T>& out) {
    if (field_value == nullptr) {
        out.reset();
        return true;
    }
    return from_value(*field_value, out);
}

template <typename Enum, std::size_t N>
[[nodiscard]] bool decode_enum(ProtocolValue const& value, std::optional<Enum>& out,
                               std::array<Enum, N> const& valid_values) {
    auto raw = value.as_uint();
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


// ---------------------------------------------------------------------------
// Enum decode_present() definitions, each delegating to decode_enum() with
// the closed set of valid values for that enum.

bool decode_present(ProtocolValue const& value, std::optional<DocumentMode>& out) {
    static constexpr std::array values{DocumentMode::edit, DocumentMode::read_only,
                                       DocumentMode::diff};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<ClipboardRequestKind>& out) {
    static constexpr std::array values{ClipboardRequestKind::write,
                                       ClipboardRequestKind::read};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<ClipboardResponseStatus>& out) {
    static constexpr std::array values{
        ClipboardResponseStatus::success, ClipboardResponseStatus::denied,
        ClipboardResponseStatus::unavailable, ClipboardResponseStatus::disconnected};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<StatusPriority>& out) {
    static constexpr std::array values{StatusPriority::error, StatusPriority::warning,
                                       StatusPriority::information, StatusPriority::progress};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<PromptKind>& out) {
    static constexpr std::array values{PromptKind::path, PromptKind::find,
                                       PromptKind::replace, PromptKind::settings,
                                       PromptKind::command_argument,
                                       PromptKind::palette};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<PromptControlKind>& out) {
    static constexpr std::array values{PromptControlKind::input, PromptControlKind::toggle,
                                       PromptControlKind::count};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<SearchMode>& out) {
    static constexpr std::array values{SearchMode::file, SearchMode::line,
                                       SearchMode::symbol, SearchMode::text,
                                       SearchMode::command};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<FindReplaceError>& out) {
    static constexpr std::array values{
        FindReplaceError::none, FindReplaceError::invalid_pattern,
        FindReplaceError::invalid_utf8, FindReplaceError::invalid_selection,
        FindReplaceError::budget_exhausted, FindReplaceError::cancelled,
        FindReplaceError::no_match, FindReplaceError::stale_revision,
        FindReplaceError::document_rejected, FindReplaceError::workspace_rejected,
        FindReplaceError::recovery_rejected};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<SettingScope>& out) {
    static constexpr std::array values{SettingScope::defaults, SettingScope::user,
                                       SettingScope::workspace, SettingScope::language,
                                       SettingScope::document};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<SettingKey>& out) {
    static constexpr std::array values{
        SettingKey::indent_width, SettingKey::indent_style, SettingKey::indent_detection,
        SettingKey::auto_indent, SettingKey::line_ending, SettingKey::final_newline,
        SettingKey::encoding, SettingKey::word_wrap, SettingKey::theme, SettingKey::keymap,
        SettingKey::search_case_sensitive, SettingKey::search_whole_word,
        SettingKey::search_regular_expression, SettingKey::undo_byte_budget,
        SettingKey::recovery_byte_budget, SettingKey::typing_coalescing_ms};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<TextEncoding>& out) {
    static constexpr std::array values{TextEncoding::utf8, TextEncoding::utf8_bom,
                                       TextEncoding::utf16le, TextEncoding::utf16be,
                                       TextEncoding::windows1252, TextEncoding::iso88591};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<IndentStyle>& out) {
    static constexpr std::array values{IndentStyle::spaces, IndentStyle::tabs};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<LineEnding>& out) {
    static constexpr std::array values{LineEnding::lf, LineEnding::crlf, LineEnding::cr,
                                       LineEnding::mixed};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<TabKind>& out) {
    static constexpr std::array values{TabKind::document, TabKind::live_diff,
                                       TabKind::read_only_output, TabKind::search_results,
                                       TabKind::tree_view};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<TabRecoveryBadge>& out) {
    static constexpr std::array values{TabRecoveryBadge::none, TabRecoveryBadge::pending,
                                       TabRecoveryBadge::durable, TabRecoveryBadge::failed};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<JournalDocumentKeyKind>& out) {
    static constexpr std::array values{JournalDocumentKeyKind::saved,
                                       JournalDocumentKeyKind::untitled};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<DiffLineKind>& out) {
    static constexpr std::array values{DiffLineKind::added, DiffLineKind::removed,
                                       DiffLineKind::modified};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<ExternalAction>& out) {
    static constexpr std::array values{ExternalAction::reload, ExternalAction::keep_buffer,
                                       ExternalAction::open_diff};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<ExternalDocumentStatus>& out) {
    static constexpr std::array values{ExternalDocumentStatus::externally_modified,
                                       ExternalDocumentStatus::externally_removed};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<FollowMode>& out) {
    static constexpr std::array values{FollowMode::following, FollowMode::paused};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<TreeProviderKind>& out) {
    static constexpr std::array values{TreeProviderKind::filesystem, TreeProviderKind::git,
                                       TreeProviderKind::symbols};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<TreeNodeKind>& out) {
    static constexpr std::array values{TreeNodeKind::root, TreeNodeKind::directory,
                                       TreeNodeKind::file, TreeNodeKind::symlink,
                                       TreeNodeKind::git_entry, TreeNodeKind::symbol};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<GitTreeStatus>& out) {
    static constexpr std::array values{GitTreeStatus::added, GitTreeStatus::modified,
                                       GitTreeStatus::deleted, GitTreeStatus::renamed,
                                       GitTreeStatus::untracked};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<SyntaxScope>& out) {
    return decode_enum(value, out, all_syntax_scopes);
}

bool decode_present(ProtocolValue const& value, std::optional<BracketKind>& out) {
    static constexpr std::array values{BracketKind::round, BracketKind::square,
                                       BracketKind::curly};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<BracketRole>& out) {
    static constexpr std::array values{BracketRole::open, BracketRole::close};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<CommentKind>& out) {
    static constexpr std::array values{CommentKind::line, CommentKind::block};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<CommentTokenRole>& out) {
    static constexpr std::array values{CommentTokenRole::line, CommentTokenRole::block_open,
                                       CommentTokenRole::block_close};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<LspDiagnosticSeverity>& out) {
    static constexpr std::array values{
        LspDiagnosticSeverity::error, LspDiagnosticSeverity::warning,
        LspDiagnosticSeverity::information, LspDiagnosticSeverity::hint};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<ShellNodeKind>& out) {
    static constexpr std::array values{
        ShellNodeKind::header, ShellNodeKind::header_field, ShellNodeKind::footer,
        ShellNodeKind::footer_field, ShellNodeKind::footer_action, ShellNodeKind::tab_bar,
        ShellNodeKind::tab, ShellNodeKind::panel, ShellNodeKind::panel_provider,
        ShellNodeKind::pane, ShellNodeKind::scrollbar, ShellNodeKind::prompt_reservation,
        ShellNodeKind::empty_state};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<FocusTarget>& out) {
    static constexpr std::array values{FocusTarget::editor, FocusTarget::panel,
                                       FocusTarget::prompt};
    return decode_enum(value, out, values);
}

bool decode_present(ProtocolValue const& value, std::optional<SemanticRole>& out) {
    return decode_enum(value, out, all_semantic_roles);
}

// ---------------------------------------------------------------------------
// Strong-id to_value()/decode_present() definitions.

ProtocolValue to_value(Revision const& value) {
    return ProtocolValue::make_uint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<Revision>& out) {
    auto raw = value.as_uint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue to_value(ByteOffset const& value) {
    return ProtocolValue::make_uint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<ByteOffset>& out) {
    auto raw = value.as_uint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue to_value(LineIndex const& value) {
    return ProtocolValue::make_uint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<LineIndex>& out) {
    auto raw = value.as_uint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue to_value(CellIndex const& value) {
    return ProtocolValue::make_uint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<CellIndex>& out) {
    auto raw = value.as_uint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue to_value(ClientId const& value) {
    return ProtocolValue::make_uint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<ClientId>& out) {
    auto raw = value.as_uint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue to_value(ViewId const& value) {
    return ProtocolValue::make_uint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<ViewId>& out) {
    auto raw = value.as_uint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue to_value(WorkspaceId const& value) {
    return ProtocolValue::make_uint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<WorkspaceId>& out) {
    auto raw = value.as_uint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue to_value(CapabilityId const& value) {
    return ProtocolValue::make_text(std::string{value.value()});
}
bool decode_present(ProtocolValue const& value, std::optional<CapabilityId>& out) {
    auto const* text = value.as_text();
    if (!text) return false;
    out.emplace(*text);
    return true;
}

ProtocolValue to_value(StatusId const& value) {
    return ProtocolValue::make_uint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<StatusId>& out) {
    auto raw = value.as_uint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue to_value(TabId const& value) {
    return ProtocolValue::make_uint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<TabId>& out) {
    auto raw = value.as_uint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue to_value(FileDocumentId const& value) {
    return ProtocolValue::make_uint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<FileDocumentId>& out) {
    auto raw = value.as_uint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue to_value(DiffFileId const& value) {
    return ProtocolValue::make_text(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<DiffFileId>& out) {
    auto const* text = value.as_text();
    if (!text) return false;
    out.emplace(*text);
    return true;
}

ProtocolValue to_value(PaneId const& value) {
    return ProtocolValue::make_uint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<PaneId>& out) {
    auto raw = value.as_uint();
    if (!raw || *raw > std::numeric_limits<std::uint32_t>::max()) return false;
    out.emplace(static_cast<std::uint32_t>(*raw));
    return true;
}

ProtocolValue to_value(TreeProviderId const& value) {
    return ProtocolValue::make_text(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<TreeProviderId>& out) {
    auto const* text = value.as_text();
    if (!text) return false;
    out.emplace(*text);
    return true;
}

ProtocolValue to_value(TreeNodeId const& value) {
    return ProtocolValue::make_text(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<TreeNodeId>& out) {
    auto const* text = value.as_text();
    if (!text) return false;
    out.emplace(*text);
    return true;
}

ProtocolValue to_value(TreeRevision const& value) {
    return ProtocolValue::make_uint(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<TreeRevision>& out) {
    auto raw = value.as_uint();
    if (!raw) return false;
    out.emplace(*raw);
    return true;
}

ProtocolValue to_value(LanguageId const& value) {
    return ProtocolValue::make_text(value.value());
}
bool decode_present(ProtocolValue const& value, std::optional<LanguageId>& out) {
    auto const* text = value.as_text();
    if (!text) return false;
    out.emplace(*text);
    return true;
}

ProtocolValue to_value(UntitledDocumentId const& value) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(value.bytes().size());
    for (std::byte b : value.bytes()) bytes.push_back(static_cast<std::uint8_t>(b));
    return ProtocolValue::make_bytes(std::move(bytes));
}
bool decode_present(ProtocolValue const& value, std::optional<UntitledDocumentId>& out) {
    auto const* bytes = value.as_bytes();
    if (!bytes || bytes->size() != 16) return false;
    std::array<std::byte, 16> raw{};
    for (std::size_t i = 0; i < 16; ++i) raw[i] = static_cast<std::byte>((*bytes)[i]);
    out.emplace(raw);
    return true;
}

ProtocolValue to_value(JournalDocumentKey const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", to_value(value.kind()));
    if (value.kind() == JournalDocumentKeyKind::saved) {
        fields.emplace_back("path", to_value(value.saved_path()));
    } else {
        fields.emplace_back("untitled_id", to_value(value.untitled_id()));
    }
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<JournalDocumentKey>& out) {
    auto kind = require_field<JournalDocumentKeyKind>(value.field("kind"));
    if (!kind) return false;
    if (*kind == JournalDocumentKeyKind::saved) {
        auto path = require_field<std::string>(value.field("path"));
        if (!path) return false;
        out.emplace(JournalDocumentKey::saved(*path));
    } else {
        auto id = require_field<UntitledDocumentId>(value.field("untitled_id"));
        if (!id) return false;
        out.emplace(JournalDocumentKey::untitled(*id));
    }
    return true;
}

// ---------------------------------------------------------------------------
// types.h / document.h / selection.h / history.h composite definitions.
//
// Every composite decode_present() below starts by rejecting a non-object
// wire value outright: value.field() already returns nullptr for every key
// when the value is not an object, which require_field() and
// decode_optional_field() both turn into "field absent" -- but a struct
// whose fields are *all* domain-optional (e.g. SessionTopology) would then
// wrongly decode a malformed non-object value (an array, a bare integer) as
// "every field absent" instead of rejecting it. The explicit as_object()
// check below closes that gap uniformly.

ProtocolValue to_value(DocumentPosition const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("byte_offset", to_value(value.byte_offset));
    fields.emplace_back("line", to_value(value.line));
    fields.emplace_back("cell", to_value(value.cell));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DocumentPosition>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto byte_offset = require_field<ByteOffset>(value.field("byte_offset"));
    auto line = require_field<LineIndex>(value.field("line"));
    auto cell = require_field<CellIndex>(value.field("cell"));
    if (!byte_offset || !line || !cell) return false;
    out.emplace(DocumentPosition{*byte_offset, *line, *cell});
    return true;
}

ProtocolValue to_value(DocumentViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", to_value(value.revision));
    fields.emplace_back("text", to_value(value.text));
    fields.emplace_back("caret", to_value(value.caret));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DocumentViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto revision = require_field<Revision>(value.field("revision"));
    auto text = require_field<std::string>(value.field("text"));
    auto caret = require_field<ByteOffset>(value.field("caret"));
    if (!revision || !text || !caret) return false;
    out.emplace(DocumentViewState{*revision, *text, *caret});
    return true;
}

ProtocolValue to_value(DocumentDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", to_value(value.base_revision));
    fields.emplace_back("revision", to_value(value.revision));
    fields.emplace_back("start", to_value(value.start));
    fields.emplace_back("erased_bytes", to_value(value.erased_bytes));
    fields.emplace_back("inserted_text", to_value(value.inserted_text));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DocumentDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto base_revision = require_field<Revision>(value.field("base_revision"));
    auto revision = require_field<Revision>(value.field("revision"));
    auto start = require_field<ByteOffset>(value.field("start"));
    auto erased_bytes = require_field<std::uint64_t>(value.field("erased_bytes"));
    auto inserted_text = require_field<std::string>(value.field("inserted_text"));
    if (!base_revision || !revision || !start || !erased_bytes || !inserted_text) {
        return false;
    }
    out.emplace(DocumentDelta{*base_revision, *revision, *start, *erased_bytes,
                              *inserted_text});
    return true;
}

ProtocolValue to_value(Selection const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("anchor", to_value(value.anchor));
    fields.emplace_back("active", to_value(value.active));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<Selection>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto anchor = require_field<DocumentPosition>(value.field("anchor"));
    auto active = require_field<DocumentPosition>(value.field("active"));
    if (!anchor || !active) return false;
    out.emplace(Selection{*anchor, *active});
    return true;
}

ProtocolValue to_value(SelectionSet const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("selections", to_value(value.items()));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SelectionSet>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto selections =
        require_field<std::vector<Selection>>(value.field("selections"));
    if (!selections || selections->empty()) return false;
    out.emplace(*selections);
    return true;
}

ProtocolValue to_value(SelectionViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("selections", to_value(value.selections));
    fields.emplace_back("first_visual_row", to_value(value.first_visual_row));
    fields.emplace_back("first_visual_column", to_value(value.first_visual_column));
    fields.emplace_back("desired_cell", to_value(value.desired_cell));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SelectionViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto selections = require_field<SelectionSet>(value.field("selections"));
    auto first_visual_row =
        require_field<std::uint32_t>(value.field("first_visual_row"));
    auto first_visual_column =
        require_field<std::uint32_t>(value.field("first_visual_column"));
    if (!selections || !first_visual_row || !first_visual_column) return false;
    std::optional<CellIndex> desired_cell;
    if (!decode_optional_field(value.field("desired_cell"), desired_cell)) return false;
    out.emplace(SelectionViewState{*selections, *first_visual_row,
                                   *first_visual_column, desired_cell});
    return true;
}

ProtocolValue to_value(SelectionViewDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", to_value(value.changed));
    fields.emplace_back("replacement", to_value(value.replacement));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SelectionViewDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto changed = require_field<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<SelectionViewState> replacement;
    if (!decode_optional_field(value.field("replacement"), replacement)) return false;
    out.emplace(SelectionViewDelta{*changed, std::move(replacement)});
    return true;
}

ProtocolValue to_value(HistoryViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("can_undo", to_value(value.can_undo));
    fields.emplace_back("can_redo", to_value(value.can_redo));
    fields.emplace_back("retained_bytes", to_value(value.retained_bytes));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<HistoryViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto can_undo = require_field<bool>(value.field("can_undo"));
    auto can_redo = require_field<bool>(value.field("can_redo"));
    auto retained_bytes = require_field<std::uint64_t>(value.field("retained_bytes"));
    if (!can_undo || !can_redo || !retained_bytes) return false;
    out.emplace(HistoryViewState{*can_undo, *can_redo, *retained_bytes});
    return true;
}

ProtocolValue to_value(HistoryDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", to_value(value.changed));
    fields.emplace_back("replacement", to_value(value.replacement));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<HistoryDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto changed = require_field<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<HistoryViewState> replacement;
    if (!decode_optional_field(value.field("replacement"), replacement)) return false;
    out.emplace(HistoryDelta{*changed, std::move(replacement)});
    return true;
}

// ---------------------------------------------------------------------------
// ui_layout.h composite definitions (Rect/GridSize/ShellLabel/AccessibilityNode/
// PaneGeometry/ShellViewState).

ProtocolValue to_value(Rect const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("x", to_value(value.x));
    fields.emplace_back("y", to_value(value.y));
    fields.emplace_back("width", to_value(value.width));
    fields.emplace_back("height", to_value(value.height));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<Rect>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto x = require_field<int>(value.field("x"));
    auto y = require_field<int>(value.field("y"));
    auto width = require_field<int>(value.field("width"));
    auto height = require_field<int>(value.field("height"));
    if (!x || !y || !width || !height) return false;
    out.emplace(Rect{*x, *y, *width, *height});
    return true;
}

ProtocolValue to_value(GridSize const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("columns", to_value(value.columns));
    fields.emplace_back("rows", to_value(value.rows));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<GridSize>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto columns = require_field<int>(value.field("columns"));
    auto rows = require_field<int>(value.field("rows"));
    if (!columns || !rows) return false;
    out.emplace(GridSize{*columns, *rows});
    return true;
}

ProtocolValue to_value(ShellLabel const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("accessible_label", to_value(value.accessible_label));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ShellLabel>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto id = require_field<std::string>(value.field("id"));
    auto accessible_label = require_field<std::string>(value.field("accessible_label"));
    if (!id || !accessible_label) return false;
    out.emplace(ShellLabel{*id, *accessible_label});
    return true;
}

ProtocolValue to_value(AccessibilityNode const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", to_value(value.kind));
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("label", to_value(value.label));
    fields.emplace_back("rect", to_value(value.rect));
    fields.emplace_back("role", to_value(value.role));
    fields.emplace_back("content", to_value(value.content));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<AccessibilityNode>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto kind = require_field<ShellNodeKind>(value.field("kind"));
    auto id = require_field<std::string>(value.field("id"));
    auto label = require_field<std::string>(value.field("label"));
    auto rect = require_field<Rect>(value.field("rect"));
    auto role = require_field<SemanticRole>(value.field("role"));
    auto content = require_field<std::string>(value.field("content"));
    if (!kind || !id || !label || !rect || !role || !content) return false;
    out.emplace(AccessibilityNode{*kind, *id, *label, *rect, *role, *content});
    return true;
}

ProtocolValue to_value(PaneGeometry const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("frame", to_value(value.frame));
    fields.emplace_back("content", to_value(value.content));
    fields.emplace_back("scrollbar", to_value(value.scrollbar));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<PaneGeometry>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto id = require_field<PaneId>(value.field("id"));
    auto frame = require_field<Rect>(value.field("frame"));
    auto content = require_field<Rect>(value.field("content"));
    auto scrollbar = require_field<Rect>(value.field("scrollbar"));
    if (!id || !frame || !content || !scrollbar) return false;
    out.emplace(PaneGeometry{*id, *frame, *content, *scrollbar});
    return true;
}

ProtocolValue to_value(TabHit const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("rect", to_value(value.rect));
    fields.emplace_back("index", to_value(value.index));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TabHit>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto rect = require_field<Rect>(value.field("rect"));
    auto index = require_field<std::uint32_t>(value.field("index"));
    if (!rect || !index) return false;
    out.emplace(TabHit{*rect, *index});
    return true;
}

ProtocolValue to_value(ShellViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("viewport", to_value(value.viewport));
    fields.emplace_back("header", to_value(value.header));
    fields.emplace_back("footer", to_value(value.footer));
    fields.emplace_back("tab_bar", to_value(value.tab_bar));
    fields.emplace_back("panel", to_value(value.panel));
    fields.emplace_back("panel_scrollbar", to_value(value.panel_scrollbar));
    fields.emplace_back("prompt", to_value(value.prompt));
    fields.emplace_back("panes", to_value(value.panes));
    fields.emplace_back("tab_hits", to_value(value.tab_hits));
    fields.emplace_back("accessibility_nodes", to_value(value.accessibility_nodes));
    fields.emplace_back("focus", to_value(value.focus));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ShellViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto viewport = require_field<GridSize>(value.field("viewport"));
    auto panes = require_field<std::vector<PaneGeometry>>(value.field("panes"));
    auto tab_hits = require_field<std::vector<TabHit>>(value.field("tab_hits"));
    auto accessibility_nodes =
        require_field<std::vector<AccessibilityNode>>(value.field("accessibility_nodes"));
    if (!viewport || !panes || !tab_hits || !accessibility_nodes) return false;
    ShellViewState result;
    result.viewport = *viewport;
    if (auto const* focus_field = value.field("focus")) {
        std::optional<FocusTarget> focus;
        if (!decode_present(*focus_field, focus) || !focus) return false;
        result.focus = *focus;
    }
    if (!decode_optional_field(value.field("header"), result.header)) return false;
    if (!decode_optional_field(value.field("footer"), result.footer)) return false;
    if (!decode_optional_field(value.field("tab_bar"), result.tab_bar)) return false;
    if (!decode_optional_field(value.field("panel"), result.panel)) return false;
    if (!decode_optional_field(value.field("panel_scrollbar"), result.panel_scrollbar)) return false;
    if (!decode_optional_field(value.field("prompt"), result.prompt)) return false;
    result.panes = *panes;
    result.tab_hits = *tab_hits;
    result.accessibility_nodes = *accessibility_nodes;
    out.emplace(std::move(result));
    return true;
}

// ---------------------------------------------------------------------------
// prompt.h composite definitions.

ProtocolValue to_value(PromptInput const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("accessible_label", to_value(value.accessible_label));
    fields.emplace_back("value", to_value(value.value));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<PromptInput>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto id = require_field<std::string>(value.field("id"));
    auto accessible_label = require_field<std::string>(value.field("accessible_label"));
    auto text_value = require_field<std::string>(value.field("value"));
    if (!id || !accessible_label || !text_value) return false;
    out.emplace(PromptInput{*id, *accessible_label, *text_value});
    return true;
}

ProtocolValue to_value(PromptToggle const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("accessible_label", to_value(value.accessible_label));
    fields.emplace_back("value", to_value(value.value));
    fields.emplace_back("width", to_value(value.width));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<PromptToggle>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto id = require_field<std::string>(value.field("id"));
    auto accessible_label = require_field<std::string>(value.field("accessible_label"));
    auto toggle_value = require_field<bool>(value.field("value"));
    auto width = require_field<int>(value.field("width"));
    if (!id || !accessible_label || !toggle_value || !width) return false;
    out.emplace(PromptToggle{*id, *accessible_label, *toggle_value, *width});
    return true;
}

ProtocolValue to_value(PromptMatchCount const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("accessible_label", to_value(value.accessible_label));
    fields.emplace_back("value", to_value(value.value));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<PromptMatchCount>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto id = require_field<std::string>(value.field("id"));
    auto accessible_label = require_field<std::string>(value.field("accessible_label"));
    auto text_value = require_field<std::string>(value.field("value"));
    if (!id || !accessible_label || !text_value) return false;
    out.emplace(PromptMatchCount{*id, *accessible_label, *text_value});
    return true;
}

ProtocolValue to_value(PromptControlView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", to_value(value.kind));
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("accessible_label", to_value(value.accessible_label));
    fields.emplace_back("value", to_value(value.value));
    fields.emplace_back("checked", to_value(value.checked));
    fields.emplace_back("rect", to_value(value.rect));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<PromptControlView>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto kind = require_field<PromptControlKind>(value.field("kind"));
    auto id = require_field<std::string>(value.field("id"));
    auto accessible_label = require_field<std::string>(value.field("accessible_label"));
    auto text_value = require_field<std::string>(value.field("value"));
    auto checked = require_field<bool>(value.field("checked"));
    auto rect = require_field<Rect>(value.field("rect"));
    if (!kind || !id || !accessible_label || !text_value || !checked || !rect) {
        return false;
    }
    out.emplace(PromptControlView{*kind, *id, *accessible_label, *text_value,
                                  *checked, *rect});
    return true;
}

ProtocolValue to_value(PromptViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", to_value(value.kind));
    fields.emplace_back("accessible_label", to_value(value.accessible_label));
    fields.emplace_back("rect", to_value(value.rect));
    fields.emplace_back("controls", to_value(value.controls));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<PromptViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto kind = require_field<PromptKind>(value.field("kind"));
    auto accessible_label = require_field<std::string>(value.field("accessible_label"));
    auto rect = require_field<Rect>(value.field("rect"));
    auto controls = require_field<std::vector<PromptControlView>>(value.field("controls"));
    if (!kind || !accessible_label || !rect || !controls) return false;
    out.emplace(PromptViewState{*kind, *accessible_label, *rect, *controls});
    return true;
}

// ---------------------------------------------------------------------------
// status.h composite definitions.

ProtocolValue to_value(StatusAction const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("accessible_label", to_value(value.accessible_label));
    fields.emplace_back("command_id", to_value(value.command_id));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<StatusAction>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto id = require_field<std::string>(value.field("id"));
    auto accessible_label = require_field<std::string>(value.field("accessible_label"));
    auto command_id = require_field<std::string>(value.field("command_id"));
    if (!id || !accessible_label || !command_id) return false;
    out.emplace(StatusAction{*id, *accessible_label, *command_id});
    return true;
}

ProtocolValue to_value(StatusItemView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("priority", to_value(value.priority));
    fields.emplace_back("generation", to_value(value.generation));
    fields.emplace_back("accessible_label", to_value(value.accessible_label));
    fields.emplace_back("actions", to_value(value.actions));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<StatusItemView>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto id = require_field<StatusId>(value.field("id"));
    auto priority = require_field<StatusPriority>(value.field("priority"));
    auto generation = require_field<std::uint64_t>(value.field("generation"));
    auto accessible_label = require_field<std::string>(value.field("accessible_label"));
    auto actions = require_field<std::vector<StatusAction>>(value.field("actions"));
    if (!id || !priority || !generation || !accessible_label || !actions) {
        return false;
    }
    out.emplace(StatusItemView{*id, *priority, *generation, *accessible_label,
                               *actions});
    return true;
}

ProtocolValue to_value(StatusViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("items", to_value(value.items));
    fields.emplace_back("selected", to_value(static_cast<std::uint64_t>(value.selected)));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<StatusViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto items = require_field<std::vector<StatusItemView>>(value.field("items"));
    auto selected = require_field<std::uint64_t>(value.field("selected"));
    if (!items || !selected) return false;
    out.emplace(StatusViewState{*items, static_cast<std::size_t>(*selected)});
    return true;
}

ProtocolValue to_value(StatusActionInvocation const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("status_id", to_value(value.status_id));
    fields.emplace_back("action_id", to_value(value.action_id));
    fields.emplace_back("generation", to_value(value.generation));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<StatusActionInvocation>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto status_id = require_field<StatusId>(value.field("status_id"));
    auto action_id = require_field<std::string>(value.field("action_id"));
    auto generation = require_field<std::uint64_t>(value.field("generation"));
    if (!status_id || !action_id || !generation) return false;
    out.emplace(StatusActionInvocation{*status_id, *action_id, *generation});
    return true;
}

ProtocolValue to_value(PromptStatusViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("prompt", to_value(value.prompt));
    fields.emplace_back("status", to_value(value.status));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<PromptStatusViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto status = require_field<StatusViewState>(value.field("status"));
    if (!status) return false;
    std::optional<PromptViewState> prompt;
    if (!decode_optional_field(value.field("prompt"), prompt)) return false;
    out.emplace(PromptStatusViewState{std::move(prompt), *status});
    return true;
}

ProtocolValue to_value(PromptStatusDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", to_value(value.changed));
    fields.emplace_back("replacement", to_value(value.replacement));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<PromptStatusDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto changed = require_field<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<PromptStatusViewState> replacement;
    if (!decode_optional_field(value.field("replacement"), replacement)) return false;
    out.emplace(PromptStatusDelta{*changed, std::move(replacement)});
    return true;
}

// ---------------------------------------------------------------------------
// clipboard.h composite definitions.

ProtocolValue to_value(ClipboardRequest const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("kind", to_value(value.kind));
    fields.emplace_back("request_revision", to_value(value.request_revision));
    fields.emplace_back("text", to_value(value.text));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ClipboardRequest>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto id = require_field<std::uint64_t>(value.field("id"));
    auto kind = require_field<ClipboardRequestKind>(value.field("kind"));
    auto request_revision = require_field<Revision>(value.field("request_revision"));
    auto text = require_field<std::string>(value.field("text"));
    if (!id || !kind || !request_revision || !text) return false;
    out.emplace(ClipboardRequest{*id, *kind, *request_revision, *text});
    return true;
}

ProtocolValue to_value(ClipboardResponse const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("request_revision", to_value(value.request_revision));
    fields.emplace_back("observed_document_revision",
                        to_value(value.observed_document_revision));
    fields.emplace_back("status", to_value(value.status));
    fields.emplace_back("text", to_value(value.text));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ClipboardResponse>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto id = require_field<std::uint64_t>(value.field("id"));
    auto request_revision = require_field<Revision>(value.field("request_revision"));
    auto observed_document_revision =
        require_field<Revision>(value.field("observed_document_revision"));
    auto status = require_field<ClipboardResponseStatus>(value.field("status"));
    auto text = require_field<std::string>(value.field("text"));
    if (!id || !request_revision || !observed_document_revision || !status || !text) {
        return false;
    }
    out.emplace(ClipboardResponse{*id, *request_revision,
                                  *observed_document_revision, *status, *text});
    return true;
}

ProtocolValue to_value(ClipboardViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("fragments", to_value(value.fragments));
    fields.emplace_back("plain_text", to_value(value.plain_text));
    fields.emplace_back("pending_read", to_value(value.pending_read));
    fields.emplace_back("pending_write", to_value(value.pending_write));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ClipboardViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto fragments = require_field<std::vector<std::string>>(value.field("fragments"));
    auto plain_text = require_field<std::string>(value.field("plain_text"));
    if (!fragments || !plain_text) return false;
    ClipboardViewState result;
    result.fragments = *fragments;
    result.plain_text = *plain_text;
    if (!decode_optional_field(value.field("pending_read"), result.pending_read)) {
        return false;
    }
    if (!decode_optional_field(value.field("pending_write"), result.pending_write)) {
        return false;
    }
    out.emplace(std::move(result));
    return true;
}

ProtocolValue to_value(ClipboardDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", to_value(value.changed));
    fields.emplace_back("replacement", to_value(value.replacement));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ClipboardDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto changed = require_field<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<ClipboardViewState> replacement;
    if (!decode_optional_field(value.field("replacement"), replacement)) return false;
    out.emplace(ClipboardDelta{*changed, std::move(replacement)});
    return true;
}

// ---------------------------------------------------------------------------
// search.h composite definitions.

ProtocolValue to_value(SearchResult const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("mode", to_value(value.mode));
    fields.emplace_back("path", to_value(value.path));
    fields.emplace_back("label", to_value(value.label));
    fields.emplace_back("line", to_value(value.line));
    fields.emplace_back("column", to_value(static_cast<std::uint64_t>(value.column)));
    fields.emplace_back("score", to_value(value.score));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SearchResult>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto mode = require_field<SearchMode>(value.field("mode"));
    auto path = require_field<std::string>(value.field("path"));
    auto label = require_field<std::string>(value.field("label"));
    auto column = require_field<std::uint64_t>(value.field("column"));
    auto score = require_field<int>(value.field("score"));
    if (!mode || !path || !label || !column || !score) return false;
    SearchResult result;
    result.mode = *mode;
    result.path = *path;
    result.label = *label;
    if (!decode_optional_field(value.field("line"), result.line)) return false;
    result.column = static_cast<std::size_t>(*column);
    result.score = *score;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue to_value(SearchViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", to_value(value.revision));
    fields.emplace_back("palette_open", to_value(value.palette_open));
    fields.emplace_back("query", to_value(value.query));
    fields.emplace_back("mode", to_value(value.mode));
    fields.emplace_back("results", to_value(value.results));
    if (value.selected_index) {
        fields.emplace_back("selected_index",
                            to_value(static_cast<std::uint64_t>(*value.selected_index)));
    } else {
        fields.emplace_back("selected_index", ProtocolValue::make_null());
    }
    fields.emplace_back("search_generation", to_value(value.search_generation));
    fields.emplace_back("searching", to_value(value.searching));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SearchViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto revision = require_field<Revision>(value.field("revision"));
    auto palette_open = require_field<bool>(value.field("palette_open"));
    auto query = require_field<std::string>(value.field("query"));
    auto mode = require_field<SearchMode>(value.field("mode"));
    auto results = require_field<std::vector<SearchResult>>(value.field("results"));
    auto search_generation = require_field<std::uint64_t>(value.field("search_generation"));
    auto searching = require_field<bool>(value.field("searching"));
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
    if (!decode_optional_field(value.field("selected_index"), selected_index)) {
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

ProtocolValue to_value(SearchDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", to_value(value.base_revision));
    fields.emplace_back("revision", to_value(value.revision));
    fields.emplace_back("state", to_value(value.state));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SearchDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto base_revision = require_field<Revision>(value.field("base_revision"));
    auto revision = require_field<Revision>(value.field("revision"));
    if (!base_revision || !revision) return false;
    SearchDelta result;
    result.base_revision = *base_revision;
    result.revision = *revision;
    if (!decode_optional_field(value.field("state"), result.state)) return false;
    out.emplace(std::move(result));
    return true;
}

// ---------------------------------------------------------------------------
// find_replace.h composite definitions.

ProtocolValue to_value(ByteRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("begin", to_value(value.begin));
    fields.emplace_back("end", to_value(value.end));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ByteRange>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto begin = require_field<ByteOffset>(value.field("begin"));
    auto end = require_field<ByteOffset>(value.field("end"));
    if (!begin || !end) return false;
    out.emplace(ByteRange{*begin, *end});
    return true;
}

ProtocolValue to_value(FindOptions const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("case_sensitive", to_value(value.case_sensitive));
    fields.emplace_back("whole_word", to_value(value.whole_word));
    fields.emplace_back("regex", to_value(value.regex));
    fields.emplace_back("selection_only", to_value(value.selection_only));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FindOptions>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto case_sensitive = require_field<bool>(value.field("case_sensitive"));
    auto whole_word = require_field<bool>(value.field("whole_word"));
    auto regex = require_field<bool>(value.field("regex"));
    auto selection_only = require_field<bool>(value.field("selection_only"));
    if (!case_sensitive || !whole_word || !regex || !selection_only) return false;
    out.emplace(FindOptions{*case_sensitive, *whole_word, *regex, *selection_only});
    return true;
}

ProtocolValue to_value(FindRequest const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("query", to_value(value.query));
    fields.emplace_back("options", to_value(value.options));
    fields.emplace_back("selection", to_value(value.selection));
    fields.emplace_back("work_budget", to_value(value.work_budget));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FindRequest>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto query = require_field<std::string>(value.field("query"));
    auto options = require_field<FindOptions>(value.field("options"));
    std::optional<ByteRange> selection;
    auto work_budget = require_field<std::uint64_t>(value.field("work_budget"));
    if (!query || !options ||
        !decode_optional_field(value.field("selection"), selection) ||
        !work_budget) {
        return false;
    }
    out.emplace(FindRequest{*query, *options, std::move(selection),
                            *work_budget, nullptr});
    return true;
}

ProtocolValue to_value(FindMatch const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("begin", to_value(value.begin));
    fields.emplace_back("end", to_value(value.end));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FindMatch>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto begin = require_field<ByteOffset>(value.field("begin"));
    auto end = require_field<ByteOffset>(value.field("end"));
    if (!begin || !end) return false;
    out.emplace(FindMatch{*begin, *end});
    return true;
}

ProtocolValue to_value(WorkspaceFileReplacement const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("path", to_value(value.path));
    fields.emplace_back("before", to_value(value.before));
    fields.emplace_back("after", to_value(value.after));
    fields.emplace_back("matches", to_value(value.matches));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<WorkspaceFileReplacement>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto path = require_field<std::string>(value.field("path"));
    auto before = require_field<std::string>(value.field("before"));
    auto after = require_field<std::string>(value.field("after"));
    auto matches = require_field<std::vector<FindMatch>>(value.field("matches"));
    if (!path || !before || !after || !matches) return false;
    out.emplace(WorkspaceFileReplacement{*path, *before, *after, *matches});
    return true;
}

ProtocolValue to_value(WorkspaceReplacePreview const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("source_revision", to_value(value.source_revision));
    fields.emplace_back("query", to_value(value.query));
    fields.emplace_back("replacement", to_value(value.replacement));
    fields.emplace_back("options", to_value(value.options));
    fields.emplace_back("changes", to_value(value.changes));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<WorkspaceReplacePreview>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto source_revision = require_field<Revision>(value.field("source_revision"));
    auto query = require_field<std::string>(value.field("query"));
    auto replacement = require_field<std::string>(value.field("replacement"));
    auto options = require_field<FindOptions>(value.field("options"));
    auto changes = require_field<std::vector<WorkspaceFileReplacement>>(value.field("changes"));
    if (!source_revision || !query || !replacement || !options || !changes) return false;
    out.emplace(WorkspaceReplacePreview{*source_revision, *query,
                                        *replacement, *options, *changes});
    return true;
}

ProtocolValue to_value(FindReplaceViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("generation", to_value(value.generation));
    fields.emplace_back("open", to_value(value.open));
    fields.emplace_back("replace_mode", to_value(value.replace_mode));
    fields.emplace_back("source_revision", to_value(value.source_revision));
    fields.emplace_back("query", to_value(value.query));
    fields.emplace_back("replacement", to_value(value.replacement));
    fields.emplace_back("options", to_value(value.options));
    fields.emplace_back("matches", to_value(value.matches));
    if (value.active_match) {
        fields.emplace_back("active_match",
                            to_value(static_cast<std::uint64_t>(*value.active_match)));
    } else {
        fields.emplace_back("active_match", ProtocolValue::make_null());
    }
    fields.emplace_back("error", to_value(value.error));
    fields.emplace_back("message", to_value(value.message));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FindReplaceViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto generation = require_field<std::uint64_t>(value.field("generation"));
    auto open = require_field<bool>(value.field("open"));
    auto replace_mode = require_field<bool>(value.field("replace_mode"));
    auto source_revision = require_field<Revision>(value.field("source_revision"));
    auto query = require_field<std::string>(value.field("query"));
    auto replacement = require_field<std::string>(value.field("replacement"));
    auto options = require_field<FindOptions>(value.field("options"));
    auto matches = require_field<std::vector<FindMatch>>(value.field("matches"));
    auto error = require_field<FindReplaceError>(value.field("error"));
    auto message = require_field<std::string>(value.field("message"));
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
    if (!decode_optional_field(value.field("active_match"), active_match)) {
        return false;
    }
    if (active_match) result.active_match = static_cast<std::size_t>(*active_match);
    result.error = *error;
    result.message = *message;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue to_value(FindReplaceDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", to_value(value.changed));
    fields.emplace_back("base_generation", to_value(value.base_generation));
    fields.emplace_back("replacement", to_value(value.replacement));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FindReplaceDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto changed = require_field<bool>(value.field("changed"));
    auto base_generation = require_field<std::uint64_t>(value.field("base_generation"));
    if (!changed || !base_generation) return false;
    FindReplaceDelta result;
    result.changed = *changed;
    result.base_generation = *base_generation;
    if (!decode_optional_field(value.field("replacement"), result.replacement)) {
        return false;
    }
    out.emplace(std::move(result));
    return true;
}

// ---------------------------------------------------------------------------
// settings.h composite definitions, including the SettingValue tagged-union
// wire encoding (variant index selects the active alternative's decoder).

ProtocolValue to_value(SettingValue const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("index", to_value(static_cast<std::uint64_t>(value.index())));
    std::visit(
        [&fields](auto const& alt) { fields.emplace_back("value", to_value(alt)); },
        value);
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SettingValue>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto index = require_field<std::uint64_t>(value.field("index"));
    if (!index) return false;
    auto const* alt_value = value.field("value");
    if (!alt_value) return false;
    switch (*index) {
        case 0: {
            auto decoded = require_field<bool>(alt_value);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<0>, *decoded});
            return true;
        }
        case 1: {
            auto decoded = require_field<std::uint32_t>(alt_value);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<1>, *decoded});
            return true;
        }
        case 2: {
            auto decoded = require_field<std::uint64_t>(alt_value);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<2>, *decoded});
            return true;
        }
        case 3: {
            auto decoded = require_field<IndentStyle>(alt_value);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<3>, *decoded});
            return true;
        }
        case 4: {
            auto decoded = require_field<LineEnding>(alt_value);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<4>, *decoded});
            return true;
        }
        case 5: {
            auto decoded = require_field<TextEncoding>(alt_value);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<5>, *decoded});
            return true;
        }
        case 6: {
            auto decoded = require_field<std::string>(alt_value);
            if (!decoded) return false;
            out.emplace(SettingValue{std::in_place_index<6>, *decoded});
            return true;
        }
        default:
            return false;
    }
}

ProtocolValue to_value(EffectiveSetting const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("value", to_value(value.value));
    fields.emplace_back("source", to_value(value.source));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<EffectiveSetting>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto setting_value = require_field<SettingValue>(value.field("value"));
    auto source = require_field<SettingScope>(value.field("source"));
    if (!setting_value || !source) return false;
    out.emplace(EffectiveSetting{*setting_value, *source});
    return true;
}

ProtocolValue to_value(SettingViewEntry const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("key", to_value(value.key));
    fields.emplace_back("effective", to_value(value.effective));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SettingViewEntry>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto key = require_field<SettingKey>(value.field("key"));
    auto effective = require_field<EffectiveSetting>(value.field("effective"));
    if (!key || !effective) return false;
    out.emplace(SettingViewEntry{*key, *effective});
    return true;
}

ProtocolValue to_value(SettingsViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("entries", to_value(value.entries));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SettingsViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto entries =
        require_field<std::array<SettingViewEntry, setting_key_count>>(
            value.field("entries"));
    if (!entries) return false;
    SettingsViewState result;
    result.entries = *entries;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue to_value(SettingsDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("key", to_value(value.key));
    fields.emplace_back("before", to_value(value.before));
    fields.emplace_back("after", to_value(value.after));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SettingsDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto key = require_field<SettingKey>(value.field("key"));
    auto before = require_field<EffectiveSetting>(value.field("before"));
    auto after = require_field<EffectiveSetting>(value.field("after"));
    if (!key || !before || !after) return false;
    out.emplace(SettingsDelta{*key, *before, *after});
    return true;
}

ProtocolValue to_value(SettingsSectionDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changes", to_value(value.changes));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SettingsSectionDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto changes = require_field<std::vector<SettingsDelta>>(value.field("changes"));
    if (!changes) return false;
    out.emplace(SettingsSectionDelta{*changes});
    return true;
}

// ---------------------------------------------------------------------------
// input.h (keymap) composite definitions.

ProtocolValue to_value(KeyStroke const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("code", to_value(value.code));
    fields.emplace_back("control", to_value(value.control));
    fields.emplace_back("alt", to_value(value.alt));
    fields.emplace_back("meta", to_value(value.meta));
    fields.emplace_back("shift", to_value(value.shift));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<KeyStroke>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto code = require_field<std::string>(value.field("code"));
    auto control = require_field<bool>(value.field("control"));
    auto alt = require_field<bool>(value.field("alt"));
    auto meta = require_field<bool>(value.field("meta"));
    auto shift = require_field<bool>(value.field("shift"));
    if (!code || !control || !alt || !meta || !shift) return false;
    out.emplace(KeyStroke{*code, *control, *alt, *meta, *shift});
    return true;
}

ProtocolValue to_value(KeyBinding const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("sequence", to_value(value.sequence));
    fields.emplace_back("command_id", to_value(value.command_id));
    fields.emplace_back("context", to_value(value.context));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<KeyBinding>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto sequence = require_field<std::vector<KeyStroke>>(value.field("sequence"));
    auto command_id = require_field<std::string>(value.field("command_id"));
    auto context = require_field<std::string>(value.field("context"));
    if (!sequence || !command_id || !context) return false;
    out.emplace(KeyBinding{*sequence, *command_id, *context});
    return true;
}

ProtocolValue to_value(KeymapViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("name", to_value(value.name));
    fields.emplace_back("bindings", to_value(value.bindings));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<KeymapViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto name = require_field<std::string>(value.field("name"));
    auto bindings = require_field<std::vector<KeyBinding>>(value.field("bindings"));
    if (!name || !bindings) return false;
    out.emplace(KeymapViewState{*name, *bindings});
    return true;
}

ProtocolValue to_value(KeymapDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", to_value(value.changed));
    fields.emplace_back("replacement", to_value(value.replacement));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<KeymapDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto changed = require_field<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<KeymapViewState> replacement;
    if (!decode_optional_field(value.field("replacement"), replacement)) return false;
    out.emplace(KeymapDelta{*changed, std::move(replacement)});
    return true;
}

// ---------------------------------------------------------------------------
// text_encoding.h composite definitions.

ProtocolValue to_value(TextEncodingStatus const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("encoding", to_value(value.encoding));
    fields.emplace_back("line_ending", to_value(value.line_ending));
    fields.emplace_back("had_bom", to_value(value.had_bom));
    fields.emplace_back("final_newline", to_value(value.final_newline));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TextEncodingStatus>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto encoding = require_field<TextEncoding>(value.field("encoding"));
    auto line_ending = require_field<LineEnding>(value.field("line_ending"));
    auto had_bom = require_field<bool>(value.field("had_bom"));
    auto final_newline = require_field<bool>(value.field("final_newline"));
    if (!encoding || !line_ending || !had_bom || !final_newline) return false;
    out.emplace(TextEncodingStatus{*encoding, *line_ending, *had_bom, *final_newline});
    return true;
}

ProtocolValue to_value(TextEncodingViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("status", to_value(value.status));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TextEncodingViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto status = require_field<TextEncodingStatus>(value.field("status"));
    if (!status) return false;
    out.emplace(TextEncodingViewState{*status});
    return true;
}

ProtocolValue to_value(TextEncodingDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("before", to_value(value.before));
    fields.emplace_back("after", to_value(value.after));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TextEncodingDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto before = require_field<TextEncodingViewState>(value.field("before"));
    auto after = require_field<TextEncodingViewState>(value.field("after"));
    if (!before || !after) return false;
    out.emplace(TextEncodingDelta{*before, *after});
    return true;
}

// ---------------------------------------------------------------------------
// tabs.h composite definitions.

ProtocolValue to_value(TabState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("kind", to_value(value.kind));
    fields.emplace_back("document", to_value(value.document));
    fields.emplace_back("document_key", to_value(value.document_key));
    fields.emplace_back("content_identity", to_value(value.content_identity));
    fields.emplace_back("label", to_value(value.label));
    fields.emplace_back("mode", to_value(value.mode));
    fields.emplace_back("dirty", to_value(value.dirty));
    fields.emplace_back("recovery", to_value(value.recovery));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TabState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto id = require_field<TabId>(value.field("id"));
    auto kind = require_field<TabKind>(value.field("kind"));
    auto content_identity = require_field<std::string>(value.field("content_identity"));
    auto label = require_field<std::string>(value.field("label"));
    auto mode = require_field<DocumentMode>(value.field("mode"));
    auto dirty = require_field<bool>(value.field("dirty"));
    auto recovery = require_field<TabRecoveryBadge>(value.field("recovery"));
    if (!id || !kind || !content_identity || !label || !mode || !dirty || !recovery) {
        return false;
    }
    TabState result;
    result.id = *id;
    result.kind = *kind;
    if (!decode_optional_field(value.field("document"), result.document)) return false;
    if (!decode_optional_field(value.field("document_key"), result.document_key)) {
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

ProtocolValue to_value(TabViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("tabs", to_value(value.tabs));
    fields.emplace_back("active", to_value(value.active));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TabViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto tabs = require_field<std::vector<TabState>>(value.field("tabs"));
    if (!tabs) return false;
    TabViewState result;
    result.tabs = *tabs;
    if (!decode_optional_field(value.field("active"), result.active)) return false;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue to_value(TabDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("state", to_value(value.state));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TabDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    TabDelta result;
    if (!decode_optional_field(value.field("state"), result.state)) return false;
    out.emplace(std::move(result));
    return true;
}

// ---------------------------------------------------------------------------
// diff.h composite definitions.

ProtocolValue to_value(DiffLineChange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("kind", to_value(value.kind));
    if (value.baseline_line) {
        fields.emplace_back("baseline_line",
                            to_value(static_cast<std::uint64_t>(*value.baseline_line)));
    } else {
        fields.emplace_back("baseline_line", ProtocolValue::make_null());
    }
    if (value.target_line) {
        fields.emplace_back("target_line",
                            to_value(static_cast<std::uint64_t>(*value.target_line)));
    } else {
        fields.emplace_back("target_line", ProtocolValue::make_null());
    }
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DiffLineChange>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto kind = require_field<DiffLineKind>(value.field("kind"));
    if (!kind) return false;
    DiffLineChange result;
    result.kind = *kind;
    std::optional<std::uint64_t> baseline_line;
    if (!decode_optional_field(value.field("baseline_line"), baseline_line)) return false;
    if (baseline_line) result.baseline_line = static_cast<std::size_t>(*baseline_line);
    std::optional<std::uint64_t> target_line;
    if (!decode_optional_field(value.field("target_line"), target_line)) return false;
    if (target_line) result.target_line = static_cast<std::size_t>(*target_line);
    out.emplace(result);
    return true;
}

ProtocolValue to_value(DiffHunk const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("baseline_start",
                        to_value(static_cast<std::uint64_t>(value.baseline_start)));
    fields.emplace_back("target_start",
                        to_value(static_cast<std::uint64_t>(value.target_start)));
    fields.emplace_back("baseline_lines", to_value(value.baseline_lines));
    fields.emplace_back("target_lines", to_value(value.target_lines));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DiffHunk>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto baseline_start = require_field<std::uint64_t>(value.field("baseline_start"));
    auto target_start = require_field<std::uint64_t>(value.field("target_start"));
    auto baseline_lines = require_field<std::vector<std::string>>(value.field("baseline_lines"));
    auto target_lines = require_field<std::vector<std::string>>(value.field("target_lines"));
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

ProtocolValue to_value(DiffFileView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("path", to_value(value.path));
    fields.emplace_back("previous_path", to_value(value.previous_path));
    fields.emplace_back("deleted", to_value(value.deleted));
    fields.emplace_back("baseline_identity", to_value(value.baseline_identity));
    fields.emplace_back("current_content", to_value(value.current_content));
    fields.emplace_back("hunks", to_value(value.hunks));
    fields.emplace_back("changed_lines", to_value(value.changed_lines));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DiffFileView>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto id = require_field<DiffFileId>(value.field("id"));
    auto path = require_field<std::filesystem::path>(value.field("path"));
    auto deleted = require_field<bool>(value.field("deleted"));
    auto baseline_identity = require_field<std::string>(value.field("baseline_identity"));
    auto current_content = require_field<std::string>(value.field("current_content"));
    auto hunks = require_field<std::vector<DiffHunk>>(value.field("hunks"));
    auto changed_lines = require_field<std::vector<DiffLineChange>>(value.field("changed_lines"));
    if (!id || !path || !deleted || !baseline_identity || !current_content || !hunks ||
        !changed_lines) {
        return false;
    }
    std::optional<std::filesystem::path> previous_path;
    if (!decode_optional_field(value.field("previous_path"), previous_path)) {
        return false;
    }
    out.emplace(DiffFileView{*id, *path, std::move(previous_path), *deleted,
                            *baseline_identity, *current_content, *hunks,
                            *changed_lines});
    return true;
}

ProtocolValue to_value(DiffViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", to_value(value.revision));
    fields.emplace_back("files", to_value(value.files));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DiffViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto revision = require_field<Revision>(value.field("revision"));
    auto files = require_field<std::vector<DiffFileView>>(value.field("files"));
    if (!revision || !files) return false;
    out.emplace(DiffViewState{*revision, *files});
    return true;
}

ProtocolValue to_value(DiffDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", to_value(value.base_revision));
    fields.emplace_back("revision", to_value(value.revision));
    fields.emplace_back("upserted", to_value(value.upserted));
    fields.emplace_back("removed", to_value(value.removed));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DiffDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto base_revision = require_field<Revision>(value.field("base_revision"));
    auto revision = require_field<Revision>(value.field("revision"));
    auto upserted = require_field<std::vector<DiffFileView>>(value.field("upserted"));
    auto removed = require_field<std::vector<DiffFileId>>(value.field("removed"));
    if (!base_revision || !revision || !upserted || !removed) return false;
    out.emplace(DiffDelta{*base_revision, *revision, *upserted, *removed});
    return true;
}

// ---------------------------------------------------------------------------
// external_modification.h composite definitions.

ProtocolValue to_value(ExternalDocumentView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("path", to_value(value.path));
    fields.emplace_back("status", to_value(value.status));
    fields.emplace_back("accessible_status", to_value(value.accessible_status));
    fields.emplace_back("actions", to_value(value.actions));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ExternalDocumentView>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto id = require_field<DiffFileId>(value.field("id"));
    auto path = require_field<std::filesystem::path>(value.field("path"));
    auto status = require_field<ExternalDocumentStatus>(value.field("status"));
    auto accessible_status = require_field<std::string>(value.field("accessible_status"));
    auto actions = require_field<std::vector<ExternalAction>>(value.field("actions"));
    if (!id || !path || !status || !accessible_status || !actions) return false;
    out.emplace(ExternalDocumentView{*id, *path, *status, *accessible_status, *actions});
    return true;
}

ProtocolValue to_value(ExternalModificationViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", to_value(value.revision));
    fields.emplace_back("files", to_value(value.files));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ExternalModificationViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto revision = require_field<Revision>(value.field("revision"));
    auto files = require_field<std::vector<ExternalDocumentView>>(value.field("files"));
    if (!revision || !files) return false;
    out.emplace(ExternalModificationViewState{*revision, *files});
    return true;
}

ProtocolValue to_value(ExternalModificationDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", to_value(value.base_revision));
    fields.emplace_back("revision", to_value(value.revision));
    fields.emplace_back("upserted", to_value(value.upserted));
    fields.emplace_back("removed", to_value(value.removed));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ExternalModificationDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto base_revision = require_field<Revision>(value.field("base_revision"));
    auto revision = require_field<Revision>(value.field("revision"));
    auto upserted = require_field<std::vector<ExternalDocumentView>>(value.field("upserted"));
    auto removed = require_field<std::vector<DiffFileId>>(value.field("removed"));
    if (!base_revision || !revision || !upserted || !removed) return false;
    out.emplace(ExternalModificationDelta{*base_revision, *revision, *upserted, *removed});
    return true;
}

// ---------------------------------------------------------------------------
// viewport.h composite definitions.

ProtocolValue to_value(ViewportDimensions const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("columns", to_value(value.columns));
    fields.emplace_back("rows", to_value(value.rows));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ViewportDimensions>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto columns = require_field<std::uint32_t>(value.field("columns"));
    auto rows = require_field<std::uint32_t>(value.field("rows"));
    if (!columns || !rows) return false;
    out.emplace(*columns, *rows);
    return true;
}

ProtocolValue to_value(VisualRow const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("logical_line", to_value(value.logical_line));
    fields.emplace_back("first_span", to_value(value.first_span));
    fields.emplace_back("span_count", to_value(value.span_count));
    fields.emplace_back("start_cell", to_value(value.start_cell));
    fields.emplace_back("content_cells", to_value(value.content_cells));
    fields.emplace_back("visible_cells", to_value(value.visible_cells));
    fields.emplace_back("end_byte_offset", to_value(value.end_byte_offset));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<VisualRow>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto logical_line = require_field<std::uint32_t>(value.field("logical_line"));
    auto first_span = require_field<std::uint32_t>(value.field("first_span"));
    auto span_count = require_field<std::uint32_t>(value.field("span_count"));
    auto start_cell = require_field<CellIndex>(value.field("start_cell"));
    auto content_cells = require_field<std::uint32_t>(value.field("content_cells"));
    auto visible_cells = require_field<std::uint32_t>(value.field("visible_cells"));
    auto end_byte_offset = require_field<std::uint32_t>(value.field("end_byte_offset"));
    if (!logical_line || !first_span || !span_count || !start_cell || !content_cells ||
        !visible_cells || !end_byte_offset) {
        return false;
    }
    out.emplace(VisualRow{*logical_line, *first_span, *span_count, *start_cell,
                          *content_cells, *visible_cells, *end_byte_offset});
    return true;
}

ProtocolValue to_value(CellHitTarget const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("viewport_row", to_value(value.viewport_row));
    fields.emplace_back("viewport_column", to_value(value.viewport_column));
    fields.emplace_back("logical_line", to_value(value.logical_line));
    fields.emplace_back("cell", to_value(value.cell));
    fields.emplace_back("byte_offset", to_value(value.byte_offset));
    fields.emplace_back("byte_len", to_value(value.byte_len));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<CellHitTarget>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto viewport_row = require_field<std::uint32_t>(value.field("viewport_row"));
    auto viewport_column = require_field<std::uint32_t>(value.field("viewport_column"));
    auto logical_line = require_field<std::uint32_t>(value.field("logical_line"));
    auto cell = require_field<CellIndex>(value.field("cell"));
    auto byte_offset = require_field<std::uint32_t>(value.field("byte_offset"));
    auto byte_len = require_field<std::uint32_t>(value.field("byte_len"));
    if (!viewport_row || !viewport_column || !logical_line || !cell || !byte_offset ||
        !byte_len) {
        return false;
    }
    out.emplace(CellHitTarget{*viewport_row, *viewport_column, *logical_line, *cell,
                              *byte_offset, *byte_len});
    return true;
}

ProtocolValue to_value(ScrollbarMetrics const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("total_rows", to_value(value.total_rows));
    fields.emplace_back("viewport_rows", to_value(value.viewport_rows));
    fields.emplace_back("first_row", to_value(value.first_row));
    fields.emplace_back("maximum_first_row", to_value(value.maximum_first_row));
    fields.emplace_back("thumb_start", to_value(value.thumb_start));
    fields.emplace_back("thumb_size", to_value(value.thumb_size));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ScrollbarMetrics>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto total_rows = require_field<std::uint32_t>(value.field("total_rows"));
    auto viewport_rows = require_field<std::uint32_t>(value.field("viewport_rows"));
    auto first_row = require_field<std::uint32_t>(value.field("first_row"));
    auto maximum_first_row = require_field<std::uint32_t>(value.field("maximum_first_row"));
    auto thumb_start = require_field<std::uint32_t>(value.field("thumb_start"));
    auto thumb_size = require_field<std::uint32_t>(value.field("thumb_size"));
    if (!total_rows || !viewport_rows || !first_row || !maximum_first_row || !thumb_start ||
        !thumb_size) {
        return false;
    }
    out.emplace(ScrollbarMetrics{*total_rows, *viewport_rows, *first_row,
                                 *maximum_first_row, *thumb_start, *thumb_size});
    return true;
}

ProtocolValue to_value(ViewportViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("dimensions", to_value(value.dimensions));
    fields.emplace_back("first_visual_row", to_value(value.first_visual_row));
    fields.emplace_back("first_visual_column", to_value(value.first_visual_column));
    fields.emplace_back("total_visual_rows", to_value(value.total_visual_rows));
    fields.emplace_back("visible_rows", to_value(value.visible_rows));
    fields.emplace_back("hit_targets", to_value(value.hit_targets));
    fields.emplace_back("scrollbar", to_value(value.scrollbar));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ViewportViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto dimensions = require_field<ViewportDimensions>(value.field("dimensions"));
    auto first_visual_row = require_field<std::uint32_t>(value.field("first_visual_row"));
    auto first_visual_column = require_field<std::uint32_t>(value.field("first_visual_column"));
    auto total_visual_rows = require_field<std::uint32_t>(value.field("total_visual_rows"));
    auto visible_rows = require_field<std::vector<VisualRow>>(value.field("visible_rows"));
    auto hit_targets = require_field<std::vector<CellHitTarget>>(value.field("hit_targets"));
    auto scrollbar = require_field<ScrollbarMetrics>(value.field("scrollbar"));
    if (!dimensions || !first_visual_row || !first_visual_column ||
        !total_visual_rows || !visible_rows || !hit_targets || !scrollbar) {
        return false;
    }
    out.emplace(ViewportViewState{*dimensions, *first_visual_row,
                                  *first_visual_column, *total_visual_rows,
                                  *visible_rows, *hit_targets, *scrollbar});
    return true;
}

ProtocolValue to_value(ViewportDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("changed", to_value(value.changed));
    fields.emplace_back("replacement", to_value(value.replacement));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ViewportDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto changed = require_field<bool>(value.field("changed"));
    if (!changed) return false;
    std::optional<ViewportViewState> replacement;
    if (!decode_optional_field(value.field("replacement"), replacement)) return false;
    out.emplace(ViewportDelta{*changed, std::move(replacement)});
    return true;
}

// ---------------------------------------------------------------------------
// follow_edits.h composite definitions.

ProtocolValue to_value(FollowScrollOffset const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("first_row", to_value(value.first_row));
    fields.emplace_back("first_column", to_value(value.first_column));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FollowScrollOffset>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto first_row = require_field<std::uint64_t>(value.field("first_row"));
    auto first_column = require_field<std::uint64_t>(value.field("first_column"));
    if (!first_row || !first_column) return false;
    out.emplace(FollowScrollOffset{*first_row, *first_column});
    return true;
}

ProtocolValue to_value(FollowTarget const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("path", to_value(value.path));
    fields.emplace_back("deleted", to_value(value.deleted));
    fields.emplace_back("newest_hunk_line",
                        to_value(static_cast<std::uint64_t>(value.newest_hunk_line)));
    fields.emplace_back("source_revision", to_value(value.source_revision));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FollowTarget>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto id = require_field<DiffFileId>(value.field("id"));
    auto path = require_field<std::filesystem::path>(value.field("path"));
    auto deleted = require_field<bool>(value.field("deleted"));
    auto newest_hunk_line = require_field<std::uint64_t>(value.field("newest_hunk_line"));
    auto source_revision = require_field<Revision>(value.field("source_revision"));
    if (!id || !path || !deleted || !newest_hunk_line || !source_revision) return false;
    out.emplace(FollowTarget{*id, *path, *deleted,
                             static_cast<std::size_t>(*newest_hunk_line),
                             *source_revision});
    return true;
}

ProtocolValue to_value(FollowClientView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("client", to_value(value.client));
    fields.emplace_back("dimensions", to_value(value.dimensions));
    fields.emplace_back("offset", to_value(value.offset));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FollowClientView>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto client = require_field<ClientId>(value.field("client"));
    auto dimensions = require_field<ViewportDimensions>(value.field("dimensions"));
    auto offset = require_field<FollowScrollOffset>(value.field("offset"));
    if (!client || !dimensions || !offset) return false;
    out.emplace(FollowClientView{*client, *dimensions, *offset});
    return true;
}

ProtocolValue to_value(FollowEditsViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("generation", to_value(value.generation));
    fields.emplace_back("mode", to_value(value.mode));
    fields.emplace_back("active_pane", to_value(value.active_pane));
    fields.emplace_back("active_target", to_value(value.active_target));
    fields.emplace_back("queued_targets", to_value(value.queued_targets));
    fields.emplace_back("clients", to_value(value.clients));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FollowEditsViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto generation = require_field<std::uint64_t>(value.field("generation"));
    auto mode = require_field<FollowMode>(value.field("mode"));
    auto active_pane = require_field<PaneId>(value.field("active_pane"));
    auto queued_targets = require_field<std::vector<FollowTarget>>(value.field("queued_targets"));
    auto clients = require_field<std::vector<FollowClientView>>(value.field("clients"));
    if (!generation || !mode || !active_pane || !queued_targets || !clients) return false;
    FollowEditsViewState result;
    result.generation = *generation;
    result.mode = *mode;
    result.active_pane = *active_pane;
    if (!decode_optional_field(value.field("active_target"), result.active_target)) {
        return false;
    }
    result.queued_targets = *queued_targets;
    result.clients = *clients;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue to_value(FollowEditsDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_generation", to_value(value.base_generation));
    fields.emplace_back("generation", to_value(value.generation));
    fields.emplace_back("replacement", to_value(value.replacement));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FollowEditsDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto base_generation = require_field<std::uint64_t>(value.field("base_generation"));
    auto generation = require_field<std::uint64_t>(value.field("generation"));
    if (!base_generation || !generation) return false;
    FollowEditsDelta result;
    result.base_generation = *base_generation;
    result.generation = *generation;
    if (!decode_optional_field(value.field("replacement"), result.replacement)) {
        return false;
    }
    out.emplace(std::move(result));
    return true;
}

// ---------------------------------------------------------------------------
// tree.h composite definitions.

ProtocolValue to_value(TreeNodeCommand const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("label", to_value(value.label));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TreeNodeCommand>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto id = require_field<std::string>(value.field("id"));
    auto label = require_field<std::string>(value.field("label"));
    if (!id || !label) return false;
    out.emplace(TreeNodeCommand{*id, *label});
    return true;
}

ProtocolValue to_value(TreeNode const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", to_value(value.id));
    fields.emplace_back("parent_id", to_value(value.parent_id));
    fields.emplace_back("label", to_value(value.label));
    fields.emplace_back("kind", to_value(value.kind));
    fields.emplace_back("icon", to_value(value.icon));
    fields.emplace_back("commands", to_value(value.commands));
    fields.emplace_back("git_status", to_value(value.git_status));
    fields.emplace_back("workspace_path", to_value(value.workspace_path));
    if (value.source_line) {
        fields.emplace_back("source_line", to_value(*value.source_line));
    } else {
        fields.emplace_back("source_line", ProtocolValue::make_null());
    }
    fields.emplace_back("expandable", to_value(value.expandable));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TreeNode>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto id = require_field<TreeNodeId>(value.field("id"));
    auto label = require_field<std::string>(value.field("label"));
    auto kind = require_field<TreeNodeKind>(value.field("kind"));
    auto commands = require_field<std::vector<TreeNodeCommand>>(value.field("commands"));
    auto expandable = require_field<bool>(value.field("expandable"));
    if (!id || !label || !kind || !commands || !expandable) return false;
    std::optional<TreeNodeId> parent_id;
    if (!decode_optional_field(value.field("parent_id"), parent_id)) return false;
    std::optional<std::string> icon;
    if (!decode_optional_field(value.field("icon"), icon)) return false;
    std::optional<GitTreeStatus> git_status;
    if (!decode_optional_field(value.field("git_status"), git_status)) return false;
    std::optional<std::string> workspace_path;
    if (!decode_optional_field(value.field("workspace_path"), workspace_path)) return false;
    std::optional<std::uint32_t> source_line;
    if (!decode_optional_field(value.field("source_line"), source_line)) return false;
    out.emplace(TreeNode{*id, std::move(parent_id), *label, *kind, std::move(icon),
                         *commands, std::move(git_status), std::move(workspace_path),
                         std::move(source_line), *expandable});
    return true;
}

ProtocolValue to_value(TreeNodeView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("node", to_value(value.node));
    fields.emplace_back("depth", to_value(static_cast<std::uint64_t>(value.depth)));
    fields.emplace_back("expanded", to_value(value.expanded));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TreeNodeView>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto node = require_field<TreeNode>(value.field("node"));
    auto depth = require_field<std::uint64_t>(value.field("depth"));
    auto expanded = require_field<bool>(value.field("expanded"));
    if (!node || !depth || !expanded) return false;
    out.emplace(TreeNodeView{*node, static_cast<std::size_t>(*depth), *expanded});
    return true;
}

ProtocolValue to_value(TreeProviderView const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("provider_id", to_value(value.provider_id));
    fields.emplace_back("kind", to_value(value.kind));
    fields.emplace_back("nodes", to_value(value.nodes));
    fields.emplace_back("selected", to_value(value.selected));
    fields.emplace_back("first_visible", to_value(value.first_visible));
    fields.emplace_back("scrollbar", to_value(value.scrollbar));
    fields.emplace_back("visible_node_ids", to_value(value.visible_node_ids));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TreeProviderView>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto provider_id = require_field<TreeProviderId>(value.field("provider_id"));
    auto kind = require_field<TreeProviderKind>(value.field("kind"));
    auto nodes = require_field<std::vector<TreeNodeView>>(value.field("nodes"));
    std::optional<TreeNodeId> selected;
    if (!provider_id || !kind || !nodes) return false;
    if (!decode_optional_field(value.field("selected"), selected)) return false;
    auto first_visible = require_field<std::uint32_t>(value.field("first_visible"));
    auto scrollbar = require_field<ScrollbarMetrics>(value.field("scrollbar"));
    auto visible_node_ids =
        require_field<std::vector<TreeNodeId>>(value.field("visible_node_ids"));
    if (!first_visible || !scrollbar || !visible_node_ids) return false;
    out.emplace(TreeProviderView{*provider_id, *kind, *nodes, selected,
                                 *first_visible, *scrollbar,
                                 std::move(*visible_node_ids)});
    return true;
}

ProtocolValue to_value(TreeViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", to_value(value.revision));
    fields.emplace_back("providers", to_value(value.providers));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TreeViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto revision = require_field<TreeRevision>(value.field("revision"));
    auto providers = require_field<std::vector<TreeProviderView>>(value.field("providers"));
    if (!revision || !providers) return false;
    out.emplace(TreeViewState{*revision, *providers});
    return true;
}

ProtocolValue to_value(TreeProviderDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("provider_id", to_value(value.provider_id));
    fields.emplace_back("kind", to_value(value.kind));
    fields.emplace_back("remove_provider", to_value(value.remove_provider));
    fields.emplace_back("start", to_value(static_cast<std::uint64_t>(value.start)));
    fields.emplace_back("erase_count", to_value(static_cast<std::uint64_t>(value.erase_count)));
    fields.emplace_back("insert", to_value(value.insert));
    fields.emplace_back("selected", to_value(value.selected));
    fields.emplace_back("first_visible", to_value(value.first_visible));
    fields.emplace_back("scrollbar", to_value(value.scrollbar));
    fields.emplace_back("visible_node_ids", to_value(value.visible_node_ids));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TreeProviderDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto provider_id = require_field<TreeProviderId>(value.field("provider_id"));
    auto kind = require_field<TreeProviderKind>(value.field("kind"));
    auto remove_provider = require_field<bool>(value.field("remove_provider"));
    auto start = require_field<std::uint64_t>(value.field("start"));
    auto erase_count = require_field<std::uint64_t>(value.field("erase_count"));
    auto insert = require_field<std::vector<TreeNodeView>>(value.field("insert"));
    if (!provider_id || !kind || !remove_provider || !start || !erase_count || !insert) {
        return false;
    }
    std::optional<TreeNodeId> selected;
    if (!decode_optional_field(value.field("selected"), selected)) return false;
    auto first_visible = require_field<std::uint32_t>(value.field("first_visible"));
    auto scrollbar = require_field<ScrollbarMetrics>(value.field("scrollbar"));
    auto visible_node_ids =
        require_field<std::vector<TreeNodeId>>(value.field("visible_node_ids"));
    if (!first_visible || !scrollbar || !visible_node_ids) return false;
    out.emplace(TreeProviderDelta{*provider_id, *kind, *remove_provider,
                                  static_cast<std::size_t>(*start),
                                  static_cast<std::size_t>(*erase_count), *insert,
                                  selected, *first_visible, *scrollbar,
                                  std::move(*visible_node_ids)});
    return true;
}

ProtocolValue to_value(TreeDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", to_value(value.base_revision));
    fields.emplace_back("revision", to_value(value.revision));
    fields.emplace_back("snapshot_required", to_value(value.snapshot_required));
    fields.emplace_back("providers", to_value(value.providers));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TreeDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto base_revision = require_field<TreeRevision>(value.field("base_revision"));
    auto revision = require_field<TreeRevision>(value.field("revision"));
    auto snapshot_required = require_field<bool>(value.field("snapshot_required"));
    auto providers = require_field<std::vector<TreeProviderDelta>>(value.field("providers"));
    if (!base_revision || !revision || !snapshot_required || !providers) return false;
    out.emplace(TreeDelta{*base_revision, *revision, *snapshot_required, *providers});
    return true;
}

// ---------------------------------------------------------------------------
// syntax.h composite definitions.

ProtocolValue to_value(SyntaxRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("begin", to_value(value.begin));
    fields.emplace_back("end", to_value(value.end));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SyntaxRange>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto begin = require_field<ByteOffset>(value.field("begin"));
    auto end = require_field<ByteOffset>(value.field("end"));
    if (!begin || !end) return false;
    out.emplace(SyntaxRange{*begin, *end});
    return true;
}

ProtocolValue to_value(SyntaxSpan const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("begin", to_value(value.begin));
    fields.emplace_back("end", to_value(value.end));
    fields.emplace_back("scope", to_value(value.scope));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SyntaxSpan>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto begin = require_field<ByteOffset>(value.field("begin"));
    auto end = require_field<ByteOffset>(value.field("end"));
    auto scope = require_field<SyntaxScope>(value.field("scope"));
    if (!begin || !end || !scope) return false;
    out.emplace(SyntaxSpan{*begin, *end, *scope});
    return true;
}

ProtocolValue to_value(SyntaxBracketPair const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("open", to_value(value.open));
    fields.emplace_back("close", to_value(value.close));
    fields.emplace_back("kind", to_value(value.kind));
    fields.emplace_back("depth", to_value(value.depth));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SyntaxBracketPair>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto open = require_field<ByteOffset>(value.field("open"));
    auto close = require_field<ByteOffset>(value.field("close"));
    auto kind = require_field<BracketKind>(value.field("kind"));
    auto depth = require_field<std::uint32_t>(value.field("depth"));
    if (!open || !close || !kind || !depth) return false;
    out.emplace(SyntaxBracketPair{*open, *close, *kind, *depth});
    return true;
}

ProtocolValue to_value(UnmatchedBracket const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("offset", to_value(value.offset));
    fields.emplace_back("kind", to_value(value.kind));
    fields.emplace_back("role", to_value(value.role));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<UnmatchedBracket>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto offset = require_field<ByteOffset>(value.field("offset"));
    auto kind = require_field<BracketKind>(value.field("kind"));
    auto role = require_field<BracketRole>(value.field("role"));
    if (!offset || !kind || !role) return false;
    out.emplace(UnmatchedBracket{*offset, *kind, *role});
    return true;
}

ProtocolValue to_value(CommentToken const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("range", to_value(value.range));
    fields.emplace_back("role", to_value(value.role));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<CommentToken>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto range = require_field<SyntaxRange>(value.field("range"));
    auto role = require_field<CommentTokenRole>(value.field("role"));
    if (!range || !role) return false;
    out.emplace(CommentToken{*range, *role});
    return true;
}

ProtocolValue to_value(CommentRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("range", to_value(value.range));
    fields.emplace_back("kind", to_value(value.kind));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<CommentRange>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto range = require_field<SyntaxRange>(value.field("range"));
    auto kind = require_field<CommentKind>(value.field("kind"));
    if (!range || !kind) return false;
    out.emplace(CommentRange{*range, *kind});
    return true;
}

ProtocolValue to_value(LineIndentation const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("line", to_value(value.line));
    fields.emplace_back("line_start", to_value(value.line_start));
    fields.emplace_back("content_start", to_value(value.content_start));
    fields.emplace_back("spaces", to_value(value.spaces));
    fields.emplace_back("tabs", to_value(value.tabs));
    fields.emplace_back("columns", to_value(value.columns));
    fields.emplace_back("blank", to_value(value.blank));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LineIndentation>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto line = require_field<LineIndex>(value.field("line"));
    auto line_start = require_field<ByteOffset>(value.field("line_start"));
    auto content_start = require_field<ByteOffset>(value.field("content_start"));
    auto spaces = require_field<std::uint32_t>(value.field("spaces"));
    auto tabs = require_field<std::uint32_t>(value.field("tabs"));
    auto columns = require_field<std::uint32_t>(value.field("columns"));
    auto blank = require_field<bool>(value.field("blank"));
    if (!line || !line_start || !content_start || !spaces || !tabs || !columns || !blank) {
        return false;
    }
    out.emplace(LineIndentation{*line, *line_start, *content_start, *spaces, *tabs,
                                *columns, *blank});
    return true;
}

ProtocolValue to_value(SyntaxViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", to_value(value.revision()));
    fields.emplace_back("language", to_value(value.language()));
    fields.emplace_back("text_bytes", to_value(value.text_bytes()));
    fields.emplace_back("spans", to_value(value.spans()));
    fields.emplace_back("bracket_pairs", to_value(value.bracket_pairs()));
    fields.emplace_back("unmatched_brackets", to_value(value.unmatched_brackets()));
    fields.emplace_back("comment_tokens", to_value(value.comment_tokens()));
    fields.emplace_back("comment_ranges", to_value(value.comment_ranges()));
    fields.emplace_back("indentation", to_value(value.indentation()));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SyntaxViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto revision = require_field<Revision>(value.field("revision"));
    auto language = require_field<LanguageId>(value.field("language"));
    auto text_bytes = require_field<std::uint64_t>(value.field("text_bytes"));
    auto spans = require_field<std::vector<SyntaxSpan>>(value.field("spans"));
    auto bracket_pairs = require_field<std::vector<SyntaxBracketPair>>(value.field("bracket_pairs"));
    auto unmatched_brackets =
        require_field<std::vector<UnmatchedBracket>>(value.field("unmatched_brackets"));
    auto comment_tokens = require_field<std::vector<CommentToken>>(value.field("comment_tokens"));
    auto comment_ranges = require_field<std::vector<CommentRange>>(value.field("comment_ranges"));
    auto indentation = require_field<std::vector<LineIndentation>>(value.field("indentation"));
    if (!revision || !language || !text_bytes || !spans || !bracket_pairs ||
        !unmatched_brackets || !comment_tokens || !comment_ranges || !indentation) {
        return false;
    }
    out.emplace(*revision, *language, *text_bytes, *spans, *bracket_pairs,
               *unmatched_brackets, *comment_tokens, *comment_ranges, *indentation);
    return true;
}

ProtocolValue to_value(SyntaxDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", to_value(value.base_revision()));
    fields.emplace_back("revision", to_value(value.revision()));
    fields.emplace_back("language", to_value(value.language()));
    fields.emplace_back("text_bytes", to_value(value.text_bytes()));
    fields.emplace_back("spans", to_value(value.spans()));
    fields.emplace_back("bracket_pairs", to_value(value.bracket_pairs()));
    fields.emplace_back("unmatched_brackets", to_value(value.unmatched_brackets()));
    fields.emplace_back("comment_tokens", to_value(value.comment_tokens()));
    fields.emplace_back("comment_ranges", to_value(value.comment_ranges()));
    fields.emplace_back("indentation", to_value(value.indentation()));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SyntaxDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto base_revision = require_field<Revision>(value.field("base_revision"));
    auto revision = require_field<Revision>(value.field("revision"));
    if (!base_revision || !revision) return false;
    std::optional<LanguageId> language;
    if (!decode_optional_field(value.field("language"), language)) return false;
    std::optional<std::uint64_t> text_bytes;
    if (!decode_optional_field(value.field("text_bytes"), text_bytes)) return false;
    std::optional<std::vector<SyntaxSpan>> spans;
    if (!decode_optional_field(value.field("spans"), spans)) return false;
    std::optional<std::vector<SyntaxBracketPair>> bracket_pairs;
    if (!decode_optional_field(value.field("bracket_pairs"), bracket_pairs)) return false;
    std::optional<std::vector<UnmatchedBracket>> unmatched_brackets;
    if (!decode_optional_field(value.field("unmatched_brackets"), unmatched_brackets)) {
        return false;
    }
    std::optional<std::vector<CommentToken>> comment_tokens;
    if (!decode_optional_field(value.field("comment_tokens"), comment_tokens)) return false;
    std::optional<std::vector<CommentRange>> comment_ranges;
    if (!decode_optional_field(value.field("comment_ranges"), comment_ranges)) return false;
    std::optional<std::vector<LineIndentation>> indentation;
    if (!decode_optional_field(value.field("indentation"), indentation)) return false;
    out.emplace(*base_revision, *revision, std::move(language), std::move(text_bytes),
               std::move(spans), std::move(bracket_pairs), std::move(unmatched_brackets),
               std::move(comment_tokens), std::move(comment_ranges), std::move(indentation));
    return true;
}

// ---------------------------------------------------------------------------
// lsp_sync.h composite definitions.

ProtocolValue to_value(LspPosition const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("line", to_value(value.line));
    fields.emplace_back("character", to_value(value.character));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspPosition>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto line = require_field<std::uint64_t>(value.field("line"));
    auto character = require_field<std::uint64_t>(value.field("character"));
    if (!line || !character) return false;
    out.emplace(LspPosition{*line, *character});
    return true;
}

ProtocolValue to_value(LspRange const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("start", to_value(value.start));
    fields.emplace_back("end", to_value(value.end));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspRange>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto start = require_field<LspPosition>(value.field("start"));
    auto end = require_field<LspPosition>(value.field("end"));
    if (!start || !end) return false;
    out.emplace(LspRange{*start, *end});
    return true;
}

ProtocolValue to_value(LspDiagnostic const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("range", to_value(value.range));
    fields.emplace_back("severity", to_value(value.severity));
    fields.emplace_back("code", to_value(value.code));
    fields.emplace_back("message", to_value(value.message));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspDiagnostic>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto range = require_field<LspRange>(value.field("range"));
    auto code = require_field<std::string>(value.field("code"));
    auto message = require_field<std::string>(value.field("message"));
    if (!range || !code || !message) return false;
    LspDiagnostic result;
    result.range = *range;
    if (!decode_optional_field(value.field("severity"), result.severity)) return false;
    result.code = *code;
    result.message = *message;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue to_value(LspDocumentDiagnostics const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("uri", to_value(value.uri));
    fields.emplace_back("revision", to_value(value.revision));
    fields.emplace_back("diagnostics", to_value(value.diagnostics));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspDocumentDiagnostics>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto uri = require_field<std::string>(value.field("uri"));
    auto revision = require_field<Revision>(value.field("revision"));
    auto diagnostics = require_field<std::vector<LspDiagnostic>>(value.field("diagnostics"));
    if (!uri || !revision || !diagnostics) return false;
    out.emplace(LspDocumentDiagnostics{*uri, *revision, *diagnostics});
    return true;
}

ProtocolValue to_value(LspSyncViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", to_value(value.revision));
    fields.emplace_back("documents", to_value(value.documents));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspSyncViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto revision = require_field<Revision>(value.field("revision"));
    auto documents = require_field<std::vector<LspDocumentDiagnostics>>(value.field("documents"));
    if (!revision || !documents) return false;
    out.emplace(LspSyncViewState{*revision, *documents});
    return true;
}

ProtocolValue to_value(LspSyncDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", to_value(value.base_revision));
    fields.emplace_back("revision", to_value(value.revision));
    fields.emplace_back("state", to_value(value.state));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspSyncDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto base_revision = require_field<Revision>(value.field("base_revision"));
    auto revision = require_field<Revision>(value.field("revision"));
    if (!base_revision || !revision) return false;
    LspSyncDelta result;
    result.base_revision = *base_revision;
    result.revision = *revision;
    if (!decode_optional_field(value.field("state"), result.state)) return false;
    out.emplace(std::move(result));
    return true;
}

// ---------------------------------------------------------------------------
// lsp_features.h composite definitions.

ProtocolValue to_value(LspCompletionItem const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("label", to_value(value.label));
    fields.emplace_back("detail", to_value(value.detail));
    fields.emplace_back("sort_text", to_value(value.sort_text));
    fields.emplace_back("insert_text", to_value(value.insert_text));
    fields.emplace_back("replacement_range", to_value(value.replacement_range));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspCompletionItem>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto label = require_field<std::string>(value.field("label"));
    auto detail = require_field<std::string>(value.field("detail"));
    auto sort_text = require_field<std::string>(value.field("sort_text"));
    auto insert_text = require_field<std::string>(value.field("insert_text"));
    if (!label || !detail || !sort_text || !insert_text) return false;
    LspCompletionItem result;
    result.label = *label;
    result.detail = *detail;
    result.sort_text = *sort_text;
    result.insert_text = *insert_text;
    if (!decode_optional_field(value.field("replacement_range"), result.replacement_range)) {
        return false;
    }
    out.emplace(std::move(result));
    return true;
}

ProtocolValue to_value(LspCompletionViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("visible", to_value(value.visible));
    fields.emplace_back("loading", to_value(value.loading));
    fields.emplace_back("items", to_value(value.items));
    if (value.selected_index) {
        fields.emplace_back("selected_index",
                            to_value(static_cast<std::uint64_t>(*value.selected_index)));
    } else {
        fields.emplace_back("selected_index", ProtocolValue::make_null());
    }
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspCompletionViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto visible = require_field<bool>(value.field("visible"));
    auto loading = require_field<bool>(value.field("loading"));
    auto items = require_field<std::vector<LspCompletionItem>>(value.field("items"));
    if (!visible || !loading || !items) return false;
    LspCompletionViewState result;
    result.visible = *visible;
    result.loading = *loading;
    result.items = *items;
    std::optional<std::uint64_t> selected_index;
    if (!decode_optional_field(value.field("selected_index"), selected_index)) return false;
    if (selected_index) result.selected_index = static_cast<std::size_t>(*selected_index);
    out.emplace(std::move(result));
    return true;
}

ProtocolValue to_value(LspHover const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("contents", to_value(value.contents));
    fields.emplace_back("range", to_value(value.range));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspHover>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto contents = require_field<std::string>(value.field("contents"));
    if (!contents) return false;
    LspHover result;
    result.contents = *contents;
    if (!decode_optional_field(value.field("range"), result.range)) return false;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue to_value(LspNavigationTarget const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("uri", to_value(value.uri));
    fields.emplace_back("range", to_value(value.range));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspNavigationTarget>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto uri = require_field<std::string>(value.field("uri"));
    auto range = require_field<LspRange>(value.field("range"));
    if (!uri || !range) return false;
    out.emplace(LspNavigationTarget{*uri, *range});
    return true;
}

ProtocolValue to_value(LspNavigationViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("targets", to_value(value.targets));
    if (value.selected_index) {
        fields.emplace_back("selected_index",
                            to_value(static_cast<std::uint64_t>(*value.selected_index)));
    } else {
        fields.emplace_back("selected_index", ProtocolValue::make_null());
    }
    fields.emplace_back("user_navigation", to_value(value.user_navigation));
    fields.emplace_back("reveal_primary_caret", to_value(value.reveal_primary_caret));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspNavigationViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto targets = require_field<std::vector<LspNavigationTarget>>(value.field("targets"));
    auto user_navigation = require_field<bool>(value.field("user_navigation"));
    auto reveal_primary_caret = require_field<bool>(value.field("reveal_primary_caret"));
    if (!targets || !user_navigation || !reveal_primary_caret) return false;
    LspNavigationViewState result;
    result.targets = *targets;
    std::optional<std::uint64_t> selected_index;
    if (!decode_optional_field(value.field("selected_index"), selected_index)) return false;
    if (selected_index) result.selected_index = static_cast<std::size_t>(*selected_index);
    result.user_navigation = *user_navigation;
    result.reveal_primary_caret = *reveal_primary_caret;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue to_value(LspFeatureViewState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", to_value(value.revision));
    fields.emplace_back("completion", to_value(value.completion));
    fields.emplace_back("hover", to_value(value.hover));
    fields.emplace_back("navigation", to_value(value.navigation));
    fields.emplace_back("status", to_value(value.status));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspFeatureViewState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto revision = require_field<Revision>(value.field("revision"));
    auto completion = require_field<LspCompletionViewState>(value.field("completion"));
    auto navigation = require_field<LspNavigationViewState>(value.field("navigation"));
    auto status = require_field<std::string>(value.field("status"));
    if (!revision || !completion || !navigation || !status) return false;
    LspFeatureViewState result;
    result.revision = *revision;
    result.completion = *completion;
    if (!decode_optional_field(value.field("hover"), result.hover)) return false;
    result.navigation = *navigation;
    result.status = *status;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue to_value(LspFeatureDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", to_value(value.base_revision));
    fields.emplace_back("revision", to_value(value.revision));
    fields.emplace_back("state", to_value(value.state));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<LspFeatureDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto base_revision = require_field<Revision>(value.field("base_revision"));
    auto revision = require_field<Revision>(value.field("revision"));
    if (!base_revision || !revision) return false;
    LspFeatureDelta result;
    result.base_revision = *base_revision;
    result.revision = *revision;
    if (!decode_optional_field(value.field("state"), result.state)) return false;
    out.emplace(std::move(result));
    return true;
}

// ---------------------------------------------------------------------------
// theme.h composite definitions.

ProtocolValue to_value(SrgbColor const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("red", to_value(value.red));
    fields.emplace_back("green", to_value(value.green));
    fields.emplace_back("blue", to_value(value.blue));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SrgbColor>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto red = require_field<std::uint8_t>(value.field("red"));
    auto green = require_field<std::uint8_t>(value.field("green"));
    auto blue = require_field<std::uint8_t>(value.field("blue"));
    if (!red || !green || !blue) return false;
    out.emplace(
        SrgbColor::from_serialized_channels(*red, *green, *blue));
    return true;
}

ProtocolValue to_value(ThemeSnapshot const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("palette", to_value(value.palette));
    fields.emplace_back("semantic_indices", to_value(value.semantic_indices));
    fields.emplace_back("syntax_indices", to_value(value.syntax_indices));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ThemeSnapshot>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto palette =
        require_field<std::array<SrgbColor, theme_palette_size>>(value.field("palette"));
    auto semantic_indices = require_field<std::array<std::uint8_t, semantic_role_count>>(
        value.field("semantic_indices"));
    auto syntax_indices = require_field<std::array<std::uint8_t, syntax_scope_count>>(
        value.field("syntax_indices"));
    if (!palette || !semantic_indices || !syntax_indices) return false;
    out.emplace(ThemeSnapshot{*palette, *semantic_indices, *syntax_indices});
    return true;
}

// ---------------------------------------------------------------------------
// session.h composite definitions.

ProtocolValue to_value(SessionTopology const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("active_workspace", to_value(value.active_workspace));
    fields.emplace_back("active_view", to_value(value.active_view));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SessionTopology>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    SessionTopology result;
    if (!decode_optional_field(value.field("active_workspace"), result.active_workspace)) {
        return false;
    }
    if (!decode_optional_field(value.field("active_view"), result.active_view)) {
        return false;
    }
    out.emplace(std::move(result));
    return true;
}

// ---------------------------------------------------------------------------
// session_snapshot.h composite definitions: ClientSnapshotState and
// SessionSnapshotSections (the top-level container for every feature's
// snapshot-side view state).

ProtocolValue to_value(ClientSnapshotState const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("client_id", to_value(value.client_id));
    fields.emplace_back("view_id", to_value(value.view_id));
    fields.emplace_back("capabilities", to_value(value.capabilities));
    fields.emplace_back("viewport", to_value(value.viewport));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ClientSnapshotState>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto client_id = require_field<ClientId>(value.field("client_id"));
    auto view_id = require_field<ViewId>(value.field("view_id"));
    auto capabilities = require_field<std::vector<CapabilityId>>(value.field("capabilities"));
    auto viewport = require_field<ViewportViewState>(value.field("viewport"));
    if (!client_id || !view_id || !capabilities || !viewport) return false;
    out.emplace(ClientSnapshotState{*client_id, *view_id, *capabilities, *viewport});
    return true;
}

ProtocolValue to_value(SessionSnapshotSections const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("document", to_value(value.document));
    fields.emplace_back("selection", to_value(value.selection));
    fields.emplace_back("history", to_value(value.history));
    fields.emplace_back("clipboard", to_value(value.clipboard));
    fields.emplace_back("prompt_status", to_value(value.prompt_status));
    fields.emplace_back("search", to_value(value.search));
    fields.emplace_back("find_replace", to_value(value.find_replace));
    fields.emplace_back("settings", to_value(value.settings));
    fields.emplace_back("keymap", to_value(value.keymap));
    fields.emplace_back("text_encoding", to_value(value.text_encoding));
    fields.emplace_back("tabs", to_value(value.tabs));
    fields.emplace_back("diff", to_value(value.diff));
    fields.emplace_back("external_modification", to_value(value.external_modification));
    fields.emplace_back("follow_edits", to_value(value.follow_edits));
    fields.emplace_back("tree", to_value(value.tree));
    fields.emplace_back("syntax", to_value(value.syntax));
    fields.emplace_back("lsp_sync", to_value(value.lsp_sync));
    fields.emplace_back("lsp_features", to_value(value.lsp_features));
    fields.emplace_back("theme", to_value(value.theme));
    fields.emplace_back("shell", to_value(value.shell));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SessionSnapshotSections>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto document = require_field<DocumentViewState>(value.field("document"));
    auto selection = require_field<SelectionViewState>(value.field("selection"));
    auto history = require_field<HistoryViewState>(value.field("history"));
    auto clipboard = require_field<ClipboardViewState>(value.field("clipboard"));
    auto prompt_status = require_field<PromptStatusViewState>(value.field("prompt_status"));
    auto search = require_field<SearchViewState>(value.field("search"));
    auto find_replace = require_field<FindReplaceViewState>(value.field("find_replace"));
    auto settings = require_field<SettingsViewState>(value.field("settings"));
    auto keymap = require_field<KeymapViewState>(value.field("keymap"));
    auto text_encoding = require_field<TextEncodingViewState>(value.field("text_encoding"));
    auto tabs = require_field<TabViewState>(value.field("tabs"));
    auto diff = require_field<DiffViewState>(value.field("diff"));
    auto external_modification =
        require_field<ExternalModificationViewState>(value.field("external_modification"));
    auto follow_edits = require_field<FollowEditsViewState>(value.field("follow_edits"));
    auto tree = require_field<TreeViewState>(value.field("tree"));
    auto syntax = require_field<SyntaxViewState>(value.field("syntax"));
    auto lsp_sync = require_field<LspSyncViewState>(value.field("lsp_sync"));
    auto lsp_features = require_field<LspFeatureViewState>(value.field("lsp_features"));
    auto theme = require_field<ThemeSnapshot>(value.field("theme"));
    auto shell = require_field<ShellViewState>(value.field("shell"));
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

// ---------------------------------------------------------------------------
// Command-argument payload type definitions (used by the
// CommandArgumentCodecRegistry to encode/decode std::any command payloads).

ProtocolValue to_value(TextInputArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("text", to_value(value.text));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TextInputArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto text = require_field<std::string>(value.field("text"));
    if (!text) return false;
    out.emplace(TextInputArguments{*text});
    return true;
}

ProtocolValue to_value(PaletteExecuteArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("command_id", to_value(value.command_id));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<PaletteExecuteArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto command_id = require_field<std::string>(value.field("command_id"));
    if (!command_id) return false;
    out.emplace(PaletteExecuteArguments{*command_id});
    return true;
}

ProtocolValue to_value(TreeSelectArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("node_id", to_value(value.node_id));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<TreeSelectArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto node_id = require_field<TreeNodeId>(value.field("node_id"));
    if (!node_id) return false;
    out.emplace(TreeSelectArguments{*node_id});
    return true;
}

ProtocolValue to_value(FindQueryArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("query", to_value(value.query));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<FindQueryArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto query = require_field<std::string>(value.field("query"));
    if (!query) return false;
    out.emplace(FindQueryArguments{*query});
    return true;
}

ProtocolValue to_value(SelectionCommandArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("position", to_value(value.position));
    fields.emplace_back("selection", to_value(value.selection));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SelectionCommandArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    SelectionCommandArguments result;
    if (!decode_optional_field(value.field("position"), result.position)) return false;
    if (!decode_optional_field(value.field("selection"), result.selection)) return false;
    out.emplace(std::move(result));
    return true;
}

ProtocolValue to_value(ScrollLinesArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("rows", to_value(value.rows));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ScrollLinesArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto rows = require_field<std::int64_t>(value.field("rows"));
    if (!rows) return false;
    out.emplace(ScrollLinesArguments{*rows});
    return true;
}

ProtocolValue to_value(ScrollPagesArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("pages", to_value(value.pages));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ScrollPagesArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto pages = require_field<std::int64_t>(value.field("pages"));
    if (!pages) return false;
    out.emplace(ScrollPagesArguments{*pages});
    return true;
}

ProtocolValue to_value(ScrollFractionArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("numerator", to_value(value.numerator));
    fields.emplace_back("denominator", to_value(value.denominator));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ScrollFractionArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto numerator = require_field<std::uint32_t>(value.field("numerator"));
    auto denominator = require_field<std::uint32_t>(value.field("denominator"));
    if (!numerator || !denominator) return false;
    out.emplace(*numerator, *denominator);
    return true;
}

ProtocolValue to_value(DroppedContentArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("bytes", to_value(value.bytes));
    fields.emplace_back("suggested_label", to_value(value.suggested_label));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<DroppedContentArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto bytes = require_field<std::vector<std::uint8_t>>(value.field("bytes"));
    auto suggested_label = require_field<std::string>(value.field("suggested_label"));
    if (!bytes || !suggested_label) return false;
    out.emplace(DroppedContentArguments{*bytes, *suggested_label});
    return true;
}

ProtocolValue to_value(ReopenWithEncodingArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("encoding", to_value(value.encoding));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ReopenWithEncodingArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto encoding = require_field<TextEncoding>(value.field("encoding"));
    if (!encoding) return false;
    out.emplace(ReopenWithEncodingArguments{*encoding});
    return true;
}

ProtocolValue to_value(SetEncodingArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("encoding", to_value(value.encoding));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SetEncodingArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto encoding = require_field<TextEncoding>(value.field("encoding"));
    if (!encoding) return false;
    out.emplace(SetEncodingArguments{*encoding});
    return true;
}

ProtocolValue to_value(SetLineEndingArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("line_ending", to_value(value.line_ending));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SetLineEndingArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto line_ending = require_field<LineEnding>(value.field("line_ending"));
    if (!line_ending) return false;
    out.emplace(SetLineEndingArguments{*line_ending});
    return true;
}

ProtocolValue to_value(SetFinalNewlineArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("final_newline", to_value(value.final_newline));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SetFinalNewlineArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto final_newline = require_field<bool>(value.field("final_newline"));
    if (!final_newline) return false;
    out.emplace(SetFinalNewlineArguments{*final_newline});
    return true;
}

ProtocolValue to_value(SettingSetArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("scope", to_value(value.scope));
    fields.emplace_back("key", to_value(value.key));
    fields.emplace_back("value", to_value(value.value));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SettingSetArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto scope = require_field<SettingScope>(value.field("scope"));
    auto key = require_field<SettingKey>(value.field("key"));
    auto setting_value = require_field<SettingValue>(value.field("value"));
    if (!scope || !key || !setting_value) return false;
    out.emplace(SettingSetArguments{*scope, *key, *setting_value});
    return true;
}

ProtocolValue to_value(SettingResetArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("scope", to_value(value.scope));
    fields.emplace_back("key", to_value(value.key));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SettingResetArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto scope = require_field<SettingScope>(value.field("scope"));
    auto key = require_field<SettingKey>(value.field("key"));
    if (!scope || !key) return false;
    out.emplace(SettingResetArguments{*scope, *key});
    return true;
}

ProtocolValue to_value(SettingResetScopeArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("scope", to_value(value.scope));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<SettingResetScopeArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto scope = require_field<SettingScope>(value.field("scope"));
    if (!scope) return false;
    out.emplace(SettingResetScopeArguments{*scope});
    return true;
}

ProtocolValue to_value(WorkspaceReplaceArguments const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("request", to_value(value.request));
    fields.emplace_back("replacement", to_value(value.replacement));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<WorkspaceReplaceArguments>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    auto request = require_field<FindRequest>(value.field("request"));
    auto replacement = require_field<std::string>(value.field("replacement"));
    if (!request || !replacement) return false;
    out.emplace(WorkspaceReplaceArguments{*request, *replacement});
    return true;
}

ProtocolValue to_value(ThemeSectionDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("replacement", to_value(value.replacement));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ThemeSectionDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    std::optional<ThemeSnapshot> replacement;
    if (!decode_optional_field(value.field("replacement"), replacement)) return false;
    out.emplace(ThemeSectionDelta{std::move(replacement)});
    return true;
}

ProtocolValue to_value(ShellSectionDelta const& value) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("replacement", to_value(value.replacement));
    return ProtocolValue::make_object(std::move(fields));
}
bool decode_present(ProtocolValue const& value, std::optional<ShellSectionDelta>& out) {
    auto const* object = value.as_object();
    if (!object) return false;
    std::optional<ShellViewState> replacement;
    if (!decode_optional_field(value.field("replacement"), replacement)) return false;
    out.emplace(ShellSectionDelta{std::move(replacement)});
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Command argument codec registry.

struct CommandArgumentCodecRegistry::Impl {
    std::unordered_map<std::string, CommandArgumentCodec> codecs;
};

CommandArgumentCodecRegistry::CommandArgumentCodecRegistry(
    std::vector<std::pair<std::string, CommandArgumentCodec>> entries)
    : impl_{std::make_unique<Impl>()} {
    auto const descriptors = p0_command_descriptors();

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

ProtocolValue CommandArgumentCodecRegistry::encode_argument(
    std::string_view command_id, std::any const& payload) const {
    auto found = impl_->codecs.find(std::string{command_id});
    if (found == impl_->codecs.end()) {
        throw std::invalid_argument{"unknown command id: " +
                                    std::string{command_id}};
    }
    return found->second.encode(payload);
}

std::optional<std::any> CommandArgumentCodecRegistry::decode_argument(
    std::string_view command_id, ProtocolValue const& value) const {
    auto found = impl_->codecs.find(std::string{command_id});
    if (found == impl_->codecs.end()) {
        throw std::invalid_argument{"unknown command id: " +
                                    std::string{command_id}};
    }
    return found->second.decode(value);
}

namespace {

CommandArgumentCodec make_none_codec() {
    return CommandArgumentCodec{
        [](std::any const&) { return ProtocolValue::make_null(); },
        [](ProtocolValue const& value) -> std::optional<std::any> {
            if (value.kind() != ProtocolValue::Kind::null_value) {
                return std::nullopt;
            }
            return std::any{};
        }};
}

template <typename Arguments>
CommandArgumentCodec make_typed_codec() {
    return CommandArgumentCodec{
        [](std::any const& payload) {
            return to_value(std::any_cast<Arguments const&>(payload));
        },
        [](ProtocolValue const& value) -> std::optional<std::any> {
            std::optional<Arguments> decoded;
            if (!from_value(value, decoded) || !decoded.has_value()) {
                return std::nullopt;
            }
            return std::any{std::move(*decoded)};
        }};
}

CommandArgumentCodec make_workspace_apply_codec() {
    return CommandArgumentCodec{
        [](std::any const& payload) {
            if (!payload.has_value()) return ProtocolValue::make_null();
            return to_value(std::any_cast<WorkspaceReplacePreview const&>(payload));
        },
        [](ProtocolValue const& value) -> std::optional<std::any> {
            if (value.kind() == ProtocolValue::Kind::null_value) {
                return std::any{};
            }
            std::optional<WorkspaceReplacePreview> decoded;
            if (!from_value(value, decoded) || !decoded.has_value()) {
                return std::nullopt;
            }
            return std::any{std::move(*decoded)};
        }};
}

}  // namespace

CommandArgumentCodecRegistry build_command_argument_codec_registry() {
    auto const text_input_commands = text_input_command_set();
    std::unordered_set<std::string> text_input_ids;
    for (auto const& descriptor : text_input_commands.descriptors()) {
        text_input_ids.emplace(descriptor.id);
    }
    auto const selection_commands = selection_navigation_command_set();
    std::unordered_set<std::string> selection_ids;
    for (auto const& descriptor : selection_commands.descriptors()) {
        selection_ids.emplace(descriptor.id);
    }

    auto const none_codec = make_none_codec();
    auto const text_input_codec = make_typed_codec<TextInputArguments>();
    auto const palette_execute_codec = make_typed_codec<PaletteExecuteArguments>();
    auto const tree_select_codec = make_typed_codec<TreeSelectArguments>();
    auto const find_query_codec = make_typed_codec<FindQueryArguments>();
    auto const selection_codec =
        make_typed_codec<SelectionCommandArguments>();
    auto const scroll_lines_codec = make_typed_codec<ScrollLinesArguments>();
    auto const scroll_pages_codec = make_typed_codec<ScrollPagesArguments>();
    auto const scroll_fraction_codec =
        make_typed_codec<ScrollFractionArguments>();
    auto const dropped_content_codec =
        make_typed_codec<DroppedContentArguments>();
    auto const reopen_with_encoding_codec =
        make_typed_codec<ReopenWithEncodingArguments>();
    auto const set_encoding_codec = make_typed_codec<SetEncodingArguments>();
    auto const set_line_ending_codec =
        make_typed_codec<SetLineEndingArguments>();
    auto const set_final_newline_codec =
        make_typed_codec<SetFinalNewlineArguments>();
    auto const setting_set_codec = make_typed_codec<SettingSetArguments>();
    auto const setting_reset_codec = make_typed_codec<SettingResetArguments>();
    auto const setting_reset_scope_codec =
        make_typed_codec<SettingResetScopeArguments>();
    auto const workspace_replace_codec =
        make_typed_codec<WorkspaceReplaceArguments>();
    auto const workspace_apply_codec = make_workspace_apply_codec();

    std::vector<std::pair<std::string, CommandArgumentCodec>> entries;
    for (auto const& descriptor : p0_command_descriptors()) {
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

// ---------------------------------------------------------------------------
// Top-level message encode/decode functions.

std::string encode_command_request(ClientCommand const& command,
                                   CommandArgumentCodecRegistry const& registry) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", to_value(command.id));
    fields.emplace_back("base_revision", to_value(command.base_revision));
    fields.emplace_back("payload",
                        registry.encode_argument(command.id, command.payload));
    return encode_message(ProtocolMessageKind::command_request,
                          ProtocolValue::make_object(std::move(fields)));
}

DecodeCommandRequestResult decode_command_request(
    std::string_view bytes, CommandArgumentCodecRegistry const& registry,
    ProtocolLimits limits) {
    auto decoded =
        decode_message(bytes, ProtocolMessageKind::command_request, limits);
    if (decoded.error != ProtocolError::none) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    auto const& payload = *decoded.payload;
    if (!payload.as_object()) {
        return {ProtocolError::malformed_message, std::nullopt,
                "command request payload is not an object"};
    }
    auto id = require_field<std::string>(payload.field("id"));
    auto base_revision = require_field<Revision>(payload.field("base_revision"));
    if (!id || !base_revision) {
        return {ProtocolError::malformed_message, std::nullopt,
                "command request payload is malformed"};
    }
    if (!registry.contains(*id)) {
        return {ProtocolError::unsupported_command, std::nullopt,
                "command request references an unknown command id"};
    }
    auto const* payload_field = payload.field("payload");
    if (payload_field == nullptr) {
        return {ProtocolError::malformed_message, std::nullopt,
                "command request is missing its payload field"};
    }
    auto argument = registry.decode_argument(*id, *payload_field);
    if (!argument) {
        return {ProtocolError::malformed_message, std::nullopt,
                "command request payload does not match its command id"};
    }
    return {ProtocolError::none,
            ClientCommand{std::move(*id), *base_revision,
                          std::move(*argument)},
            {}};
}

std::string encode_command_result(CommandResult const& result) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back(
        "error", ProtocolValue::make_uint(
                    static_cast<std::uint8_t>(result.error)));
    fields.emplace_back("revision", to_value(result.revision));
    fields.emplace_back("message", to_value(result.message));
    return encode_message(ProtocolMessageKind::command_result,
                         ProtocolValue::make_object(std::move(fields)));
}

DecodeCommandResultResult decode_command_result(std::string_view bytes,
                                               ProtocolLimits limits) {
    auto decoded =
        decode_message(bytes, ProtocolMessageKind::command_result, limits);
    if (decoded.error != ProtocolError::none) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    auto const& payload = *decoded.payload;
    if (!payload.as_object()) {
        return {ProtocolError::malformed_message, std::nullopt,
               "command result payload is not an object"};
    }
    auto error = require_field<std::uint8_t>(payload.field("error"));
    auto revision = require_field<Revision>(payload.field("revision"));
    auto message = require_field<std::string>(payload.field("message"));
    if (!error || *error > static_cast<std::uint8_t>(
                              CommandError::revision_exhausted) ||
        !revision || !message) {
        return {ProtocolError::malformed_message, std::nullopt,
               "command result payload is malformed"};
    }
    return {ProtocolError::none,
            CommandResult{static_cast<CommandError>(*error), *revision,
                         std::move(*message)},
            {}};
}

std::string encode_session_snapshot(SessionSnapshot const& snapshot) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("revision", to_value(snapshot.revision()));
    fields.emplace_back("topology", to_value(snapshot.topology()));
    fields.emplace_back("client", to_value(snapshot.client()));
    fields.emplace_back("sections", to_value(snapshot.sections()));
    return encode_message(ProtocolMessageKind::session_snapshot,
                          ProtocolValue::make_object(std::move(fields)));
}

DecodeSessionSnapshotResult decode_session_snapshot(std::string_view bytes,
                                                    ProtocolLimits limits) {
    auto decoded =
        decode_message(bytes, ProtocolMessageKind::session_snapshot, limits);
    if (decoded.error != ProtocolError::none) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    auto const& payload = *decoded.payload;
    if (!payload.as_object()) {
        return {ProtocolError::malformed_message, std::nullopt,
                "session snapshot payload is not an object"};
    }
    auto revision = require_field<Revision>(payload.field("revision"));
    auto topology = require_field<SessionTopology>(payload.field("topology"));
    auto client = require_field<ClientSnapshotState>(payload.field("client"));
    auto sections =
        require_field<SessionSnapshotSections>(payload.field("sections"));
    if (!revision || !topology || !client || !sections) {
        return {ProtocolError::malformed_message, std::nullopt,
                "session snapshot payload is malformed"};
    }
    return {ProtocolError::none,
            SessionSnapshot{*revision, std::move(*topology),
                            std::move(*client), std::move(*sections)},
            {}};
}

std::string encode_session_delta(SessionDelta const& delta) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("base_revision", to_value(delta.base_revision()));
    fields.emplace_back("revision", to_value(delta.revision()));
    fields.emplace_back("client_id", to_value(delta.client_id()));
    fields.emplace_back("view_id", to_value(delta.view_id()));
    fields.emplace_back("capabilities", to_value(delta.capabilities()));
    fields.emplace_back("topology", to_value(delta.topology()));
    fields.emplace_back("document", to_value(delta.document()));
    fields.emplace_back("document_caret", to_value(delta.document_caret()));
    fields.emplace_back("selection", to_value(delta.selection()));
    fields.emplace_back("history", to_value(delta.history()));
    fields.emplace_back("clipboard", to_value(delta.clipboard()));
    fields.emplace_back("prompt_status", to_value(delta.prompt_status()));
    fields.emplace_back("search", to_value(delta.search()));
    fields.emplace_back("find_replace", to_value(delta.find_replace()));
    fields.emplace_back("settings", to_value(delta.settings()));
    fields.emplace_back("keymap", to_value(delta.keymap()));
    fields.emplace_back("text_encoding", to_value(delta.text_encoding()));
    fields.emplace_back("tabs", to_value(delta.tabs()));
    fields.emplace_back("diff", to_value(delta.diff()));
    fields.emplace_back("external_modification",
                        to_value(delta.external_modification()));
    fields.emplace_back("follow_edits", to_value(delta.follow_edits()));
    fields.emplace_back("tree", to_value(delta.tree()));
    fields.emplace_back("syntax", to_value(delta.syntax()));
    fields.emplace_back("lsp_sync", to_value(delta.lsp_sync()));
    fields.emplace_back("lsp_features", to_value(delta.lsp_features()));
    fields.emplace_back("theme", to_value(delta.theme()));
    fields.emplace_back("shell", to_value(delta.shell()));
    fields.emplace_back("viewport", to_value(delta.viewport()));
    return encode_message(ProtocolMessageKind::session_delta,
                          ProtocolValue::make_object(std::move(fields)));
}

DecodeSessionDeltaResult decode_session_delta(std::string_view bytes,
                                              ProtocolLimits limits) {
    auto decoded =
        decode_message(bytes, ProtocolMessageKind::session_delta, limits);
    if (decoded.error != ProtocolError::none) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    auto const& payload = *decoded.payload;
    if (!payload.as_object()) {
        return {ProtocolError::malformed_message, std::nullopt,
                "session delta payload is not an object"};
    }

    auto base_revision = require_field<Revision>(payload.field("base_revision"));
    auto revision = require_field<Revision>(payload.field("revision"));
    auto client_id = require_field<ClientId>(payload.field("client_id"));
    auto view_id = require_field<ViewId>(payload.field("view_id"));
    auto capabilities =
        require_field<std::vector<CapabilityId>>(payload.field("capabilities"));

    std::optional<SessionTopology> topology;
    std::optional<DocumentDelta> document;
    std::optional<ByteOffset> document_caret;
    std::optional<TextEncodingDelta> text_encoding;
    bool const optional_ok =
        decode_optional_field(payload.field("topology"), topology) &&
        decode_optional_field(payload.field("document"), document) &&
        decode_optional_field(payload.field("document_caret"), document_caret) &&
        decode_optional_field(payload.field("text_encoding"), text_encoding);

    auto selection = require_field<SelectionViewDelta>(payload.field("selection"));
    auto history = require_field<HistoryDelta>(payload.field("history"));
    auto clipboard = require_field<ClipboardDelta>(payload.field("clipboard"));
    auto prompt_status =
        require_field<PromptStatusDelta>(payload.field("prompt_status"));
    auto search = require_field<SearchDelta>(payload.field("search"));
    auto find_replace =
        require_field<FindReplaceDelta>(payload.field("find_replace"));
    auto settings =
        require_field<SettingsSectionDelta>(payload.field("settings"));
    auto keymap = require_field<KeymapDelta>(payload.field("keymap"));
    auto tabs = require_field<TabDelta>(payload.field("tabs"));
    auto diff = require_field<DiffDelta>(payload.field("diff"));
    auto external_modification = require_field<ExternalModificationDelta>(
        payload.field("external_modification"));
    auto follow_edits =
        require_field<FollowEditsDelta>(payload.field("follow_edits"));
    auto tree = require_field<TreeDelta>(payload.field("tree"));
    auto syntax = require_field<SyntaxDelta>(payload.field("syntax"));
    auto lsp_sync = require_field<LspSyncDelta>(payload.field("lsp_sync"));
    auto lsp_features =
        require_field<LspFeatureDelta>(payload.field("lsp_features"));
    auto theme = require_field<ThemeSectionDelta>(payload.field("theme"));
    auto shell = require_field<ShellSectionDelta>(payload.field("shell"));
    auto viewport = require_field<ViewportDelta>(payload.field("viewport"));

    if (!optional_ok || !base_revision || !revision || !client_id || !view_id ||
        !capabilities || !selection || !history || !clipboard ||
        !prompt_status || !search || !find_replace || !settings || !keymap ||
        !tabs || !diff || !external_modification || !follow_edits || !tree ||
        !syntax || !lsp_sync || !lsp_features || !theme || !shell ||
        !viewport) {
        return {ProtocolError::malformed_message, std::nullopt,
                "session delta payload is malformed"};
    }

    return {ProtocolError::none,
            decode_wire_session_delta(
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

std::string encode_clipboard_request(ClipboardRequest const& request) {
    return encode_message(ProtocolMessageKind::clipboard_request,
                          to_value(request));
}

DecodeClipboardRequestResult decode_clipboard_request(std::string_view bytes,
                                                      ProtocolLimits limits) {
    auto decoded =
        decode_message(bytes, ProtocolMessageKind::clipboard_request, limits);
    if (decoded.error != ProtocolError::none) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    std::optional<ClipboardRequest> request;
    if (!from_value(*decoded.payload, request) || !request.has_value()) {
        return {ProtocolError::malformed_message, std::nullopt,
                "clipboard request payload is malformed"};
    }
    return {ProtocolError::none, std::move(request), {}};
}

std::string encode_clipboard_response(ClipboardResponse const& response) {
    return encode_message(ProtocolMessageKind::clipboard_response,
                          to_value(response));
}

DecodeClipboardResponseResult decode_clipboard_response(
    std::string_view bytes, ProtocolLimits limits) {
    auto decoded =
        decode_message(bytes, ProtocolMessageKind::clipboard_response, limits);
    if (decoded.error != ProtocolError::none) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    std::optional<ClipboardResponse> response;
    if (!from_value(*decoded.payload, response) || !response.has_value()) {
        return {ProtocolError::malformed_message, std::nullopt,
                "clipboard response payload is malformed"};
    }
    return {ProtocolError::none, std::move(response), {}};
}

std::string encode_status_action_invocation(
    StatusActionInvocation const& invocation) {
    return encode_message(ProtocolMessageKind::status_action_invocation,
                          to_value(invocation));
}

DecodeStatusActionInvocationResult decode_status_action_invocation(
    std::string_view bytes, ProtocolLimits limits) {
    auto decoded = decode_message(
        bytes, ProtocolMessageKind::status_action_invocation, limits);
    if (decoded.error != ProtocolError::none) {
        return {decoded.error, std::nullopt, decoded.message};
    }
    std::optional<StatusActionInvocation> invocation;
    if (!from_value(*decoded.payload, invocation) || !invocation.has_value()) {
        return {ProtocolError::malformed_message, std::nullopt,
                "status action invocation payload is malformed"};
    }
    return {ProtocolError::none, std::move(invocation), {}};
}

// ---------------------------------------------------------------------------
// Binary-frame envelope: [u8 version][u8 kind][u64 request_id][u32
// declared_length][declared_length bytes]. No trailing bytes are permitted.

std::string encode_binary_frame(BinaryFrame const& frame) {
    std::string out;
    write_u8(out, frame.version);
    write_u8(out, static_cast<std::uint8_t>(frame.kind));
    write_u64(out, frame.request_id);
    write_u32(out, static_cast<std::uint32_t>(frame.bytes.size()));
    write_raw_bytes(out, frame.bytes);
    return out;
}

DecodeBinaryFrameResult decode_binary_frame(std::string_view bytes,
                                            ProtocolLimits limits) {
    if (bytes.size() > limits.max_binary_frame_bytes) {
        return {ProtocolError::binary_frame_too_large, std::nullopt,
                "binary frame exceeds the configured byte limit"};
    }
    ByteReader reader{bytes};
    std::uint8_t version = 0;
    if (!reader.read_u8(version)) {
        return {ProtocolError::truncated_message, std::nullopt,
                "binary frame is missing its version byte"};
    }
    std::uint8_t kind_byte = 0;
    if (!reader.read_u8(kind_byte)) {
        return {ProtocolError::truncated_message, std::nullopt,
                "binary frame is missing its kind byte"};
    }
    if (kind_byte != static_cast<std::uint8_t>(BinaryPayloadKind::dropped_content)) {
        return {ProtocolError::malformed_message, std::nullopt,
                "binary frame declares an unsupported payload kind"};
    }
    std::uint64_t request_id = 0;
    if (!reader.read_u64(request_id)) {
        return {ProtocolError::truncated_message, std::nullopt,
                "binary frame is missing its request id"};
    }
    std::uint32_t declared_length = 0;
    if (!reader.read_u32(declared_length)) {
        return {ProtocolError::truncated_message, std::nullopt,
                "binary frame is missing its length prefix"};
    }
    if (declared_length > limits.max_binary_frame_bytes) {
        return {ProtocolError::binary_frame_too_large, std::nullopt,
                "binary frame payload exceeds the configured byte limit"};
    }
    std::string_view raw_payload;
    if (!reader.read_bytes(declared_length, raw_payload)) {
        return {ProtocolError::truncated_message, std::nullopt,
                "binary frame payload is truncated"};
    }
    if (!reader.at_end()) {
        return {ProtocolError::malformed_message, std::nullopt,
                "binary frame has unexpected trailing bytes"};
    }
    std::vector<std::uint8_t> owned_bytes{raw_payload.begin(), raw_payload.end()};
    return {ProtocolError::none,
            BinaryFrame{version, static_cast<BinaryPayloadKind>(kind_byte),
                       request_id, std::move(owned_bytes)},
            {}};
}

}  // namespace ssg
