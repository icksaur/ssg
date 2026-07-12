#include <ssg/config.h>

namespace ssg {

// TabWidth constructor: validates the value is in [min_value, max_value].
// Throws std::invalid_argument with an actionable message on failure (I4).
TabWidth::TabWidth(int w) {
    if (w < min_value || w > max_value) {
        throw std::invalid_argument(
            "TabWidth: value must be in [" +
            std::to_string(min_value) + ", " +
            std::to_string(max_value) + "], got " +
            std::to_string(w));
    }
    value_ = w;
}

}  // namespace ssg
