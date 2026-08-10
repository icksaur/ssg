#pragma once

// Strong domain types for canonical document positions and state.
//
// These types are shared across all SSG feature components. They carry their
// value explicitly and prevent accidental mixing of incompatible integer
// indices. All constructors are constexpr and noexcept; there is no
// construction-time validation because the values are unconstrained ordinals
// (any uint64_t is a valid byte offset, line index, etc.).
//
// Construction failure contract: these types have no invalid values; types
// with invariants live in config.h.

#include <compare>
#include <cstdint>

namespace ssg {

// Monotonically increasing session revision. Revision{0} is the null
// sentinel.  Accepted state changes are totally ordered (spec invariant I3).
struct Revision {
    explicit constexpr Revision(uint64_t v = 0) noexcept : value_{v} {}
    [[nodiscard]] constexpr uint64_t value() const noexcept { return value_; }
    constexpr auto operator<=>(Revision const&) const noexcept = default;

private:
    uint64_t value_;
};

// Zero-based byte offset into a UTF-8 document buffer.  Byte offsets are
// canonical for document mutation; line and cell indices are derived (spec).
struct ByteOffset {
    explicit constexpr ByteOffset(uint64_t v = 0) noexcept : value_{v} {}
    [[nodiscard]] constexpr uint64_t value() const noexcept { return value_; }
    constexpr auto operator<=>(ByteOffset const&) const noexcept = default;

private:
    uint64_t value_;
};

// Zero-based line index within a document (zero-based, spec §Design).
struct LineIndex {
    explicit constexpr LineIndex(uint64_t v = 0) noexcept : value_{v} {}
    [[nodiscard]] constexpr uint64_t value() const noexcept { return value_; }
    constexpr auto operator<=>(LineIndex const&) const noexcept = default;

private:
    uint64_t value_;
};

// Zero-based monospace display-cell index within a rendered line.  Tab
// stops, wide graphemes, and combining marks affect this independently of
// byte offset; cell layout must match the project-pinned Unicode width tables
// (spec §Design, I7).
struct CellIndex {
    explicit constexpr CellIndex(uint64_t v = 0) noexcept : value_{v} {}
    [[nodiscard]] constexpr uint64_t value() const noexcept { return value_; }
    constexpr auto operator<=>(CellIndex const&) const noexcept = default;

private:
    uint64_t value_;
};

// Complete document position as it crosses API boundaries (spec §Design).
// All three coordinates must be consistent for the same revision; stale or
// inconsistent positions produce a typed protocol error, not a guessed result.
struct DocumentPosition {
    ByteOffset byteOffset;
    LineIndex  line;
    CellIndex  cell;

    constexpr bool operator==(DocumentPosition const&) const noexcept = default;
};

// Document editing mode (spec §Design, DocumentMode).
// The set of modes is closed; all values are always valid.
enum class DocumentMode : uint8_t {
    Edit,
    ReadOnly,
    Diff,
};

}  // namespace ssg
