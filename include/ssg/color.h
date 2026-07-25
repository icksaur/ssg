#pragma once

// Color-depth adaptation (M9-C, doc/spec-terminal-robustness.md).
//
// Themes are the sole source of color (spec.md I22): CellGrid carries the 16
// authoritative palette colors plus Theme-derived DiffTints. A client never
// mints or substitutes editor color; resolveColor only depth-adapts those theme
// colors to the nearest color its output medium can display.

#include <ssg/Theme.h>

#include <cstdint>

namespace ssg {

// The color capability a terminal advertises.
enum class ColorDepth : std::uint8_t {
    Ansi16,       // 16 base ANSI colors (SGR 30-37/90-97).
    Indexed256,   // xterm 256-color (SGR 38;5;n) — the 6x6x6 cube and gray ramp.
    Truecolor,    // 24-bit direct color (SGR 38;2;r;g;b).
};

struct ResolvedColor {
    enum class Encoding : std::uint8_t { Ansi16, Indexed256, Truecolor };

    Encoding encoding = Encoding::Truecolor;
    std::uint8_t index = 0;   // ansi16: 0..15; indexed256: 16..255; unused for truecolor.
    SrgbColor rgb; // truecolor: the exact channels; else the swatch's channels.

    friend bool operator==(ResolvedColor const&, ResolvedColor const&) = default;
};

// Map a theme color to a terminal-representable color at the given depth:
//   truecolor   -> identity (encoding=truecolor, rgb=color).
//   indexed256  -> nearest of the xterm 6x6x6 cube (16..231) and 24-step gray
//                  ramp (232..255) by squared Euclidean distance in sRGB.  The
//                  configurable system colors 0..15 are excluded so the mapping
//                  is deterministic.
//   ansi16      -> nearest of the 16 standard xterm base colors by the same
//                  metric.  These are assumed values; a terminal may re-theme
//                  them (an accepted limitation of a 16-color terminal).
// Ties break to the lowest index, so the result is a pure function with a single
// answer.  `rgb` on a reduced result carries the chosen swatch's canonical
// channels (for index-less clients and tests).
[[nodiscard]] ResolvedColor resolveColor(SrgbColor color, ColorDepth depth);

// The canonical sRGB channels of xterm palette index `index` (0..255): the 16
// base colors (0..15), the 6x6x6 cube (16..231), and the 24-step gray ramp
// (232..255).  Exposed for clients and tests that need the swatch behind an
// index.
[[nodiscard]] SrgbColor xterm256Color(std::uint8_t index);

// Scales a color's HSL lightness and saturation by `adjustment`, clamping both
// to [0, 1].  Hue is never modified, so a wash cannot change identity -- red
// stays red however hard it is pushed -- and clamping saturates rather than
// wraps, so a large multiplier yields white rather than a dark value.
//
// A neutral adjustment returns `color` EXACTLY.  Measured: this round trip is
// in fact exact for all 16.7M sRGB colors, so the short-circuit is not what
// makes identity hold today -- it is a guard so that identity CANNOT stop
// holding if the math here is ever changed (a different rounding rule or color
// space would silently perturb every color at the shipped 1.0/1.0 default).
[[nodiscard]] SrgbColor adjustBackgroundTint(SrgbColor color,
                                             TintAdjustment adjustment);

}  // namespace ssg
