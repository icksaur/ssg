#pragma once

#include "ssg/settings.h"

#include <ssg/shared_bytes.h>

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

// A move-only proof that its bytes are well-formed UTF-8 without NUL bytes.
// Minted only from an accepted decode result (`DecodeTextResult::validated`) and
// consumed by `Document` so a document can be built from decoder-validated bytes
// WITHOUT re-validating them (removing the redundant second scan on the open
// path), while the public `Document(std::string_view)` still validates for
// external callers.  Move-only + a private constructor make it hard to fabricate
// a proof for unvalidated bytes by accident.  The validated bytes are held as a
// SharedBytes so the piece-tree original and the initial persisted text can share
// one buffer instead of each keeping a copy.
class ValidatedUtf8 {
public:
    ValidatedUtf8(ValidatedUtf8&&) noexcept = default;
    ValidatedUtf8& operator=(ValidatedUtf8&&) noexcept = default;
    ValidatedUtf8(const ValidatedUtf8&) = delete;
    ValidatedUtf8& operator=(const ValidatedUtf8&) = delete;

    [[nodiscard]] std::string_view view() const noexcept { return bytes_.view(); }
    // A shared handle to the validated bytes (cheap ref-count bump), so callers
    // can share the buffer without copying.
    [[nodiscard]] SharedBytes bytes() const noexcept { return bytes_; }

private:
    explicit ValidatedUtf8(SharedBytes bytes) noexcept
        : bytes_(std::move(bytes)) {}
    [[nodiscard]] SharedBytes take() && noexcept { return std::move(bytes_); }

    friend struct DecodeTextResult;
    friend class Document;

    SharedBytes bytes_;
};

struct DecodeTextResult {
    std::optional<DecodedText> text;
    std::optional<TextEncodingError> error;

    [[nodiscard]] bool accepted() const noexcept { return text.has_value(); }

    // Mint a validation proof for the just-decoded bytes.  Precondition:
    // accepted().  The decoder is the only producer of an accepted result, so the
    // proof genuinely reflects a validation.
    [[nodiscard]] ValidatedUtf8 validated() const {
        return ValidatedUtf8{SharedBytes::owning(text->utf8)};
    }
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
