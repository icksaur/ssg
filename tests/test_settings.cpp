#include <ssg/Settings.h>
#include "test_helpers.h"
#include <algorithm>

#include <array>
#include <string>

namespace {

using ssg::SettingKey;
using ssg::SettingScope;
using ssg::SettingValue;

TEST(fiveScopeResolutionUsesMostSpecificPresentValue) {
    ssg::SettingsModel settings;
    struct Case {
        SettingScope scope;
        std::uint32_t width;
    };
    constexpr std::array cases{
        Case{SettingScope::User, 2},
        Case{SettingScope::Workspace, 3},
        Case{SettingScope::Language, 6},
        Case{SettingScope::Document, 8},
    };

    auto effective = settings.resolve(SettingKey::IndentWidth);
    ASSERT_EQ(std::get<std::uint32_t>(effective.value), 4u);
    ASSERT_EQ(effective.source, SettingScope::Defaults);
    for (const auto& entry : cases) {
        const auto result =
            settings.set(entry.scope, SettingKey::IndentWidth, SettingValue{entry.width});
        ASSERT_TRUE(result.accepted());
        effective = settings.resolve(SettingKey::IndentWidth);
        ASSERT_EQ(std::get<std::uint32_t>(effective.value), entry.width);
        ASSERT_EQ(effective.source, entry.scope);
    }

    for (auto it = cases.rbegin(); it != cases.rend(); ++it) {
        ASSERT_TRUE(settings.reset(it->scope, SettingKey::IndentWidth).accepted());
        const auto expectedScope =
            it + 1 == cases.rend() ? SettingScope::Defaults : (it + 1)->scope;
        const auto expectedWidth = it + 1 == cases.rend() ? 4u : (it + 1)->width;
        effective = settings.resolve(SettingKey::IndentWidth);
        ASSERT_EQ(std::get<std::uint32_t>(effective.value), expectedWidth);
        ASSERT_EQ(effective.source, expectedScope);
    }
}

TEST(invalidValuesAndKeysAreFailureAtomic) {
    ssg::SettingsModel settings;
    const auto before = settings.viewState();

    const auto wrongType =
        settings.set(SettingScope::User, SettingKey::IndentWidth, SettingValue{true});
    ASSERT_FALSE(wrongType.accepted());
    ASSERT_EQ(wrongType.error->code, ssg::SettingErrorCode::WrongValueType);
    ASSERT_EQ(settings.viewState(), before);

    const auto unknown = settings.set(
        SettingScope::User, static_cast<SettingKey>(255), SettingValue{true});
    ASSERT_FALSE(unknown.accepted());
    ASSERT_EQ(unknown.error->code, ssg::SettingErrorCode::UnknownKey);
    ASSERT_EQ(settings.viewState(), before);

    const auto outOfRange = settings.set(
        SettingScope::User, SettingKey::IndentWidth, SettingValue{std::uint32_t{17}});
    ASSERT_FALSE(outOfRange.accepted());
    ASSERT_EQ(outOfRange.error->code, ssg::SettingErrorCode::OutOfRange);
    ASSERT_EQ(settings.viewState(), before);

    const auto emptyIdentity =
        settings.set(SettingScope::Workspace, SettingKey::Theme, SettingValue{std::string{}});
    ASSERT_FALSE(emptyIdentity.accepted());
    ASSERT_EQ(settings.viewState(), before);

    const auto mixed = settings.set(
        SettingScope::Document, SettingKey::LineEnding,
        SettingValue{ssg::LineEnding::Mixed});
    ASSERT_FALSE(mixed.accepted());
    ASSERT_EQ(settings.viewState(), before);

    const auto unknownEnding = settings.set(
        SettingScope::Document, SettingKey::LineEnding,
        SettingValue{static_cast<ssg::LineEnding>(255)});
    ASSERT_FALSE(unknownEnding.accepted());
    ASSERT_EQ(settings.viewState(), before);

    const auto immutableDefault =
        settings.set(SettingScope::Defaults, SettingKey::WordWrap, SettingValue{true});
    ASSERT_FALSE(immutableDefault.accepted());
    ASSERT_EQ(immutableDefault.error->code, ssg::SettingErrorCode::ImmutableScope);
    ASSERT_EQ(settings.viewState(), before);
}

  TEST(viewStateDeltaAndCommandSetCoverAllOwnedSettingsIds) {
    ssg::SettingsModel settings;
    const auto before = settings.viewState();
    const auto changed =
        settings.set(SettingScope::Language, SettingKey::AutoIndent, SettingValue{false});
    ASSERT_TRUE(changed.accepted());
    ASSERT_EQ(changed.delta->key, SettingKey::AutoIndent);
    const auto beforeEntry =
        std::ranges::find(before.entries, SettingKey::AutoIndent,
                          &ssg::SettingViewEntry::key);
    ASSERT_NE(beforeEntry, before.entries.end());
    ASSERT_EQ(changed.delta->before, beforeEntry->effective);
    const auto after = settings.viewState();
    const auto afterEntry =
        std::ranges::find(after.entries, SettingKey::AutoIndent,
                          &ssg::SettingViewEntry::key);
    ASSERT_NE(afterEntry, after.entries.end());
    ASSERT_EQ(changed.delta->after, afterEntry->effective);

}

} // namespace

SSG_TEST_SUITE(test_settings) {
    RUN(fiveScopeResolutionUsesMostSpecificPresentValue);
    RUN(invalidValuesAndKeysAreFailureAtomic);
    RUN(viewStateDeltaAndCommandSetCoverAllOwnedSettingsIds);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
