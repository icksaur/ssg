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

std::filesystem::path uniqueRoot() {
    auto root = std::filesystem::current_path() / "runtime_presentation";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream out{root / "workspace" / "long.txt"};
    for (int line = 0; line < 80; ++line) out << "line " << line << "\n";
    return root;
}

TEST(viewportShellSettingsAndThemeAreLiveSections) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
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

TEST(settingsDispatchMatchesSettingsModelOracleSnapshot) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    ssg::SettingsModel oracle;
    auto set_theme = ssg::SettingSetArguments{
        ssg::SettingScope::Workspace, ssg::SettingKey::Theme,
        ssg::SettingValue{std::string{"dark"}}};
    ASSERT_TRUE(oracle.set(set_theme.scope, set_theme.key, set_theme.value).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.set", runtime.revision(), set_theme}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    auto expected = oracle.viewState();
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().settings, expected);

    auto set_wrap = ssg::SettingSetArguments{
        ssg::SettingScope::User, ssg::SettingKey::WordWrap,
        ssg::SettingValue{true}};
    ASSERT_TRUE(oracle.set(set_wrap.scope, set_wrap.key, set_wrap.value).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.set", runtime.revision(), set_wrap}).accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    expected = oracle.viewState();
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().settings, expected);

    auto reset_theme = ssg::SettingResetArguments{
        ssg::SettingScope::Workspace, ssg::SettingKey::Theme};
    ASSERT_TRUE(oracle.reset(reset_theme.scope, reset_theme.key).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.reset", runtime.revision(), reset_theme}).accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    expected = oracle.viewState();
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().settings, expected);

    auto set_keymap = ssg::SettingSetArguments{
        ssg::SettingScope::Workspace, ssg::SettingKey::Keymap,
        ssg::SettingValue{std::string{"vim"}}};
    ASSERT_TRUE(oracle.set(set_keymap.scope, set_keymap.key, set_keymap.value).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.set", runtime.revision(), set_keymap}).accepted());
    ASSERT_TRUE(oracle.reset(ssg::SettingScope::Workspace, ssg::SettingKey::Keymap).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.reset_scope", runtime.revision(), ssg::SettingResetScopeArguments{ssg::SettingScope::Workspace}}).accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    expected = oracle.viewState();
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().settings, expected);
}

TEST(editorScrollUsesTheRealPaneHeightNotAHardcoded24) {
    auto root = std::filesystem::current_path() / "runtime_presentation_scroll";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::string text;
    for (int i = 0; i < 100; ++i) text += "a\n";
    std::ofstream{root / "workspace" / "tall.txt", std::ios::binary} << text;
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"tall.txt"}}).accepted());

    // A 40-row terminal (NOT 24): a page is the real pane content height.
    const ssg::ViewportDimensions dims{80, 40};
    auto snap0 = runtime.snapshot(ssg::ClientId{1}, dims);  // populate the cache
    ASSERT_TRUE(snap0.has_value());
    if (!snap0) return;
    auto const pane_rows = static_cast<std::uint32_t>(snap0->sections().shell.panes.front().content.height);
    ASSERT_TRUE(pane_rows != 24);  // the whole point: not the hardcoded value

    // PageDown advances by the real pane height, not 24.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"view.scroll_pages", runtime.revision(), ssg::ScrollPagesArguments{1}}).accepted());
    auto after_page = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(after_page.has_value());
    if (!after_page) return;
    ASSERT_EQ(after_page->client().viewport.first_visual_row, pane_rows);

    // Scroll-to-fraction(1/1) reaches the REAL maximum for this terminal (the last
    // line becomes visible), not the 24-row-derived maximum.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"view.scroll_to_fraction", runtime.revision(), ssg::ScrollFractionArguments{1, 1}}).accepted());
    auto after_bottom = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(after_bottom.has_value());
    if (!after_bottom) return;
    ASSERT_EQ(after_bottom->client().viewport.first_visual_row,
              after_bottom->client().viewport.scrollbar.maximum_first_row);
    std::filesystem::remove_all(root);
}

} // namespace


TEST(reportedLeaderSequenceRendersAPerSnapshotHint) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

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

TEST(paletteCandidatesMatchTheCommandRegistry) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    auto const& palette = snapshot->sections().palette;
    ASSERT_TRUE(palette.mode == ssg::SearchMode::Command);
    // Every registered P0 command appears exactly once as a candidate.
    auto const descriptors = ssg::p0CommandDescriptors();
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
    RUN(viewportShellSettingsAndThemeAreLiveSections);
    RUN(settingsDispatchMatchesSettingsModelOracleSnapshot);
    RUN(reportedLeaderSequenceRendersAPerSnapshotHint);
    RUN(paletteCandidatesMatchTheCommandRegistry);
    RUN(editorScrollUsesTheRealPaneHeightNotAHardcoded24);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
