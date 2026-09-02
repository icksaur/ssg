#include <ssg/Widget.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>

namespace ssg {

namespace {
inline constexpr std::array kWidgetKindNames{
    "container", "label", "field", "checkbox", "text_input", "spacer",
    "view", "status_actions",
};
inline constexpr std::array kViewSurfaceNames{
    "tabbar", "findresults", "notice", "external_modification", "document",
    "tree",
};
}  // namespace

std::string_view widgetKindName(WidgetKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    if (index >= kWidgetKindCount) {
        throw std::invalid_argument("widgetKindName: unrecognized WidgetKind");
    }
    return kWidgetKindNames[index];
}

std::string_view viewSurfaceName(ViewSurface surface) {
    const auto found = std::ranges::find(kAllViewSurfaces, surface);
    if (found == kAllViewSurfaces.end()) {
        throw std::invalid_argument("viewSurfaceName: unrecognized ViewSurface");
    }
    return kViewSurfaceNames[static_cast<std::size_t>(
        std::ranges::distance(kAllViewSurfaces.begin(), found))];
}

}  // namespace ssg
