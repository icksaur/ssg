#pragma once

#include <cstdint>

namespace ssg {

struct ScrollbarMetrics {
    uint32_t totalRows;
    uint32_t viewportRows;
    uint32_t firstRow;
    uint32_t maximumFirstRow;
    uint32_t thumbStart;
    uint32_t thumbSize;

    bool operator==(const ScrollbarMetrics&) const noexcept = default;
};

// Both directions use the same round-half-up scaling so every gutter row
// round-trips through scrollFirstRow and scrollThumbStart.
[[nodiscard]] uint32_t scrollScaleRounded(uint32_t value, uint32_t numerator,
                                          uint32_t denominator) noexcept;
[[nodiscard]] uint32_t scrollThumbStart(uint32_t firstRow,
                                        uint32_t maximumFirstRow,
                                        uint32_t travel) noexcept;
[[nodiscard]] uint32_t scrollFirstRow(uint32_t thumbTop, uint32_t travel,
                                      uint32_t maximumFirstRow) noexcept;

}  // namespace ssg
