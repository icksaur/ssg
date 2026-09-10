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
//     CR/LF cases from GB3–GB5 are excluded by the logical-line precondition;
//     GB4/GB5 boundaries around every other GCB=Control are enforced.
//   - Terminal display-width mapping (UAX #11 EAW + emoji-data, Unicode 15.0.0)
//
// This module does NOT implement line wrapping, scrollbars, viewports, or
// any renderer-specific type (see viewport-wrap-scrollbar for those).
//
// Invariant I7: layout is deterministic and renderer-neutral. Cell widths
// follow UAX #11 East Asian Width (W/F -> 2 cells) plus emoji-data, with tab
// expansion, control-glyph widths, and invalid-byte handling as implemented
// here and pinned by tests/test_cell_layout.cpp.

#include <cstdint>
#include <string_view>
#include <vector>

namespace ssg {

// Kind of grapheme cluster in a cell run.
enum class CellKind : uint8_t {
    // Printable grapheme cluster (narrow or wide), OR a lone extending code
    // point (GCB=Extend/ZWJ/SpacingMark) that has nonzero display width (e.g.
    // a standalone wide emoji modifier such as U+1F3FB–U+1F3FF with EAW=W).
    // cell_width is 1 (narrow) or 2 (wide per UAX #11 / Emoji_Presentation).
    Text,

    // Zero-width combining cluster.  The base code point is a combining,
    // variation-selector, or zero-width extending character with no prior base
    // in the logical line, and its display width is 0.
    // cell_width is always 0.
    Combining,

    // Horizontal tab (U+0009).
    // cell_width advances to the next tab stop (1..tab_width columns).
    Tab,

    // GCB=Control cluster. C0, DEL, and C1 controls render as a visible
    // 1-cell replacement glyph; zero-width format controls consume 0 cells.
    Control,

    // Invalid UTF-8 byte.  Each individual invalid byte yields one span with
    // cell_width 1 (rendered as a visible replacement glyph).
    InvalidUtf8,
};

// One grapheme cluster mapped to its display columns.
struct CellSpan {
    uint32_t byteOffset;  // Start byte offset within the input line
    uint32_t byteLen;     // Byte length of this grapheme cluster (>= 1)
    uint32_t cellWidth;   // Display columns consumed by this cluster:
                           //   0  for CellKind::combining
                           //   1  for narrow text, control, invalid_utf8
                           //   2  for wide text
                           //   1..tab_width for tab (positional)
    CellKind kind;
};

// Cell run for one logical line.  Contains one CellSpan per grapheme cluster.
struct CellRun {
    std::vector<CellSpan> spans;
    uint32_t totalCells;  // Sum of all span.cell_width values
};

// `lineUtf8` must not contain line breaks. Invalid UTF-8 bytes each produce one
// single-cell InvalidUtf8 span. Combining marks, variation selectors, ZWJ
// sequences, and regional-indicator pairs extend the preceding span.
// Throws std::invalid_argument when `tabWidth` is outside [1, 16].
CellRun computeCellRun(std::string_view lineUtf8, int tabWidth = 4);

}  // namespace ssg
