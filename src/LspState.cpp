#include <ssg/LspState.h>

namespace ssg {
namespace {

struct Scalar {
    std::uint32_t value = 0;
    std::size_t bytes = 0;
};

std::optional<Scalar> decodeScalar(std::string_view text, std::size_t offset) {
    if (offset >= text.size()) return std::nullopt;
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first <= 0x7f) return Scalar{first, 1};

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
    if (offset + count > text.size()) return std::nullopt;
    for (std::size_t index = 1; index < count; ++index) {
        const auto byte = static_cast<unsigned char>(text[offset + index]);
        if ((byte & 0xc0) != 0x80) return std::nullopt;
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
        if (!scalar) return false;
        offset += scalar->bytes;
    }
    return true;
}

} // namespace

LspPositionResult byteOffsetToLspPosition(std::string_view text,
                                          ByteOffset byteOffset) {
    if (!validUtf8(text)) return {{}, LspPositionError::InvalidUtf8};
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
        if (!scalar) return {{}, LspPositionError::InvalidUtf8};
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
        if (!scalar) return {ByteOffset{}, LspPositionError::InvalidUtf8};
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

} // namespace ssg
