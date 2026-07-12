#pragma once

// UTF-8 grapheme segmentation and terminal cell layout.
//
// This module emits deterministic, renderer-neutral monospace cell runs for
// individual logical lines of UTF-8 text.  It implements:
//   - UTF-8 decoding (each invalid byte yields one replacement span)
//   - Extended grapheme cluster segmentation (UAX #29, Unicode 15.0.0)
//     Rules: GB6–GB8 (Hangul), GB9 (Extend/ZWJ), GB9a (SpacingMark),
//            GB9b (Prepend), GB11 (Extended_Pictographic ZWJ sequences),
//            GB12/13 (Regional Indicator flag pairs).
//     GB3–GB5 are subsumed by the no-CR/LF input precondition.
//   - Terminal display-width mapping (UAX #11 EAW + emoji-data, Unicode 15.0.0)
//
// This module does NOT implement line wrapping, scrollbars, viewports, or
// any renderer-specific type (see viewport-wrap-scrollbar for those).
//
// Invariant I7: layout is deterministic and renderer-neutral.
// See doc/features/presentation-shell.md §Cell-width rules for the normative
// contract, including tab expansion, control glyph widths, and invalid-byte
// handling.

#include <cstdint>
#include <string_view>
#include <vector>

namespace ssg {

// Kind of grapheme cluster in a cell run.
enum class CellKind : uint8_t {
    // Printable grapheme cluster (narrow or wide).
    // cell_width is 1 (narrow) or 2 (wide per UAX #11 / emoji-data).
    text,

    // Zero-width combining cluster.  The base code point is itself a combining
    // or zero-width character with no prior base in the logical line.
    // cell_width is always 0.
    combining,

    // Horizontal tab (U+0009).
    // cell_width advances to the next tab stop (1..tab_width columns).
    tab,

    // Non-tab C0 control (U+0000–U+0008, U+000A–U+001F), DEL (U+007F), or
    // C1 control (U+0080–U+009F).  Rendered as a visible 1-cell replacement
    // glyph; cell_width is always 1.
    control,

    // Invalid UTF-8 byte.  Each individual invalid byte yields one span with
    // cell_width 1 (rendered as a visible replacement glyph).
    invalid_utf8,
};

// One grapheme cluster mapped to its display columns.
struct CellSpan {
    uint32_t byte_offset;  // Start byte offset within the input line
    uint32_t byte_len;     // Byte length of this grapheme cluster (>= 1)
    uint32_t cell_width;   // Display columns consumed by this cluster:
                           //   0  for CellKind::combining
                           //   1  for narrow text, control, invalid_utf8
                           //   2  for wide text
                           //   1..tab_width for tab (positional)
    CellKind kind;
};

// Cell run for one logical line.  Contains one CellSpan per grapheme cluster.
struct CellRun {
    std::vector<CellSpan> spans;
    uint32_t total_cells;  // Sum of all span.cell_width values
};

// Compute the cell run for one logical line of UTF-8 text.
//
// Preconditions:
//   - line_utf8 must not contain '\n' or '\r'.
//   - tab_width must be in [1, 16].
//
// Each invalid UTF-8 byte (lone continuation, overlong lead, truncated
// multi-byte sequence, or byte > U+10FFFF encoding range) yields exactly one
// CellSpan with kind=invalid_utf8 and cell_width=1.
//
// Grapheme cluster extensions (combining marks, variation selectors, ZWJ
// sequences, regional-indicator flag pairs) are absorbed into the preceding
// cluster's byte_len; they do not produce additional spans.
CellRun compute_cell_run(std::string_view line_utf8, int tab_width = 4);

}  // namespace ssg
