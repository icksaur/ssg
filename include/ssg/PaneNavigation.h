#pragma once

#include <ssg/types.h>

#include <cstdint>
#include <compare>

namespace ssg {

class PaneId {
public:
    constexpr explicit PaneId(std::uint32_t value = 0) noexcept : value_(value) {}
    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }
    constexpr auto operator<=>(const PaneId&) const = default;

private:
    std::uint32_t value_;
};

enum class SplitAxis : std::uint8_t {
    Horizontal = 0,
    Vertical = 1,
};

enum class PaneDirection : std::uint8_t {
    Left = 0,
    Right = 1,
    Up = 2,
    Down = 3,
};

}  // namespace ssg
