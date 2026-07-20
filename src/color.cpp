#include <ssg/color.h>

#include <array>
#include <cstdint>

namespace ssg {

namespace {

// The 16 standard xterm base colors (indices 0..15).  These are the canonical
// xterm defaults; a terminal may re-theme them, which is the accepted limitation
// of reducing to 16 colors.
constexpr std::array<SrgbColor, 16> k_base16{{
    {0, 0, 0},       {128, 0, 0},     {0, 128, 0},     {128, 128, 0},
    {0, 0, 128},     {128, 0, 128},   {0, 128, 128},   {192, 192, 192},
    {128, 128, 128}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
    {0, 0, 255},     {255, 0, 255},   {0, 255, 255},   {255, 255, 255},
}};

// The 6-level cube channel values: level 0 is 0, levels 1..5 are 55 + 40*level.
constexpr std::uint8_t cube_channel(int level) {
    return level == 0 ? std::uint8_t{0}
                      : static_cast<std::uint8_t>(55 + 40 * level);
}

std::uint32_t squared_distance(SrgbColor a, SrgbColor b) {
    int const dr = static_cast<int>(a.red) - static_cast<int>(b.red);
    int const dg = static_cast<int>(a.green) - static_cast<int>(b.green);
    int const db = static_cast<int>(a.blue) - static_cast<int>(b.blue);
    return static_cast<std::uint32_t>(dr * dr + dg * dg + db * db);
}

// Nearest index in [first, last] (inclusive) whose xterm swatch is closest to
// `color`; ties break to the lowest index so the result is deterministic.
std::uint8_t nearest_index(SrgbColor color, int first, int last) {
    std::uint8_t best = static_cast<std::uint8_t>(first);
    std::uint32_t best_distance =
        squared_distance(color, xterm256_color(best));
    for (int index = first + 1; index <= last; ++index) {
        auto const candidate = static_cast<std::uint8_t>(index);
        std::uint32_t const distance =
            squared_distance(color, xterm256_color(candidate));
        if (distance < best_distance) {
            best = candidate;
            best_distance = distance;
        }
    }
    return best;
}

}  // namespace

SrgbColor xterm256_color(std::uint8_t index) {
    if (index < 16) {
        return k_base16[index];
    }
    if (index < 232) {
        int const offset = index - 16;
        int const r = offset / 36;
        int const g = (offset / 6) % 6;
        int const b = offset % 6;
        return {cube_channel(r), cube_channel(g), cube_channel(b)};
    }
    // 232..255: 24-step gray ramp, value = 8 + 10*step.
    auto const value = static_cast<std::uint8_t>(8 + 10 * (index - 232));
    return {value, value, value};
}

ResolvedColor resolve_color(SrgbColor color, ColorDepth depth) {
    switch (depth) {
        case ColorDepth::Truecolor:
            return {ResolvedColor::Encoding::Truecolor, 0, color};
        case ColorDepth::Indexed256: {
            // Search the cube and gray ramp (16..255); the configurable system
            // colors 0..15 are excluded so the mapping is deterministic.
            std::uint8_t const index = nearest_index(color, 16, 255);
            return {ResolvedColor::Encoding::Indexed256, index,
                    xterm256_color(index)};
        }
        case ColorDepth::Ansi16: {
            std::uint8_t const index = nearest_index(color, 0, 15);
            return {ResolvedColor::Encoding::Ansi16, index,
                    xterm256_color(index)};
        }
    }
    return {ResolvedColor::Encoding::Truecolor, 0, color};
}

}  // namespace ssg
