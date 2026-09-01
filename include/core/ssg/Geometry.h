#pragma once

// The shell's core geometry value types. Extracted from ShellState.h so the
// widget layer (Widget.h -> WidgetStack) can measure/place rects without pulling
// in the whole shell surface -- ShellState.h is the HIGH layer (it owns the
// layout request, which now carries a composed-chrome descriptor), while Rect is
// the LOW primitive both it and Widget.h share. Keeping this header dependency-
// free breaks the ShellState <-> Widget include cycle.

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
