#pragma once

#include "ssg/settings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

enum class LineTerminator : std::uint8_t {
    none,
    lf,
    crlf,
    cr,
};

enum class FinalNewlinePolicy : std::uint8_t {
    preserve,
    ensure_present,
    ensure_absent,
};

enum class TextEncodingErrorCode : std::uint8_t {
    invalid_input,
    invalid_metadata,
    lossy_conversion,
};

struct TextEncodingError {
    TextEncodingErrorCode code = TextEncodingErrorCode::invalid_input;
    std::size_t utf8_offset = 0;
    std::string message;

    friend bool operator==(const TextEncodingError&,
                           const TextEncodingError&) = default;
};

struct TextEncodingStatus {
    TextEncoding encoding = TextEncoding::utf8;
    LineEnding line_ending = LineEnding::lf;
    bool had_bom = false;
    bool final_newline = false;

    friend bool operator==(const TextEncodingStatus&,
                           const TextEncodingStatus&) = default;
};

struct DecodedText {
    std::string utf8;
    std::vector<LineTerminator> line_terminators;
    TextEncodingStatus status;

    friend bool operator==(const DecodedText&, const DecodedText&) = default;
};

struct DecodeTextResult {
    std::optional<DecodedText> text;
    std::optional<TextEncodingError> error;

    [[nodiscard]] bool accepted() const noexcept { return text.has_value(); }
};

struct EncodeTextOptions {
    TextEncoding encoding = TextEncoding::utf8;
    LineEnding line_ending = LineEnding::mixed;
    FinalNewlinePolicy final_newline = FinalNewlinePolicy::preserve;
};

struct EncodeTextResult {
    std::vector<std::uint8_t> bytes;
    std::optional<TextEncodingError> error;

    [[nodiscard]] bool accepted() const noexcept { return !error.has_value(); }
};

[[nodiscard]] DecodeTextResult decode_text(
    std::span<const std::uint8_t> bytes);
[[nodiscard]] DecodeTextResult decode_text(
    std::span<const std::uint8_t> bytes, TextEncoding encoding);
[[nodiscard]] EncodeTextResult encode_text(const DecodedText& text);
[[nodiscard]] EncodeTextResult encode_text(
    const DecodedText& text, EncodeTextOptions options);

struct TextEncodingViewState {
    TextEncodingStatus status;

    friend bool operator==(const TextEncodingViewState&,
                           const TextEncodingViewState&) = default;
};

struct TextEncodingDelta {
    TextEncodingViewState before;
    TextEncodingViewState after;

    friend bool operator==(const TextEncodingDelta&,
                           const TextEncodingDelta&) = default;
};

[[nodiscard]] TextEncodingViewState make_text_encoding_view_state(
    const DecodedText& text) noexcept;
[[nodiscard]] std::optional<TextEncodingDelta> derive_text_encoding_delta(
    const TextEncodingViewState& before,
    const TextEncodingViewState& after);

struct TextEncodingCommandDescriptor {
    std::string_view id;
};

struct TextEncodingCommandSet {
    std::array<TextEncodingCommandDescriptor, 4> descriptors;
};

struct ReopenWithEncodingArguments {
    TextEncoding encoding = TextEncoding::utf8;
    bool operator==(const ReopenWithEncodingArguments&) const = default;
};

struct SetEncodingArguments {
    TextEncoding encoding = TextEncoding::utf8;
    bool operator==(const SetEncodingArguments&) const = default;
};

struct SetLineEndingArguments {
    LineEnding line_ending = LineEnding::lf;
    bool operator==(const SetLineEndingArguments&) const = default;
};

struct SetFinalNewlineArguments {
    bool final_newline = false;
    bool operator==(const SetFinalNewlineArguments&) const = default;
};

inline constexpr TextEncodingCommandSet text_encoding_command_set{{
    TextEncodingCommandDescriptor{"file.reopen_with_encoding"},
    TextEncodingCommandDescriptor{"file.set_encoding"},
    TextEncodingCommandDescriptor{"file.set_line_ending"},
    TextEncodingCommandDescriptor{"file.set_final_newline"},
}};

} // namespace ssg
