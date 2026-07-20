#include "ssg/Settings.h"
#include "test_helpers.h"

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

TEST(setAndResetCompensationsRestoreScopedAndEffectiveState) {
    ssg::SettingsModel settings;
    ASSERT_TRUE(settings
                    .set(SettingScope::User, SettingKey::Theme,
                         SettingValue{std::string{"light"}})
                    .accepted());

    const auto setResult =
        settings.set(SettingScope::Workspace, SettingKey::Theme,
                     SettingValue{std::string{"dark"}});
    ASSERT_TRUE(setResult.accepted());
    auto effective = settings.resolve(SettingKey::Theme);
    ASSERT_EQ(std::get<std::string>(effective.value), "dark");
    ASSERT_TRUE(settings.apply(setResult.compensation).accepted());
    effective = settings.resolve(SettingKey::Theme);
    ASSERT_EQ(std::get<std::string>(effective.value), "light");
    ASSERT_FALSE(settings.scopedValue(SettingScope::Workspace, SettingKey::Theme).has_value());

    ASSERT_TRUE(settings
                    .set(SettingScope::Workspace, SettingKey::Theme,
                         SettingValue{std::string{"dark"}})
                    .accepted());
    const auto resetResult = settings.reset(SettingScope::Workspace, SettingKey::Theme);
    ASSERT_TRUE(resetResult.accepted());
    effective = settings.resolve(SettingKey::Theme);
    ASSERT_EQ(std::get<std::string>(effective.value), "light");
    ASSERT_TRUE(settings.apply(resetResult.compensation).accepted());
    effective = settings.resolve(SettingKey::Theme);
    ASSERT_EQ(std::get<std::string>(effective.value), "dark");
}

TEST(staleCompensationDoesNotOverwriteANewerChange) {
    ssg::SettingsModel settings;
    const auto first =
        settings.set(SettingScope::User, SettingKey::WordWrap, SettingValue{true});
    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(
        settings.set(SettingScope::User, SettingKey::WordWrap, SettingValue{false}).accepted());

    const auto stale = settings.apply(first.compensation);
    ASSERT_FALSE(stale.accepted());
    ASSERT_EQ(stale.error->code, ssg::SettingErrorCode::StaleCompensation);
    auto effective = settings.resolve(SettingKey::WordWrap);
    ASSERT_EQ(std::get<bool>(effective.value), false);

    const auto aba =
        settings.set(SettingScope::User, SettingKey::WordWrap, SettingValue{true});
    ASSERT_TRUE(aba.accepted());
    ASSERT_TRUE(
        settings.set(SettingScope::User, SettingKey::WordWrap, SettingValue{false}).accepted());
    ASSERT_TRUE(
        settings.set(SettingScope::User, SettingKey::WordWrap, SettingValue{true}).accepted());
    ASSERT_FALSE(settings.apply(aba.compensation).accepted());
    effective = settings.resolve(SettingKey::WordWrap);
    ASSERT_EQ(std::get<bool>(effective.value), true);

    ssg::SettingsModel reloaded;
    const auto beforeReload =
        reloaded.set(SettingScope::User, SettingKey::WordWrap, SettingValue{true});
    ASSERT_TRUE(beforeReload.accepted());
    const auto serialized = reloaded.exportScope(SettingScope::User);
    ASSERT_TRUE(reloaded.importScope(SettingScope::User, serialized).ok);
    ASSERT_TRUE(
        reloaded.set(SettingScope::User, SettingKey::WordWrap, SettingValue{true}).accepted());
    const auto preReloadCompensation = reloaded.apply(beforeReload.compensation);
    ASSERT_FALSE(preReloadCompensation.accepted());
    effective = reloaded.resolve(SettingKey::WordWrap);
    ASSERT_EQ(std::get<bool>(effective.value), true);
}

TEST(viewStateDeltaAndCommandSetCoverAllOwnedSettingsIds) {
    ssg::SettingsModel settings;
    const auto before = settings.viewState();
    const auto changed =
        settings.set(SettingScope::Language, SettingKey::AutoIndent, SettingValue{false});
    ASSERT_TRUE(changed.accepted());
    ASSERT_EQ(changed.delta->key, SettingKey::AutoIndent);
    ASSERT_EQ(changed.delta->before, before.find(SettingKey::AutoIndent)->effective);
    const auto after = settings.viewState();
    ASSERT_EQ(changed.delta->after, after.find(SettingKey::AutoIndent)->effective);

    constexpr ssg::SettingsCommandSet commands;
    constexpr std::array expected{
        std::string_view{"settings.open"},
        std::string_view{"settings.set"},
        std::string_view{"settings.reset"},
        std::string_view{"settings.reset_scope"},
        std::string_view{"settings.export_workspace"},
        std::string_view{"settings.import_workspace"},
    };
    static_assert(commands.descriptors.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQ(commands.descriptors[i].id, expected[i]);
        for (std::size_t j = i + 1; j < expected.size(); ++j) {
            ASSERT_NE(commands.descriptors[i].id, commands.descriptors[j].id);
        }
    }
}

} // namespace

int main() {
    RUN(fiveScopeResolutionUsesMostSpecificPresentValue);
    RUN(invalidValuesAndKeysAreFailureAtomic);
    RUN(setAndResetCompensationsRestoreScopedAndEffectiveState);
    RUN(staleCompensationDoesNotOverwriteANewerChange);
    RUN(viewStateDeltaAndCommandSetCoverAllOwnedSettingsIds);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
