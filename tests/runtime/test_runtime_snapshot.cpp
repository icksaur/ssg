#include "command_cases.h"
#include "../test_helpers.h"

#include <ssg/EditorRuntime.h>
#include <ssg/EditorSessionBuilder.h>
#include <ssg/Keymap.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#ifndef SSG_SOURCE_SCAN_ROOT
#error "SSG_SOURCE_SCAN_ROOT must name the source tree"
#endif

namespace {

std::filesystem::path uniqueRoot(std::string_view name) {
    auto root = std::filesystem::current_path() / ("runtime_snapshot_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    return root;
}

ssg::EditorRuntimeConfig configFor(const std::filesystem::path& root) {
    return {root / "workspace", root / "scratch", root / "recovery"};
}

TEST(constructionRejectsInvalidCwd) {
    auto root = uniqueRoot("invalid_cwd");
    auto result = ssg::EditorRuntime::create(configFor(root / "missing"));
    ASSERT_FALSE(result.accepted());
    ASSERT_FALSE(result.message.empty());
}

TEST(commandCaseTableExactlyMatchesP0Catalog) {
    auto descriptors = ssg::p0CommandDescriptors();
    std::set<std::string> actual;
    for (const auto& descriptor : descriptors) actual.insert(descriptor.id);

    std::set<std::string> expected;
    for (const auto& command : ssg::test::runtime_command_cases) {
        ASSERT_TRUE(expected.insert(std::string{command.id}).second);
        ASSERT_FALSE(command.owner.empty());
    }

    ASSERT_EQ(actual, expected);
}

TEST(runtimeConstructsAttachesAndProducesLiveSnapshot) {
    auto root = uniqueRoot("snapshot");
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;

    auto& runtime = *created.runtime;
    ssg::InvocationPrincipal principal{ssg::ClientId{7}, ssg::InvocationOrigin::InProcess};
    ASSERT_TRUE(runtime.attach(std::move(principal), ssg::ViewId{9}).accepted());

    auto snapshot = runtime.snapshot(ssg::ClientId{7}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->revision(), runtime.revision());
    ASSERT_EQ(snapshot->client().clientId, ssg::ClientId{7});
    ASSERT_EQ(snapshot->client().viewId, ssg::ViewId{9});
}

TEST(runtimeSourcesDoNotIncludeFixtureModel) {
    auto root = std::filesystem::path{SSG_SOURCE_SCAN_ROOT};
    bool found = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root / "src"}) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".cpp" && entry.path().extension() != ".h") continue;
        const auto relative = std::filesystem::relative(entry.path(), root).generic_string();
        if (relative.rfind("src/runtime/", 0) != 0 && relative != "src/EditorRuntime.cpp") continue;
        std::ifstream input{entry.path()};
        const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        found = found || text.find("FixtureModel") != std::string::npos;
    }
    ASSERT_FALSE(found);
}

TEST(runtimePublishesValidCuratedKeymap) {
    auto root = uniqueRoot("keymap_valid");
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto& keymap = snapshot->sections().keymap;
    ASSERT_FALSE(keymap.bindings.empty());
    ASSERT_TRUE(ssg::KeymapMatcher{keymap}.validate({}).empty());
    ASSERT_TRUE(ssg::KeymapMatcher{keymap}.hasGlobalBinding("settings.open", {}));
}

TEST(curatedKeymapBindingsAreArgumentFree) {
    auto root = uniqueRoot("keymap_argfree");
    std::ofstream{root / "workspace" / "doc.txt"} << "alpha\nbeta\n";
    auto created = ssg::EditorRuntime::create(configFor(root));
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
        commands.insert(binding.commandId);
    }
    for (const auto& command : commands) {
        auto result = runtime.dispatch(ssg::ClientId{1}, {command, runtime.revision(), {}});
        const bool argumentError =
            result.message.find("requires") != std::string::npos ||
            result.message.find("wrong type") != std::string::npos ||
            result.message.find("payload") != std::string::npos;
        if (argumentError) {
            std::cerr << "  argument-required command bound: " << command
                      << " (" << result.message << ")\n";
        }
        ASSERT_FALSE(argumentError);
    }
}

TEST(curatedKeymapResolvesPerContext) {
    auto root = uniqueRoot("keymap_resolve");
    auto created = ssg::EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto& keymap = snapshot->sections().keymap;

    const auto down = *ssg::KeyCodec{}.parseSequence({"ArrowDown"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(down, "editor").commandId,
              std::string{"cursor.line_down"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(down, "panel").commandId,
              std::string{"tree.select_next"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(down, "prompt").commandId,
              std::string{"prompt.next"});

    const auto save = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyS"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(save, "editor").commandId,
              std::string{"file.save"});

    // The settings.open escape hatch resolves in every context.
    const auto settings = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyF", "KeyT"});
    for (const auto context : {"editor", "panel", "prompt"}) {
        ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(settings, context).commandId,
                  std::string{"settings.open"});
    }

    // M7-M selection/multi-cursor bindings: Shift+Arrow extends the selection in
    // the editor; the multi-cursor and find/replace chords resolve globally.
    const auto shiftRight = *ssg::KeyCodec{}.parseSequence({"Shift+ArrowRight"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(shiftRight, "editor").commandId,
              std::string{"select.right"});
    const auto shiftUp = *ssg::KeyCodec{}.parseSequence({"Shift+ArrowUp"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(shiftUp, "editor").commandId,
              std::string{"select.line_up"});
    // Plain ArrowRight is still cursor motion, distinct from the shifted stroke.
    const auto plainRight = *ssg::KeyCodec{}.parseSequence({"ArrowRight"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(plainRight, "editor").commandId,
              std::string{"cursor.right"});
    const auto addNext = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyD"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(addNext, "editor").commandId,
              std::string{"select.add_next_occurrence"});
    const auto findOpen = *ssg::KeyCodec{}.parseSequence({"Escape", "Slash"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(findOpen, "editor").commandId,
              std::string{"find.open"});
    const auto replaceOpen = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyR"});
    ASSERT_EQ(ssg::KeymapMatcher{keymap}.resolveSequence(replaceOpen, "editor").commandId,
              std::string{"replace.open"});
}

TEST(addCursorChordProducesMultipleSelections) {
    auto root = uniqueRoot("multi_cursor");
    std::ofstream{root / "workspace" / "m.txt"} << "alpha\nbeta\n";
    auto created = ssg::EditorRuntime::create(configFor(root));
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
    const auto chord = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyJ"});
    auto resolved = ssg::KeymapMatcher{snapshot->sections().keymap}.resolveSequence(chord, "editor");
    ASSERT_EQ(resolved.kind, ssg::KeymapMatchKind::Resolved);
    ASSERT_EQ(resolved.commandId, std::string{"select.add_cursor_down"});
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {resolved.commandId, runtime.revision(), {}}).accepted());

    auto after = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(after.has_value());
    if (!after) return;
    ASSERT_TRUE(after->sections().selection.selections.items().size() > std::size_t{1});
}

TEST(settingsOpenFocusesASettingsPrompt) {
    auto root = uniqueRoot("settings_open");
    auto created = ssg::EditorRuntime::create(configFor(root));
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
    ASSERT_TRUE(snapshot->sections().promptStatus.prompt.has_value());
}

} // namespace

int main() {
    RUN(constructionRejectsInvalidCwd);
    RUN(commandCaseTableExactlyMatchesP0Catalog);
    RUN(runtimeConstructsAttachesAndProducesLiveSnapshot);
    RUN(runtimeSourcesDoNotIncludeFixtureModel);
    RUN(runtimePublishesValidCuratedKeymap);
    RUN(curatedKeymapBindingsAreArgumentFree);
    RUN(curatedKeymapResolvesPerContext);
    RUN(addCursorChordProducesMultipleSelections);
    RUN(settingsOpenFocusesASettingsPrompt);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
