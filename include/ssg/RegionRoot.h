#pragma once

// The closed set of region roots: the well-known places a client attaches a
// library-composed layout tree in its native frame. A region root is a PLACEMENT
// ROLE, not a "header" or "footer" slot -- header and footer are just how a text
// editor conventionally uses the top and bottom roles. The set is closed and
// medium-agnostic for the same reason WidgetKind and SemanticRole are: a client
// must be able to enumerate the places it may be asked to render, and a
// composition that targets a role a client lacks is rejected early.
//
// Mirrors the SemanticRole discipline: a scoped enum, a count, a mirrored array
// bound by static_assert, and a name table (RegionRole.cpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ssg {

enum class RegionRole : std::uint8_t {
    Top,       // a top edge region (a text editor's header lives here)
    Bottom,    // a bottom edge region (a footer/status line lives here)
    Leading,   // a leading-edge region (a native client's sidebar)
    Trailing,  // a trailing-edge region
    Overlay,   // a layer above the content (palette/finder overlays)
};

inline constexpr std::size_t kRegionRoleCount = 5;
inline constexpr std::array kAllRegionRoles{
    RegionRole::Top,      RegionRole::Bottom, RegionRole::Leading,
    RegionRole::Trailing, RegionRole::Overlay,
};
static_assert(kAllRegionRoles.size() == kRegionRoleCount);

// The stable wire/diagnostic name of a region role. Throws std::invalid_argument
// on a corrupt/out-of-range enumerator, never a silent wrong slot.
[[nodiscard]] std::string_view regionRoleName(RegionRole role);

}  // namespace ssg
