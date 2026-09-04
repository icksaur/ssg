#include <ssg/lsp_sync_client.h>
#include <ssg/startup_audit.h>

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

LspFrameResult frameFailure(LspFrameError error, std::string message) {
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

std::optional<Scalar> decodeScalar(std::string_view text, std::size_t offset) {
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

bool validUtf8(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto scalar = decodeScalar(text, offset);
        if (!scalar) {
            return false;
        }
        offset += scalar->bytes;
    }
    return true;
}

std::string jsonEscape(std::string_view value) {
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

void appendUtf8(std::string& target, std::uint32_t value) {
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
    enum class Kind { NullValue, Boolean, Number, String, Array, Object };
    Kind kind = Kind::NullValue;
    bool boolean = false;
    std::string scalar;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    const Json* member(std::string_view name) const {
        const auto found = object.find(std::string{name});
        return found == object.end() ? nullptr : &found->second;
    }
    std::optional<std::int64_t> integer() const {
        if (kind != Kind::Number) {
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
    JsonParser(std::string_view input, std::size_t maximumDepth)
        : input_(input), maximumDepth_(maximumDepth) {}

    std::optional<Json> parse() {
        auto value = parseValue(0);
        skipSpace();
        if (!value || offset_ != input_.size()) {
            return std::nullopt;
        }
        return value;
    }

private:
    void skipSpace() {
        while (offset_ < input_.size() &&
               (input_[offset_] == ' ' || input_[offset_] == '\t' ||
                input_[offset_] == '\r' || input_[offset_] == '\n')) {
            ++offset_;
        }
    }

    bool consume(char expected) {
        skipSpace();
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

    std::optional<std::string> parseString() {
        if (!consume('"')) {
            return std::nullopt;
        }
        std::string result;
        while (offset_ < input_.size()) {
            const char ch = input_[offset_++];
            if (ch == '"') {
                return validUtf8(result) ? std::optional{std::move(result)}
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
                appendUtf8(result, value);
                break;
            }
            default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<Json> parseValue(std::size_t depth) {
        skipSpace();
        if (depth > maximumDepth_ || offset_ >= input_.size()) {
            return std::nullopt;
        }
        if (input_[offset_] == '"') {
            auto string = parseString();
            if (!string) return std::nullopt;
            Json value;
            value.kind = Json::Kind::String;
            value.scalar = std::move(*string);
            return value;
        }
        if (input_[offset_] == '{') {
            ++offset_;
            Json value;
            value.kind = Json::Kind::Object;
            skipSpace();
            if (consume('}')) return value;
            while (true) {
                auto key = parseString();
                if (!key || !consume(':')) return std::nullopt;
                auto child = parseValue(depth + 1);
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
            value.kind = Json::Kind::Array;
            skipSpace();
            if (consume(']')) return value;
            while (true) {
                auto child = parseValue(depth + 1);
                if (!child) return std::nullopt;
                value.array.push_back(std::move(*child));
                if (consume(']')) return value;
                if (!consume(',')) return std::nullopt;
            }
        }
        for (const auto& literal :
             {std::pair{"null", Json::Kind::NullValue},
              std::pair{"true", Json::Kind::Boolean},
              std::pair{"false", Json::Kind::Boolean}}) {
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
        value.kind = Json::Kind::Number;
        value.scalar = std::string{input_.substr(start, offset_ - start)};
        return value;
    }

    std::string_view input_;
    std::size_t maximumDepth_;
    std::size_t offset_ = 0;
};

std::optional<LspPosition> jsonPosition(const Json& value) {
    if (value.kind != Json::Kind::Object) return std::nullopt;
    const auto* line = value.member("line");
    const auto* character = value.member("character");
    if (!line || !character) return std::nullopt;
    const auto lineValue = line->integer();
    const auto characterValue = character->integer();
    if (!lineValue || !characterValue || *lineValue < 0 ||
        *characterValue < 0) {
        return std::nullopt;
    }
    return LspPosition{static_cast<std::uint64_t>(*lineValue),
                       static_cast<std::uint64_t>(*characterValue)};
}

LspSyncResult ioFailure(const LspIoResult& result) {
    switch (result.status) {
    case LspIoStatus::Timeout:
        return {LspSyncError::Timeout, result.message};
    case LspIoStatus::Closed:
        return {LspSyncError::StreamClosed, result.message};
    case LspIoStatus::Error:
        return {LspSyncError::StreamError, result.message};
    case LspIoStatus::Ok:
        break;
    }
    return {};
}

} // namespace

std::string encodeLspFrame(std::string_view payload) {
    return "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" +
           std::string{payload};
}

LspFrameDecoder::LspFrameDecoder(LspFrameConfig config) : config_(config) {
    if (config_.maximumHeaderBytes == 0 ||
        config_.maximumMessageBytes == 0) {
        throw std::invalid_argument("LSP frame limits must be non-zero");
    }
}

LspFrameResult LspFrameDecoder::feed(std::string_view bytes) {
    if (failed_) {
        return frameFailure(LspFrameError::DecoderFailed,
                             "LSP frame decoder is already failed");
    }
    buffer_.append(bytes);
    LspFrameResult result;
    while (true) {
        const auto end = buffer_.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (buffer_.size() > config_.maximumHeaderBytes) {
                failed_ = true;
                return frameFailure(LspFrameError::HeaderTooLarge,
                                     "LSP header exceeds configured limit");
            }
            return result;
        }
        if (end + 4 > config_.maximumHeaderBytes) {
            failed_ = true;
            return frameFailure(LspFrameError::HeaderTooLarge,
                                 "LSP header exceeds configured limit");
        }
        std::optional<std::size_t> length;
        std::size_t lineStart = 0;
        while (lineStart < end) {
            const auto lineEnd = buffer_.find("\r\n", lineStart);
            const auto stop = lineEnd == std::string::npos ? end : lineEnd;
            const std::string_view line{buffer_.data() + lineStart,
                                        stop - lineStart};
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                failed_ = true;
                return frameFailure(LspFrameError::InvalidContentLength,
                                     "malformed LSP header line");
            }
            auto name = lower(line.substr(0, colon));
            if (name == "content-length") {
                if (length) {
                    failed_ = true;
                    return frameFailure(
                        LspFrameError::DuplicateContentLength,
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
                    return frameFailure(LspFrameError::InvalidContentLength,
                                         "invalid LSP Content-Length");
                }
                length = parsed;
            }
            lineStart = stop + 2;
        }
        if (!length) {
            failed_ = true;
            return frameFailure(LspFrameError::MissingContentLength,
                                 "missing LSP Content-Length");
        }
        if (*length > config_.maximumMessageBytes) {
            failed_ = true;
            return frameFailure(LspFrameError::MessageTooLarge,
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

LspPositionResult byteOffsetToLspPosition(std::string_view text,
                                              ByteOffset byteOffset) {
    if (!validUtf8(text)) {
        return {{}, LspPositionError::InvalidUtf8};
    }
    const auto requested = byteOffset.value();
    if (requested > text.size()) {
        return {{}, LspPositionError::InvalidUtf8Boundary};
    }
    std::uint64_t line = 0;
    std::uint64_t character = 0;
    for (std::size_t offset = 0; offset < text.size();) {
        if (offset == requested) {
            return {{line, character}, LspPositionError::None};
        }
        const auto scalar = decodeScalar(text, offset);
        if (!scalar) {
            return {{}, LspPositionError::InvalidUtf8};
        }
        if (offset + scalar->bytes > requested) {
            return {{}, LspPositionError::InvalidUtf8Boundary};
        }
        if (scalar->value == '\r') {
            if (offset + 1 < text.size() && text[offset + 1] == '\n') {
                if (requested == offset + 1) {
                    return {{}, LspPositionError::InvalidUtf8Boundary};
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
        return {{line, character}, LspPositionError::None};
    }
    return {{}, LspPositionError::InvalidUtf8Boundary};
}

LspByteOffsetResult lspPositionToByteOffset(std::string_view text,
                                                LspPosition position) {
    if (!validUtf8(text)) {
        return {ByteOffset{}, LspPositionError::InvalidUtf8};
    }
    std::uint64_t line = 0;
    std::uint64_t character = 0;
    for (std::size_t offset = 0;;) {
        if (line == position.line && character == position.character) {
            return {ByteOffset{offset}, LspPositionError::None};
        }
        if (offset >= text.size()) {
            return {ByteOffset{},
                    line < position.line ? LspPositionError::LineOutOfRange
                                         : LspPositionError::CharacterOutOfRange};
        }
        const auto scalar = decodeScalar(text, offset);
        if (!scalar) {
            return {ByteOffset{}, LspPositionError::InvalidUtf8};
        }
        if (scalar->value == '\r' || scalar->value == '\n') {
            if (line == position.line) {
                return {ByteOffset{}, LspPositionError::CharacterOutOfRange};
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
            return {ByteOffset{}, LspPositionError::SplitSurrogate};
        }
        character += units;
        offset += scalar->bytes;
    }
}

struct LspSyncClient::Impl {
    struct DocumentState {
        std::string languageId;
        std::int64_t version = 1;
        std::uint64_t revision{0};
        std::string text;
    };

    LspByteStream* stream;
    LspSyncConfig config;
    std::chrono::milliseconds timeout;
    LspFrameDecoder decoder;
    LspLifecycleState lifecycle = LspLifecycleState::Stopped;
    LspSyncViewState view;
    std::map<std::string, DocumentState> documents;
    std::set<std::uint64_t> pending;
    std::set<std::uint64_t> cancelled;
    std::vector<LspCompletedResponse> completed;
    std::uint64_t nextId = 1;
    std::uint64_t initializeId = 0;
    std::uint64_t shutdownId = 0;

    Impl(LspByteStream& source, LspSyncConfig limits,
         std::chrono::milliseconds ioTimeout)
        : stream(&source), config(limits), timeout(ioTimeout),
          decoder(config.framing) {
        noteOptionalConstruction(OptionalSubsystem::Lsp);
        if (timeout.count() < 0 || config.maximumReadBytes == 0 ||
            config.maximumDocuments == 0 ||
            config.maximumPendingRequests == 0 ||
            config.maximumJsonDepth == 0) {
            throw std::invalid_argument("LSP sync limits must be non-zero");
        }
    }

    LspSyncResult send(std::string_view payload) {
        const auto result = stream->write(encodeLspFrame(payload), timeout);
        return result.accepted() ? LspSyncResult{} : ioFailure(result);
    }

    LspRequestResult sendRequest(std::string method,
                                  std::string paramsJson) {
        if (pending.size() >= config.maximumPendingRequests) {
            return {0, LspSyncError::RequestLimitExceeded,
                    "LSP pending request limit exceeded"};
        }
        if (nextId == std::numeric_limits<std::uint64_t>::max()) {
            return {0, LspSyncError::RequestLimitExceeded,
                    "LSP request id space is exhausted"};
        }
        const auto id = nextId;
        const auto payload =
            "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
            ",\"method\":" + jsonEscape(method) + ",\"params\":" +
            paramsJson + "}";
        const auto sent = send(payload);
        if (!sent.accepted()) {
            return {0, sent.error, sent.message};
        }
        ++nextId;
        pending.insert(id);
        return {id, LspSyncError::None, {}};
    }

    LspSyncResult failMalformed(std::string message) {
        lifecycle = LspLifecycleState::Failed;
        return {LspSyncError::MalformedMessage, std::move(message)};
    }

    LspSyncResult rejectDiagnostics(std::string message) {
        return {LspSyncError::MalformedMessage, std::move(message)};
    }

    LspSyncResult publishDiagnostics(const Json& params) {
        if (params.kind != Json::Kind::Object) {
            return rejectDiagnostics("diagnostic params must be an object");
        }
        const auto* uriValue = params.member("uri");
        const auto* items = params.member("diagnostics");
        if (!uriValue || uriValue->kind != Json::Kind::String || !items ||
            items->kind != Json::Kind::Array) {
            return rejectDiagnostics(
                "diagnostic params are missing uri or diagnostics");
        }
        const auto document = documents.find(uriValue->scalar);
        if (document == documents.end()) {
            return {LspSyncError::UnknownDocument,
                    "diagnostics target an unknown document"};
        }
        if (const auto* version = params.member("version")) {
            const auto value = version->integer();
            if (!value || *value != document->second.version) {
                return {LspSyncError::StaleDiagnostics,
                        "diagnostics carry a stale document version"};
            }
        }
        if (items->array.size() > config.maximumDiagnosticsPerDocument) {
            return {LspSyncError::DiagnosticLimitExceeded,
                    "diagnostic count exceeds configured limit"};
        }
        std::vector<LspDiagnostic> parsed;
        parsed.reserve(items->array.size());
        std::size_t messageBytes = 0;
        for (const auto& item : items->array) {
            if (item.kind != Json::Kind::Object) {
                return rejectDiagnostics(
                    "diagnostic entry must be an object");
            }
            const auto* range = item.member("range");
            const auto* message = item.member("message");
            if (!range || range->kind != Json::Kind::Object || !message ||
                message->kind != Json::Kind::String) {
                return rejectDiagnostics(
                    "diagnostic entry is missing range or message");
            }
            const auto* start = range->member("start");
            const auto* end = range->member("end");
            if (!start || !end) {
                return rejectDiagnostics("diagnostic range is incomplete");
            }
            auto startPosition = jsonPosition(*start);
            auto endPosition = jsonPosition(*end);
            const auto startOffset = startPosition
                                          ? lspPositionToByteOffset(
                                                document->second.text,
                                                *startPosition)
                                          : LspByteOffsetResult{
                                                ByteOffset{0},
                                                LspPositionError::InvalidUtf8};
            const auto endOffset =
                endPosition
                    ? lspPositionToByteOffset(document->second.text,
                                                  *endPosition)
                    : LspByteOffsetResult{ByteOffset{0},
                                          LspPositionError::InvalidUtf8};
            if (!startPosition || !endPosition || !startOffset.accepted() ||
                !endOffset.accepted() ||
                endOffset.offset < startOffset.offset) {
                return rejectDiagnostics(
                    "diagnostic range is outside the document");
            }
            LspDiagnostic diagnostic;
            diagnostic.range = {*startPosition, *endPosition};
            diagnostic.message = message->scalar;
            messageBytes += diagnostic.message.size();
            if (messageBytes > config.maximumDiagnosticMessageBytes) {
                return {LspSyncError::DiagnosticLimitExceeded,
                        "diagnostic messages exceed configured limit"};
            }
            if (const auto* severity = item.member("severity")) {
                const auto value = severity->integer();
                if (!value || *value < 1 || *value > 4) {
                    return rejectDiagnostics(
                        "diagnostic severity is invalid");
                }
                diagnostic.severity =
                    static_cast<LspDiagnosticSeverity>(*value);
            }
            if (const auto* code = item.member("code")) {
                if (code->kind != Json::Kind::String &&
                    code->kind != Json::Kind::Number) {
                    return rejectDiagnostics("diagnostic code is invalid");
                }
                diagnostic.code = code->scalar;
            }
            parsed.push_back(std::move(diagnostic));
        }
        auto updated = view;
        auto found = std::lower_bound(
            updated.documents.begin(), updated.documents.end(),
            uriValue->scalar,
            [](const LspDocumentDiagnostics& left, const std::string& right) {
                return left.uri < right;
            });
        LspDocumentDiagnostics replacement{uriValue->scalar,
                                           document->second.revision,
                                           std::move(parsed)};
        if (found != updated.documents.end() &&
            found->uri == uriValue->scalar) {
            *found = std::move(replacement);
        } else {
            updated.documents.insert(found, std::move(replacement));
        }
        updated.revision = std::uint64_t{view.revision + 1};
        view = std::move(updated);
        return {};
    }

    LspSyncResult process(const Json& message, std::string_view payloadJson) {
        if (message.kind != Json::Kind::Object) {
            return failMalformed("LSP message must be an object");
        }
        const auto* rpc = message.member("jsonrpc");
        if (!rpc || rpc->kind != Json::Kind::String ||
            rpc->scalar != "2.0") {
            return failMalformed("LSP message has invalid jsonrpc version");
        }
        if (const auto* method = message.member("method")) {
            if (method->kind != Json::Kind::String) {
                return failMalformed("LSP method must be a string");
            }
            if (method->scalar == "textDocument/publishDiagnostics") {
                const auto* params = message.member("params");
                if (!params) return failMalformed("diagnostics missing params");
                return publishDiagnostics(*params);
            }
            return {};
        }
        const auto* id = message.member("id");
        if (!id) {
            return failMalformed("LSP response is missing id");
        }
        const auto number = id->integer();
        if (!number || *number <= 0) {
            return failMalformed("LSP response id is invalid");
        }
        const auto requestId = static_cast<std::uint64_t>(*number);
        if (cancelled.erase(requestId) != 0) {
            pending.erase(requestId);
            completed.push_back(
                {requestId, LspCompletedResponseStatus::Cancelled, {}, {}});
            return {};
        }
        if (pending.erase(requestId) == 0) {
            return failMalformed("LSP response id is unknown");
        }
        if (message.member("error")) {
            if (requestId == initializeId) lifecycle = LspLifecycleState::Failed;
            if (requestId == initializeId || requestId == shutdownId) {
                return {LspSyncError::ServerError,
                        "LSP server returned an error"};
            }
            completed.push_back(
                {requestId, LspCompletedResponseStatus::ServerError,
                 std::string{payloadJson}, "LSP server returned an error"});
            return {};
        }
        if (!message.member("result")) {
            return failMalformed("LSP response has neither result nor error");
        }
        if (requestId == initializeId &&
            lifecycle == LspLifecycleState::Initializing) {
            const auto initialized = send(
                "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\","
                "\"params\":{}}");
            if (!initialized.accepted()) {
                lifecycle = LspLifecycleState::Failed;
                return initialized;
            }
            lifecycle = LspLifecycleState::Ready;
        } else if (requestId == shutdownId &&
                   lifecycle == LspLifecycleState::ShuttingDown) {
            const auto exited =
                send("{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":null}");
            if (!exited.accepted()) {
                lifecycle = LspLifecycleState::Failed;
                return exited;
            }
            lifecycle = LspLifecycleState::Stopped;
        } else {
            completed.push_back(
                {requestId, LspCompletedResponseStatus::Result,
                 std::string{payloadJson}, {}});
        }
        return {};
    }
};

LspSyncClient::LspSyncClient(LspByteStream& stream, LspSyncConfig config,
                             std::chrono::milliseconds ioTimeout)
    : impl_(std::make_unique<Impl>(stream, config, ioTimeout)) {}
LspSyncClient::~LspSyncClient() = default;
LspSyncClient::LspSyncClient(LspSyncClient&&) noexcept = default;
LspSyncClient& LspSyncClient::operator=(LspSyncClient&&) noexcept = default;

LspSyncResult LspSyncClient::initialize(std::string rootUri) {
    if (impl_->lifecycle != LspLifecycleState::Stopped || rootUri.empty()) {
        return {LspSyncError::InvalidState,
                "LSP initialize requires a stopped client and root URI"};
    }
    const auto result = impl_->sendRequest(
        "initialize",
        "{\"processId\":null,\"rootUri\":" + jsonEscape(rootUri) +
            ",\"capabilities\":{}}");
    if (!result.accepted()) {
        return {result.error, result.message};
    }
    impl_->initializeId = result.id;
    impl_->lifecycle = LspLifecycleState::Initializing;
    return {};
}

LspSyncResult LspSyncClient::shutdown() {
    if (impl_->lifecycle != LspLifecycleState::Ready) {
        return {LspSyncError::InvalidState,
                "LSP shutdown requires a ready client"};
    }
    const auto result = impl_->sendRequest("shutdown", "null");
    if (!result.accepted()) {
        return {result.error, result.message};
    }
    impl_->shutdownId = result.id;
    impl_->lifecycle = LspLifecycleState::ShuttingDown;
    return {};
}

LspSyncResult LspSyncClient::poll() {
    if (impl_->lifecycle == LspLifecycleState::Stopped ||
        impl_->lifecycle == LspLifecycleState::Failed) {
        return {LspSyncError::InvalidState,
                "LSP poll requires an active client"};
    }
    std::string bytes;
    const auto read = impl_->stream->read(
        bytes, impl_->config.maximumReadBytes, impl_->timeout);
    if (!read.accepted()) {
        return ioFailure(read);
    }
    if (bytes.empty()) {
        return {LspSyncError::StreamClosed,
                "LSP stream returned an empty successful read"};
    }
    auto framed = impl_->decoder.feed(bytes);
    if (!framed.accepted()) {
        return impl_->failMalformed(framed.message);
    }
    std::vector<std::pair<Json, std::string>> messages;
    messages.reserve(framed.messages.size());
    for (const auto& payload : framed.messages) {
        auto parsed =
            JsonParser{payload, impl_->config.maximumJsonDepth}.parse();
        if (!parsed) {
            return impl_->failMalformed("LSP payload is malformed JSON");
        }
        messages.emplace_back(std::move(*parsed), payload);
    }
    LspSyncResult firstError;
    for (const auto& [message, payload] : messages) {
        auto result = impl_->process(message, payload);
        if (impl_->lifecycle == LspLifecycleState::Failed) {
            return result;
        }
        if (!result.accepted() && firstError.accepted()) {
            firstError = std::move(result);
        }
    }
    return firstError;
}

LspSyncResult LspSyncClient::openDocument(
    std::string uri, std::string languageId, std::uint64_t revision,
    std::string text) {
    if (impl_->lifecycle != LspLifecycleState::Ready) {
        return {LspSyncError::InvalidState,
                "opening an LSP document requires a ready client"};
    }
    if (uri.empty() || languageId.empty() || revision == 0 ||
        !validUtf8(text)) {
        return {LspSyncError::InvalidArgument,
                "LSP document URI, language, revision, or UTF-8 is invalid"};
    }
    if (impl_->documents.contains(uri)) {
        return {LspSyncError::InvalidState, "LSP document is already open"};
    }
    if (impl_->documents.size() >= impl_->config.maximumDocuments) {
        return {LspSyncError::RequestLimitExceeded,
                "LSP document limit exceeded"};
    }
    const auto payload =
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
        "\"params\":{\"textDocument\":{\"uri\":" +
        jsonEscape(uri) + ",\"languageId\":" + jsonEscape(languageId) +
        ",\"version\":1,\"text\":" + jsonEscape(text) + "}}}";
    auto sent = impl_->send(payload);
    if (!sent.accepted()) return sent;
    impl_->documents.emplace(
        std::move(uri),
        Impl::DocumentState{std::move(languageId), 1, revision, std::move(text)});
    return {};
}

LspSyncResult LspSyncClient::changeDocument(std::string_view uri,
                                             std::uint64_t revision,
                                             std::string text) {
    if (impl_->lifecycle != LspLifecycleState::Ready) {
        return {LspSyncError::InvalidState,
                "changing an LSP document requires a ready client"};
    }
    const auto found = impl_->documents.find(std::string{uri});
    if (found == impl_->documents.end()) {
        return {LspSyncError::UnknownDocument, "LSP document is not open"};
    }
    if (revision <= found->second.revision) {
        return {LspSyncError::StaleDocument,
                "LSP document change revision is stale"};
    }
    if (!validUtf8(text) ||
        found->second.version == std::numeric_limits<std::int64_t>::max()) {
        return {LspSyncError::InvalidArgument,
                "LSP document text or next version is invalid"};
    }
    const auto version = found->second.version + 1;
    const auto payload =
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
        "\"params\":{\"textDocument\":{\"uri\":" +
        jsonEscape(uri) + ",\"version\":" + std::to_string(version) +
        "},\"contentChanges\":[{\"text\":" + jsonEscape(text) + "}]}}";
    auto sent = impl_->send(payload);
    if (!sent.accepted()) return sent;
    found->second.version = version;
    found->second.revision = revision;
    found->second.text = std::move(text);
    return {};
}

LspSyncResult LspSyncClient::closeDocument(std::string_view uri) {
    if (impl_->lifecycle != LspLifecycleState::Ready) {
        return {LspSyncError::InvalidState,
                "closing an LSP document requires a ready client"};
    }
    const auto found = impl_->documents.find(std::string{uri});
    if (found == impl_->documents.end()) {
        return {LspSyncError::UnknownDocument, "LSP document is not open"};
    }
    const auto payload =
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\","
        "\"params\":{\"textDocument\":{\"uri\":" +
        jsonEscape(uri) + "}}}";
    auto sent = impl_->send(payload);
    if (!sent.accepted()) return sent;
    impl_->documents.erase(found);
    auto diagnostic = std::find_if(
        impl_->view.documents.begin(), impl_->view.documents.end(),
        [&](const LspDocumentDiagnostics& value) { return value.uri == uri; });
    if (diagnostic != impl_->view.documents.end()) {
        impl_->view.documents.erase(diagnostic);
        impl_->view.revision = std::uint64_t{impl_->view.revision + 1};
    }
    return {};
}

std::optional<std::int64_t> LspSyncClient::documentVersion(
    std::string_view uri) const {
    const auto found = impl_->documents.find(std::string{uri});
    return found == impl_->documents.end()
               ? std::nullopt
               : std::optional<std::int64_t>{found->second.version};
}

std::optional<LspDocumentSnapshot> LspSyncClient::documentSnapshot(
    std::string_view uri) const {
    const auto found = impl_->documents.find(std::string{uri});
    if (found == impl_->documents.end()) return std::nullopt;
    return LspDocumentSnapshot{found->first, found->second.revision,
                               found->second.version, found->second.text};
}

LspRequestResult LspSyncClient::request(std::string method,
                                        std::string paramsJson) {
    if (impl_->lifecycle != LspLifecycleState::Ready) {
        return {0, LspSyncError::InvalidState,
                "LSP request requires a ready client"};
    }
    if (method.empty()) {
        return {0, LspSyncError::InvalidArgument,
                "LSP request method must not be empty"};
    }
    auto params =
        JsonParser{paramsJson, impl_->config.maximumJsonDepth}.parse();
    if (!params || (params->kind != Json::Kind::Object &&
                    params->kind != Json::Kind::Array &&
                    params->kind != Json::Kind::NullValue)) {
        return {0, LspSyncError::InvalidArgument,
                "LSP request params must be valid JSON parameters"};
    }
    return impl_->sendRequest(std::move(method), std::move(paramsJson));
}

LspSyncResult LspSyncClient::cancel(std::uint64_t requestId) {
    if (!impl_->pending.contains(requestId) ||
        impl_->cancelled.contains(requestId) ||
        requestId == impl_->initializeId || requestId == impl_->shutdownId) {
        return {LspSyncError::UnknownRequest,
                "LSP cancellation targets an unknown request"};
    }
    const auto payload =
        "{\"jsonrpc\":\"2.0\",\"method\":\"$/cancelRequest\","
        "\"params\":{\"id\":" +
        std::to_string(requestId) + "}}";
    auto sent = impl_->send(payload);
    if (!sent.accepted()) return sent;
    impl_->cancelled.insert(requestId);
    return {};
}

std::vector<LspCompletedResponse> LspSyncClient::takeCompletedResponses() {
    auto completed = std::move(impl_->completed);
    impl_->completed.clear();
    return completed;
}

LspLifecycleState LspSyncClient::state() const noexcept {
    return impl_->lifecycle;
}

const LspSyncViewState& LspSyncClient::viewState() const noexcept {
    return impl_->view;
}

} // namespace ssg
