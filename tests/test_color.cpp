#include <ssg/color.h>

#include "test_helpers.h"

#include <array>
#include <cstdint>

// An INDEPENDENT reimplementation of the xterm-256 palette and nearest-swatch
// search, authored here so it shares no code with production (src/color.cpp).
// If both agree over a broad sRGB sample and the hand cases, the production
// mapping is pinned.

namespace {

// Independent literal copy of the 16 xterm base colors.
constexpr std::array<ssg::SrgbColor, 16> ref_base16{{
    {0, 0, 0},       {128, 0, 0},     {0, 128, 0},     {128, 128, 0},
    {0, 0, 128},     {128, 0, 128},   {0, 128, 128},   {192, 192, 192},
    {128, 128, 128}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
    {0, 0, 255},     {255, 0, 255},   {0, 255, 255},   {255, 255, 255},
}};

std::uint8_t ref_cube_channel(int level) {
    return level == 0 ? std::uint8_t{0}
                      : static_cast<std::uint8_t>(55 + 40 * level);
}

ssg::SrgbColor ref_xterm(int index) {
    if (index < 16) return ref_base16[static_cast<std::size_t>(index)];
    if (index < 232) {
        int const offset = index - 16;
        return {ref_cube_channel(offset / 36), ref_cube_channel((offset / 6) % 6),
                ref_cube_channel(offset % 6)};
    }
    auto const v = static_cast<std::uint8_t>(8 + 10 * (index - 232));
    return {v, v, v};
}

long ref_distance(ssg::SrgbColor a, ssg::SrgbColor b) {
    long const dr = static_cast<long>(a.red) - b.red;
    long const dg = static_cast<long>(a.green) - b.green;
    long const db = static_cast<long>(a.blue) - b.blue;
    return dr * dr + dg * dg + db * db;
}

int ref_nearest(ssg::SrgbColor color, int first, int last) {
    int best = first;
    long best_distance = ref_distance(color, ref_xterm(first));
    for (int index = first + 1; index <= last; ++index) {
        long const distance = ref_distance(color, ref_xterm(index));
        if (distance < best_distance) {  // strict: ties keep the lower index
            best = index;
            best_distance = distance;
        }
    }
    return best;
}

}  // namespace

TEST(truecolor_is_identity) {
    ssg::SrgbColor const c{37, 200, 9};
    auto r = ssg::resolve_color(c, ssg::ColorDepth::truecolor);
    ASSERT_TRUE(r.encoding == ssg::ResolvedColor::Encoding::truecolor);
    ASSERT_TRUE(r.rgb == c);
}

TEST(xterm256_swatches_match_the_reference) {
    // Boundaries: cube corners, gray-ramp ends, a base color.
    ASSERT_TRUE(ssg::xterm256_color(0) == (ssg::SrgbColor{0, 0, 0}));
    ASSERT_TRUE(ssg::xterm256_color(12) == (ssg::SrgbColor{0, 0, 255}));
    ASSERT_TRUE(ssg::xterm256_color(16) == (ssg::SrgbColor{0, 0, 0}));
    ASSERT_TRUE(ssg::xterm256_color(21) == (ssg::SrgbColor{0, 0, 255}));
    ASSERT_TRUE(ssg::xterm256_color(196) == (ssg::SrgbColor{255, 0, 0}));
    ASSERT_TRUE(ssg::xterm256_color(231) == (ssg::SrgbColor{255, 255, 255}));
    ASSERT_TRUE(ssg::xterm256_color(232) == (ssg::SrgbColor{8, 8, 8}));
    ASSERT_TRUE(ssg::xterm256_color(255) == (ssg::SrgbColor{238, 238, 238}));
    for (int index = 0; index < 256; ++index) {
        ASSERT_TRUE(ssg::xterm256_color(static_cast<std::uint8_t>(index)) ==
                    ref_xterm(index));
    }
}

TEST(indexed256_matches_reference_over_broad_sample) {
    for (int r = 0; r <= 255; r += 15) {
        for (int g = 0; g <= 255; g += 15) {
            for (int b = 0; b <= 255; b += 15) {
                ssg::SrgbColor const c{static_cast<std::uint8_t>(r),
                                       static_cast<std::uint8_t>(g),
                                       static_cast<std::uint8_t>(b)};
                auto got = ssg::resolve_color(c, ssg::ColorDepth::indexed256);
                int const expected = ref_nearest(c, 16, 255);
                ASSERT_TRUE(got.encoding ==
                            ssg::ResolvedColor::Encoding::indexed256);
                ASSERT_EQ(static_cast<int>(got.index), expected);
                ASSERT_TRUE(got.rgb == ref_xterm(expected));
            }
        }
    }
}

TEST(ansi16_matches_reference_over_broad_sample) {
    for (int r = 0; r <= 255; r += 15) {
        for (int g = 0; g <= 255; g += 15) {
            for (int b = 0; b <= 255; b += 15) {
                ssg::SrgbColor const c{static_cast<std::uint8_t>(r),
                                       static_cast<std::uint8_t>(g),
                                       static_cast<std::uint8_t>(b)};
                auto got = ssg::resolve_color(c, ssg::ColorDepth::ansi16);
                int const expected = ref_nearest(c, 0, 15);
                ASSERT_TRUE(got.encoding ==
                            ssg::ResolvedColor::Encoding::ansi16);
                ASSERT_EQ(static_cast<int>(got.index), expected);
            }
        }
    }
}

TEST(exact_swatches_map_to_themselves) {
    // Every cube and gray swatch resolves to its own index at indexed256.
    for (int index = 16; index < 256; ++index) {
        auto const swatch = ssg::xterm256_color(static_cast<std::uint8_t>(index));
        auto got = ssg::resolve_color(swatch, ssg::ColorDepth::indexed256);
        ASSERT_EQ(static_cast<int>(got.index), index);
    }
    // Every base color resolves to its own index at ansi16.
    for (int index = 0; index < 16; ++index) {
        auto const swatch = ssg::xterm256_color(static_cast<std::uint8_t>(index));
        auto got = ssg::resolve_color(swatch, ssg::ColorDepth::ansi16);
        ASSERT_EQ(static_cast<int>(got.index), index);
    }
}

TEST(ties_break_to_the_lowest_index) {
    // (64,0,0) is exactly equidistant from black(0,0,0)=index 0 and
    // red(128,0,0)=index 1 (64^2 either way); the lower index 0 must win.
    ssg::SrgbColor const midpoint{64, 0, 0};
    ASSERT_EQ(ref_distance(midpoint, ref_xterm(0)), ref_distance(midpoint, ref_xterm(1)));
    auto got = ssg::resolve_color(midpoint, ssg::ColorDepth::ansi16);
    ASSERT_EQ(static_cast<int>(got.index), 0);
}

int main() {
    RUN(truecolor_is_identity);
    RUN(xterm256_swatches_match_the_reference);
    RUN(indexed256_matches_reference_over_broad_sample);
    RUN(ansi16_matches_reference_over_broad_sample);
    RUN(exact_swatches_map_to_themselves);
    RUN(ties_break_to_the_lowest_index);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
