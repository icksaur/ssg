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
    None,
    Lf,
    Crlf,
    Cr,
};

enum class FinalNewlinePolicy : std::uint8_t {
    Preserve,
    EnsurePresent,
    EnsureAbsent,
};

enum class TextEncodingErrorCode : std::uint8_t {
    InvalidInput,
    InvalidMetadata,
    LossyConversion,
};

struct TextEncodingError {
    TextEncodingErrorCode code = TextEncodingErrorCode::InvalidInput;
    std::size_t utf8Offset = 0;
    std::string message;

    friend bool operator==(const TextEncodingError&,
                           const TextEncodingError&) = default;
};

struct TextEncodingStatus {
    TextEncoding encoding = TextEncoding::Utf8;
    LineEnding lineEnding = LineEnding::Lf;
    bool hadBom = false;
    bool finalNewline = false;

    friend bool operator==(const TextEncodingStatus&,
                           const TextEncodingStatus&) = default;
};

struct DecodedText {
    std::string utf8;
    std::vector<LineTerminator> lineTerminators;
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
    TextEncoding encoding = TextEncoding::Utf8;
    LineEnding lineEnding = LineEnding::Mixed;
    FinalNewlinePolicy finalNewline = FinalNewlinePolicy::Preserve;
};

struct EncodeTextResult {
    std::vector<std::uint8_t> bytes;
    std::optional<TextEncodingError> error;

    [[nodiscard]] bool accepted() const noexcept { return !error.has_value(); }
};

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

class TextCodec {
public:
    [[nodiscard]] DecodeTextResult decode(
        std::span<const std::uint8_t> bytes) const;
    [[nodiscard]] DecodeTextResult decode(
        std::span<const std::uint8_t> bytes, TextEncoding encoding) const;
    [[nodiscard]] EncodeTextResult encode(const DecodedText& text) const;
    [[nodiscard]] EncodeTextResult encode(
        const DecodedText& text, EncodeTextOptions options) const;
    [[nodiscard]] TextEncodingViewState viewState(
        const DecodedText& text) const noexcept;
    [[nodiscard]] std::optional<TextEncodingDelta> deriveDelta(
        const TextEncodingViewState& before,
        const TextEncodingViewState& after) const;
};

struct TextEncodingCommandDescriptor {
    std::string_view id;
};

struct TextEncodingCommandSet {
    std::array<TextEncodingCommandDescriptor, 4> descriptors;
};

struct ReopenWithEncodingArguments {
    TextEncoding encoding = TextEncoding::Utf8;
    bool operator==(const ReopenWithEncodingArguments&) const = default;
};

struct SetEncodingArguments {
    TextEncoding encoding = TextEncoding::Utf8;
    bool operator==(const SetEncodingArguments&) const = default;
};

struct SetLineEndingArguments {
    LineEnding lineEnding = LineEnding::Lf;
    bool operator==(const SetLineEndingArguments&) const = default;
};

struct SetFinalNewlineArguments {
    bool finalNewline = false;
    bool operator==(const SetFinalNewlineArguments&) const = default;
};

inline constexpr TextEncodingCommandSet kTextEncodingCommandSet{{
    TextEncodingCommandDescriptor{"file.reopen_with_encoding"},
    TextEncodingCommandDescriptor{"file.set_encoding"},
    TextEncodingCommandDescriptor{"file.set_line_ending"},
    TextEncodingCommandDescriptor{"file.set_final_newline"},
}};

} // namespace ssg
