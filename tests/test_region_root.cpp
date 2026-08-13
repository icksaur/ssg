// Discipline test for the closed region-root role vocabulary (spec §Generic
// layout tree / region roots). Mirrors the SemanticRole discipline check: the
// enumeration, the count, and the name table stay in lockstep.

#include "ssg/RegionRoot.h"
#include "test_helpers.h"

#include <string_view>

namespace {

using ssg::kAllRegionRoles;
using ssg::kRegionRoleCount;
using ssg::RegionRole;
using ssg::regionRoleName;

// The mirrored array enumerates exactly the declared count.
TEST(regionRoleArrayMatchesCount) {
    ASSERT_EQ(kAllRegionRoles.size(), kRegionRoleCount);
}

// Every region role has a distinct, non-empty name -- the enumeration a client
// profile is checked against.
TEST(everyRegionRoleHasADistinctName) {
    for (std::size_t i = 0; i < kAllRegionRoles.size(); ++i) {
        ASSERT_TRUE(!regionRoleName(kAllRegionRoles[i]).empty());
        for (std::size_t j = i + 1; j < kAllRegionRoles.size(); ++j) {
            ASSERT_NE(regionRoleName(kAllRegionRoles[i]),
                      regionRoleName(kAllRegionRoles[j]));
        }
    }
}

// The set covers the placements the spec names: top/bottom edges, an overlay
// layer, and leading/trailing edges for a native sidebar.
TEST(regionRolesCoverTheNamedPlacements) {
    ASSERT_EQ(regionRoleName(RegionRole::Top), std::string_view{"top"});
    ASSERT_EQ(regionRoleName(RegionRole::Bottom), std::string_view{"bottom"});
    ASSERT_EQ(regionRoleName(RegionRole::Leading), std::string_view{"leading"});
    ASSERT_EQ(regionRoleName(RegionRole::Trailing),
              std::string_view{"trailing"});
    ASSERT_EQ(regionRoleName(RegionRole::Overlay), std::string_view{"overlay"});
}

}  // namespace

int main() {
    RUN(regionRoleArrayMatchesCount);
    RUN(everyRegionRoleHasADistinctName);
    RUN(regionRolesCoverTheNamedPlacements);
    return failed == 0 ? 0 : 1;
}
