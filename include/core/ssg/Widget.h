#pragma once

#include <ssg/detail/generated/semantic_wire_manifest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ssg {

// The medium-neutral UI-VM leaf vocabulary. Terminal cell measurement, text
// composition, and row packing belong to WidgetLayout in ssg_grid.
enum class WidgetKind : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_WIDGET_KIND_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};

inline constexpr std::array kAllWidgetKinds{
#define SSG_ENUMERATOR(symbol, ordinal) WidgetKind::symbol,
    SSG_WIDGET_KIND_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_WIDGET_KIND_ENUMERATORS
inline constexpr std::size_t kWidgetKindCount = kAllWidgetKinds.size();

// An opaque client-rendered surface. The library owns its tree placement,
// presence, and authoritative data channel; each client owns presentation.
enum class ViewSurface : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_VIEW_SURFACE_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};

inline constexpr std::array kAllViewSurfaces{
#define SSG_ENUMERATOR(symbol, ordinal) ViewSurface::symbol,
    SSG_VIEW_SURFACE_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_VIEW_SURFACE_ENUMERATORS
inline constexpr std::size_t kViewSurfaceCount = kAllViewSurfaces.size();

enum class Overflow : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_OVERFLOW_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_OVERFLOW_ENUMERATORS

[[nodiscard]] std::string_view widgetKindName(WidgetKind kind);
[[nodiscard]] std::string_view viewSurfaceName(ViewSurface surface);

}  // namespace ssg
