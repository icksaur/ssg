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

TextEncodingError invalidInput(std::size_t offset, std::string message) {
    return {TextEncodingErrorCode::InvalidInput, offset, std::move(message)};
}

void appendUtf8(std::string& output, char32_t value) {
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

ScalarResult decodeUtf8(std::span<const std::uint8_t> input,
                         std::size_t baseOffset = 0) {
    OpenPhaseTimer timer{OpenPhase::DecodeValidate};
    noteUtf8Validation();
    ScalarResult result;
    for (std::size_t index = 0; index < input.size();) {
        const auto start = index;
        const auto first = input[index++];
        char32_t value = 0;
        std::size_t continuationCount = 0;
        char32_t minimum = 0;
        if (first <= 0x7f) {
            value = first;
        } else if (first >= 0xc2 && first <= 0xdf) {
            value = first & 0x1f;
            continuationCount = 1;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            value = first & 0x0f;
            continuationCount = 2;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            value = first & 0x07;
            continuationCount = 3;
            minimum = 0x10000;
        } else {
            result.error = invalidInput(
                baseOffset + start, "invalid UTF-8 leading byte");
            return result;
        }
        if (index + continuationCount > input.size()) {
            result.error = invalidInput(
                baseOffset + start, "truncated UTF-8 sequence");
            return result;
        }
        for (std::size_t count = 0; count < continuationCount; ++count) {
            const auto byte = input[index++];
            if ((byte & 0xc0) != 0x80) {
                result.error = invalidInput(
                    baseOffset + index - 1, "invalid UTF-8 continuation byte");
                return result;
            }
            value = (value << 6) | (byte & 0x3f);
        }
        if ((continuationCount != 0 && value < minimum) ||
            (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) {
            result.error = invalidInput(
                baseOffset + start, "invalid UTF-8 scalar value");
            return result;
        }
        result.scalars.push_back({value, baseOffset + start});
    }
    return result;
}

ScalarResult decodeUtf16(std::span<const std::uint8_t> input,
                          bool littleEndian, std::size_t baseOffset) {
    OpenPhaseTimer timer{OpenPhase::DecodeValidate};
    ScalarResult result;
    if (input.size() % 2 != 0) {
        result.error = invalidInput(
            baseOffset + input.size() - 1, "odd UTF-16 byte count");
        return result;
    }
    const auto unit = [littleEndian](std::uint8_t first,
                                      std::uint8_t second) {
        return static_cast<std::uint16_t>(
            littleEndian ? first | (second << 8) : (first << 8) | second);
    };
    for (std::size_t index = 0; index < input.size(); index += 2) {
        const auto first = unit(input[index], input[index + 1]);
        char32_t value = first;
        if (first >= 0xd800 && first <= 0xdbff) {
            if (index + 3 >= input.size()) {
                result.error = invalidInput(
                    baseOffset + index, "unpaired UTF-16 high surrogate");
                return result;
            }
            const auto second = unit(input[index + 2], input[index + 3]);
            if (second < 0xdc00 || second > 0xdfff) {
                result.error = invalidInput(
                    baseOffset + index, "unpaired UTF-16 high surrogate");
                return result;
            }
            value = 0x10000 +
                    ((static_cast<char32_t>(first) - 0xd800) << 10) +
                    (static_cast<char32_t>(second) - 0xdc00);
            index += 2;
        } else if (first >= 0xdc00 && first <= 0xdfff) {
            result.error = invalidInput(
                baseOffset + index, "unpaired UTF-16 low surrogate");
            return result;
        }
        result.scalars.push_back({value, baseOffset + index});
    }
    return result;
}

constexpr std::array<char32_t, 32> kWindows1252High{
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
    0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
};

ScalarResult decodeSingleByte(std::span<const std::uint8_t> input,
                                TextEncoding encoding) {
    OpenPhaseTimer timer{OpenPhase::DecodeValidate};
    ScalarResult result;
    result.scalars.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto byte = input[index];
        char32_t value = byte;
        if (encoding == TextEncoding::Windows1252 &&
            byte >= 0x80 && byte <= 0x9f) {
            value = kWindows1252High[byte - 0x80];
        }
        result.scalars.push_back({value, index});
    }
    return result;
}

LineTerminator toTerminator(LineEnding ending) {
    switch (ending) {
    case LineEnding::Lf: return LineTerminator::Lf;
    case LineEnding::Crlf: return LineTerminator::Crlf;
    case LineEnding::Cr: return LineTerminator::Cr;
    case LineEnding::Mixed: return LineTerminator::Lf;
    }
    return LineTerminator::Lf;
}

LineEnding detectedLineEnding(
    const std::vector<LineTerminator>& terminators) {
    std::optional<LineTerminator> first;
    for (const auto terminator : terminators) {
        if (terminator == LineTerminator::None) continue;
        if (!first.has_value()) {
            first = terminator;
        } else if (*first != terminator) {
            return LineEnding::Mixed;
        }
    }
    if (!first.has_value() || *first == LineTerminator::Lf) {
        return LineEnding::Lf;
    }
    if (*first == LineTerminator::Crlf) return LineEnding::Crlf;
    return LineEnding::Cr;
}

DecodeTextResult normalized(ScalarResult scalarResult,
                            TextEncoding encoding, bool hadBom) {
    if (scalarResult.error.has_value()) {
        return {std::nullopt, std::move(scalarResult.error)};
    }
    OpenPhaseTimer timer{OpenPhase::EolScan};
    DecodedText text;
    text.status.encoding = encoding;
    text.status.had_bom = hadBom;
    for (std::size_t index = 0; index < scalarResult.scalars.size(); ++index) {
        const auto value = scalarResult.scalars[index].value;
        if (value == 0) {
            return {std::nullopt,
                    invalidInput(scalarResult.scalars[index].utf8_offset,
                                  "NUL byte is not valid document text")};
        }
        if (value == U'\r') {
            if (index + 1 < scalarResult.scalars.size() &&
                scalarResult.scalars[index + 1].value == U'\n') {
                ++index;
                text.line_terminators.push_back(LineTerminator::Crlf);
            } else {
                text.line_terminators.push_back(LineTerminator::Cr);
            }
            text.utf8.push_back('\n');
        } else if (value == U'\n') {
            text.line_terminators.push_back(LineTerminator::Lf);
            text.utf8.push_back('\n');
        } else {
            appendUtf8(text.utf8, value);
        }
    }
    if (!text.utf8.empty() && text.utf8.back() != '\n') {
        text.line_terminators.push_back(LineTerminator::None);
    }
    text.status.line_ending = detectedLineEnding(text.line_terminators);
    text.status.final_newline =
        !text.line_terminators.empty() &&
        text.line_terminators.back() != LineTerminator::None;
    return {std::move(text), std::nullopt};
}

std::span<const std::uint8_t> skip(
    std::span<const std::uint8_t> bytes, std::size_t count) {
    return bytes.subspan(std::min(count, bytes.size()));
}

// Fused single-pass UTF-8 decode + EOL-normalize (LF-2b, the dominant lever).
// Validates each sequence and copies its bytes STRAIGHT THROUGH to the utf8
// output (an accepted UTF-8 sequence is already canonical, so re-encoding is a
// no-op), normalizing CR/CRLF/CR -> LF and recording the per-line terminator, in
// ONE walk with no std::vector<Scalar> intermediate. Malformed-byte offsets and
// classification are identical to decode_utf8 + normalized (see the offset parity
// tests in test_text_encoding.cpp); `base_offset` makes them BOM-relative.
DecodeTextResult decodeUtf8Fused(std::span<const std::uint8_t> input,
                                   std::size_t baseOffset,
                                   TextEncoding encoding, bool hadBom) {
    OpenPhaseTimer timer{OpenPhase::DecodeValidate};
    noteUtf8Validation();
    DecodedText text;
    text.status.encoding = encoding;
    text.status.had_bom = hadBom;
    text.utf8.reserve(input.size());
    const std::size_t n = input.size();
    std::size_t index = 0;
    while (index < n) {
        const std::uint8_t first = input[index];
        if (first == 0x00) {
            return {std::nullopt,
                    invalidInput(baseOffset + index,
                                  "NUL byte is not valid document text")};
        }
        if (first == '\r') {
            if (index + 1 < n && input[index + 1] == '\n') {
                text.line_terminators.push_back(LineTerminator::Crlf);
                index += 2;
            } else {
                text.line_terminators.push_back(LineTerminator::Cr);
                index += 1;
            }
            text.utf8.push_back('\n');
            continue;
        }
        if (first == '\n') {
            text.line_terminators.push_back(LineTerminator::Lf);
            text.utf8.push_back('\n');
            index += 1;
            continue;
        }
        if (first <= 0x7f) {
            text.utf8.push_back(static_cast<char>(first));
            index += 1;
            continue;
        }
        std::size_t continuationCount = 0;
        char32_t minimum = 0;
        char32_t value = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            value = first & 0x1f;
            continuationCount = 1;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            value = first & 0x0f;
            continuationCount = 2;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            value = first & 0x07;
            continuationCount = 3;
            minimum = 0x10000;
        } else {
            return {std::nullopt,
                    invalidInput(baseOffset + index,
                                  "invalid UTF-8 leading byte")};
        }
        if (index + 1 + continuationCount > n) {
            return {std::nullopt,
                    invalidInput(baseOffset + index,
                                  "truncated UTF-8 sequence")};
        }
        for (std::size_t count = 1; count <= continuationCount; ++count) {
            const std::uint8_t byte = input[index + count];
            if ((byte & 0xc0) != 0x80) {
                return {std::nullopt,
                        invalidInput(baseOffset + index + count,
                                      "invalid UTF-8 continuation byte")};
            }
            value = (value << 6) | (byte & 0x3f);
        }
        if (value < minimum || (value >= 0xd800 && value <= 0xdfff) ||
            value > 0x10ffff) {
            return {std::nullopt,
                    invalidInput(baseOffset + index,
                                  "invalid UTF-8 scalar value")};
        }
        text.utf8.append(reinterpret_cast<const char*>(input.data() + index),
                         continuationCount + 1);
        index += continuationCount + 1;
    }
    if (!text.utf8.empty() && text.utf8.back() != '\n') {
        text.line_terminators.push_back(LineTerminator::None);
    }
    text.status.line_ending = detectedLineEnding(text.line_terminators);
    text.status.final_newline =
        !text.line_terminators.empty() &&
        text.line_terminators.back() != LineTerminator::None;
    return {std::move(text), std::nullopt};
}

DecodeTextResult decodeSelected(std::span<const std::uint8_t> bytes,
                                 TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::Utf8:
    case TextEncoding::Utf8Bom: {
        const bool bom = bytes.size() >= 3 && bytes[0] == 0xef &&
                         bytes[1] == 0xbb && bytes[2] == 0xbf;
        return decodeUtf8Fused(
            bom ? skip(bytes, 3) : bytes, bom ? 3 : 0,
            bom ? TextEncoding::Utf8Bom : encoding, bom);
    }
    case TextEncoding::Utf16le:
    case TextEncoding::Utf16be: {
        const bool little = encoding == TextEncoding::Utf16le;
        const bool matchingBom =
            bytes.size() >= 2 &&
            ((little && bytes[0] == 0xff && bytes[1] == 0xfe) ||
             (!little && bytes[0] == 0xfe && bytes[1] == 0xff));
        const bool oppositeBom =
            bytes.size() >= 2 &&
            ((little && bytes[0] == 0xfe && bytes[1] == 0xff) ||
             (!little && bytes[0] == 0xff && bytes[1] == 0xfe));
        if (oppositeBom) {
            return {std::nullopt,
                    invalidInput(0, "UTF-16 BOM does not match encoding")};
        }
        return normalized(
            decodeUtf16(matchingBom ? skip(bytes, 2) : bytes,
                         little, matchingBom ? 2 : 0),
            encoding, matchingBom);
    }
    case TextEncoding::Windows1252:
    case TextEncoding::Iso88591:
        return normalized(decodeSingleByte(bytes, encoding), encoding, false);
    }
    return {std::nullopt, invalidInput(0, "unsupported encoding")};
}

std::optional<std::uint8_t> windows1252Byte(char32_t value) {
    if (value <= 0x7f || (value >= 0xa0 && value <= 0xff)) {
        return static_cast<std::uint8_t>(value);
    }
    const auto found = std::find(
        kWindows1252High.begin(), kWindows1252High.end(), value);
    if (found == kWindows1252High.end()) return std::nullopt;
    return static_cast<std::uint8_t>(
        0x80 + std::distance(kWindows1252High.begin(), found));
}

void appendUtf16(std::vector<std::uint8_t>& output, char32_t value,
                  bool littleEndian) {
    const auto appendUnit = [&output, littleEndian](std::uint16_t unit) {
        const auto low = static_cast<std::uint8_t>(unit & 0xff);
        const auto high = static_cast<std::uint8_t>(unit >> 8);
        output.push_back(littleEndian ? low : high);
        output.push_back(littleEndian ? high : low);
    };
    if (value <= 0xffff) {
        appendUnit(static_cast<std::uint16_t>(value));
    } else {
        value -= 0x10000;
        appendUnit(static_cast<std::uint16_t>(0xd800 + (value >> 10)));
        appendUnit(static_cast<std::uint16_t>(0xdc00 + (value & 0x3ff)));
    }
}

std::vector<Scalar> outputScalars(const DecodedText& text,
                                   EncodeTextOptions options,
                                   TextEncodingError& error) {
    const auto decoded = decodeUtf8(std::span{
        reinterpret_cast<const std::uint8_t*>(text.utf8.data()),
        text.utf8.size()});
    if (decoded.error.has_value()) {
        error = *decoded.error;
        return {};
    }
    std::size_t expectedTerminators = 0;
    for (const auto scalar : decoded.scalars) {
        if (scalar.value == U'\n') ++expectedTerminators;
    }
    if (!decoded.scalars.empty() && decoded.scalars.back().value != U'\n') {
        ++expectedTerminators;
    }
    if (expectedTerminators != text.line_terminators.size()) {
        error = {TextEncodingErrorCode::InvalidMetadata, 0,
                 "line terminator metadata does not match text"};
        return {};
    }

    std::vector<Scalar> output;
    std::size_t terminatorIndex = 0;
    for (std::size_t index = 0; index < decoded.scalars.size(); ++index) {
        const auto scalar = decoded.scalars[index];
        if (scalar.value != U'\n') {
            output.push_back(scalar);
            continue;
        }
        const auto stored = text.line_terminators[terminatorIndex++];
        if (stored == LineTerminator::None) {
            error = {TextEncodingErrorCode::InvalidMetadata,
                     scalar.utf8_offset, "newline has no terminator metadata"};
            return {};
        }
        const bool final = index + 1 == decoded.scalars.size();
        if (final &&
            options.final_newline == FinalNewlinePolicy::EnsureAbsent) {
            continue;
        }
        const auto selected =
            options.line_ending == LineEnding::Mixed
                ? stored
                : toTerminator(options.line_ending);
        if (selected == LineTerminator::Crlf) {
            output.push_back({U'\r', scalar.utf8_offset});
            output.push_back({U'\n', scalar.utf8_offset});
        } else if (selected == LineTerminator::Cr) {
            output.push_back({U'\r', scalar.utf8_offset});
        } else {
            output.push_back({U'\n', scalar.utf8_offset});
        }
    }
    if (!decoded.scalars.empty() && decoded.scalars.back().value != U'\n') {
        if (text.line_terminators[terminatorIndex] != LineTerminator::None) {
            error = {TextEncodingErrorCode::InvalidMetadata,
                     decoded.scalars.back().utf8_offset,
                     "unterminated line has terminator metadata"};
            return {};
        }
        if (options.final_newline == FinalNewlinePolicy::EnsurePresent) {
            auto ending = options.line_ending;
            if (ending == LineEnding::Mixed) {
                ending = text.status.line_ending == LineEnding::Mixed
                             ? LineEnding::Lf
                             : text.status.line_ending;
            }
            const auto selected = toTerminator(ending);
            if (selected == LineTerminator::Crlf) {
                output.push_back({U'\r', text.utf8.size()});
                output.push_back({U'\n', text.utf8.size()});
            } else if (selected == LineTerminator::Cr) {
                output.push_back({U'\r', text.utf8.size()});
            } else {
                output.push_back({U'\n', text.utf8.size()});
            }
        }
    }
    return output;
}

} // namespace

DecodeTextResult decodeText(std::span<const std::uint8_t> bytes) {
    if (bytes.size() >= 3 && bytes[0] == 0xef &&
        bytes[1] == 0xbb && bytes[2] == 0xbf) {
        return decodeSelected(bytes, TextEncoding::Utf8Bom);
    }
    if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
        return decodeSelected(bytes, TextEncoding::Utf16le);
    }
    if (bytes.size() >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff) {
        return decodeSelected(bytes, TextEncoding::Utf16be);
    }
    return decodeSelected(bytes, TextEncoding::Utf8);
}

DecodeTextResult decodeText(std::span<const std::uint8_t> bytes,
                             TextEncoding encoding) {
    return decodeSelected(bytes, encoding);
}

EncodeTextResult encodeText(const DecodedText& text) {
    return encodeText(
        text, {text.status.encoding, LineEnding::Mixed,
               FinalNewlinePolicy::Preserve});
}

EncodeTextResult encodeText(const DecodedText& text,
                             EncodeTextOptions options) {
    TextEncodingError error;
    const auto scalars = outputScalars(text, options, error);
    if (!error.message.empty()) return {{}, std::move(error)};

    EncodeTextResult result;
    if (options.encoding == TextEncoding::Utf8Bom) {
        result.bytes.insert(result.bytes.end(), {0xef, 0xbb, 0xbf});
    } else if (options.encoding == TextEncoding::Utf16le) {
        result.bytes.insert(result.bytes.end(), {0xff, 0xfe});
    } else if (options.encoding == TextEncoding::Utf16be) {
        result.bytes.insert(result.bytes.end(), {0xfe, 0xff});
    }

    for (const auto scalar : scalars) {
        if (options.encoding == TextEncoding::Utf8 ||
            options.encoding == TextEncoding::Utf8Bom) {
            std::string encoded;
            appendUtf8(encoded, scalar.value);
            result.bytes.insert(result.bytes.end(), encoded.begin(), encoded.end());
        } else if (options.encoding == TextEncoding::Utf16le ||
                   options.encoding == TextEncoding::Utf16be) {
            appendUtf16(result.bytes, scalar.value,
                         options.encoding == TextEncoding::Utf16le);
        } else {
            std::optional<std::uint8_t> encoded;
            if (options.encoding == TextEncoding::Iso88591) {
                if (scalar.value <= 0xff) {
                    encoded = static_cast<std::uint8_t>(scalar.value);
                }
            } else {
                encoded = windows1252Byte(scalar.value);
            }
            if (!encoded.has_value()) {
                return {{},
                        TextEncodingError{
                            TextEncodingErrorCode::LossyConversion,
                            scalar.utf8_offset,
                            "text is not representable in selected encoding"}};
            }
            result.bytes.push_back(*encoded);
        }
    }
    return result;
}

TextEncodingViewState makeTextEncodingViewState(
    const DecodedText& text) noexcept {
    return {text.status};
}

std::optional<TextEncodingDelta> deriveTextEncodingDelta(
    const TextEncodingViewState& before,
    const TextEncodingViewState& after) {
    if (before == after) return std::nullopt;
    return TextEncodingDelta{before, after};
}

} // namespace ssg
