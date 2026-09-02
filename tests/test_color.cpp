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
    auto r = ssg::ColorResolver{ssg::ColorDepth::Truecolor}.resolve(c);
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
                auto got = ssg::ColorResolver{ssg::ColorDepth::Indexed256}.resolve(c);
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
                auto got = ssg::ColorResolver{ssg::ColorDepth::Ansi16}.resolve(c);
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
        auto got = ssg::ColorResolver{ssg::ColorDepth::Indexed256}.resolve(swatch);
        ASSERT_EQ(static_cast<int>(got.index), index);
    }
    // Every base color resolves to its own index at ansi16.
    for (int index = 0; index < 16; ++index) {
        auto const swatch = ssg::xterm256Color(static_cast<std::uint8_t>(index));
        auto got = ssg::ColorResolver{ssg::ColorDepth::Ansi16}.resolve(swatch);
        ASSERT_EQ(static_cast<int>(got.index), index);
    }
}

TEST(tiesBreakToTheLowestIndex) {
    // (64,0,0) is exactly equidistant from black(0,0,0)=index 0 and
    // red(128,0,0)=index 1 (64^2 either way); the lower index 0 must win.
    ssg::SrgbColor const midpoint{64, 0, 0};
    ASSERT_EQ(refDistance(midpoint, refXterm(0)), refDistance(midpoint, refXterm(1)));
    auto got = ssg::ColorResolver{ssg::ColorDepth::Ansi16}.resolve(midpoint);
    ASSERT_EQ(static_cast<int>(got.index), 0);
}


SSG_TEST_SUITE(test_color) {
    RUN(truecolorIsIdentity);
    RUN(xterm256SwatchesMatchTheReference);
    RUN(indexed256MatchesReferenceOverBroadSample);
    RUN(ansi16MatchesReferenceOverBroadSample);
    RUN(exactSwatchesMapToThemselves);
    RUN(tiesBreakToTheLowestIndex);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}

