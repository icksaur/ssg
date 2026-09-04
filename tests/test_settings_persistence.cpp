#include <ssg/Settings.h>
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

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeText(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

void restartRoundTrip(const ssg::SettingsPaths& paths) {
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

TEST(linuxAndWindowsSeamsRestartWithTheSameSemantics) {
    TemporaryDirectory root{"restart"};
    restartRoundTrip(ssg::linuxSettingsPaths(
        root.path() / "linux-user", root.path() / "linux-workspaces", "/work/Project"));
    restartRoundTrip(ssg::windowsSettingsPaths(
        root.path() / "windows-user", root.path() / "windows-workspaces",
        R"(C:\Work\Project)"));
}

TEST(workspaceKeysFollowPlatformIdentityRules) {
    TemporaryDirectory root{"identity"};
    const auto linuxUpper = ssg::linuxSettingsPaths(
        root.path() / "u", root.path() / "w", "/work/Project");
    const auto linuxLower = ssg::linuxSettingsPaths(
        root.path() / "u", root.path() / "w", "/work/project");
    ASSERT_NE(linuxUpper.workspaceFile, linuxLower.workspaceFile);

    const auto windowsUpper = ssg::windowsSettingsPaths(
        root.path() / "u", root.path() / "w", R"(C:\Work\Project)");
    const auto windowsLower = ssg::windowsSettingsPaths(
        root.path() / "u", root.path() / "w", "c:/work/project");
    ASSERT_EQ(windowsUpper.workspaceFile, windowsLower.workspaceFile);
}

TEST(unknownFutureFieldSurvivesWithoutBecomingASetting) {
    TemporaryDirectory root{"unknown"};
    const auto paths =
        ssg::linuxSettingsPaths(root.path() / "u", root.path() / "w", "/workspace");
    ssg::SettingsPersistence persistence{paths};
    ssg::SettingsModel initial;
    ASSERT_TRUE(persistence.save(initial).ok);

    auto contents = readText(paths.userFile);
    contents += "future.setting=s:opaque%20value\n";
    writeText(paths.userFile, contents);

    ssg::SettingsModel restarted;
    ASSERT_TRUE(persistence.load(restarted).ok);
    ASSERT_EQ(restarted.resolve(ssg::SettingKey::Theme).value,
              initial.resolve(ssg::SettingKey::Theme).value);
    ASSERT_TRUE(persistence.save(restarted).ok);
    ASSERT_TRUE(readText(paths.userFile).find(
                    "future.setting=s:opaque%20value\n") != std::string::npos);
}

TEST(invalidSchemaOrKnownValueIsLoadAtomic) {
    TemporaryDirectory root{"invalid"};
    const auto paths =
        ssg::linuxSettingsPaths(root.path() / "u", root.path() / "w", "/workspace");
    ssg::SettingsModel settings;
    ASSERT_TRUE(settings
                    .set(ssg::SettingScope::User, ssg::SettingKey::Theme,
                         ssg::SettingValue{std::string{"retained"}})
                    .accepted());
    const auto before = settings.viewState();

    writeText(paths.userFile, "schema=1\nindent_width=u32:99\n");
    const ssg::SettingsPersistence persistence{paths};
    const auto invalidValue = persistence.load(settings);
    ASSERT_FALSE(invalidValue.ok);
    ASSERT_EQ(settings.viewState(), before);

    writeText(paths.userFile, "schema=2\ntheme=s:future\n");
    const auto invalidSchema = persistence.load(settings);
    ASSERT_FALSE(invalidSchema.ok);
    ASSERT_EQ(settings.viewState(), before);
}

TEST(languageAndDocumentRecordsRoundTripForRecoveryOwner) {
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
                    .importScope(ssg::SettingScope::Language,
                                  original.exportScope(ssg::SettingScope::Language))
                    .ok);
    ASSERT_TRUE(restored
                    .importScope(ssg::SettingScope::Document,
                                  original.exportScope(ssg::SettingScope::Document))
                    .ok);
    ASSERT_EQ(restored.resolve(ssg::SettingKey::AutoIndent).value,
              ssg::SettingValue{false});
    ASSERT_EQ(restored.resolve(ssg::SettingKey::WordWrap).value,
              ssg::SettingValue{true});
}

} // namespace

SSG_TEST_SUITE(test_settings_persistence) {
    RUN(linuxAndWindowsSeamsRestartWithTheSameSemantics);
    RUN(workspaceKeysFollowPlatformIdentityRules);
    RUN(unknownFutureFieldSurvivesWithoutBecomingASetting);
    RUN(invalidSchemaOrKnownValueIsLoadAtomic);
    RUN(languageAndDocumentRecordsRoundTripForRecoveryOwner);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
