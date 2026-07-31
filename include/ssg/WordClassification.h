#pragma once

#include <cstdint>

namespace ssg {

// The single definition of a "word byte" shared by every word-aware feature:
// word motions (cursor.word_*), word selection (select.word_*), the
// next-occurrence needle (select.add_next_occurrence), and the find-word-under-
// cursor seed.  A word byte is an ASCII letter, digit, `_`, or any non-ASCII
// byte (>= 0x80), so multi-byte UTF-8 sequences are treated as word content.
// Keeping this in one place stops those features from disagreeing about where a
// word begins and ends.
[[nodiscard]] inline bool isWordByte(unsigned char byte) noexcept {
    return byte >= 0x80 || (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
           byte == '_';
}

}  // namespace ssg
