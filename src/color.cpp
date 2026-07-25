#include <ssg/color.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace ssg {

namespace {

// The 16 standard xterm base colors (indices 0..15).  These are the canonical
// xterm defaults; a terminal may re-theme them, which is the accepted limitation
// of reducing to 16 colors.
constexpr std::array<SrgbColor, 16> kBase16{{
    {0, 0, 0},       {128, 0, 0},     {0, 128, 0},     {128, 128, 0},
    {0, 0, 128},     {128, 0, 128},   {0, 128, 128},   {192, 192, 192},
    {128, 128, 128}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
    {0, 0, 255},     {255, 0, 255},   {0, 255, 255},   {255, 255, 255},
}};

// HSL, used only to scale a background wash's lightness and saturation.  Hue is
// carried through untouched.
struct Hsl {
    double hue = 0.0;
    double saturation = 0.0;
    double lightness = 0.0;
};

double clamp01(double value) {
    return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
}

Hsl toHsl(SrgbColor color) {
    const double red = color.red / 255.0;
    const double green = color.green / 255.0;
    const double blue = color.blue / 255.0;
    const double high = std::max({red, green, blue});
    const double low = std::min({red, green, blue});
    const double lightness = (high + low) / 2.0;
    if (high == low) return {0.0, 0.0, lightness};  // Achromatic: hue undefined.

    const double span = high - low;
    const double saturation =
        lightness > 0.5 ? span / (2.0 - high - low) : span / (high + low);
    double hue = 0.0;
    if (high == red) {
        hue = (green - blue) / span + (green < blue ? 6.0 : 0.0);
    } else if (high == green) {
        hue = (blue - red) / span + 2.0;
    } else {
        hue = (red - green) / span + 4.0;
    }
    return {hue / 6.0, saturation, lightness};
}

double hueChannel(double p, double q, double t) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0 / 2.0) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

std::uint8_t quantize(double channel) {
    return static_cast<std::uint8_t>(clamp01(channel) * 255.0 + 0.5);
}

SrgbColor fromHsl(Hsl value) {
    if (value.saturation == 0.0) {
        const auto grey = quantize(value.lightness);
        return {grey, grey, grey};
    }
    const double q = value.lightness < 0.5
                         ? value.lightness * (1.0 + value.saturation)
                         : value.lightness + value.saturation -
                               value.lightness * value.saturation;
    const double p = 2.0 * value.lightness - q;
    return {quantize(hueChannel(p, q, value.hue + 1.0 / 3.0)),
            quantize(hueChannel(p, q, value.hue)),
            quantize(hueChannel(p, q, value.hue - 1.0 / 3.0))};
}

// The 6-level cube channel values: level 0 is 0, levels 1..5 are 55 + 40*level.
constexpr std::uint8_t cubeChannel(int level) {
    return level == 0 ? std::uint8_t{0}
                      : static_cast<std::uint8_t>(55 + 40 * level);
}

std::uint32_t squaredDistance(SrgbColor a, SrgbColor b) {
    int const dr = static_cast<int>(a.red) - static_cast<int>(b.red);
    int const dg = static_cast<int>(a.green) - static_cast<int>(b.green);
    int const db = static_cast<int>(a.blue) - static_cast<int>(b.blue);
    return static_cast<std::uint32_t>(dr * dr + dg * dg + db * db);
}

// Nearest index in [first, last] (inclusive) whose xterm swatch is closest to
// `color`; ties break to the lowest index so the result is deterministic.
std::uint8_t nearestIndex(SrgbColor color, int first, int last) {
    std::uint8_t best = static_cast<std::uint8_t>(first);
    std::uint32_t bestDistance =
        squaredDistance(color, xterm256Color(best));
    for (int index = first + 1; index <= last; ++index) {
        auto const candidate = static_cast<std::uint8_t>(index);
        std::uint32_t const distance =
            squaredDistance(color, xterm256Color(candidate));
        if (distance < bestDistance) {
            best = candidate;
            bestDistance = distance;
        }
    }
    return best;
}

}  // namespace

SrgbColor xterm256Color(std::uint8_t index) {
    if (index < 16) {
        return kBase16[index];
    }
    if (index < 232) {
        int const offset = index - 16;
        int const r = offset / 36;
        int const g = (offset / 6) % 6;
        int const b = offset % 6;
        return {cubeChannel(r), cubeChannel(g), cubeChannel(b)};
    }
    // 232..255: 24-step gray ramp, value = 8 + 10*step.
    auto const value = static_cast<std::uint8_t>(8 + 10 * (index - 232));
    return {value, value, value};
}

SrgbColor adjustBackgroundTint(SrgbColor color, TintAdjustment adjustment) {
    // Exact identity for the shipped defaults.  Measured, the round trip below
    // is already exact for every sRGB color, so this is a guard rather than a
    // fix: it keeps identity true if the math is ever changed.
    if (adjustment.neutral()) return color;

    const auto hsl = toHsl(color);
    return fromHsl({hsl.hue, clamp01(hsl.saturation * adjustment.saturation),
                    clamp01(hsl.lightness * adjustment.brightness)});
}

ResolvedColor resolveColor(SrgbColor color, ColorDepth depth) {
    switch (depth) {
        case ColorDepth::Truecolor:
            return {ResolvedColor::Encoding::Truecolor, 0, color};
        case ColorDepth::Indexed256: {
            // Search the cube and gray ramp (16..255); the configurable system
            // colors 0..15 are excluded so the mapping is deterministic.
            std::uint8_t const index = nearestIndex(color, 16, 255);
            return {ResolvedColor::Encoding::Indexed256, index,
                    xterm256Color(index)};
        }
        case ColorDepth::Ansi16: {
            std::uint8_t const index = nearestIndex(color, 0, 15);
            return {ResolvedColor::Encoding::Ansi16, index,
                    xterm256Color(index)};
        }
    }
    return {ResolvedColor::Encoding::Truecolor, 0, color};
}

}  // namespace ssg
