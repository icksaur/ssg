#include "../test_helpers.h"
#include "../grid_test_frame.h"
#include "../grid_test_view.h"

#include <ssg/Editor.h>
#include <ssg/FindReplace.h>
#include <ssg/Keymap.h>
#include <ssg/Selection.h>
#include <ssg/PromptSurface.h>
#include <ssg/TextInputCommands.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

std::optional<ssg::GridPresentation> projectFrame(
    ssg::Editor& runtime, ssg::ViewportDimensions dimensions) {
    return ssg::test::projectGridFrame(runtime, dimensions);
}

std::filesystem::path uniqueRoot() {
    auto root = std::filesystem::temp_directory_path() /
                ("runtime_editing_" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "edit.txt"} << "abc";
    return root;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

ssg::WorkspaceSnapshot diskSnapshot(
    const std::filesystem::path& root, std::uint64_t revision) {
    ssg::WorkspaceSnapshot snapshot;
    snapshot.revision = revision;
    for (auto const& entry :
         std::filesystem::recursive_directory_iterator{root}) {
        if (!entry.is_regular_file()) continue;
        auto relative = entry.path().lexically_relative(root).generic_string();
        snapshot.files.push_back({relative, readText(entry.path())});
    }
    return snapshot;
}

TEST(runtimeTextSelectionAndHistoryMatchFeatureOperations) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"edit.txt"}}).accepted());

    auto setPosition = runtime.dispatch({"cursor.set_position",  ssg::SelectionCommandArguments{ssg::resolveSelectionPosition("abc", ssg::ByteOffset{3}), std::nullopt}});
    ASSERT_TRUE(setPosition.accepted());
    auto typed = runtime.dispatch({"text.insert",  ssg::TextInputArguments{"d"}});
    ASSERT_TRUE(typed.accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"abcd"});

    auto undo = runtime.dispatch({"edit.undo",  {}});
    ASSERT_TRUE(undo.accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"abc"});
    auto redo = runtime.dispatch({"edit.redo",  {}});
    ASSERT_TRUE(redo.accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"abcd"});
}

TEST(typingUndoBreaksOnWordAndLineBoundaries) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"edit.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"cursor.set_position",  ssg::SelectionCommandArguments{ssg::resolveSelectionPosition("abc", ssg::ByteOffset{3}), std::nullopt}}).accepted());

    const auto type = [&](char character) {
        return runtime.dispatch({"text.insert",  ssg::TextInputArguments{std::string{character}}}).accepted();
    };
    for (char character : std::string{"foo bar"}) ASSERT_TRUE(type(character));
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"abcfoo bar"});

    // The space sealed the "foo " unit, so the first undo removes only "bar".
    ASSERT_TRUE(runtime.dispatch({"edit.undo",  {}}).accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"abcfoo "});
    // The second undo removes the "foo " word unit.
    ASSERT_TRUE(runtime.dispatch({"edit.undo",  {}}).accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"abc"});

    // Newlines seal a unit per line.
    ASSERT_TRUE(type('x'));
    ASSERT_TRUE(runtime.dispatch({"text.newline",  {}}).accepted());
    ASSERT_TRUE(type('y'));
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"abcx\ny"});
    ASSERT_TRUE(runtime.dispatch({"edit.undo",  {}}).accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"abcx\n"});
}

TEST(workspaceReplaceDispatchMatchesFeaturePreviewAndDiskApply) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "other.txt"} << "cat";
    auto created = ssg::createEditor({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"edit.txt"}}).accepted());

    ssg::FindRequest request{"cat", {}, std::nullopt, 100000, nullptr};
    auto oracle = ssg::previewWorkspaceReplace(
        diskSnapshot(root / "workspace", 1), request, "dog");
    ASSERT_TRUE(oracle.accepted());
    ASSERT_EQ(oracle.preview->changes.size(), std::size_t{1});

    auto preview = runtime.dispatch({"replace.workspace_preview",  ssg::WorkspaceReplaceArguments{request, "dog"}});
    ASSERT_TRUE(preview.accepted());
    auto apply = runtime.dispatch({"replace.workspace_apply",  {}});
    ASSERT_TRUE(apply.accepted());
    ASSERT_EQ(readText(root / "workspace" / "other.txt"), std::string{"dog"});
    ASSERT_EQ(readText(root / "workspace" / "edit.txt"), std::string{"abc"});
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"abc"});
}

TEST(workspaceReplaceRejectsStaleAndOutOfBoundsPreview) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace / ".ssg" / "scratch");
    std::filesystem::create_directories(workspace / ".ssg" / "recovery");
    std::ofstream{workspace / "other.txt"} << "cat";
    auto created = ssg::createEditor({
        workspace, workspace / ".ssg" / "scratch",
        workspace / ".ssg" / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    ssg::FindRequest request{"cat", {}, std::nullopt, 100000, nullptr};
    auto oracle = ssg::previewWorkspaceReplace(
        diskSnapshot(workspace, 1), request, "dog");
    ASSERT_TRUE(oracle.accepted());
    ASSERT_TRUE(runtime.dispatch({"replace.workspace_preview",  ssg::WorkspaceReplaceArguments{request, "dog"}}).accepted());

    auto escaped = *oracle.preview;
    escaped.changes.front().path = "../outside.txt";
    auto rejectedPath = runtime.dispatch({"replace.workspace_apply",  escaped});
    ASSERT_FALSE(rejectedPath.accepted());
    ASSERT_FALSE(std::filesystem::exists(root / "outside.txt"));

    std::ofstream{workspace / ".ssg" / "scratch" / "hidden.txt"} << "cat";
    auto runtimeState = *oracle.preview;
    runtimeState.changes.front().path = ".ssg/scratch/hidden.txt";
    runtimeState.changes.front().before = "cat";
    runtimeState.changes.front().after = "dog";
    auto rejectedState = runtime.dispatch({"replace.workspace_apply",  runtimeState});
    ASSERT_FALSE(rejectedState.accepted());
    ASSERT_EQ(readText(workspace / ".ssg" / "scratch" / "hidden.txt"), std::string{"cat"});

    auto rejectedType = runtime.dispatch({"replace.workspace_apply",  std::string{"wrong"}});
    ASSERT_FALSE(rejectedType.accepted());
    ASSERT_EQ(readText(workspace / "other.txt"), std::string{"cat"});

    std::ofstream{workspace / "other.txt", std::ios::binary | std::ios::trunc} << "fresh";
    auto rejectedStale = runtime.dispatch({"replace.workspace_apply",  {}});
    ASSERT_FALSE(rejectedStale.accepted());
    ASSERT_EQ(readText(workspace / "other.txt"), std::string{"fresh"});
}

TEST(workspaceReplaceUpdatesOpenDocumentSnapshotAndDisk) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "edit.txt"} << "cat cat";
    auto created = ssg::createEditor({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"edit.txt"}}).accepted());

    ssg::FindRequest request{"cat", {}, std::nullopt, 100000, nullptr};
    auto oracle = ssg::previewWorkspaceReplace(
        diskSnapshot(root / "workspace", 1), request, "dog");
    ASSERT_TRUE(oracle.accepted());
    ASSERT_TRUE(runtime.dispatch({"replace.workspace_preview",  ssg::WorkspaceReplaceArguments{request, "dog"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"replace.workspace_apply",  {}}).accepted());
    ASSERT_EQ(readText(root / "workspace" / "edit.txt"), std::string{"dog dog"});
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"dog dog"});
    auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->documentText, std::string{"dog dog"});
}

TEST(workspaceSearchAndReplaceExcludeRuntimeStateRoots) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace / ".ssg" / "scratch");
    std::filesystem::create_directories(workspace / ".ssg" / "recovery");
    std::ofstream{workspace / "visible.txt"} << "secret";
    std::ofstream{workspace / ".ssg" / "scratch" / "hidden.txt"} << "secret";
    std::ofstream{workspace / ".ssg" / "recovery" / "journal.txt"} << "secret";

    auto created = ssg::createEditor({
        workspace, workspace / ".ssg" / "scratch",
        workspace / ".ssg" / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    ASSERT_TRUE(runtime.dispatch({"search.workspace",  std::string{"#secret"}}).accepted());
    ssg::FindRequest request{"secret", {}, std::nullopt, 100000, nullptr};
    ASSERT_TRUE(runtime.dispatch({"replace.workspace_preview",  ssg::WorkspaceReplaceArguments{request, "public"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"replace.workspace_apply",  {}}).accepted());
    ASSERT_EQ(readText(workspace / "visible.txt"), std::string{"public"});
    ASSERT_EQ(readText(workspace / ".ssg" / "scratch" / "hidden.txt"), std::string{"secret"});
    ASSERT_EQ(readText(workspace / ".ssg" / "recovery" / "journal.txt"), std::string{"secret"});
}

TEST(findUpdateQueryProjectsMatchesAndPromptAndNextCycles) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "hits.txt"} << "cat cat cat";

    auto created = ssg::createEditor({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"hits.txt"}}).accepted());

    ASSERT_TRUE(runtime.dispatch({"find.open",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"find.update_query",  ssg::FindQueryArguments{"cat"}}).accepted());

    auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& find = snapshot->findReplace;
    ASSERT_TRUE(find.open);
    ASSERT_EQ(find.query, std::string{"cat"});
    ASSERT_EQ(find.matches.size(), std::size_t{3});
    ASSERT_EQ(find.matches[0].begin.value(), std::uint64_t{0});
    ASSERT_EQ(find.matches[0].end.value(), std::uint64_t{3});
    ASSERT_EQ(find.matches[1].begin.value(), std::uint64_t{4});
    ASSERT_EQ(find.matches[1].end.value(), std::uint64_t{7});
    ASSERT_EQ(find.matches[2].begin.value(), std::uint64_t{8});
    ASSERT_EQ(find.matches[2].end.value(), std::uint64_t{11});

    ASSERT_EQ(snapshot->promptStatus.activeKind,
              std::optional{ssg::PromptKind::Find});

    const auto activeAfter = [&](int advances) -> std::size_t {
        for (int i = 0; i < advances; ++i) {
            (void)runtime.dispatch({"find.next",  {}});
        }
        auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 12});
        return snap->findReplace.activeMatch.value_or(999);
    };
    ASSERT_EQ(activeAfter(1), std::size_t{1});
    ASSERT_EQ(activeAfter(1), std::size_t{2});
    ASSERT_EQ(activeAfter(1), std::size_t{0});
}

TEST(findCloseSucceedsWithoutAnActiveDocument) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    auto created = ssg::createEditor({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    // No document is open: find.close (and next/previous) must not be rejected by
    // the active-document guard, so a find opened before the last tab closed can
    // still be dismissed.
    ASSERT_TRUE(runtime.dispatch({"find.close",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"find.next",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"find.previous",  {}}).accepted());
}

TEST(findCloseDoesNotCancelAnUnrelatedPrompt) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "doc.txt"} << "hello";
    auto created = ssg::createEditor({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"doc.txt"}}).accepted());

    ASSERT_TRUE(runtime.dispatch({"palette.open",  {}}).accepted());
    auto before = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(before.has_value());
    if (before) {
        ASSERT_TRUE(before->promptStatus.activeKind.has_value());
        ASSERT_EQ(*before->promptStatus.activeKind,
                  ssg::PromptKind::Palette);
    }

    // A find.close while the palette prompt is active must leave the palette
    // prompt intact (it only owns the find prompt).
    ASSERT_TRUE(runtime.dispatch({"find.close",  {}}).accepted());
    auto after = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(after.has_value());
    if (after) {
        ASSERT_TRUE(after->promptStatus.activeKind.has_value());
        ASSERT_EQ(*after->promptStatus.activeKind,
                  ssg::PromptKind::Palette);
    }
}

TEST(findClosesWhenSwitchingToADifferentDocument) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "a.txt"} << "cat cat cat";
    std::ofstream{workspace / "b.txt"} << "dog dog dog";
    auto created = ssg::createEditor({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"a.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"find.open",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"find.update_query",  ssg::FindQueryArguments{"cat"}}).accepted());
    {
        auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) ASSERT_EQ(snap->findReplace.matches.size(), std::size_t{3});
    }

    // Switching to another freshly opened document (which shares revision 1 with
    // a.txt) must dismiss find: identity, not revision equality, binds the state.
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"b.txt"}}).accepted());
    auto after = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(after.has_value());
    if (after) {
        ASSERT_FALSE(after->findReplace.open);
        ASSERT_FALSE(after->promptStatus.activeKind.has_value());
    }
}

TEST(findScrollsTheViewportToFollowTheActiveMatch) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::string text;
    for (int line = 0; line < 50; ++line) {
        text += (line == 40) ? "target here" : "filler";
        text += '\n';
    }
    std::ofstream{workspace / "tall.txt"} << text;
    auto created = ssg::createEditor({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"tall.txt"}}).accepted());

    // Baseline: the viewport starts at the top.
    {
        auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) ASSERT_EQ(snap->viewport.firstVisualRow, std::uint32_t{0});
    }

    ASSERT_TRUE(runtime.dispatch({"find.open",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"find.update_query",  ssg::FindQueryArguments{"target"}}).accepted());

    // The match on line 40 lies below the initial 24-row viewport, so revealing
    // it must scroll down and the match's logical line must be visible.
    auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snap.has_value());
    if (!snap) return;
    auto const& viewport = snap->viewport;
    ASSERT_TRUE(viewport.firstVisualRow > std::uint32_t{0});
    bool matchLineVisible = false;
    for (auto const& row : viewport.visibleRows) {
        if (row.logicalLine == 40) matchLineVisible = true;
    }
    ASSERT_TRUE(matchLineVisible);
}

TEST(replaceCurrentReplacesActiveMatchAndResetsToFirst) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "r.txt"} << "cat cat cat";
    auto created = ssg::createEditor({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"r.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"replace.open",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"find.update_query",  ssg::FindQueryArguments{"cat"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"replace.update_replacement",  ssg::FindQueryArguments{"dog"}}).accepted());

    {
        auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) {
            ASSERT_EQ(snap->findReplace.replacement, std::string{"dog"});
            ASSERT_EQ(snap->findReplace.matches.size(), std::size_t{3});
            ASSERT_EQ(snap->promptStatus.activeKind,
                      std::optional{ssg::PromptKind::Replace});
        }
    }

    ASSERT_TRUE(runtime.dispatch({"replace.current",  {}}).accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"dog cat cat"});
    auto after = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(after.has_value());
    if (after) {
        auto const& fr = after->findReplace;
        ASSERT_EQ(fr.matches.size(), std::size_t{2});
        // Reset-to-first: active index is 0, now pointing at the match at [4,7).
        ASSERT_TRUE(fr.activeMatch.has_value());
        if (fr.activeMatch) ASSERT_EQ(*fr.activeMatch, std::size_t{0});
        if (fr.matches.size() == 2) {
            ASSERT_EQ(fr.matches[0].begin.value(), std::uint64_t{4});
            ASSERT_EQ(fr.matches[0].end.value(), std::uint64_t{7});
        }
    }
}

TEST(replaceAllReplacesEveryMatch) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "r.txt"} << "cat cat cat";
    auto created = ssg::createEditor({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"r.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"replace.open",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"find.update_query",  ssg::FindQueryArguments{"cat"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"replace.update_replacement",  ssg::FindQueryArguments{"dog"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"replace.all",  {}}).accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"dog dog dog"});
    auto after = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(after.has_value());
    if (after) {
        ASSERT_TRUE(after->findReplace.matches.empty());
        ASSERT_FALSE(after->findReplace.activeMatch.has_value());
    }
}

TEST(replaceCommandsAreBenignNoOpsWithoutAReplacePrompt) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "r.txt"} << "cat cat cat";
    auto created = ssg::createEditor({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"r.txt"}}).accepted());

    // A find prompt (not replace) is open: replace commands must be benign
    // success no-ops that do not mutate the document or controller state.
    ASSERT_TRUE(runtime.dispatch({"find.open",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"find.update_query",  ssg::FindQueryArguments{"cat"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"replace.update_replacement",  ssg::FindQueryArguments{"dog"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"replace.current",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"replace.all",  {}}).accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"cat cat cat"});
    auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snap.has_value());
    if (snap) ASSERT_TRUE(snap->findReplace.replacement.empty());
}

TEST(findToggleCaseFlipsOptionAndChangesMatchesAndGuardsWhenNoPrompt) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "m.txt"} << "Cat cat CAT";
    auto created = ssg::createEditor({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"m.txt"}}).accepted());

    // No find/replace prompt yet: find.toggle_case must be a benign no-op that
    // leaves the (default) options untouched.
    ASSERT_TRUE(runtime.dispatch({"find.toggle_case",  {}}).accepted());
    {
        auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) ASSERT_FALSE(snap->findReplace.options.caseSensitive);
    }

    ASSERT_TRUE(runtime.dispatch({"find.open",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"find.update_query",  ssg::FindQueryArguments{"cat"}}).accepted());
    {
        // Case-insensitive (default): all three "cat"s match.
        auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) ASSERT_EQ(snap->findReplace.matches.size(), std::size_t{3});
    }

    // Toggle case sensitivity: now only the lowercase "cat" matches.
    ASSERT_TRUE(runtime.dispatch({"find.toggle_case",  {}}).accepted());
    auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snap.has_value());
    if (snap) {
        ASSERT_TRUE(snap->findReplace.options.caseSensitive);
        ASSERT_EQ(snap->findReplace.matches.size(), std::size_t{1});
    }
}

TEST(replaceOpenPreservesFindOptions) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "m.txt"} << "Cat cat CAT";
    auto created = ssg::createEditor({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"m.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"find.open",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"find.update_query",  ssg::FindQueryArguments{"cat"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"find.toggle_case",  {}}).accepted());
    // Case-sensitive find matched only the lowercase "cat".
    {
        auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) ASSERT_EQ(snap->findReplace.matches.size(), std::size_t{1});
    }
    // Opening replace must NOT widen the match population: options carry over so
    // replace.all acts on exactly what the user reviewed.
    ASSERT_TRUE(runtime.dispatch({"replace.open",  {}}).accepted());
    auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snap.has_value());
    if (snap) {
        ASSERT_TRUE(snap->findReplace.options.caseSensitive);
        ASSERT_EQ(snap->findReplace.matches.size(), std::size_t{1});
    }
}

TEST(findCloseDismissesTheReplacePrompt) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "m.txt"} << "cat cat";
    auto created = ssg::createEditor({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"m.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"replace.open",  {}}).accepted());
    {
        auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) {
            ASSERT_TRUE(snap->findReplace.open);
            ASSERT_TRUE(snap->promptStatus.activeKind.has_value());
        }
    }
    // A single find.close must close the controller AND dismiss the replace
    // prompt (no stale prompt requiring a second cancel).
    ASSERT_TRUE(runtime.dispatch({"find.close",  {}}).accepted());
    auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snap.has_value());
    if (snap) {
        ASSERT_FALSE(snap->findReplace.open);
        ASSERT_FALSE(snap->promptStatus.activeKind.has_value());
    }
}

TEST(pointerSelectionCommandsFocusTheEditorKeyboardMotionDoesNot) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"edit.txt"}}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    auto focus = [&] {
        auto snap = projectFrame(runtime, dims);
        return snap ? ssg::effectiveUiFocus(snap->uiTree)
                    : ssg::FocusTarget::Editor;
    };
    auto focusPanel = [&] {
        auto snap = projectFrame(runtime, dims);
        bool const shown = snap && snap->panel.has_value();
        if (!shown) {
            ASSERT_TRUE(runtime.dispatch({"panel.toggle",  {}}).accepted());
        }
        ASSERT_TRUE(runtime.dispatch({"panel.focus",  {}}).accepted());
    };

    // A pointer click-to-caret (cursor.set_position) from panel focus moves focus
    // to the editor.
    focusPanel();
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);
    ASSERT_TRUE(runtime.dispatch({"cursor.set_position",  ssg::SelectionCommandArguments{ssg::resolveSelectionPosition("abc", ssg::ByteOffset{1}), std::nullopt}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Editor);

    // A pointer drag (select.set_range) likewise focuses the editor.
    focusPanel();
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);
    ASSERT_TRUE(runtime.dispatch({"select.set_range",
        ssg::SelectionCommandArguments{std::nullopt, ssg::Selection{ssg::resolveSelectionPosition("abc", ssg::ByteOffset{0}).value(), ssg::resolveSelectionPosition("abc", ssg::ByteOffset{2}).value()}}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Editor);

    // A pointer Alt+click add-caret (select.add_range) focuses the editor too.
    focusPanel();
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);
    ASSERT_TRUE(runtime.dispatch({"select.add_range",
        ssg::SelectionCommandArguments{std::nullopt, ssg::Selection{ssg::resolveSelectionPosition("abc", ssg::ByteOffset{1}).value(), ssg::resolveSelectionPosition("abc", ssg::ByteOffset{1}).value()}}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Editor);

    // A pointer Alt+drag add-range (select.set_ranges) focuses the editor too.
    focusPanel();
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);
    ASSERT_TRUE(runtime.dispatch({"select.set_ranges",
        ssg::SelectionCommandArguments{std::nullopt, std::nullopt, {ssg::Selection{ssg::resolveSelectionPosition("abc", ssg::ByteOffset{0}).value(), ssg::resolveSelectionPosition("abc", ssg::ByteOffset{2}).value()}}}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Editor);

    // A KEYBOARD caret motion (a different SelectionCommand) does NOT change focus:
    // dispatched from panel focus, the caret moves but the keyboard stays on the panel.
    focusPanel();
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);
    ASSERT_TRUE(runtime.dispatch({"cursor.left",  {}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);
    ASSERT_TRUE(runtime.dispatch({"select.line_down",  {}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);
}

TEST(editRevealsThePrimaryCaretFreeScrollDoesNotAndFollowsPrimary) {
    // A document taller than the pane, one single-cell line per row.
    auto root = testRuntimePath("runtime_editing_reveal");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::string text;
    for (int i = 0; i < 100; ++i) text += "a\n";  // line L starts at byte L*2
    std::ofstream{root / "workspace" / "tall.txt", std::ios::binary} << text;
    auto created = ssg::createEditor({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"tall.txt"}}).accepted());

    const ssg::ViewportDimensions dims{80, 24};
    ssg::test::GridTestView grid{dims};
    auto firstRow = [&] {
        auto snap = grid.present(runtime);
        return snap ? snap->viewport.firstVisualRow : 0U;
    };
    auto maximum = [&] {
        auto snap = grid.present(runtime);
        return snap ? snap->viewport.scrollbar.maximumFirstRow : 0U;
    };
    // Snapshot once to populate the pane-height cache; the caret is at the top.
    ASSERT_EQ(firstRow(), 0U);
    auto const maxFirst = maximum();
    ASSERT_TRUE(maxFirst > 0);  // the document is scrollable

    // Free scroll DOWN with no edit: the offset moves and does NOT snap back.
    ASSERT_TRUE(grid.dispatch(runtime, {"view.scroll_lines",  ssg::ScrollLinesArguments{40}}).accepted());
    ASSERT_EQ(firstRow(), 40U);   // caret (line 0) is now off-screen above
    ASSERT_EQ(firstRow(), 40U);   // a second read without an edit stays put

    // Typing at the (off-screen) caret reveals it: minimal offset to show line 0.
    ASSERT_TRUE(runtime.dispatch({"text.insert",  ssg::TextInputArguments{"x"}}).accepted());
    ASSERT_EQ(firstRow(), 0U);

    // Move the caret to the last line (navigation reveals it to the bottom), then
    // free-scroll to the top so the caret is off-screen below.
    auto doc = runtime.activeDocumentText();
    auto endPos = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{static_cast<std::uint32_t>(doc.size())});
    ASSERT_TRUE(endPos.has_value());
    ASSERT_TRUE(runtime.dispatch({"cursor.set_position",  ssg::SelectionCommandArguments{endPos, std::nullopt}}).accepted());
    ASSERT_TRUE(grid.dispatch(runtime, {"view.scroll_lines",  ssg::ScrollLinesArguments{-200}}).accepted());
    ASSERT_EQ(firstRow(), 0U);
    // An edit at the bottom caret reveals it to the maximum offset (last line shown).
    ASSERT_TRUE(runtime.dispatch({"text.insert",  ssg::TextInputArguments{"y"}}).accepted());
    ASSERT_EQ(firstRow(), maximum());

    // Two cursors: secondary near the top (line 0), PRIMARY near the bottom (the
    // back selection). select.add_range pushes the new range to the back.
    doc = runtime.activeDocumentText();
    auto top = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{0});
    auto bottomLineStart = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{static_cast<std::uint32_t>(doc.size()) - 2});
    ASSERT_TRUE(top.has_value());
    ASSERT_TRUE(bottomLineStart.has_value());
    ASSERT_TRUE(runtime.dispatch({"cursor.set_position",  ssg::SelectionCommandArguments{top, std::nullopt}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"select.add_range",  ssg::SelectionCommandArguments{std::nullopt, ssg::Selection{*bottomLineStart, *bottomLineStart}}}).accepted());
    // Free-scroll to the top so the primary (bottom) caret is off-screen below.
    ASSERT_TRUE(grid.dispatch(runtime, {"view.scroll_lines",  ssg::ScrollLinesArguments{-200}}).accepted());
    ASSERT_EQ(firstRow(), 0U);
    // A multi-cursor insert reveals the PRIMARY caret (bottom), not the secondary
    // (top): the offset jumps down far enough to show it, rather than staying at 0.
    ASSERT_TRUE(runtime.dispatch({"text.insert",  ssg::TextInputArguments{"z"}}).accepted());
    // Asserted as "the primary caret is on screen" rather than "the offset equals
    // the maximum".  The primary caret sits on the second-to-last line, so
    // revealing it lands one row short of the maximum.  This read `== maximum()`
    // while the viewport scrolled against the terminal height instead of the pane
    // content height: the maximum was then too small, the reveal clamped to it,
    // and the assertion passed for the wrong reason -- masking the very bug that
    // left the last rows of every document unreachable.
    {
        auto snap = grid.present(runtime);
        ASSERT_TRUE(snap.has_value());
        auto const& view = snap->viewport;
        auto const caretLine =
            snap->selections.primary().active.line.value();
        ASSERT_TRUE(view.firstVisualRow > 0);  // did not follow the secondary caret
        ASSERT_TRUE(caretLine >= view.firstVisualRow);
        ASSERT_TRUE(caretLine < view.firstVisualRow + view.visibleRows.size());
    }
    std::filesystem::remove_all(root);
}

TEST(undoAndPasteRevealTheCaret) {
    auto root = testRuntimePath("runtime_editing_reveal2");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::string text;
    for (int i = 0; i < 100; ++i) text += "a\n";
    std::ofstream{root / "workspace" / "tall.txt", std::ios::binary} << text;
    auto created = ssg::createEditor({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"tall.txt"}}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    ssg::test::GridTestView grid{dims};
    auto firstRow = [&] {
        auto snap = grid.present(runtime);
        return snap ? snap->viewport.firstVisualRow : 0U;
    };
    ASSERT_EQ(firstRow(), 0U);

    // Type a character (caret at top), then scroll away and UNDO: undo reveals.
    ASSERT_TRUE(runtime.dispatch({"text.insert",  ssg::TextInputArguments{"x"}}).accepted());
    ASSERT_TRUE(grid.dispatch(runtime, {"view.scroll_lines",  ssg::ScrollLinesArguments{40}}).accepted());
    ASSERT_EQ(firstRow(), 40U);
    ASSERT_TRUE(runtime.dispatch({"edit.undo",  {}}).accepted());
    ASSERT_EQ(firstRow(), 0U);

    // Copy a line, collapse the caret to the top, scroll away, and PASTE: the
    // paste inserts a duplicate (a real document mutation) and reveals the caret.
    // (Pasting over the same selection would reproduce identical bytes — a no-op
    // that correctly does not mutate or reveal.)
    ASSERT_TRUE(runtime.dispatch({"select.line_down",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"clipboard.copy",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"cursor.set_position",  ssg::SelectionCommandArguments{ssg::resolveSelectionPosition(text, ssg::ByteOffset{0}), std::nullopt}}).accepted());
    ASSERT_TRUE(grid.dispatch(runtime, {"view.scroll_lines",  ssg::ScrollLinesArguments{40}}).accepted());
    ASSERT_EQ(firstRow(), 40U);
    ASSERT_TRUE(runtime.dispatch({"clipboard.paste",  {}}).accepted());
    // The pasted "a\n" pushes the caret to line 1; revealing from row 40 scrolls
    // up so the caret's row sits at the viewport top (first_row == its row).
    ASSERT_EQ(firstRow(), 1U);
    std::filesystem::remove_all(root);
}

TEST(multiCursorPastePreservesAllCursors) {
    auto root = testRuntimePath("runtime_editing_mcpaste");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "m.txt", std::ios::binary} << "aaa\nbbb\nccc\n";
    auto created = ssg::createEditor({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"m.txt"}}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    auto selectionCount = [&] {
        auto snap = projectFrame(runtime, dims);
        return snap ? snap->selections.items().size() : std::size_t{0};
    };

    // Build two cursors (top of line 0 and top of line 1), copy, then paste. The
    // paste must not collapse the multi-cursor set to a single caret.
    auto doc = runtime.activeDocumentText();
    auto p0 = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{0});
    auto p1 = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{4});
    ASSERT_TRUE(p0.has_value() && p1.has_value());
    ASSERT_TRUE(runtime.dispatch({"cursor.set_position",  ssg::SelectionCommandArguments{p0, std::nullopt}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"select.add_range",  ssg::SelectionCommandArguments{std::nullopt, ssg::Selection{*p1, *p1}}}).accepted());
    ASSERT_EQ(selectionCount(), std::size_t{2});
    ASSERT_TRUE(runtime.dispatch({"select.line_end",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"clipboard.copy",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"clipboard.paste",  {}}).accepted());
    ASSERT_EQ(selectionCount(), std::size_t{2});
    std::filesystem::remove_all(root);
}

TEST(multiCursorTypingReplacesEachSelectionAndKeepsAllCursors) {
    auto root = testRuntimePath("runtime_editing_mctype");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "m.txt", std::ios::binary}
        << "aaa\nbbb\nccc\n";
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",
                                  std::string{"m.txt"}}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    auto selectionCount = [&] {
        auto snap = projectFrame(runtime, dims);
        return snap ? snap->selections.items().size()
                    : std::size_t{0};
    };

    // Two RANGE selections over "aaa" and "bbb" (as Alt+d would build over a
    // repeated word).
    auto doc = runtime.activeDocumentText();
    auto p0 = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{0});
    auto p3 = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{3});
    auto p4 = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{4});
    auto p7 = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{7});
    ASSERT_TRUE(p0 && p3 && p4 && p7);
    ASSERT_TRUE(runtime.dispatch({"select.set_range",
         ssg::SelectionCommandArguments{std::nullopt,
                                        ssg::Selection{*p0, *p3}}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"select.add_range",
         ssg::SelectionCommandArguments{std::nullopt,
                                        ssg::Selection{*p4, *p7}}}).accepted());
    ASSERT_EQ(selectionCount(), std::size_t{2});

    // Typing replaces EACH selection and leaves a caret at each edit -- the
    // multi-cursor must survive (regression: it used to collapse to one).
    ASSERT_TRUE(runtime.dispatch({"text.insert",
                                  ssg::TextInputArguments{"X"}}).accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"X\nX\nccc\n"});
    ASSERT_EQ(selectionCount(), std::size_t{2});

    // Continuing to type inserts at BOTH carets, so multi-cursor editing works
    // across successive keystrokes.
    ASSERT_TRUE(runtime.dispatch({"text.insert",
                                  ssg::TextInputArguments{"Y"}}).accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"XY\nXY\nccc\n"});
    ASSERT_EQ(selectionCount(), std::size_t{2});

    // Undo and redo across the multi-cursor edits keep all cursors too (the
    // bindHistory clamp must preserve the set, not collapse it).
    ASSERT_TRUE(runtime.dispatch({"edit.undo",  {}}).accepted());
    ASSERT_EQ(selectionCount(), std::size_t{2});
    ASSERT_TRUE(runtime.dispatch({"edit.redo",  {}}).accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"XY\nXY\nccc\n"});
    ASSERT_EQ(selectionCount(), std::size_t{2});
    std::filesystem::remove_all(root);
}

TEST(replaceAllRevealsTheCaretWhenNoMatchRemains) {
    auto root = testRuntimePath("runtime_editing_replacereveal");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    // A tall doc with the only match near the bottom.
    std::string text;
    for (int i = 0; i < 90; ++i) text += "filler\n";
    text += "needle\n";
    std::ofstream{root / "workspace" / "t.txt", std::ios::binary} << text;
    auto created = ssg::createEditor({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"t.txt"}}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    ssg::test::GridTestView grid{dims};
    auto firstRow = [&] {
        auto snap = grid.present(runtime);
        return snap ? snap->viewport.firstVisualRow : 0U;
    };
    ASSERT_EQ(firstRow(), 0U);  // caret at top; the match is off-screen far below

    ASSERT_TRUE(runtime.dispatch({"replace.open",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"find.update_query",  ssg::FindQueryArguments{"needle"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"replace.update_replacement",  ssg::FindQueryArguments{"pin"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"replace.all",  {}}).accepted());
    // No match remains, but the caret (now at the replaced text near the bottom)
    // is revealed rather than left off-screen.
    ASSERT_TRUE(firstRow() > 0U);
    std::filesystem::remove_all(root);
}

TEST(promptCommandsFulfillFindReplaceByActiveKind) {
    auto root = testRuntimePath("runtime_editing_prompt_fulfillment");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "f.txt"} << "cat cat cat";
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",
                                  std::string{"f.txt"}})
                    .accepted());

    ASSERT_TRUE(runtime.dispatch({"find.open",  {}})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch({"find.update_query",
                                  ssg::FindQueryArguments{"cat"}})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch({"prompt.submit",  {}})
                    .accepted());
    auto findAfterSubmit =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(findAfterSubmit.has_value());
    if (findAfterSubmit) {
        ASSERT_TRUE(findAfterSubmit->findReplace.open);
        ASSERT_EQ(findAfterSubmit->findReplace.activeMatch,
                  std::optional<std::size_t>{1});
        ASSERT_TRUE(findAfterSubmit->promptStatus.activeKind.has_value());
        ASSERT_EQ(*findAfterSubmit->promptStatus.activeKind,
                  ssg::PromptKind::Find);
    }

    ASSERT_TRUE(runtime.dispatch({"prompt.previous",  {}})
                    .accepted());
    auto findAfterPrevious =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(findAfterPrevious.has_value());
    if (findAfterPrevious) {
        ASSERT_EQ(findAfterPrevious->findReplace.activeMatch,
                  std::optional<std::size_t>{0});
    }

    ASSERT_TRUE(runtime.dispatch({"replace.open",  {}})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch({"replace.update_replacement",
                                  ssg::FindQueryArguments{"dog"}})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch({"prompt.submit",  {}})
                    .accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"dog cat cat"});
    auto replaceAfterSubmit =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(replaceAfterSubmit.has_value());
    std::uint64_t generationBeforeCancel = 0;
    if (replaceAfterSubmit) {
        generationBeforeCancel = replaceAfterSubmit->findReplace.generation;
        ASSERT_TRUE(replaceAfterSubmit->promptStatus.activeKind.has_value());
        ASSERT_EQ(*replaceAfterSubmit->promptStatus.activeKind,
                  ssg::PromptKind::Replace);
    }

    ASSERT_TRUE(runtime.dispatch({"prompt.cancel",  {}})
                    .accepted());
    auto afterCancel =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(afterCancel.has_value());
    if (afterCancel) {
        auto const& find = afterCancel->findReplace;
        ASSERT_FALSE(find.open);
        ASSERT_FALSE(find.replaceMode);
        ASSERT_TRUE(find.matches.empty());
        ASSERT_FALSE(find.activeMatch.has_value());
        ASSERT_TRUE(find.generation > generationBeforeCancel);
        ASSERT_FALSE(afterCancel->promptStatus.activeKind.has_value());
    }
    std::filesystem::remove_all(root);
}

// find.word_under_cursor: leader,8 seeds find with the word under the caret and
// searches the whole document literally.
namespace {
ssg::EditorCreateResult openWith(const std::filesystem::path& root,
                                          std::string_view name,
                                          std::string_view contents) {
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / std::string{name}} << contents;
    return ssg::createEditor({workspace, root / "scratch", root / "recovery"});
}
}  // namespace

TEST(findWordUnderCursorSeedsTheCaretWordAndFindsEveryOccurrence) {
    auto root = uniqueRoot();
    auto created = openWith(root, "w.txt", "alpha beta alpha");
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"w.txt"}}).accepted());

    // The caret starts at offset 0, inside "alpha".
    ASSERT_TRUE(runtime.dispatch({"find.word_under_cursor",  {}}).accepted());
    auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& find = snapshot->findReplace;
    ASSERT_TRUE(find.open);
    ASSERT_EQ(find.query, std::string{"alpha"});
    ASSERT_EQ(find.matches.size(), std::size_t{2});
    ASSERT_FALSE(find.options.regex);
    ASSERT_EQ(snapshot->promptStatus.activeKind,
              std::optional{ssg::PromptKind::Find});
    // The query is live, not just prompt text: next moves to the second "alpha".
    ASSERT_TRUE(runtime.dispatch({"find.next",  {}}).accepted());
    auto advanced = projectFrame(runtime, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(advanced.has_value());
    if (advanced) ASSERT_EQ(advanced->findReplace.activeMatch.value_or(999), std::size_t{1});
    std::filesystem::remove_all(root);
}

TEST(findWordUnderCursorTakesTheWordWhenTheCaretSitsJustPastIt) {
    auto root = uniqueRoot();
    auto created = openWith(root, "w.txt", "alpha beta");
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"w.txt"}}).accepted());
    // Move the caret to offset 5 -- the space, immediately past "alpha".
    ASSERT_TRUE(runtime.dispatch({"cursor.word_right",  {}}).accepted());

    ASSERT_TRUE(runtime.dispatch({"find.word_under_cursor",  {}}).accepted());
    auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) ASSERT_EQ(snapshot->findReplace.query, std::string{"alpha"});
    std::filesystem::remove_all(root);
}

TEST(findWordUnderCursorPrefersTheSelectionAndSearchesItLiterally) {
    auto root = uniqueRoot();
    // "a.b" occurs literally at offsets 0 and 8; "axb" (offset 4) matches "a.b"
    // only as a regex.  A forced-literal, whole-document search therefore finds
    // exactly two matches: three would mean regex mode leaked, one would mean the
    // search was restricted to the selected range.
    auto created = openWith(root, "w.txt", "a.b axb a.b");
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"w.txt"}}).accepted());
    // Select the first three bytes, "a.b", spanning a word boundary the caret
    // word would never include.
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(runtime.dispatch({"select.right",  {}}).accepted());
    }
    // Turn regex AND selection-only ON first; the command must force both off so
    // the seeded word is searched literally across the whole document.
    ASSERT_TRUE(runtime.dispatch({"find.open",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"find.toggle_regex",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"find.toggle_selection",  {}}).accepted());

    ASSERT_TRUE(runtime.dispatch({"find.word_under_cursor",  {}}).accepted());
    auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& find = snapshot->findReplace;
    ASSERT_EQ(find.query, std::string{"a.b"});
    ASSERT_FALSE(find.options.regex);
    ASSERT_FALSE(find.options.selectionOnly);
    ASSERT_EQ(find.matches.size(), std::size_t{2});
    std::filesystem::remove_all(root);
}

TEST(findWordUnderCursorIsANoOpWithNoWordUnderTheCaret) {
    auto root = uniqueRoot();
    auto created = openWith(root, "w.txt", "a  b");
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"w.txt"}}).accepted());
    // Move the caret to offset 2, between the two spaces: no word on either side.
    ASSERT_TRUE(runtime.dispatch({"cursor.right",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"cursor.right",  {}}).accepted());

    // Reported success, but no find controller and no prompt were opened.
    ASSERT_TRUE(runtime.dispatch({"find.word_under_cursor",  {}}).accepted());
    auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_FALSE(snapshot->findReplace.open);
    ASSERT_FALSE(snapshot->promptStatus.activeKind.has_value());
    std::filesystem::remove_all(root);
}

TEST(promptFocusIsSingleAndResolvesToItsRegion) {
    // The mapping itself (pure): palette -> header, every other kind -> footer.
    static_assert(ssg::promptFocusRegion(ssg::PromptKind::Palette) ==
                  ssg::PromptRegion::Header);
    static_assert(ssg::promptFocusRegion(ssg::PromptKind::Find) ==
                  ssg::PromptRegion::Footer);
    static_assert(ssg::promptFocusRegion(ssg::PromptKind::Replace) ==
                  ssg::PromptRegion::Footer);

    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "hits.txt"} << "cat cat cat";
    auto created = ssg::createEditor({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"hits.txt"}}).accepted());

    const ssg::ViewportDimensions dims{80, 24};

    // No prompt: focus is not Prompt.
    {
        auto snap = projectFrame(runtime, dims);
        ASSERT_TRUE(snap.has_value());
        if (snap) {
            ASSERT_TRUE(ssg::effectiveUiFocus(
                            snap->uiTree) !=
                        ssg::FocusTarget::Prompt);
        }
    }

    // Palette (a picker) is HEADER-anchored: focus is Prompt, the header input
    // line is populated, and NO footer prompt reservation exists.
    ASSERT_TRUE(runtime.dispatch({"palette.open",  {}}).accepted());
    {
        auto snap = projectFrame(runtime, dims);
        ASSERT_TRUE(snap.has_value());
        if (!snap) return;
        const auto& s = *snap;
        ASSERT_EQ(ssg::effectiveUiFocus(s.uiTree), ssg::FocusTarget::Prompt);
        ASSERT_TRUE(snap->layout.find(ssg::UiNodeId{
            std::string{ssg::kHeaderPromptInputNodeId}}) != nullptr);
        ASSERT_TRUE(snap->layout.find(ssg::UiNodeId{
            std::string{ssg::kFooterPromptNodeId}}) == nullptr);
    }
    ASSERT_TRUE(runtime.dispatch({"palette.close",  {}}).accepted());
    {
        auto snap = projectFrame(runtime, dims);
        ASSERT_TRUE(snap.has_value());
        if (snap) {
            ASSERT_TRUE(ssg::effectiveUiFocus(
                            snap->uiTree) !=
                        ssg::FocusTarget::Prompt);
        }
    }

    // Find is FOOTER-anchored: focus is Prompt, a footer reservation exists, and
    // NO header input line.
    ASSERT_TRUE(runtime.dispatch({"find.open",  {}}).accepted());
    {
        auto snap = projectFrame(runtime, dims);
        ASSERT_TRUE(snap.has_value());
        if (!snap) return;
        const auto& s = *snap;
        ASSERT_EQ(ssg::effectiveUiFocus(s.uiTree), ssg::FocusTarget::Prompt);
        ASSERT_TRUE(snap->layout.find(ssg::UiNodeId{
            std::string{ssg::kFooterPromptNodeId}}) != nullptr);
        ASSERT_TRUE(snap->layout.find(ssg::UiNodeId{
            std::string{ssg::kHeaderPromptInputNodeId}}) == nullptr);
    }
}

} // namespace

SSG_TEST_SUITE(test_session_editing) {
    RUN(runtimeTextSelectionAndHistoryMatchFeatureOperations);
    RUN(typingUndoBreaksOnWordAndLineBoundaries);
    RUN(workspaceReplaceDispatchMatchesFeaturePreviewAndDiskApply);
    RUN(workspaceReplaceRejectsStaleAndOutOfBoundsPreview);
    RUN(workspaceReplaceUpdatesOpenDocumentSnapshotAndDisk);
    RUN(workspaceSearchAndReplaceExcludeRuntimeStateRoots);
    RUN(pointerSelectionCommandsFocusTheEditorKeyboardMotionDoesNot);
    RUN(editRevealsThePrimaryCaretFreeScrollDoesNotAndFollowsPrimary);
    RUN(undoAndPasteRevealTheCaret);
    RUN(multiCursorPastePreservesAllCursors);
    RUN(multiCursorTypingReplacesEachSelectionAndKeepsAllCursors);
    RUN(replaceAllRevealsTheCaretWhenNoMatchRemains);
    RUN(promptCommandsFulfillFindReplaceByActiveKind);
    RUN(findWordUnderCursorSeedsTheCaretWordAndFindsEveryOccurrence);
    RUN(findWordUnderCursorTakesTheWordWhenTheCaretSitsJustPastIt);
    RUN(findWordUnderCursorPrefersTheSelectionAndSearchesItLiterally);
    RUN(findWordUnderCursorIsANoOpWithNoWordUnderTheCaret);
    RUN(promptFocusIsSingleAndResolvesToItsRegion);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
