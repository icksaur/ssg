#pragma once

#include <ssg/detail/generated/semantic_wire_manifest.h>

#include <cstdint>

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
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_SPLIT_AXIS_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_SPLIT_AXIS_ENUMERATORS

enum class PaneDirection : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_PANE_DIRECTION_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_PANE_DIRECTION_ENUMERATORS

}  // namespace ssg
