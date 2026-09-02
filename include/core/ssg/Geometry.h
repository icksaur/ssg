#pragma once

// Medium-neutral geometry values shared by semantic UI and presentation edges.

namespace ssg {

struct GridSize {
    int columns = 0;
    int rows = 0;
    friend bool operator==(const GridSize&, const GridSize&) = default;
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr int right() const noexcept { return x + width; }
    [[nodiscard]] constexpr int bottom() const noexcept { return y + height; }
    friend bool operator==(const Rect&, const Rect&) = default;
};

}  // namespace ssg
