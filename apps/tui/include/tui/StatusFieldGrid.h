#pragma once

#include <ssg/Style.h>

#include <string>
#include <string_view>

namespace ssg {

[[nodiscard]] std::string statusFieldGridDisplay(
    std::string_view providerId,
    std::string_view semanticValue,
    const Style& style);

}  // namespace ssg
