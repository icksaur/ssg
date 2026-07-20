#include "ssg/settings.h"
#include "test_helpers.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::string_view label) {
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("ssg-settings-" + std::string{label} + "-" + std::to_string(seed));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

void restart_round_trip(const ssg::SettingsPaths& paths) {
    ssg::SettingsModel original;
    ASSERT_TRUE(original
                    .set(ssg::SettingScope::User, ssg::SettingKey::Theme,
                         ssg::SettingValue{std::string{"light"}})
                    .accepted());
    ASSERT_TRUE(original
                    .set(ssg::SettingScope::Workspace, ssg::SettingKey::Theme,
                         ssg::SettingValue{std::string{"dark"}})
                    .accepted());
    ASSERT_TRUE(original
                    .set(ssg::SettingScope::User, ssg::SettingKey::IndentWidth,
                         ssg::SettingValue{std::uint32_t{2}})
                    .accepted());

    ssg::SettingsPersistence persistence{paths};
    ASSERT_TRUE(persistence.save(original).ok);

    ssg::SettingsModel restarted;
    ASSERT_TRUE(persistence.load(restarted).ok);
    const auto theme = restarted.resolve(ssg::SettingKey::Theme);
    ASSERT_EQ(std::get<std::string>(theme.value), "dark");
    ASSERT_EQ(theme.source, ssg::SettingScope::Workspace);
    const auto width = restarted.resolve(ssg::SettingKey::IndentWidth);
    ASSERT_EQ(std::get<std::uint32_t>(width.value), 2u);
}

TEST(linux_and_windows_seams_restart_with_the_same_semantics) {
    TemporaryDirectory root{"restart"};
    restart_round_trip(ssg::linux_settings_paths(
        root.path() / "linux-user", root.path() / "linux-workspaces", "/work/Project"));
    restart_round_trip(ssg::windows_settings_paths(
        root.path() / "windows-user", root.path() / "windows-workspaces",
        R"(C:\Work\Project)"));
}

TEST(workspace_keys_follow_platform_identity_rules) {
    TemporaryDirectory root{"identity"};
    const auto linux_upper = ssg::linux_settings_paths(
        root.path() / "u", root.path() / "w", "/work/Project");
    const auto linux_lower = ssg::linux_settings_paths(
        root.path() / "u", root.path() / "w", "/work/project");
    ASSERT_NE(linux_upper.workspace_file, linux_lower.workspace_file);

    const auto windows_upper = ssg::windows_settings_paths(
        root.path() / "u", root.path() / "w", R"(C:\Work\Project)");
    const auto windows_lower = ssg::windows_settings_paths(
        root.path() / "u", root.path() / "w", "c:/work/project");
    ASSERT_EQ(windows_upper.workspace_file, windows_lower.workspace_file);
}

TEST(unknown_future_field_survives_without_becoming_a_setting) {
    TemporaryDirectory root{"unknown"};
    const auto paths =
        ssg::linux_settings_paths(root.path() / "u", root.path() / "w", "/workspace");
    ssg::SettingsPersistence persistence{paths};
    ssg::SettingsModel initial;
    ASSERT_TRUE(persistence.save(initial).ok);

    auto contents = read_text(paths.user_file);
    contents += "future.setting=s:opaque%20value\n";
    write_text(paths.user_file, contents);

    ssg::SettingsModel restarted;
    ASSERT_TRUE(persistence.load(restarted).ok);
    ASSERT_EQ(restarted.resolve(ssg::SettingKey::Theme).value,
              initial.resolve(ssg::SettingKey::Theme).value);
    ASSERT_TRUE(persistence.save(restarted).ok);
    ASSERT_TRUE(read_text(paths.user_file).find(
                    "future.setting=s:opaque%20value\n") != std::string::npos);
}

TEST(invalid_schema_or_known_value_is_load_atomic) {
    TemporaryDirectory root{"invalid"};
    const auto paths =
        ssg::linux_settings_paths(root.path() / "u", root.path() / "w", "/workspace");
    ssg::SettingsModel settings;
    ASSERT_TRUE(settings
                    .set(ssg::SettingScope::User, ssg::SettingKey::Theme,
                         ssg::SettingValue{std::string{"retained"}})
                    .accepted());
    const auto before = settings.view_state();

    write_text(paths.user_file, "schema=1\nindent_width=u32:99\n");
    const ssg::SettingsPersistence persistence{paths};
    const auto invalid_value = persistence.load(settings);
    ASSERT_FALSE(invalid_value.ok);
    ASSERT_EQ(settings.view_state(), before);

    write_text(paths.user_file, "schema=2\ntheme=s:future\n");
    const auto invalid_schema = persistence.load(settings);
    ASSERT_FALSE(invalid_schema.ok);
    ASSERT_EQ(settings.view_state(), before);
}

TEST(language_and_document_records_round_trip_for_recovery_owner) {
    ssg::SettingsModel original;
    ASSERT_TRUE(original
                    .set(ssg::SettingScope::Language, ssg::SettingKey::AutoIndent,
                         ssg::SettingValue{false})
                    .accepted());
    ASSERT_TRUE(original
                    .set(ssg::SettingScope::Document, ssg::SettingKey::WordWrap,
                         ssg::SettingValue{true})
                    .accepted());

    ssg::SettingsModel restored;
    ASSERT_TRUE(restored
                    .import_scope(ssg::SettingScope::Language,
                                  original.export_scope(ssg::SettingScope::Language))
                    .ok);
    ASSERT_TRUE(restored
                    .import_scope(ssg::SettingScope::Document,
                                  original.export_scope(ssg::SettingScope::Document))
                    .ok);
    ASSERT_EQ(restored.resolve(ssg::SettingKey::AutoIndent).value,
              ssg::SettingValue{false});
    ASSERT_EQ(restored.resolve(ssg::SettingKey::WordWrap).value,
              ssg::SettingValue{true});
}

} // namespace

int main() {
    RUN(linux_and_windows_seams_restart_with_the_same_semantics);
    RUN(workspace_keys_follow_platform_identity_rules);
    RUN(unknown_future_field_survives_without_becoming_a_setting);
    RUN(invalid_schema_or_known_value_is_load_atomic);
    RUN(language_and_document_records_round_trip_for_recovery_owner);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
