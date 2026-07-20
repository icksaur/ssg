#include "ssg/text_encoding.h"

#include <ssg/open_metrics.h>

#include <algorithm>
#include <utility>

namespace ssg {
namespace {

struct Scalar {
    char32_t value;
    std::size_t utf8_offset;
};

struct ScalarResult {
    std::vector<Scalar> scalars;
    std::optional<TextEncodingError> error;
};

TextEncodingError invalid_input(std::size_t offset, std::string message) {
    return {TextEncodingErrorCode::invalid_input, offset, std::move(message)};
}

void append_utf8(std::string& output, char32_t value) {
    if (value <= 0x7f) {
        output.push_back(static_cast<char>(value));
    } else if (value <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (value >> 6)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else if (value <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (value >> 12)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (value >> 18)));
        output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    }
}

ScalarResult decode_utf8(std::span<const std::uint8_t> input,
                         std::size_t base_offset = 0) {
    OpenPhaseTimer timer{OpenPhase::decode_validate};
    note_utf8_validation();
    ScalarResult result;
    for (std::size_t index = 0; index < input.size();) {
        const auto start = index;
        const auto first = input[index++];
        char32_t value = 0;
        std::size_t continuation_count = 0;
        char32_t minimum = 0;
        if (first <= 0x7f) {
            value = first;
        } else if (first >= 0xc2 && first <= 0xdf) {
            value = first & 0x1f;
            continuation_count = 1;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            value = first & 0x0f;
            continuation_count = 2;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            value = first & 0x07;
            continuation_count = 3;
            minimum = 0x10000;
        } else {
            result.error = invalid_input(
                base_offset + start, "invalid UTF-8 leading byte");
            return result;
        }
        if (index + continuation_count > input.size()) {
            result.error = invalid_input(
                base_offset + start, "truncated UTF-8 sequence");
            return result;
        }
        for (std::size_t count = 0; count < continuation_count; ++count) {
            const auto byte = input[index++];
            if ((byte & 0xc0) != 0x80) {
                result.error = invalid_input(
                    base_offset + index - 1, "invalid UTF-8 continuation byte");
                return result;
            }
            value = (value << 6) | (byte & 0x3f);
        }
        if ((continuation_count != 0 && value < minimum) ||
            (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) {
            result.error = invalid_input(
                base_offset + start, "invalid UTF-8 scalar value");
            return result;
        }
        result.scalars.push_back({value, base_offset + start});
    }
    return result;
}

ScalarResult decode_utf16(std::span<const std::uint8_t> input,
                          bool little_endian, std::size_t base_offset) {
    OpenPhaseTimer timer{OpenPhase::decode_validate};
    ScalarResult result;
    if (input.size() % 2 != 0) {
        result.error = invalid_input(
            base_offset + input.size() - 1, "odd UTF-16 byte count");
        return result;
    }
    const auto unit = [little_endian](std::uint8_t first,
                                      std::uint8_t second) {
        return static_cast<std::uint16_t>(
            little_endian ? first | (second << 8) : (first << 8) | second);
    };
    for (std::size_t index = 0; index < input.size(); index += 2) {
        const auto first = unit(input[index], input[index + 1]);
        char32_t value = first;
        if (first >= 0xd800 && first <= 0xdbff) {
            if (index + 3 >= input.size()) {
                result.error = invalid_input(
                    base_offset + index, "unpaired UTF-16 high surrogate");
                return result;
            }
            const auto second = unit(input[index + 2], input[index + 3]);
            if (second < 0xdc00 || second > 0xdfff) {
                result.error = invalid_input(
                    base_offset + index, "unpaired UTF-16 high surrogate");
                return result;
            }
            value = 0x10000 +
                    ((static_cast<char32_t>(first) - 0xd800) << 10) +
                    (static_cast<char32_t>(second) - 0xdc00);
            index += 2;
        } else if (first >= 0xdc00 && first <= 0xdfff) {
            result.error = invalid_input(
                base_offset + index, "unpaired UTF-16 low surrogate");
            return result;
        }
        result.scalars.push_back({value, base_offset + index});
    }
    return result;
}

constexpr std::array<char32_t, 32> windows1252_high{
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
    0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
};

ScalarResult decode_single_byte(std::span<const std::uint8_t> input,
                                TextEncoding encoding) {
    OpenPhaseTimer timer{OpenPhase::decode_validate};
    ScalarResult result;
    result.scalars.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto byte = input[index];
        char32_t value = byte;
        if (encoding == TextEncoding::windows1252 &&
            byte >= 0x80 && byte <= 0x9f) {
            value = windows1252_high[byte - 0x80];
        }
        result.scalars.push_back({value, index});
    }
    return result;
}

LineTerminator to_terminator(LineEnding ending) {
    switch (ending) {
    case LineEnding::lf: return LineTerminator::lf;
    case LineEnding::crlf: return LineTerminator::crlf;
    case LineEnding::cr: return LineTerminator::cr;
    case LineEnding::mixed: return LineTerminator::lf;
    }
    return LineTerminator::lf;
}

LineEnding detected_line_ending(
    const std::vector<LineTerminator>& terminators) {
    std::optional<LineTerminator> first;
    for (const auto terminator : terminators) {
        if (terminator == LineTerminator::none) continue;
        if (!first.has_value()) {
            first = terminator;
        } else if (*first != terminator) {
            return LineEnding::mixed;
        }
    }
    if (!first.has_value() || *first == LineTerminator::lf) {
        return LineEnding::lf;
    }
    if (*first == LineTerminator::crlf) return LineEnding::crlf;
    return LineEnding::cr;
}

DecodeTextResult normalized(ScalarResult scalar_result,
                            TextEncoding encoding, bool had_bom) {
    if (scalar_result.error.has_value()) {
        return {std::nullopt, std::move(scalar_result.error)};
    }
    OpenPhaseTimer timer{OpenPhase::eol_scan};
    DecodedText text;
    text.status.encoding = encoding;
    text.status.had_bom = had_bom;
    for (std::size_t index = 0; index < scalar_result.scalars.size(); ++index) {
        const auto value = scalar_result.scalars[index].value;
        if (value == U'\r') {
            if (index + 1 < scalar_result.scalars.size() &&
                scalar_result.scalars[index + 1].value == U'\n') {
                ++index;
                text.line_terminators.push_back(LineTerminator::crlf);
            } else {
                text.line_terminators.push_back(LineTerminator::cr);
            }
            text.utf8.push_back('\n');
        } else if (value == U'\n') {
            text.line_terminators.push_back(LineTerminator::lf);
            text.utf8.push_back('\n');
        } else {
            append_utf8(text.utf8, value);
        }
    }
    if (!text.utf8.empty() && text.utf8.back() != '\n') {
        text.line_terminators.push_back(LineTerminator::none);
    }
    text.status.line_ending = detected_line_ending(text.line_terminators);
    text.status.final_newline =
        !text.line_terminators.empty() &&
        text.line_terminators.back() != LineTerminator::none;
    return {std::move(text), std::nullopt};
}

std::span<const std::uint8_t> skip(
    std::span<const std::uint8_t> bytes, std::size_t count) {
    return bytes.subspan(std::min(count, bytes.size()));
}

DecodeTextResult decode_selected(std::span<const std::uint8_t> bytes,
                                 TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::utf8:
    case TextEncoding::utf8_bom: {
        const bool bom = bytes.size() >= 3 && bytes[0] == 0xef &&
                         bytes[1] == 0xbb && bytes[2] == 0xbf;
        return normalized(
            decode_utf8(bom ? skip(bytes, 3) : bytes, bom ? 3 : 0),
            bom ? TextEncoding::utf8_bom : encoding, bom);
    }
    case TextEncoding::utf16le:
    case TextEncoding::utf16be: {
        const bool little = encoding == TextEncoding::utf16le;
        const bool matching_bom =
            bytes.size() >= 2 &&
            ((little && bytes[0] == 0xff && bytes[1] == 0xfe) ||
             (!little && bytes[0] == 0xfe && bytes[1] == 0xff));
        const bool opposite_bom =
            bytes.size() >= 2 &&
            ((little && bytes[0] == 0xfe && bytes[1] == 0xff) ||
             (!little && bytes[0] == 0xff && bytes[1] == 0xfe));
        if (opposite_bom) {
            return {std::nullopt,
                    invalid_input(0, "UTF-16 BOM does not match encoding")};
        }
        return normalized(
            decode_utf16(matching_bom ? skip(bytes, 2) : bytes,
                         little, matching_bom ? 2 : 0),
            encoding, matching_bom);
    }
    case TextEncoding::windows1252:
    case TextEncoding::iso88591:
        return normalized(decode_single_byte(bytes, encoding), encoding, false);
    }
    return {std::nullopt, invalid_input(0, "unsupported encoding")};
}

std::optional<std::uint8_t> windows1252_byte(char32_t value) {
    if (value <= 0x7f || (value >= 0xa0 && value <= 0xff)) {
        return static_cast<std::uint8_t>(value);
    }
    const auto found = std::find(
        windows1252_high.begin(), windows1252_high.end(), value);
    if (found == windows1252_high.end()) return std::nullopt;
    return static_cast<std::uint8_t>(
        0x80 + std::distance(windows1252_high.begin(), found));
}

void append_utf16(std::vector<std::uint8_t>& output, char32_t value,
                  bool little_endian) {
    const auto append_unit = [&output, little_endian](std::uint16_t unit) {
        const auto low = static_cast<std::uint8_t>(unit & 0xff);
        const auto high = static_cast<std::uint8_t>(unit >> 8);
        output.push_back(little_endian ? low : high);
        output.push_back(little_endian ? high : low);
    };
    if (value <= 0xffff) {
        append_unit(static_cast<std::uint16_t>(value));
    } else {
        value -= 0x10000;
        append_unit(static_cast<std::uint16_t>(0xd800 + (value >> 10)));
        append_unit(static_cast<std::uint16_t>(0xdc00 + (value & 0x3ff)));
    }
}

std::vector<Scalar> output_scalars(const DecodedText& text,
                                   EncodeTextOptions options,
                                   TextEncodingError& error) {
    const auto decoded = decode_utf8(std::span{
        reinterpret_cast<const std::uint8_t*>(text.utf8.data()),
        text.utf8.size()});
    if (decoded.error.has_value()) {
        error = *decoded.error;
        return {};
    }
    std::size_t expected_terminators = 0;
    for (const auto scalar : decoded.scalars) {
        if (scalar.value == U'\n') ++expected_terminators;
    }
    if (!decoded.scalars.empty() && decoded.scalars.back().value != U'\n') {
        ++expected_terminators;
    }
    if (expected_terminators != text.line_terminators.size()) {
        error = {TextEncodingErrorCode::invalid_metadata, 0,
                 "line terminator metadata does not match text"};
        return {};
    }

    std::vector<Scalar> output;
    std::size_t terminator_index = 0;
    for (std::size_t index = 0; index < decoded.scalars.size(); ++index) {
        const auto scalar = decoded.scalars[index];
        if (scalar.value != U'\n') {
            output.push_back(scalar);
            continue;
        }
        const auto stored = text.line_terminators[terminator_index++];
        if (stored == LineTerminator::none) {
            error = {TextEncodingErrorCode::invalid_metadata,
                     scalar.utf8_offset, "newline has no terminator metadata"};
            return {};
        }
        const bool final = index + 1 == decoded.scalars.size();
        if (final &&
            options.final_newline == FinalNewlinePolicy::ensure_absent) {
            continue;
        }
        const auto selected =
            options.line_ending == LineEnding::mixed
                ? stored
                : to_terminator(options.line_ending);
        if (selected == LineTerminator::crlf) {
            output.push_back({U'\r', scalar.utf8_offset});
            output.push_back({U'\n', scalar.utf8_offset});
        } else if (selected == LineTerminator::cr) {
            output.push_back({U'\r', scalar.utf8_offset});
        } else {
            output.push_back({U'\n', scalar.utf8_offset});
        }
    }
    if (!decoded.scalars.empty() && decoded.scalars.back().value != U'\n') {
        if (text.line_terminators[terminator_index] != LineTerminator::none) {
            error = {TextEncodingErrorCode::invalid_metadata,
                     decoded.scalars.back().utf8_offset,
                     "unterminated line has terminator metadata"};
            return {};
        }
        if (options.final_newline == FinalNewlinePolicy::ensure_present) {
            auto ending = options.line_ending;
            if (ending == LineEnding::mixed) {
                ending = text.status.line_ending == LineEnding::mixed
                             ? LineEnding::lf
                             : text.status.line_ending;
            }
            const auto selected = to_terminator(ending);
            if (selected == LineTerminator::crlf) {
                output.push_back({U'\r', text.utf8.size()});
                output.push_back({U'\n', text.utf8.size()});
            } else if (selected == LineTerminator::cr) {
                output.push_back({U'\r', text.utf8.size()});
            } else {
                output.push_back({U'\n', text.utf8.size()});
            }
        }
    }
    return output;
}

} // namespace

DecodeTextResult decode_text(std::span<const std::uint8_t> bytes) {
    if (bytes.size() >= 3 && bytes[0] == 0xef &&
        bytes[1] == 0xbb && bytes[2] == 0xbf) {
        return decode_selected(bytes, TextEncoding::utf8_bom);
    }
    if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
        return decode_selected(bytes, TextEncoding::utf16le);
    }
    if (bytes.size() >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff) {
        return decode_selected(bytes, TextEncoding::utf16be);
    }
    return decode_selected(bytes, TextEncoding::utf8);
}

DecodeTextResult decode_text(std::span<const std::uint8_t> bytes,
                             TextEncoding encoding) {
    return decode_selected(bytes, encoding);
}

EncodeTextResult encode_text(const DecodedText& text) {
    return encode_text(
        text, {text.status.encoding, LineEnding::mixed,
               FinalNewlinePolicy::preserve});
}

EncodeTextResult encode_text(const DecodedText& text,
                             EncodeTextOptions options) {
    TextEncodingError error;
    const auto scalars = output_scalars(text, options, error);
    if (!error.message.empty()) return {{}, std::move(error)};

    EncodeTextResult result;
    if (options.encoding == TextEncoding::utf8_bom) {
        result.bytes.insert(result.bytes.end(), {0xef, 0xbb, 0xbf});
    } else if (options.encoding == TextEncoding::utf16le) {
        result.bytes.insert(result.bytes.end(), {0xff, 0xfe});
    } else if (options.encoding == TextEncoding::utf16be) {
        result.bytes.insert(result.bytes.end(), {0xfe, 0xff});
    }

    for (const auto scalar : scalars) {
        if (options.encoding == TextEncoding::utf8 ||
            options.encoding == TextEncoding::utf8_bom) {
            std::string encoded;
            append_utf8(encoded, scalar.value);
            result.bytes.insert(result.bytes.end(), encoded.begin(), encoded.end());
        } else if (options.encoding == TextEncoding::utf16le ||
                   options.encoding == TextEncoding::utf16be) {
            append_utf16(result.bytes, scalar.value,
                         options.encoding == TextEncoding::utf16le);
        } else {
            std::optional<std::uint8_t> encoded;
            if (options.encoding == TextEncoding::iso88591) {
                if (scalar.value <= 0xff) {
                    encoded = static_cast<std::uint8_t>(scalar.value);
                }
            } else {
                encoded = windows1252_byte(scalar.value);
            }
            if (!encoded.has_value()) {
                return {{},
                        TextEncodingError{
                            TextEncodingErrorCode::lossy_conversion,
                            scalar.utf8_offset,
                            "text is not representable in selected encoding"}};
            }
            result.bytes.push_back(*encoded);
        }
    }
    return result;
}

TextEncodingViewState make_text_encoding_view_state(
    const DecodedText& text) noexcept {
    return {text.status};
}

std::optional<TextEncodingDelta> derive_text_encoding_delta(
    const TextEncodingViewState& before,
    const TextEncodingViewState& after) {
    if (before == after) return std::nullopt;
    return TextEncodingDelta{before, after};
}

} // namespace ssg
