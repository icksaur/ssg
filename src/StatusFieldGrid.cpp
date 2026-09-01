#include <ssg/StatusFieldGrid.h>

namespace ssg {

std::string statusFieldGridDisplay(std::string_view providerId,
                                   std::string_view semanticValue,
                                   const Style& style) {
    if (providerId == "path") {
        return style.cwdPrefix + std::string{semanticValue};
    }
    return std::string{semanticValue};
}

}  // namespace ssg
