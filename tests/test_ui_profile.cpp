// Seam test for the UI-VM client profile / missing-widget rejection (spec
// §Missing-widget detection). Asserts the rejection seam names the unsupported
// kind and that the profile is a distinct, positive-declaration set.

#include "ssg/RegionRoot.h"
#include "ssg/UiProfile.h"
#include "ssg/Widget.h"
#include "test_helpers.h"

#include <array>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

using ssg::ClientUiProfile;
using ssg::kAllRegionRoles;
using ssg::kAllWidgetKinds;
using ssg::RegionRole;
using ssg::regionRoleName;
using ssg::WidgetKind;
using ssg::widgetKindName;

// The vocabulary enumerates exactly its count, and every kind has a distinct
// name (the enumeration a profile is checked against).
TEST(everyWidgetKindHasADistinctName) {
    ASSERT_EQ(kAllWidgetKinds.size(), ssg::kWidgetKindCount);
    for (std::size_t i = 0; i < kAllWidgetKinds.size(); ++i) {
        for (std::size_t j = i + 1; j < kAllWidgetKinds.size(); ++j) {
            ASSERT_NE(widgetKindName(kAllWidgetKinds[i]),
                      widgetKindName(kAllWidgetKinds[j]));
        }
    }
}

// A default profile renders NOTHING: support must be positively declared, so a
// missing declaration is a loud gap, never a silent pass.
TEST(defaultProfileSupportsNoWidgetKind) {
    const ClientUiProfile empty;
    for (const WidgetKind kind : kAllWidgetKinds) {
        ASSERT_TRUE(!empty.supports(kind));
    }
}

// The full profile renders the whole current vocabulary -- widgets AND regions.
TEST(fullProfileSupportsEveryWidgetKind) {
    const ClientUiProfile full = ClientUiProfile::full();
    for (const WidgetKind kind : kAllWidgetKinds) {
        ASSERT_TRUE(full.supports(kind));
    }
    for (const RegionRole role : kAllRegionRoles) {
        ASSERT_TRUE(full.supports(role));
    }
}

// A default profile also supports no region role: region placement, like widget
// rendering, must be positively declared.
TEST(defaultProfileSupportsNoRegionRole) {
    const ClientUiProfile empty;
    for (const RegionRole role : kAllRegionRoles) {
        ASSERT_TRUE(!empty.supports(role));
    }
}

// A composition using only supported kinds is accepted (nullopt), including the
// empty composition.
TEST(profileAcceptsSupportedAndEmptyCompositions) {
    const ClientUiProfile profile{WidgetKind::Container, WidgetKind::Label};
    const std::vector<WidgetKind> empty;
    ASSERT_TRUE(!profile.firstUnsupported(empty).has_value());
    ASSERT_TRUE(!profile
                     .firstUnsupported(std::vector{WidgetKind::Label,
                                                   WidgetKind::Container})
                     .has_value());
}

// CONTRACT witness: a composition using a kind outside the profile is rejected,
// and the rejection NAMES the first unsupported kind (never silently drops it).
TEST(profileRejectsAndNamesTheFirstUnsupportedKind) {
    const ClientUiProfile profile{WidgetKind::Container, WidgetKind::Label};
    const std::vector kinds{WidgetKind::Container, WidgetKind::Checkbox,
                            WidgetKind::TextInput};
    const std::optional<WidgetKind> rejected = profile.firstUnsupported(kinds);
    ASSERT_TRUE(rejected.has_value());
    ASSERT_EQ(*rejected, WidgetKind::Checkbox);
    ASSERT_EQ(widgetKindName(*rejected), std::string_view{"checkbox"});
}

// An out-of-range enumerator can never be constructed into a profile: it throws
// rather than corrupting the support set.
TEST(profileNeverAdmitsAnInvalidWidgetKind) {
    const auto corrupt = static_cast<WidgetKind>(200);
    bool threw = false;
    try {
        const ClientUiProfile profile{corrupt};
        (void)profile;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

// The rejection oracle covers region roles too: a composition targeting a region
// the profile does not support is rejected and NAMES the first unsupported role.
TEST(profileRejectsAndNamesTheFirstUnsupportedRegion) {
    ClientUiProfile profile;
    profile.allowRegions({RegionRole::Top, RegionRole::Bottom});
    const std::vector roles{RegionRole::Top, RegionRole::Overlay,
                            RegionRole::Leading};
    const std::optional<RegionRole> rejected = profile.firstUnsupported(roles);
    ASSERT_TRUE(rejected.has_value());
    ASSERT_EQ(*rejected, RegionRole::Overlay);
    ASSERT_EQ(regionRoleName(*rejected), std::string_view{"overlay"});
}

// A profile supporting the required regions accepts them (nullopt).
TEST(profileAcceptsSupportedRegions) {
    ClientUiProfile profile;
    profile.allowRegions({RegionRole::Top, RegionRole::Bottom});
    ASSERT_TRUE(!profile
                     .firstUnsupported(std::vector{RegionRole::Bottom,
                                                   RegionRole::Top})
                     .has_value());
}

// The profile is a positive set built from the vocabulary alone, independent of
// the authorization capability set: value-comparable, carrying no principal or
// permission.
TEST(profileIsAValueBuiltFromTheVocabularyAlone) {
    const ClientUiProfile a{WidgetKind::Field};
    const ClientUiProfile b{WidgetKind::Field};
    const ClientUiProfile c{WidgetKind::Spacer};
    ASSERT_EQ(a, b);
    ASSERT_NE(a, c);
}

}  // namespace

int main() {
    RUN(everyWidgetKindHasADistinctName);
    RUN(defaultProfileSupportsNoWidgetKind);
    RUN(fullProfileSupportsEveryWidgetKind);
    RUN(defaultProfileSupportsNoRegionRole);
    RUN(profileAcceptsSupportedAndEmptyCompositions);
    RUN(profileRejectsAndNamesTheFirstUnsupportedKind);
    RUN(profileNeverAdmitsAnInvalidWidgetKind);
    RUN(profileRejectsAndNamesTheFirstUnsupportedRegion);
    RUN(profileAcceptsSupportedRegions);
    RUN(profileIsAValueBuiltFromTheVocabularyAlone);
    return failed == 0 ? 0 : 1;
}
