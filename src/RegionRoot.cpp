#include <ssg/RegionRoot.h>

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace ssg {

namespace {
constexpr std::array kRegionRoleNames{
    std::string_view{"top"},      std::string_view{"bottom"},
    std::string_view{"leading"},  std::string_view{"trailing"},
    std::string_view{"overlay"},
};
static_assert(kRegionRoleNames.size() == kRegionRoleCount);
}  // namespace

std::string_view regionRoleName(RegionRole role) {
    const auto index = static_cast<std::size_t>(role);
    if (index >= kRegionRoleCount) {
        throw std::invalid_argument("regionRoleName: unrecognized RegionRole");
    }
    return kRegionRoleNames[index];
}

}  // namespace ssg
