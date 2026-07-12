#include <ssg/protocol.h>

#include <ssg/document.h>
#include <ssg/selection.h>
#include <ssg/text_input_commands.h>

#include <any>
#include <charconv>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
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

}  // namespace ssg
