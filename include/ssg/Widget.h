#pragma once

#include <array>
#include <cstdint>

namespace ssg {

// The medium-neutral UI-VM leaf vocabulary. Terminal cell measurement, text
// composition, and row packing belong to the TUI's WidgetLayout.
enum class WidgetKind : std::uint8_t {
    Container = 0,
    Label = 1,
    Field = 2,
    Checkbox = 3,
    TextInput = 4,
    Spacer = 5,
    View = 6,
};

// An opaque client-rendered surface. The library owns its tree placement,
// presence, and authoritative data channel; each client owns presentation.
enum class ViewSurface : std::uint8_t {
    TabBar = 0,
    FindResults = 3,
    Notice = 6,
    ExternalModification = 7,
    Document = 8,
    Tree = 9,
};

inline constexpr std::array kAllViewSurfaces{
    ViewSurface::TabBar, ViewSurface::FindResults, ViewSurface::Notice,
    ViewSurface::ExternalModification, ViewSurface::Document, ViewSurface::Tree,
};
enum class Overflow : std::uint8_t {
    None = 0,
    Truncate = 1,
    ScrollTail = 2,
};

}  // namespace ssg
