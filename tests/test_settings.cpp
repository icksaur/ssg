#include "ssg/settings.h"
#include "test_helpers.h"

#include <array>
#include <string>

namespace {

using ssg::SettingKey;
using ssg::SettingScope;
using ssg::SettingValue;

TEST(five_scope_resolution_uses_most_specific_present_value) {
    ssg::SettingsModel settings;
    struct Case {
        SettingScope scope;
        std::uint32_t width;
    };
    constexpr std::array cases{
        Case{SettingScope::user, 2},
        Case{SettingScope::workspace, 3},
        Case{SettingScope::language, 6},
        Case{SettingScope::document, 8},
    };

    auto effective = settings.resolve(SettingKey::indent_width);
    ASSERT_EQ(std::get<std::uint32_t>(effective.value), 4u);
    ASSERT_EQ(effective.source, SettingScope::defaults);
    for (const auto& entry : cases) {
        const auto result =
            settings.set(entry.scope, SettingKey::indent_width, SettingValue{entry.width});
        ASSERT_TRUE(result.accepted());
        effective = settings.resolve(SettingKey::indent_width);
        ASSERT_EQ(std::get<std::uint32_t>(effective.value), entry.width);
        ASSERT_EQ(effective.source, entry.scope);
    }

    for (auto it = cases.rbegin(); it != cases.rend(); ++it) {
        ASSERT_TRUE(settings.reset(it->scope, SettingKey::indent_width).accepted());
        const auto expected_scope =
            it + 1 == cases.rend() ? SettingScope::defaults : (it + 1)->scope;
        const auto expected_width = it + 1 == cases.rend() ? 4u : (it + 1)->width;
        effective = settings.resolve(SettingKey::indent_width);
        ASSERT_EQ(std::get<std::uint32_t>(effective.value), expected_width);
        ASSERT_EQ(effective.source, expected_scope);
    }
}

TEST(invalid_values_and_keys_are_failure_atomic) {
    ssg::SettingsModel settings;
    const auto before = settings.view_state();

    const auto wrong_type =
        settings.set(SettingScope::user, SettingKey::indent_width, SettingValue{true});
    ASSERT_FALSE(wrong_type.accepted());
    ASSERT_EQ(wrong_type.error->code, ssg::SettingErrorCode::wrong_value_type);
    ASSERT_EQ(settings.view_state(), before);

    const auto unknown = settings.set(
        SettingScope::user, static_cast<SettingKey>(255), SettingValue{true});
    ASSERT_FALSE(unknown.accepted());
    ASSERT_EQ(unknown.error->code, ssg::SettingErrorCode::unknown_key);
    ASSERT_EQ(settings.view_state(), before);

    const auto out_of_range = settings.set(
        SettingScope::user, SettingKey::indent_width, SettingValue{std::uint32_t{17}});
    ASSERT_FALSE(out_of_range.accepted());
    ASSERT_EQ(out_of_range.error->code, ssg::SettingErrorCode::out_of_range);
    ASSERT_EQ(settings.view_state(), before);

    const auto empty_identity =
        settings.set(SettingScope::workspace, SettingKey::theme, SettingValue{std::string{}});
    ASSERT_FALSE(empty_identity.accepted());
    ASSERT_EQ(settings.view_state(), before);

    const auto mixed = settings.set(
        SettingScope::document, SettingKey::line_ending,
        SettingValue{ssg::LineEnding::mixed});
    ASSERT_FALSE(mixed.accepted());
    ASSERT_EQ(settings.view_state(), before);

    const auto unknown_ending = settings.set(
        SettingScope::document, SettingKey::line_ending,
        SettingValue{static_cast<ssg::LineEnding>(255)});
    ASSERT_FALSE(unknown_ending.accepted());
    ASSERT_EQ(settings.view_state(), before);

    const auto immutable_default =
        settings.set(SettingScope::defaults, SettingKey::word_wrap, SettingValue{true});
    ASSERT_FALSE(immutable_default.accepted());
    ASSERT_EQ(immutable_default.error->code, ssg::SettingErrorCode::immutable_scope);
    ASSERT_EQ(settings.view_state(), before);
}

TEST(set_and_reset_compensations_restore_scoped_and_effective_state) {
    ssg::SettingsModel settings;
    ASSERT_TRUE(settings
                    .set(SettingScope::user, SettingKey::theme,
                         SettingValue{std::string{"light"}})
                    .accepted());

    const auto set_result =
        settings.set(SettingScope::workspace, SettingKey::theme,
                     SettingValue{std::string{"dark"}});
    ASSERT_TRUE(set_result.accepted());
    auto effective = settings.resolve(SettingKey::theme);
    ASSERT_EQ(std::get<std::string>(effective.value), "dark");
    ASSERT_TRUE(settings.apply(set_result.compensation).accepted());
    effective = settings.resolve(SettingKey::theme);
    ASSERT_EQ(std::get<std::string>(effective.value), "light");
    ASSERT_FALSE(settings.scoped_value(SettingScope::workspace, SettingKey::theme).has_value());

    ASSERT_TRUE(settings
                    .set(SettingScope::workspace, SettingKey::theme,
                         SettingValue{std::string{"dark"}})
                    .accepted());
    const auto reset_result = settings.reset(SettingScope::workspace, SettingKey::theme);
    ASSERT_TRUE(reset_result.accepted());
    effective = settings.resolve(SettingKey::theme);
    ASSERT_EQ(std::get<std::string>(effective.value), "light");
    ASSERT_TRUE(settings.apply(reset_result.compensation).accepted());
    effective = settings.resolve(SettingKey::theme);
    ASSERT_EQ(std::get<std::string>(effective.value), "dark");
}

TEST(stale_compensation_does_not_overwrite_a_newer_change) {
    ssg::SettingsModel settings;
    const auto first =
        settings.set(SettingScope::user, SettingKey::word_wrap, SettingValue{true});
    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(
        settings.set(SettingScope::user, SettingKey::word_wrap, SettingValue{false}).accepted());

    const auto stale = settings.apply(first.compensation);
    ASSERT_FALSE(stale.accepted());
    ASSERT_EQ(stale.error->code, ssg::SettingErrorCode::stale_compensation);
    auto effective = settings.resolve(SettingKey::word_wrap);
    ASSERT_EQ(std::get<bool>(effective.value), false);

    const auto aba =
        settings.set(SettingScope::user, SettingKey::word_wrap, SettingValue{true});
    ASSERT_TRUE(aba.accepted());
    ASSERT_TRUE(
        settings.set(SettingScope::user, SettingKey::word_wrap, SettingValue{false}).accepted());
    ASSERT_TRUE(
        settings.set(SettingScope::user, SettingKey::word_wrap, SettingValue{true}).accepted());
    ASSERT_FALSE(settings.apply(aba.compensation).accepted());
    effective = settings.resolve(SettingKey::word_wrap);
    ASSERT_EQ(std::get<bool>(effective.value), true);

    ssg::SettingsModel reloaded;
    const auto before_reload =
        reloaded.set(SettingScope::user, SettingKey::word_wrap, SettingValue{true});
    ASSERT_TRUE(before_reload.accepted());
    const auto serialized = reloaded.export_scope(SettingScope::user);
    ASSERT_TRUE(reloaded.import_scope(SettingScope::user, serialized).ok);
    ASSERT_TRUE(
        reloaded.set(SettingScope::user, SettingKey::word_wrap, SettingValue{true}).accepted());
    const auto pre_reload_compensation = reloaded.apply(before_reload.compensation);
    ASSERT_FALSE(pre_reload_compensation.accepted());
    effective = reloaded.resolve(SettingKey::word_wrap);
    ASSERT_EQ(std::get<bool>(effective.value), true);
}

TEST(view_state_delta_and_command_set_are_typed_and_complete_for_task_scope) {
    ssg::SettingsModel settings;
    const auto before = settings.view_state();
    const auto changed =
        settings.set(SettingScope::language, SettingKey::auto_indent, SettingValue{false});
    ASSERT_TRUE(changed.accepted());
    ASSERT_EQ(changed.delta->key, SettingKey::auto_indent);
    ASSERT_EQ(changed.delta->before, before.find(SettingKey::auto_indent)->effective);
    const auto after = settings.view_state();
    ASSERT_EQ(changed.delta->after, after.find(SettingKey::auto_indent)->effective);

    constexpr ssg::SettingsCommandSet commands;
    static_assert(commands.descriptors.size() == 2);
    ASSERT_EQ(commands.descriptors[0].id, std::string_view{"settings.set"});
    ASSERT_EQ(commands.descriptors[1].id, std::string_view{"settings.reset"});
}

} // namespace

int main() {
    RUN(five_scope_resolution_uses_most_specific_present_value);
    RUN(invalid_values_and_keys_are_failure_atomic);
    RUN(set_and_reset_compensations_restore_scoped_and_effective_state);
    RUN(stale_compensation_does_not_overwrite_a_newer_change);
    RUN(view_state_delta_and_command_set_are_typed_and_complete_for_task_scope);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
