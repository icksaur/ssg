#include <ssg/config.h>

namespace ssg {

TabWidth::TabWidth(int w) {
    if (w < kMinValue || w > kMaxValue) {
        throw std::invalid_argument(
            "TabWidth: value must be in [" +
            std::to_string(kMinValue) + ", " +
            std::to_string(kMaxValue) + "], got " +
            std::to_string(w));
    }
    value_ = w;
}

}  // namespace ssg
