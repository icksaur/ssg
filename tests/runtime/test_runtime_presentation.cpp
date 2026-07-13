#include "../test_helpers.h"

#include <ssg/editor_runtime.h>
#include <ssg/editor_session_assembly.h>
#include <ssg/input.h>
#include <ssg/settings.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace {

std::filesystem::path unique_root() {
    auto root = std::filesystem::current_path() / "runtime_presentation";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream out{root / "workspace" / "long.txt"};
    for (int line = 0; line < 80; ++line) out << "line " << line << "\n";
    return root;
}

TEST(viewport_shell_settings_and_theme_are_live_sections) {
    auto root = unique_root();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"long.txt"}}).accepted());

    auto before = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(before->client().viewport.first_visual_row, 0U);
    ASSERT_EQ(before->sections().theme.palette.size(), ssg::theme_palette_size);

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"view.scroll_lines", runtime.revision(), ssg::ScrollLinesArguments{5}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    auto after = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(after.has_value());
    ASSERT_EQ(after->client().viewport.first_visual_row, 5U);
    ASSERT_TRUE(after->sections().shell.panel.has_value());
}

TEST(settings_dispatch_matches_settings_model_oracle_snapshot) {
    auto root = unique_root();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());

    ssg::SettingsModel oracle;
    auto set_theme = ssg::SettingSetArguments{
        ssg::SettingScope::workspace, ssg::SettingKey::theme,
        ssg::SettingValue{std::string{"dark"}}};
    ASSERT_TRUE(oracle.set(set_theme.scope, set_theme.key, set_theme.value).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.set", runtime.revision(), set_theme}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    auto expected = oracle.view_state();
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().settings, expected);

    auto set_wrap = ssg::SettingSetArguments{
        ssg::SettingScope::user, ssg::SettingKey::word_wrap,
        ssg::SettingValue{true}};
    ASSERT_TRUE(oracle.set(set_wrap.scope, set_wrap.key, set_wrap.value).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.set", runtime.revision(), set_wrap}).accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    expected = oracle.view_state();
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().settings, expected);

    auto reset_theme = ssg::SettingResetArguments{
        ssg::SettingScope::workspace, ssg::SettingKey::theme};
    ASSERT_TRUE(oracle.reset(reset_theme.scope, reset_theme.key).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.reset", runtime.revision(), reset_theme}).accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    expected = oracle.view_state();
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().settings, expected);

    auto set_keymap = ssg::SettingSetArguments{
        ssg::SettingScope::workspace, ssg::SettingKey::keymap,
        ssg::SettingValue{std::string{"vim"}}};
    ASSERT_TRUE(oracle.set(set_keymap.scope, set_keymap.key, set_keymap.value).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.set", runtime.revision(), set_keymap}).accepted());
    ASSERT_TRUE(oracle.reset(ssg::SettingScope::workspace, ssg::SettingKey::keymap).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.reset_scope", runtime.revision(), ssg::SettingResetScopeArguments{ssg::SettingScope::workspace}}).accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    expected = oracle.view_state();
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().settings, expected);
}

} // namespace

TEST(reported_leader_sequence_renders_a_per_snapshot_hint) {
    auto root = unique_root();
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());

    const auto leader_content = [](ssg::SessionSnapshot const& snapshot) {
        for (auto const& node : snapshot.sections().shell.accessibility_nodes) {
            if (node.id == "leader") return node.content;
        }
        return std::string{};
    };

    // A snapshot with a reported leader sequence carries the hint.
    auto with_leader = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12},
                                        ssg::KeySequence{ssg::KeyStroke{"Escape"}});
    ASSERT_TRUE(with_leader.has_value());
    if (with_leader) ASSERT_EQ(leader_content(*with_leader), std::string{"leader: Escape"});

    // A snapshot with no reported sequence (a second client, or the same client
    // not in leader mode) carries no hint -- the state is per snapshot call.
    auto without_leader = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(without_leader.has_value());
    if (without_leader) ASSERT_TRUE(leader_content(*without_leader).empty());
}

TEST(palette_candidates_match_the_command_registry) {
    auto root = unique_root();
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    auto const& palette = snapshot->sections().palette;
    ASSERT_TRUE(palette.mode == ssg::SearchMode::command);
    // Every registered P0 command appears exactly once as a candidate.
    auto const descriptors = ssg::p0_command_descriptors();
    ASSERT_EQ(palette.candidates.size(), descriptors.size());
    std::set<std::string> candidate_ids;
    for (auto const& candidate : palette.candidates) {
        ASSERT_FALSE(candidate.label.empty());
        candidate_ids.insert(candidate.id);
    }
    for (auto const& descriptor : descriptors) {
        ASSERT_TRUE(candidate_ids.contains(descriptor.id));
    }
}

int main() {
    RUN(viewport_shell_settings_and_theme_are_live_sections);
    RUN(settings_dispatch_matches_settings_model_oracle_snapshot);
    RUN(reported_leader_sequence_renders_a_per_snapshot_hint);
    RUN(palette_candidates_match_the_command_registry);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
