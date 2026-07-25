#include <ssg/color.h>

#include "test_helpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

// An INDEPENDENT reimplementation of the xterm-256 palette and nearest-swatch
// search, authored here so it shares no code with production (src/color.cpp).
// If both agree over a broad sRGB sample and the hand cases, the production
// mapping is pinned.

namespace {

// Independent literal copy of the 16 xterm base colors.
constexpr std::array<ssg::SrgbColor, 16> kRefBase16{{
    {0, 0, 0},       {128, 0, 0},     {0, 128, 0},     {128, 128, 0},
    {0, 0, 128},     {128, 0, 128},   {0, 128, 128},   {192, 192, 192},
    {128, 128, 128}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
    {0, 0, 255},     {255, 0, 255},   {0, 255, 255},   {255, 255, 255},
}};

std::uint8_t refCubeChannel(int level) {
    return level == 0 ? std::uint8_t{0}
                      : static_cast<std::uint8_t>(55 + 40 * level);
}

ssg::SrgbColor refXterm(int index) {
    if (index < 16) return kRefBase16[static_cast<std::size_t>(index)];
    if (index < 232) {
        int const offset = index - 16;
        return {refCubeChannel(offset / 36), refCubeChannel((offset / 6) % 6),
                refCubeChannel(offset % 6)};
    }
    auto const v = static_cast<std::uint8_t>(8 + 10 * (index - 232));
    return {v, v, v};
}

long refDistance(ssg::SrgbColor a, ssg::SrgbColor b) {
    long const dr = static_cast<long>(a.red) - b.red;
    long const dg = static_cast<long>(a.green) - b.green;
    long const db = static_cast<long>(a.blue) - b.blue;
    return dr * dr + dg * dg + db * db;
}

int refNearest(ssg::SrgbColor color, int first, int last) {
    int best = first;
    long bestDistance = refDistance(color, refXterm(first));
    for (int index = first + 1; index <= last; ++index) {
        long const distance = refDistance(color, refXterm(index));
        if (distance < bestDistance) {  // strict: ties keep the lower index
            best = index;
            bestDistance = distance;
        }
    }
    return best;
}

}  // namespace

TEST(truecolorIsIdentity) {
    ssg::SrgbColor const c{37, 200, 9};
    auto r = ssg::resolveColor(c, ssg::ColorDepth::Truecolor);
    ASSERT_TRUE(r.encoding == ssg::ResolvedColor::Encoding::Truecolor);
    ASSERT_TRUE(r.rgb == c);
}

TEST(xterm256SwatchesMatchTheReference) {
    // Boundaries: cube corners, gray-ramp ends, a base color.
    ASSERT_TRUE(ssg::xterm256Color(0) == (ssg::SrgbColor{0, 0, 0}));
    ASSERT_TRUE(ssg::xterm256Color(12) == (ssg::SrgbColor{0, 0, 255}));
    ASSERT_TRUE(ssg::xterm256Color(16) == (ssg::SrgbColor{0, 0, 0}));
    ASSERT_TRUE(ssg::xterm256Color(21) == (ssg::SrgbColor{0, 0, 255}));
    ASSERT_TRUE(ssg::xterm256Color(196) == (ssg::SrgbColor{255, 0, 0}));
    ASSERT_TRUE(ssg::xterm256Color(231) == (ssg::SrgbColor{255, 255, 255}));
    ASSERT_TRUE(ssg::xterm256Color(232) == (ssg::SrgbColor{8, 8, 8}));
    ASSERT_TRUE(ssg::xterm256Color(255) == (ssg::SrgbColor{238, 238, 238}));
    for (int index = 0; index < 256; ++index) {
        ASSERT_TRUE(ssg::xterm256Color(static_cast<std::uint8_t>(index)) ==
                    refXterm(index));
    }
}

TEST(indexed256MatchesReferenceOverBroadSample) {
    for (int r = 0; r <= 255; r += 15) {
        for (int g = 0; g <= 255; g += 15) {
            for (int b = 0; b <= 255; b += 15) {
                ssg::SrgbColor const c{static_cast<std::uint8_t>(r),
                                       static_cast<std::uint8_t>(g),
                                       static_cast<std::uint8_t>(b)};
                auto got = ssg::resolveColor(c, ssg::ColorDepth::Indexed256);
                int const expected = refNearest(c, 16, 255);
                ASSERT_TRUE(got.encoding ==
                            ssg::ResolvedColor::Encoding::Indexed256);
                ASSERT_EQ(static_cast<int>(got.index), expected);
                ASSERT_TRUE(got.rgb == refXterm(expected));
            }
        }
    }
}

TEST(ansi16MatchesReferenceOverBroadSample) {
    for (int r = 0; r <= 255; r += 15) {
        for (int g = 0; g <= 255; g += 15) {
            for (int b = 0; b <= 255; b += 15) {
                ssg::SrgbColor const c{static_cast<std::uint8_t>(r),
                                       static_cast<std::uint8_t>(g),
                                       static_cast<std::uint8_t>(b)};
                auto got = ssg::resolveColor(c, ssg::ColorDepth::Ansi16);
                int const expected = refNearest(c, 0, 15);
                ASSERT_TRUE(got.encoding ==
                            ssg::ResolvedColor::Encoding::Ansi16);
                ASSERT_EQ(static_cast<int>(got.index), expected);
            }
        }
    }
}

TEST(exactSwatchesMapToThemselves) {
    // Every cube and gray swatch resolves to its own index at indexed256.
    for (int index = 16; index < 256; ++index) {
        auto const swatch = ssg::xterm256Color(static_cast<std::uint8_t>(index));
        auto got = ssg::resolveColor(swatch, ssg::ColorDepth::Indexed256);
        ASSERT_EQ(static_cast<int>(got.index), index);
    }
    // Every base color resolves to its own index at ansi16.
    for (int index = 0; index < 16; ++index) {
        auto const swatch = ssg::xterm256Color(static_cast<std::uint8_t>(index));
        auto got = ssg::resolveColor(swatch, ssg::ColorDepth::Ansi16);
        ASSERT_EQ(static_cast<int>(got.index), index);
    }
}

TEST(tiesBreakToTheLowestIndex) {
    // (64,0,0) is exactly equidistant from black(0,0,0)=index 0 and
    // red(128,0,0)=index 1 (64^2 either way); the lower index 0 must win.
    ssg::SrgbColor const midpoint{64, 0, 0};
    ASSERT_EQ(refDistance(midpoint, refXterm(0)), refDistance(midpoint, refXterm(1)));
    auto got = ssg::resolveColor(midpoint, ssg::ColorDepth::Ansi16);
    ASSERT_EQ(static_cast<int>(got.index), 0);
}


// ---------------------------------------------------------------------------
// Background tint adjustment (doc/spec-background-tint-adjust.md).

// An INDEPENDENT HSL round trip, authored here rather than shared with
// production, so agreement between the two pins the transform.
struct RefHsl { double h, s, l; };

RefHsl refToHsl(ssg::SrgbColor c) {
    const double r = c.red / 255.0, g = c.green / 255.0, b = c.blue / 255.0;
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    const double l = (mx + mn) / 2.0;
    if (mx == mn) return {0.0, 0.0, l};
    const double d = mx - mn;
    const double s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
    double h = 0.0;
    if (mx == r) h = (g - b) / d + (g < b ? 6.0 : 0.0);
    else if (mx == g) h = (b - r) / d + 2.0;
    else h = (r - g) / d + 4.0;
    return {h / 6.0, s, l};
}

double refHueChannel(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0 / 2.0) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

ssg::SrgbColor refFromHsl(RefHsl v) {
    const auto quantize = [](double channel) {
        const double scaled = channel * 255.0;
        const double clamped = scaled < 0.0 ? 0.0 : (scaled > 255.0 ? 255.0 : scaled);
        return static_cast<std::uint8_t>(clamped + 0.5);
    };
    if (v.s == 0.0) {
        const auto grey = quantize(v.l);
        return {grey, grey, grey};
    }
    const double q = v.l < 0.5 ? v.l * (1.0 + v.s) : v.l + v.s - v.l * v.s;
    const double p = 2.0 * v.l - q;
    return {quantize(refHueChannel(p, q, v.h + 1.0 / 3.0)),
            quantize(refHueChannel(p, q, v.h)),
            quantize(refHueChannel(p, q, v.h - 1.0 / 3.0))};
}

ssg::SrgbColor refAdjust(ssg::SrgbColor c, float brightness, float saturation) {
    auto hsl = refToHsl(c);
    const auto clamp01 = [](double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); };
    hsl.s = clamp01(hsl.s * saturation);
    hsl.l = clamp01(hsl.l * brightness);
    return refFromHsl(hsl);
}

// The property the shipped 1.0/1.0 default depends on.  An sRGB -> HSL -> sRGB
// round trip is lossy, so this can only hold via an explicit short-circuit.
TEST(neutralAdjustmentIsExactlyIdentity) {
    const auto theme = ssg::defaultTheme();
    for (const auto& color : theme.palette) {
        ASSERT_EQ(ssg::adjustBackgroundTint(color, {}), color);
        ASSERT_EQ(ssg::adjustBackgroundTint(color, {1.0f, 1.0f}), color);
    }
    // Also across a broad sample, not just the 16 theme colors.
    for (int r = 0; r <= 255; r += 17) {
        for (int g = 0; g <= 255; g += 17) {
            for (int b = 0; b <= 255; b += 51) {
                const ssg::SrgbColor c{static_cast<std::uint8_t>(r),
                                       static_cast<std::uint8_t>(g),
                                       static_cast<std::uint8_t>(b)};
                ASSERT_EQ(ssg::adjustBackgroundTint(c, {}), c);
            }
        }
    }
}

TEST(adjustmentMatchesAnIndependentHslImplementation) {
    const std::array<std::pair<float, float>, 6> cases{{
        {0.5f, 1.0f}, {1.5f, 1.0f}, {1.0f, 0.5f},
        {1.0f, 1.5f}, {0.8f, 0.6f}, {1.2f, 1.3f},
    }};
    for (const auto& [brightness, saturation] : cases) {
        for (int r = 0; r <= 255; r += 51) {
            for (int g = 0; g <= 255; g += 51) {
                for (int b = 0; b <= 255; b += 85) {
                    const ssg::SrgbColor c{static_cast<std::uint8_t>(r),
                                           static_cast<std::uint8_t>(g),
                                           static_cast<std::uint8_t>(b)};
                    ASSERT_EQ(ssg::adjustBackgroundTint(c, {brightness, saturation}),
                              refAdjust(c, brightness, saturation));
                }
            }
        }
    }
}

TEST(zeroSaturationIsGreyAndZeroBrightnessIsBlack) {
    const ssg::SrgbColor vivid{200, 60, 40};
    const auto grey = ssg::adjustBackgroundTint(vivid, {1.0f, 0.0f});
    ASSERT_EQ(grey.red, grey.green);
    ASSERT_EQ(grey.green, grey.blue);
    const auto black = ssg::adjustBackgroundTint(vivid, {0.0f, 1.0f});
    ASSERT_EQ(black, (ssg::SrgbColor{0, 0, 0}));
}

// Clamping must saturate, never wrap: a wrapped value would make a large
// multiplier produce a DARKER color, which is the opposite of what was asked.
TEST(largeMultipliersClampRatherThanWrap) {
    for (const auto& c : ssg::defaultTheme().palette) {
        const auto bright = ssg::adjustBackgroundTint(c, {1000.0f, 1.0f});
        ASSERT_EQ(bright, (ssg::SrgbColor{255, 255, 255}));
        const auto saturated = ssg::adjustBackgroundTint(c, {1.0f, 1000.0f});
        // Saturating cannot darken the lightness axis.
        ASSERT_TRUE(refToHsl(saturated).l >= refToHsl(c).l - 0.01);
    }
}

// Hue is identity under any multiplier, so a wash never changes what it means.
TEST(hueIsNeverModified) {
    const std::array<ssg::SrgbColor, 4> colors{{
        {200, 60, 40}, {60, 200, 80}, {70, 90, 220}, {210, 200, 60}}};
    for (const auto& c : colors) {
        const double before = refToHsl(c).h;
        for (float m : {0.3f, 0.7f, 1.4f, 2.0f}) {
            const auto adjusted = ssg::adjustBackgroundTint(c, {m, m});
            const auto hsl = refToHsl(adjusted);
            // Achromatic results have no meaningful hue; skip those.
            if (hsl.s == 0.0 || hsl.l == 0.0 || hsl.l == 1.0) continue;
            ASSERT_TRUE(std::abs(hsl.h - before) < 0.02);
        }
    }
}

int main() {
    RUN(truecolorIsIdentity);
    RUN(xterm256SwatchesMatchTheReference);
    RUN(indexed256MatchesReferenceOverBroadSample);
    RUN(ansi16MatchesReferenceOverBroadSample);
    RUN(exactSwatchesMapToThemselves);
    RUN(tiesBreakToTheLowestIndex);
    RUN(neutralAdjustmentIsExactlyIdentity);
    RUN(adjustmentMatchesAnIndependentHslImplementation);
    RUN(zeroSaturationIsGreyAndZeroBrightnessIsBlack);
    RUN(largeMultipliersClampRatherThanWrap);
    RUN(hueIsNeverModified);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
