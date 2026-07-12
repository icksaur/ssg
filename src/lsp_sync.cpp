#include <ssg/lsp_sync.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace ssg {
namespace {

LspFrameResult frame_failure(LspFrameError error, std::string message) {
    return {{}, error, std::move(message)};
}

std::string lower(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto ch : value) {
        result.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

struct Scalar {
    std::uint32_t value = 0;
    std::size_t bytes = 0;
};

std::optional<Scalar> decode_scalar(std::string_view text, std::size_t offset) {
    if (offset >= text.size()) {
        return std::nullopt;
    }
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first <= 0x7f) {
        return Scalar{first, 1};
    }
    std::size_t count = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
        count = 2;
        value = first & 0x1f;
        minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
        count = 3;
        value = first & 0x0f;
        minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
        count = 4;
        value = first & 0x07;
        minimum = 0x10000;
    } else {
        return std::nullopt;
    }
    if (offset + count > text.size()) {
        return std::nullopt;
    }
    for (std::size_t index = 1; index < count; ++index) {
        const auto byte = static_cast<unsigned char>(text[offset + index]);
        if ((byte & 0xc0) != 0x80) {
            return std::nullopt;
        }
        value = (value << 6) | (byte & 0x3f);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) {
        return std::nullopt;
    }
    return Scalar{value, count};
}

bool valid_utf8(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto scalar = decode_scalar(text, offset);
        if (!scalar) {
            return false;
        }
        offset += scalar->bytes;
    }
    return true;
}

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const auto ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (byte < 0x20) {
                result += "\\u00";
                result.push_back(hex[byte >> 4]);
                result.push_back(hex[byte & 0x0f]);
            } else {
                result.push_back(ch);
            }
        }
    }
    result.push_back('"');
    return result;
}

void append_utf8(std::string& target, std::uint32_t value) {
    if (value <= 0x7f) {
        target.push_back(static_cast<char>(value));
    } else if (value <= 0x7ff) {
        target.push_back(static_cast<char>(0xc0 | (value >> 6)));
        target.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else if (value <= 0xffff) {
        target.push_back(static_cast<char>(0xe0 | (value >> 12)));
        target.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else {
        target.push_back(static_cast<char>(0xf0 | (value >> 18)));
        target.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    }
}

struct Json {
    enum class Kind { null_value, boolean, number, string, array, object };
    Kind kind = Kind::null_value;
    bool boolean = false;
    std::string scalar;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    const Json* member(std::string_view name) const {
        const auto found = object.find(std::string{name});
        return found == object.end() ? nullptr : &found->second;
    }
    std::optional<std::int64_t> integer() const {
        if (kind != Kind::number) {
            return std::nullopt;
        }
        std::int64_t value = 0;
        const auto parsed =
            std::from_chars(scalar.data(), scalar.data() + scalar.size(), value);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != scalar.data() + scalar.size()) {
            return std::nullopt;
        }
        return value;
    }
};

class JsonParser {
public:
    JsonParser(std::string_view input, std::size_t maximum_depth)
        : input_(input), maximum_depth_(maximum_depth) {}

    std::optional<Json> parse() {
        auto value = parse_value(0);
        skip_space();
        if (!value || offset_ != input_.size()) {
            return std::nullopt;
        }
        return value;
    }

private:
    void skip_space() {
        while (offset_ < input_.size() &&
               (input_[offset_] == ' ' || input_[offset_] == '\t' ||
                input_[offset_] == '\r' || input_[offset_] == '\n')) {
            ++offset_;
        }
    }

    bool consume(char expected) {
        skip_space();
        if (offset_ >= input_.size() || input_[offset_] != expected) {
            return false;
        }
        ++offset_;
        return true;
    }

    std::optional<std::uint32_t> hex4() {
        if (offset_ + 4 > input_.size()) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char ch = input_[offset_++];
            value <<= 4;
            if (ch >= '0' && ch <= '9') value += ch - '0';
            else if (ch >= 'a' && ch <= 'f') value += ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') value += ch - 'A' + 10;
            else return std::nullopt;
        }
        return value;
    }

    std::optional<std::string> parse_string() {
        if (!consume('"')) {
            return std::nullopt;
        }
        std::string result;
        while (offset_ < input_.size()) {
            const char ch = input_[offset_++];
            if (ch == '"') {
                return valid_utf8(result) ? std::optional{std::move(result)}
                                          : std::nullopt;
            }
            if (static_cast<unsigned char>(ch) < 0x20) {
                return std::nullopt;
            }
            if (ch != '\\') {
                result.push_back(ch);
                continue;
            }
            if (offset_ >= input_.size()) {
                return std::nullopt;
            }
            switch (input_[offset_++]) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                auto first = hex4();
                if (!first) return std::nullopt;
                std::uint32_t value = *first;
                if (value >= 0xd800 && value <= 0xdbff) {
                    if (offset_ + 2 > input_.size() ||
                        input_[offset_] != '\\' || input_[offset_ + 1] != 'u') {
                        return std::nullopt;
                    }
                    offset_ += 2;
                    auto second = hex4();
                    if (!second || *second < 0xdc00 || *second > 0xdfff) {
                        return std::nullopt;
                    }
                    value = 0x10000 + ((value - 0xd800) << 10) +
                            (*second - 0xdc00);
                } else if (value >= 0xdc00 && value <= 0xdfff) {
                    return std::nullopt;
                }
                append_utf8(result, value);
                break;
            }
            default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<Json> parse_value(std::size_t depth) {
        skip_space();
        if (depth > maximum_depth_ || offset_ >= input_.size()) {
            return std::nullopt;
        }
        if (input_[offset_] == '"') {
            auto string = parse_string();
            if (!string) return std::nullopt;
            Json value;
            value.kind = Json::Kind::string;
            value.scalar = std::move(*string);
            return value;
        }
        if (input_[offset_] == '{') {
            ++offset_;
            Json value;
            value.kind = Json::Kind::object;
            skip_space();
            if (consume('}')) return value;
            while (true) {
                auto key = parse_string();
                if (!key || !consume(':')) return std::nullopt;
                auto child = parse_value(depth + 1);
                if (!child || !value.object.emplace(std::move(*key),
                                                    std::move(*child)).second) {
                    return std::nullopt;
                }
                if (consume('}')) return value;
                if (!consume(',')) return std::nullopt;
            }
        }
        if (input_[offset_] == '[') {
            ++offset_;
            Json value;
            value.kind = Json::Kind::array;
            skip_space();
            if (consume(']')) return value;
            while (true) {
                auto child = parse_value(depth + 1);
                if (!child) return std::nullopt;
                value.array.push_back(std::move(*child));
                if (consume(']')) return value;
                if (!consume(',')) return std::nullopt;
            }
        }
        for (const auto& literal :
             {std::pair{"null", Json::Kind::null_value},
              std::pair{"true", Json::Kind::boolean},
              std::pair{"false", Json::Kind::boolean}}) {
            const std::string_view word = literal.first;
            if (input_.substr(offset_, word.size()) == word) {
                offset_ += word.size();
                Json value;
                value.kind = literal.second;
                value.boolean = word == "true";
                return value;
            }
        }
        const auto start = offset_;
        if (input_[offset_] == '-') ++offset_;
        if (offset_ >= input_.size()) return std::nullopt;
        if (input_[offset_] == '0') {
            ++offset_;
        } else if (input_[offset_] >= '1' && input_[offset_] <= '9') {
            while (offset_ < input_.size() && std::isdigit(
                       static_cast<unsigned char>(input_[offset_]))) {
                ++offset_;
            }
        } else {
            return std::nullopt;
        }
        if (offset_ < input_.size() && input_[offset_] == '.') {
            ++offset_;
            const auto fraction = offset_;
            while (offset_ < input_.size() && std::isdigit(
                       static_cast<unsigned char>(input_[offset_]))) {
                ++offset_;
            }
            if (fraction == offset_) return std::nullopt;
        }
        if (offset_ < input_.size() &&
            (input_[offset_] == 'e' || input_[offset_] == 'E')) {
            ++offset_;
            if (offset_ < input_.size() &&
                (input_[offset_] == '+' || input_[offset_] == '-')) {
                ++offset_;
            }
            const auto exponent = offset_;
            while (offset_ < input_.size() && std::isdigit(
                       static_cast<unsigned char>(input_[offset_]))) {
                ++offset_;
            }
            if (exponent == offset_) return std::nullopt;
        }
        Json value;
        value.kind = Json::Kind::number;
        value.scalar = std::string{input_.substr(start, offset_ - start)};
        return value;
    }

    std::string_view input_;
    std::size_t maximum_depth_;
    std::size_t offset_ = 0;
};

std::optional<LspPosition> json_position(const Json& value) {
    if (value.kind != Json::Kind::object) return std::nullopt;
    const auto* line = value.member("line");
    const auto* character = value.member("character");
    if (!line || !character) return std::nullopt;
    const auto line_value = line->integer();
    const auto character_value = character->integer();
    if (!line_value || !character_value || *line_value < 0 ||
        *character_value < 0) {
        return std::nullopt;
    }
    return LspPosition{static_cast<std::uint64_t>(*line_value),
                       static_cast<std::uint64_t>(*character_value)};
}

LspSyncResult io_failure(const LspIoResult& result) {
    switch (result.status) {
    case LspIoStatus::timeout:
        return {LspSyncError::timeout, result.message};
    case LspIoStatus::closed:
        return {LspSyncError::stream_closed, result.message};
    case LspIoStatus::error:
        return {LspSyncError::stream_error, result.message};
    case LspIoStatus::ok:
        break;
    }
    return {};
}

} // namespace

std::string encode_lsp_frame(std::string_view payload) {
    return "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" +
           std::string{payload};
}

LspFrameDecoder::LspFrameDecoder(LspFrameConfig config) : config_(config) {
    if (config_.maximum_header_bytes == 0 ||
        config_.maximum_message_bytes == 0) {
        throw std::invalid_argument("LSP frame limits must be non-zero");
    }
}

LspFrameResult LspFrameDecoder::feed(std::string_view bytes) {
    if (failed_) {
        return frame_failure(LspFrameError::decoder_failed,
                             "LSP frame decoder is already failed");
    }
    buffer_.append(bytes);
    LspFrameResult result;
    while (true) {
        const auto end = buffer_.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (buffer_.size() > config_.maximum_header_bytes) {
                failed_ = true;
                return frame_failure(LspFrameError::header_too_large,
                                     "LSP header exceeds configured limit");
            }
            return result;
        }
        if (end + 4 > config_.maximum_header_bytes) {
            failed_ = true;
            return frame_failure(LspFrameError::header_too_large,
                                 "LSP header exceeds configured limit");
        }
        std::optional<std::size_t> length;
        std::size_t line_start = 0;
        while (line_start < end) {
            const auto line_end = buffer_.find("\r\n", line_start);
            const auto stop = line_end == std::string::npos ? end : line_end;
            const std::string_view line{buffer_.data() + line_start,
                                        stop - line_start};
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                failed_ = true;
                return frame_failure(LspFrameError::invalid_content_length,
                                     "malformed LSP header line");
            }
            auto name = lower(line.substr(0, colon));
            if (name == "content-length") {
                if (length) {
                    failed_ = true;
                    return frame_failure(
                        LspFrameError::duplicate_content_length,
                        "duplicate LSP Content-Length header");
                }
                auto value = line.substr(colon + 1);
                while (!value.empty() &&
                       (value.front() == ' ' || value.front() == '\t')) {
                    value.remove_prefix(1);
                }
                std::size_t parsed = 0;
                const auto converted = std::from_chars(
                    value.data(), value.data() + value.size(), parsed);
                if (value.empty() || converted.ec != std::errc{} ||
                    converted.ptr != value.data() + value.size()) {
                    failed_ = true;
                    return frame_failure(LspFrameError::invalid_content_length,
                                         "invalid LSP Content-Length");
                }
                length = parsed;
            }
            line_start = stop + 2;
        }
        if (!length) {
            failed_ = true;
            return frame_failure(LspFrameError::missing_content_length,
                                 "missing LSP Content-Length");
        }
        if (*length > config_.maximum_message_bytes) {
            failed_ = true;
            return frame_failure(LspFrameError::message_too_large,
                                 "LSP message exceeds configured limit");
        }
        const auto body = end + 4;
        if (buffer_.size() - body < *length) {
            return result;
        }
        result.messages.emplace_back(buffer_.substr(body, *length));
        buffer_.erase(0, body + *length);
    }
}

LspPositionResult byte_offset_to_lsp_position(std::string_view text,
                                              ByteOffset byte_offset) {
    if (!valid_utf8(text)) {
        return {{}, LspPositionError::invalid_utf8};
    }
    const auto requested = byte_offset.value();
    if (requested > text.size()) {
        return {{}, LspPositionError::invalid_utf8_boundary};
    }
    std::uint64_t line = 0;
    std::uint64_t character = 0;
    for (std::size_t offset = 0; offset < text.size();) {
        if (offset == requested) {
            return {{line, character}, LspPositionError::none};
        }
        const auto scalar = decode_scalar(text, offset);
        if (!scalar) {
            return {{}, LspPositionError::invalid_utf8};
        }
        if (offset + scalar->bytes > requested) {
            return {{}, LspPositionError::invalid_utf8_boundary};
        }
        if (scalar->value == '\r') {
            if (offset + 1 < text.size() && text[offset + 1] == '\n') {
                if (requested == offset + 1) {
                    return {{}, LspPositionError::invalid_utf8_boundary};
                }
                offset += 2;
            } else {
                ++offset;
            }
            ++line;
            character = 0;
        } else if (scalar->value == '\n') {
            offset += scalar->bytes;
            ++line;
            character = 0;
        } else {
            offset += scalar->bytes;
            character += scalar->value > 0xffff ? 2 : 1;
        }
    }
    if (requested == text.size()) {
        return {{line, character}, LspPositionError::none};
    }
    return {{}, LspPositionError::invalid_utf8_boundary};
}

LspByteOffsetResult lsp_position_to_byte_offset(std::string_view text,
                                                LspPosition position) {
    if (!valid_utf8(text)) {
        return {ByteOffset{}, LspPositionError::invalid_utf8};
    }
    std::uint64_t line = 0;
    std::uint64_t character = 0;
    for (std::size_t offset = 0;;) {
        if (line == position.line && character == position.character) {
            return {ByteOffset{offset}, LspPositionError::none};
        }
        if (offset >= text.size()) {
            return {ByteOffset{},
                    line < position.line ? LspPositionError::line_out_of_range
                                         : LspPositionError::character_out_of_range};
        }
        const auto scalar = decode_scalar(text, offset);
        if (!scalar) {
            return {ByteOffset{}, LspPositionError::invalid_utf8};
        }
        if (scalar->value == '\r' || scalar->value == '\n') {
            if (line == position.line) {
                return {ByteOffset{}, LspPositionError::character_out_of_range};
            }
            if (scalar->value == '\r' && offset + 1 < text.size() &&
                text[offset + 1] == '\n') {
                offset += 2;
            } else {
                offset += scalar->bytes;
            }
            ++line;
            character = 0;
            continue;
        }
        const std::uint64_t units = scalar->value > 0xffff ? 2 : 1;
        if (line == position.line && character < position.character &&
            position.character < character + units) {
            return {ByteOffset{}, LspPositionError::split_surrogate};
        }
        character += units;
        offset += scalar->bytes;
    }
}

LspSyncDelta derive_lsp_sync_delta(const LspSyncViewState& base,
                                   const LspSyncViewState& target) {
    return {base.revision, target.revision,
            base == target ? std::nullopt
                           : std::optional<LspSyncViewState>{target}};
}

LspSyncReplayResult replay_lsp_sync_delta(const LspSyncViewState& base,
                                          const LspSyncDelta& delta) {
    if (delta.base_revision != base.revision) {
        return {std::nullopt, LspSyncReplayError::stale_revision};
    }
    if (delta.revision < delta.base_revision ||
        (delta.state && delta.state->revision != delta.revision) ||
        (!delta.state && delta.revision != delta.base_revision)) {
        return {std::nullopt, LspSyncReplayError::malformed_delta};
    }
    return {delta.state ? delta.state
                        : std::optional<LspSyncViewState>{base},
            LspSyncReplayError::none};
}

struct LspSyncClient::Impl {
    struct DocumentState {
        std::string language_id;
        std::int64_t version = 1;
        Revision revision{0};
        std::string text;
    };

    LspByteStream* stream;
    LspSyncConfig config;
    std::chrono::milliseconds timeout;
    LspFrameDecoder decoder;
    LspLifecycleState lifecycle = LspLifecycleState::stopped;
    LspSyncViewState view;
    std::map<std::string, DocumentState> documents;
    std::set<std::uint64_t> pending;
    std::set<std::uint64_t> cancelled;
    std::uint64_t next_id = 1;
    std::uint64_t initialize_id = 0;
    std::uint64_t shutdown_id = 0;

    Impl(LspByteStream& source, LspSyncConfig limits,
         std::chrono::milliseconds io_timeout)
        : stream(&source), config(limits), timeout(io_timeout),
          decoder(config.framing) {
        if (timeout.count() < 0 || config.maximum_read_bytes == 0 ||
            config.maximum_documents == 0 ||
            config.maximum_pending_requests == 0 ||
            config.maximum_json_depth == 0) {
            throw std::invalid_argument("LSP sync limits must be non-zero");
        }
    }

    LspSyncResult send(std::string_view payload) {
        const auto result = stream->write(encode_lsp_frame(payload), timeout);
        return result.accepted() ? LspSyncResult{} : io_failure(result);
    }

    LspRequestResult send_request(std::string method,
                                  std::string params_json) {
        if (pending.size() >= config.maximum_pending_requests) {
            return {0, LspSyncError::request_limit_exceeded,
                    "LSP pending request limit exceeded"};
        }
        if (next_id == std::numeric_limits<std::uint64_t>::max()) {
            return {0, LspSyncError::request_limit_exceeded,
                    "LSP request id space is exhausted"};
        }
        const auto id = next_id;
        const auto payload =
            "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
            ",\"method\":" + json_escape(method) + ",\"params\":" +
            params_json + "}";
        const auto sent = send(payload);
        if (!sent.accepted()) {
            return {0, sent.error, sent.message};
        }
        ++next_id;
        pending.insert(id);
        return {id, LspSyncError::none, {}};
    }

    LspSyncResult fail_malformed(std::string message) {
        lifecycle = LspLifecycleState::failed;
        return {LspSyncError::malformed_message, std::move(message)};
    }

    LspSyncResult reject_diagnostics(std::string message) {
        return {LspSyncError::malformed_message, std::move(message)};
    }

    LspSyncResult publish_diagnostics(const Json& params) {
        if (params.kind != Json::Kind::object) {
            return reject_diagnostics("diagnostic params must be an object");
        }
        const auto* uri_value = params.member("uri");
        const auto* items = params.member("diagnostics");
        if (!uri_value || uri_value->kind != Json::Kind::string || !items ||
            items->kind != Json::Kind::array) {
            return reject_diagnostics(
                "diagnostic params are missing uri or diagnostics");
        }
        const auto document = documents.find(uri_value->scalar);
        if (document == documents.end()) {
            return {LspSyncError::unknown_document,
                    "diagnostics target an unknown document"};
        }
        if (const auto* version = params.member("version")) {
            const auto value = version->integer();
            if (!value || *value != document->second.version) {
                return {LspSyncError::stale_diagnostics,
                        "diagnostics carry a stale document version"};
            }
        }
        if (items->array.size() > config.maximum_diagnostics_per_document) {
            return {LspSyncError::diagnostic_limit_exceeded,
                    "diagnostic count exceeds configured limit"};
        }
        std::vector<LspDiagnostic> parsed;
        parsed.reserve(items->array.size());
        std::size_t message_bytes = 0;
        for (const auto& item : items->array) {
            if (item.kind != Json::Kind::object) {
                return reject_diagnostics(
                    "diagnostic entry must be an object");
            }
            const auto* range = item.member("range");
            const auto* message = item.member("message");
            if (!range || range->kind != Json::Kind::object || !message ||
                message->kind != Json::Kind::string) {
                return reject_diagnostics(
                    "diagnostic entry is missing range or message");
            }
            const auto* start = range->member("start");
            const auto* end = range->member("end");
            if (!start || !end) {
                return reject_diagnostics("diagnostic range is incomplete");
            }
            auto start_position = json_position(*start);
            auto end_position = json_position(*end);
            const auto start_offset = start_position
                                          ? lsp_position_to_byte_offset(
                                                document->second.text,
                                                *start_position)
                                          : LspByteOffsetResult{
                                                ByteOffset{0},
                                                LspPositionError::invalid_utf8};
            const auto end_offset =
                end_position
                    ? lsp_position_to_byte_offset(document->second.text,
                                                  *end_position)
                    : LspByteOffsetResult{ByteOffset{0},
                                          LspPositionError::invalid_utf8};
            if (!start_position || !end_position || !start_offset.accepted() ||
                !end_offset.accepted() ||
                end_offset.offset < start_offset.offset) {
                return reject_diagnostics(
                    "diagnostic range is outside the document");
            }
            LspDiagnostic diagnostic;
            diagnostic.range = {*start_position, *end_position};
            diagnostic.message = message->scalar;
            message_bytes += diagnostic.message.size();
            if (message_bytes > config.maximum_diagnostic_message_bytes) {
                return {LspSyncError::diagnostic_limit_exceeded,
                        "diagnostic messages exceed configured limit"};
            }
            if (const auto* severity = item.member("severity")) {
                const auto value = severity->integer();
                if (!value || *value < 1 || *value > 4) {
                    return reject_diagnostics(
                        "diagnostic severity is invalid");
                }
                diagnostic.severity =
                    static_cast<LspDiagnosticSeverity>(*value);
            }
            if (const auto* code = item.member("code")) {
                if (code->kind != Json::Kind::string &&
                    code->kind != Json::Kind::number) {
                    return reject_diagnostics("diagnostic code is invalid");
                }
                diagnostic.code = code->scalar;
            }
            parsed.push_back(std::move(diagnostic));
        }
        auto updated = view;
        auto found = std::lower_bound(
            updated.documents.begin(), updated.documents.end(),
            uri_value->scalar,
            [](const LspDocumentDiagnostics& left, const std::string& right) {
                return left.uri < right;
            });
        LspDocumentDiagnostics replacement{uri_value->scalar,
                                           document->second.revision,
                                           std::move(parsed)};
        if (found != updated.documents.end() &&
            found->uri == uri_value->scalar) {
            *found = std::move(replacement);
        } else {
            updated.documents.insert(found, std::move(replacement));
        }
        updated.revision = Revision{view.revision.value() + 1};
        view = std::move(updated);
        return {};
    }

    LspSyncResult process(const Json& message) {
        if (message.kind != Json::Kind::object) {
            return fail_malformed("LSP message must be an object");
        }
        const auto* rpc = message.member("jsonrpc");
        if (!rpc || rpc->kind != Json::Kind::string ||
            rpc->scalar != "2.0") {
            return fail_malformed("LSP message has invalid jsonrpc version");
        }
        if (const auto* method = message.member("method")) {
            if (method->kind != Json::Kind::string) {
                return fail_malformed("LSP method must be a string");
            }
            if (method->scalar == "textDocument/publishDiagnostics") {
                const auto* params = message.member("params");
                if (!params) return fail_malformed("diagnostics missing params");
                return publish_diagnostics(*params);
            }
            return {};
        }
        const auto* id = message.member("id");
        if (!id) {
            return fail_malformed("LSP response is missing id");
        }
        const auto number = id->integer();
        if (!number || *number <= 0) {
            return fail_malformed("LSP response id is invalid");
        }
        const auto request_id = static_cast<std::uint64_t>(*number);
        if (cancelled.erase(request_id) != 0) {
            pending.erase(request_id);
            return {};
        }
        if (pending.erase(request_id) == 0) {
            return fail_malformed("LSP response id is unknown");
        }
        if (message.member("error")) {
            if (request_id == initialize_id) lifecycle = LspLifecycleState::failed;
            return {LspSyncError::server_error, "LSP server returned an error"};
        }
        if (!message.member("result")) {
            return fail_malformed("LSP response has neither result nor error");
        }
        if (request_id == initialize_id &&
            lifecycle == LspLifecycleState::initializing) {
            const auto initialized = send(
                "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\","
                "\"params\":{}}");
            if (!initialized.accepted()) {
                lifecycle = LspLifecycleState::failed;
                return initialized;
            }
            lifecycle = LspLifecycleState::ready;
        } else if (request_id == shutdown_id &&
                   lifecycle == LspLifecycleState::shutting_down) {
            const auto exited =
                send("{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":null}");
            if (!exited.accepted()) {
                lifecycle = LspLifecycleState::failed;
                return exited;
            }
            lifecycle = LspLifecycleState::stopped;
        }
        return {};
    }
};

LspSyncClient::LspSyncClient(LspByteStream& stream, LspSyncConfig config,
                             std::chrono::milliseconds io_timeout)
    : impl_(std::make_unique<Impl>(stream, config, io_timeout)) {}
LspSyncClient::~LspSyncClient() = default;
LspSyncClient::LspSyncClient(LspSyncClient&&) noexcept = default;
LspSyncClient& LspSyncClient::operator=(LspSyncClient&&) noexcept = default;

LspSyncResult LspSyncClient::initialize(std::string root_uri) {
    if (impl_->lifecycle != LspLifecycleState::stopped || root_uri.empty()) {
        return {LspSyncError::invalid_state,
                "LSP initialize requires a stopped client and root URI"};
    }
    const auto result = impl_->send_request(
        "initialize",
        "{\"processId\":null,\"rootUri\":" + json_escape(root_uri) +
            ",\"capabilities\":{}}");
    if (!result.accepted()) {
        return {result.error, result.message};
    }
    impl_->initialize_id = result.id;
    impl_->lifecycle = LspLifecycleState::initializing;
    return {};
}

LspSyncResult LspSyncClient::shutdown() {
    if (impl_->lifecycle != LspLifecycleState::ready) {
        return {LspSyncError::invalid_state,
                "LSP shutdown requires a ready client"};
    }
    const auto result = impl_->send_request("shutdown", "null");
    if (!result.accepted()) {
        return {result.error, result.message};
    }
    impl_->shutdown_id = result.id;
    impl_->lifecycle = LspLifecycleState::shutting_down;
    return {};
}

LspSyncResult LspSyncClient::poll() {
    if (impl_->lifecycle == LspLifecycleState::stopped ||
        impl_->lifecycle == LspLifecycleState::failed) {
        return {LspSyncError::invalid_state,
                "LSP poll requires an active client"};
    }
    std::string bytes;
    const auto read = impl_->stream->read(
        bytes, impl_->config.maximum_read_bytes, impl_->timeout);
    if (!read.accepted()) {
        return io_failure(read);
    }
    if (bytes.empty()) {
        return {LspSyncError::stream_closed,
                "LSP stream returned an empty successful read"};
    }
    auto framed = impl_->decoder.feed(bytes);
    if (!framed.accepted()) {
        return impl_->fail_malformed(framed.message);
    }
    std::vector<Json> messages;
    messages.reserve(framed.messages.size());
    for (const auto& payload : framed.messages) {
        auto parsed =
            JsonParser{payload, impl_->config.maximum_json_depth}.parse();
        if (!parsed) {
            return impl_->fail_malformed("LSP payload is malformed JSON");
        }
        messages.push_back(std::move(*parsed));
    }
    LspSyncResult first_error;
    for (const auto& message : messages) {
        auto result = impl_->process(message);
        if (impl_->lifecycle == LspLifecycleState::failed) {
            return result;
        }
        if (!result.accepted() && first_error.accepted()) {
            first_error = std::move(result);
        }
    }
    return first_error;
}

LspSyncResult LspSyncClient::open_document(
    std::string uri, std::string language_id, Revision revision,
    std::string text) {
    if (impl_->lifecycle != LspLifecycleState::ready) {
        return {LspSyncError::invalid_state,
                "opening an LSP document requires a ready client"};
    }
    if (uri.empty() || language_id.empty() || revision.value() == 0 ||
        !valid_utf8(text)) {
        return {LspSyncError::invalid_argument,
                "LSP document URI, language, revision, or UTF-8 is invalid"};
    }
    if (impl_->documents.contains(uri)) {
        return {LspSyncError::invalid_state, "LSP document is already open"};
    }
    if (impl_->documents.size() >= impl_->config.maximum_documents) {
        return {LspSyncError::request_limit_exceeded,
                "LSP document limit exceeded"};
    }
    const auto payload =
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
        "\"params\":{\"textDocument\":{\"uri\":" +
        json_escape(uri) + ",\"languageId\":" + json_escape(language_id) +
        ",\"version\":1,\"text\":" + json_escape(text) + "}}}";
    auto sent = impl_->send(payload);
    if (!sent.accepted()) return sent;
    impl_->documents.emplace(
        std::move(uri),
        Impl::DocumentState{std::move(language_id), 1, revision, std::move(text)});
    return {};
}

LspSyncResult LspSyncClient::change_document(std::string_view uri,
                                             Revision revision,
                                             std::string text) {
    if (impl_->lifecycle != LspLifecycleState::ready) {
        return {LspSyncError::invalid_state,
                "changing an LSP document requires a ready client"};
    }
    const auto found = impl_->documents.find(std::string{uri});
    if (found == impl_->documents.end()) {
        return {LspSyncError::unknown_document, "LSP document is not open"};
    }
    if (revision <= found->second.revision) {
        return {LspSyncError::stale_document,
                "LSP document change revision is stale"};
    }
    if (!valid_utf8(text) ||
        found->second.version == std::numeric_limits<std::int64_t>::max()) {
        return {LspSyncError::invalid_argument,
                "LSP document text or next version is invalid"};
    }
    const auto version = found->second.version + 1;
    const auto payload =
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":" +
        json_escape(uri) + ",\"version\":" + std::to_string(version) +
        "},\"contentChanges\":[{\"text\":" + json_escape(text) + "}]}}";
    auto sent = impl_->send(payload);
    if (!sent.accepted()) return sent;
    found->second.version = version;
    found->second.revision = revision;
    found->second.text = std::move(text);
    return {};
}

LspSyncResult LspSyncClient::close_document(std::string_view uri) {
    if (impl_->lifecycle != LspLifecycleState::ready) {
        return {LspSyncError::invalid_state,
                "closing an LSP document requires a ready client"};
    }
    const auto found = impl_->documents.find(std::string{uri});
    if (found == impl_->documents.end()) {
        return {LspSyncError::unknown_document, "LSP document is not open"};
    }
    const auto payload =
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\","
        "\"params\":{\"textDocument\":{\"uri\":" +
        json_escape(uri) + "}}}";
    auto sent = impl_->send(payload);
    if (!sent.accepted()) return sent;
    impl_->documents.erase(found);
    auto diagnostic = std::find_if(
        impl_->view.documents.begin(), impl_->view.documents.end(),
        [&](const LspDocumentDiagnostics& value) { return value.uri == uri; });
    if (diagnostic != impl_->view.documents.end()) {
        impl_->view.documents.erase(diagnostic);
        impl_->view.revision = Revision{impl_->view.revision.value() + 1};
    }
    return {};
}

std::optional<std::int64_t> LspSyncClient::document_version(
    std::string_view uri) const {
    const auto found = impl_->documents.find(std::string{uri});
    return found == impl_->documents.end()
               ? std::nullopt
               : std::optional<std::int64_t>{found->second.version};
}

LspRequestResult LspSyncClient::request(std::string method,
                                        std::string params_json) {
    if (impl_->lifecycle != LspLifecycleState::ready) {
        return {0, LspSyncError::invalid_state,
                "LSP request requires a ready client"};
    }
    if (method.empty()) {
        return {0, LspSyncError::invalid_argument,
                "LSP request method must not be empty"};
    }
    auto params =
        JsonParser{params_json, impl_->config.maximum_json_depth}.parse();
    if (!params || (params->kind != Json::Kind::object &&
                    params->kind != Json::Kind::array &&
                    params->kind != Json::Kind::null_value)) {
        return {0, LspSyncError::invalid_argument,
                "LSP request params must be valid JSON parameters"};
    }
    return impl_->send_request(std::move(method), std::move(params_json));
}

LspSyncResult LspSyncClient::cancel(std::uint64_t request_id) {
    if (!impl_->pending.contains(request_id) ||
        impl_->cancelled.contains(request_id) ||
        request_id == impl_->initialize_id || request_id == impl_->shutdown_id) {
        return {LspSyncError::unknown_request,
                "LSP cancellation targets an unknown request"};
    }
    const auto payload =
        "{\"jsonrpc\":\"2.0\",\"method\":\"$/cancelRequest\","
        "\"params\":{\"id\":" +
        std::to_string(request_id) + "}}";
    auto sent = impl_->send(payload);
    if (!sent.accepted()) return sent;
    impl_->cancelled.insert(request_id);
    return {};
}

LspLifecycleState LspSyncClient::state() const noexcept {
    return impl_->lifecycle;
}

const LspSyncViewState& LspSyncClient::view_state() const noexcept {
    return impl_->view;
}

} // namespace ssg
