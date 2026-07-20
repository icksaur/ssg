#include "command_cases.h"
#include "../test_helpers.h"

#include <ssg/editor_runtime.h>
#include <ssg/editor_session_assembly.h>
#include <ssg/input.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#ifndef SSG_SOURCE_SCAN_ROOT
#error "SSG_SOURCE_SCAN_ROOT must name the source tree"
#endif

namespace {

std::filesystem::path unique_root(std::string_view name) {
    auto root = std::filesystem::current_path() / ("runtime_snapshot_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    return root;
}

ssg::EditorRuntimeConfig config_for(const std::filesystem::path& root) {
    return {root / "workspace", root / "scratch", root / "recovery"};
}

TEST(construction_rejects_invalid_cwd) {
    auto root = unique_root("invalid_cwd");
    auto result = ssg::EditorRuntime::create(config_for(root / "missing"));
    ASSERT_FALSE(result.accepted());
    ASSERT_FALSE(result.message.empty());
}

TEST(command_case_table_exactly_matches_p0_catalog) {
    auto descriptors = ssg::p0_command_descriptors();
    std::set<std::string> actual;
    for (const auto& descriptor : descriptors) actual.insert(descriptor.id);

    std::set<std::string> expected;
    for (const auto& command : ssg::test::runtime_command_cases) {
        ASSERT_TRUE(expected.insert(std::string{command.id}).second);
        ASSERT_FALSE(command.owner.empty());
    }

    ASSERT_EQ(actual, expected);
}

TEST(runtime_constructs_attaches_and_produces_live_snapshot) {
    auto root = unique_root("snapshot");
    auto created = ssg::EditorRuntime::create(config_for(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;

    auto& runtime = *created.runtime;
    ssg::InvocationPrincipal principal{ssg::ClientId{7}, ssg::InvocationOrigin::InProcess};
    ASSERT_TRUE(runtime.attach(std::move(principal), ssg::ViewId{9}).accepted());

    auto snapshot = runtime.snapshot(ssg::ClientId{7}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->revision(), runtime.revision());
    ASSERT_EQ(snapshot->client().client_id, ssg::ClientId{7});
    ASSERT_EQ(snapshot->client().view_id, ssg::ViewId{9});
}

TEST(runtime_sources_do_not_include_fixture_model) {
    auto root = std::filesystem::path{SSG_SOURCE_SCAN_ROOT};
    bool found = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root / "src"}) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".cpp" && entry.path().extension() != ".h") continue;
        const auto relative = std::filesystem::relative(entry.path(), root).generic_string();
        if (relative.rfind("src/runtime/", 0) != 0 && relative != "src/editor_runtime.cpp") continue;
        std::ifstream input{entry.path()};
        const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        found = found || text.find("FixtureModel") != std::string::npos;
    }
    ASSERT_FALSE(found);
}

TEST(runtime_publishes_valid_curated_keymap) {
    auto root = unique_root("keymap_valid");
    auto created = ssg::EditorRuntime::create(config_for(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto& keymap = snapshot->sections().keymap;
    ASSERT_FALSE(keymap.bindings.empty());
    ASSERT_TRUE(ssg::validate_keymap(keymap, {}).empty());
    ASSERT_TRUE(ssg::has_global_binding(keymap, "settings.open", {}));
}

TEST(curated_keymap_bindings_are_argument_free) {
    auto root = unique_root("keymap_argfree");
    std::ofstream{root / "workspace" / "doc.txt"} << "alpha\nbeta\n";
    auto created = ssg::EditorRuntime::create(config_for(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    (void)runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"doc.txt"}});
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    // Every bound command must dispatch with an empty payload without failing
    // for a missing/mistyped argument.  Benign state failures (e.g. a prompt
    // command with no open prompt) are allowed; an argument-shaped failure is
    // not (doc/spec-keymap.md K5).
    std::set<std::string> commands;
    for (const auto& binding : snapshot->sections().keymap.bindings) {
        commands.insert(binding.command_id);
    }
    for (const auto& command : commands) {
        auto result = runtime.dispatch(ssg::ClientId{1}, {command, runtime.revision(), {}});
        const bool argument_error =
            result.message.find("requires") != std::string::npos ||
            result.message.find("wrong type") != std::string::npos ||
            result.message.find("payload") != std::string::npos;
        if (argument_error) {
            std::cerr << "  argument-required command bound: " << command
                      << " (" << result.message << ")\n";
        }
        ASSERT_FALSE(argument_error);
    }
}

TEST(curated_keymap_resolves_per_context) {
    auto root = unique_root("keymap_resolve");
    auto created = ssg::EditorRuntime::create(config_for(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto& keymap = snapshot->sections().keymap;

    const auto down = *ssg::parse_key_sequence({"ArrowDown"});
    ASSERT_EQ(ssg::resolve_key_sequence(keymap, down, "editor").command_id,
              std::string{"cursor.line_down"});
    ASSERT_EQ(ssg::resolve_key_sequence(keymap, down, "panel").command_id,
              std::string{"tree.select_next"});
    ASSERT_EQ(ssg::resolve_key_sequence(keymap, down, "prompt").command_id,
              std::string{"palette.next"});

    const auto save = *ssg::parse_key_sequence({"Escape", "KeyS"});
    ASSERT_EQ(ssg::resolve_key_sequence(keymap, save, "editor").command_id,
              std::string{"file.save"});

    // The settings.open escape hatch resolves in every context.
    const auto settings = *ssg::parse_key_sequence({"Escape", "KeyF", "KeyT"});
    for (const auto context : {"editor", "panel", "prompt"}) {
        ASSERT_EQ(ssg::resolve_key_sequence(keymap, settings, context).command_id,
                  std::string{"settings.open"});
    }

    // M7-M selection/multi-cursor bindings: Shift+Arrow extends the selection in
    // the editor; the multi-cursor and find/replace chords resolve globally.
    const auto shift_right = *ssg::parse_key_sequence({"Shift+ArrowRight"});
    ASSERT_EQ(ssg::resolve_key_sequence(keymap, shift_right, "editor").command_id,
              std::string{"select.right"});
    const auto shift_up = *ssg::parse_key_sequence({"Shift+ArrowUp"});
    ASSERT_EQ(ssg::resolve_key_sequence(keymap, shift_up, "editor").command_id,
              std::string{"select.line_up"});
    // Plain ArrowRight is still cursor motion, distinct from the shifted stroke.
    const auto plain_right = *ssg::parse_key_sequence({"ArrowRight"});
    ASSERT_EQ(ssg::resolve_key_sequence(keymap, plain_right, "editor").command_id,
              std::string{"cursor.right"});
    const auto add_next = *ssg::parse_key_sequence({"Escape", "KeyD"});
    ASSERT_EQ(ssg::resolve_key_sequence(keymap, add_next, "editor").command_id,
              std::string{"select.add_next_occurrence"});
    const auto find_open = *ssg::parse_key_sequence({"Escape", "Slash"});
    ASSERT_EQ(ssg::resolve_key_sequence(keymap, find_open, "editor").command_id,
              std::string{"find.open"});
    const auto replace_open = *ssg::parse_key_sequence({"Escape", "KeyR"});
    ASSERT_EQ(ssg::resolve_key_sequence(keymap, replace_open, "editor").command_id,
              std::string{"replace.open"});
}

TEST(add_cursor_chord_produces_multiple_selections) {
    auto root = unique_root("multi_cursor");
    std::ofstream{root / "workspace" / "m.txt"} << "alpha\nbeta\n";
    auto created = ssg::EditorRuntime::create(config_for(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"m.txt"}}).accepted());

    // Resolve the add-cursor-down chord from the published keymap, then dispatch
    // the resolved command: the snapshot must show more than one selection.
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto chord = *ssg::parse_key_sequence({"Escape", "KeyJ"});
    auto resolved = ssg::resolve_key_sequence(snapshot->sections().keymap, chord, "editor");
    ASSERT_EQ(resolved.kind, ssg::KeymapMatchKind::Resolved);
    ASSERT_EQ(resolved.command_id, std::string{"select.add_cursor_down"});
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {resolved.command_id, runtime.revision(), {}}).accepted());

    auto after = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(after.has_value());
    if (!after) return;
    ASSERT_TRUE(after->sections().selection.selections.items().size() > std::size_t{1});
}

TEST(settings_open_focuses_a_settings_prompt) {
    auto root = unique_root("settings_open");
    auto created = ssg::EditorRuntime::create(config_for(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"settings.open", runtime.revision(), {}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    // The chord actually opens: focus moves to the prompt with a visible input.
    ASSERT_EQ(snapshot->sections().shell.focus, ssg::FocusTarget::Prompt);
    ASSERT_TRUE(snapshot->sections().prompt_status.prompt.has_value());
}

} // namespace

int main() {
    RUN(construction_rejects_invalid_cwd);
    RUN(command_case_table_exactly_matches_p0_catalog);
    RUN(runtime_constructs_attaches_and_produces_live_snapshot);
    RUN(runtime_sources_do_not_include_fixture_model);
    RUN(runtime_publishes_valid_curated_keymap);
    RUN(curated_keymap_bindings_are_argument_free);
    RUN(curated_keymap_resolves_per_context);
    RUN(add_cursor_chord_produces_multiple_selections);
    RUN(settings_open_focuses_a_settings_prompt);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
