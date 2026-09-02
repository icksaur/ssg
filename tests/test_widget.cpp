#include "ssg/Widget.h"
#include "test_helpers.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

TEST(widgetAndViewSurfaceNamesCoverTheirClosedInventories) {
    for (const auto kind : ssg::kAllWidgetKinds) {
        ASSERT_TRUE(!ssg::widgetKindName(kind).empty());
    }
    for (const auto surface : ssg::kAllViewSurfaces) {
        ASSERT_TRUE(!ssg::viewSurfaceName(surface).empty());
    }
}

TEST(widgetAndViewSurfaceNamesRejectCorruptValues) {
    ASSERT_THROWS(
        ssg::widgetKindName(static_cast<ssg::WidgetKind>(ssg::kWidgetKindCount)),
        std::invalid_argument);
    ASSERT_THROWS(
        ssg::viewSurfaceName(
            static_cast<ssg::ViewSurface>(
                std::numeric_limits<std::uint8_t>::max())),
        std::invalid_argument);
}

}  // namespace

int main() {
    RUN(widgetAndViewSurfaceNamesCoverTheirClosedInventories);
    RUN(widgetAndViewSurfaceNamesRejectCorruptValues);
    return failed == 0 ? 0 : 1;
}
