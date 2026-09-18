#include "../test_helpers.h"
#include "../grid_test_frame.h"
#include "../grid_test_view.h"

#include <ssg/Editor.h>
#include <ssg/FindReplace.h>
#include <ssg/Keymap.h>
#include <ssg/Selection.h>
#include <ssg/PromptSurface.h>
#include <ssg/TextInputCommands.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
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
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "edit.txt"} << "abc";
    return root;
}

TEST(runtimeTextSelectionAndHistoryMatchFeatureOperations) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor({root / "workspace", root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"edit.txt"}).accepted());

    auto setPosition = ssg::test::setSelections(runtime, {{3, 3}});
    ASSERT_TRUE(setPosition.accepted());
    auto typed = ssg::test::typeText(runtime, "d");
    ASSERT_TRUE(typed.accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"abcd"});

    auto undo = runtime.dispatch("edit.undo");
    ASSERT_TRUE(undo.accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"abc"});
    auto redo = runtime.dispatch("edit.redo");
    ASSERT_TRUE(redo.accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"abcd"});
}

TEST(typingUndoBreaksOnWordAndLineBoundaries) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor({root / "workspace", root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"edit.txt"}).accepted());
    ASSERT_TRUE(ssg::test::setSelections(runtime, {{3, 3}}).accepted());

    const auto type = [&](char character) {
        return ssg::test::typeText(runtime, std::string{character}).accepted();
    };
    for (char character : std::string{"foo bar"}) ASSERT_TRUE(type(character));
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"abcfoo bar"});

    // The space sealed the "foo " unit, so the first undo removes only "bar".
    ASSERT_TRUE(runtime.dispatch("edit.undo").accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"abcfoo "});
    // The second undo removes the "foo " word unit.
    ASSERT_TRUE(runtime.dispatch("edit.undo").accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"abc"});

    // Newlines seal a unit per line.
    ASSERT_TRUE(type('x'));
    ASSERT_TRUE(runtime.dispatch("text.newline").accepted());
    ASSERT_TRUE(type('y'));
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"abcx\ny"});
    ASSERT_TRUE(runtime.dispatch("edit.undo").accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"abcx\n"});
}

TEST(searchPanelEditsSubmitsPublishesAndCancelsWithoutEagerWork) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::ofstream{workspace / "other.txt"} << "alpha\nbeta\n";
    auto created = ssg::createEditor(
        {workspace, root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    const auto open = runtime.input(ssg::ClientKeyInput{
        ssg::KeyStroke{.code = ssg::KeyCode::KeyF,
                       .mod = true,
                       .shift = true},
        {}});
    ASSERT_EQ(open.outcome, ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(runtime.tree.activeProviderBinding()->kind,
              ssg::TreeProviderKind::Search);
    auto type = [&](std::string text) {
        return runtime.input(ssg::ClientKeyInput{{}, std::move(text)});
    };
    auto key = [&](ssg::KeyCode code) {
        return runtime.input(
            ssg::ClientKeyInput{ssg::KeyStroke{.code = code}, {}});
    };

    ASSERT_EQ(type("a").outcome, ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(type("e\xCC\x81").outcome,
              ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(key(ssg::KeyCode::Backspace).outcome,
              ssg::ClientInputOutcome::Dispatched);
    auto state = runtime.tree.searchState(ssg::TreeProviderId{"search"});
    ASSERT_EQ(state->query.text(), std::string{"a"});
    ASSERT_TRUE(state->editing);
    ASSERT_FALSE(runtime.workspaceSearchPending());

    ASSERT_EQ(key(ssg::KeyCode::Enter).outcome,
              ssg::ClientInputOutcome::Dispatched);
    ASSERT_TRUE(runtime.workspaceSearchPending());
    ASSERT_EQ(runtime.search.viewState().mode, ssg::SearchMode::Text);
    auto treeView = runtime.tree.viewState();
    auto active = ssg::activeTreeProvider(treeView);
    ASSERT_TRUE(active->nodes.empty());
    ASSERT_FALSE(active->selected.has_value());

    while (runtime.workspaceSearchPending()) runtime.advanceWorkspaceSearch();
    treeView = runtime.tree.viewState();
    active = ssg::activeTreeProvider(treeView);
    ASSERT_EQ(active->nodes.size(), std::size_t{3});
    ASSERT_EQ(active->nodes[0].node.workspacePath,
              std::optional<std::string>{"edit.txt"});
    ASSERT_EQ(active->nodes[0].node.sourceLine,
              std::optional<std::uint64_t>{0});
    ASSERT_EQ(active->nodes[0].node.sourceColumn,
              std::optional<std::uint64_t>{1});
    ASSERT_EQ(active->nodes[1].node.workspacePath,
              std::optional<std::string>{"other.txt"});
    ASSERT_EQ(active->nodes[2].node.workspacePath,
              std::optional<std::string>{"other.txt"});

    (void)runtime.workspaceSearch("a");
    while (runtime.workspaceSearchPending()) runtime.advanceWorkspaceSearch();
    treeView = runtime.tree.viewState();
    active = ssg::activeTreeProvider(treeView);
    ASSERT_EQ(active->nodes.size(), std::size_t{3});

    ASSERT_EQ(key(ssg::KeyCode::ArrowUp).outcome,
              ssg::ClientInputOutcome::Dispatched);
    state = runtime.tree.searchState(ssg::TreeProviderId{"search"});
    ASSERT_FALSE(state->editing);
    ASSERT_EQ(runtime.tree.selectedNode()->workspacePath,
              std::optional<std::string>{"other.txt"});
    const auto browsingQuery = state->query;
    ASSERT_EQ(key(ssg::KeyCode::Home).outcome,
              ssg::ClientInputOutcome::Unhandled);
    ASSERT_EQ(key(ssg::KeyCode::Delete).outcome,
              ssg::ClientInputOutcome::Unhandled);
    state = runtime.tree.searchState(ssg::TreeProviderId{"search"});
    ASSERT_FALSE(state->editing);
    ASSERT_EQ(state->query, browsingQuery);

    runtime.screen.focusEditor();
    ASSERT_EQ(runtime.input(ssg::SearchQueryPointerInput{}).outcome,
              ssg::ClientInputOutcome::Dispatched);
    state = runtime.tree.searchState(ssg::TreeProviderId{"search"});
    ASSERT_TRUE(state->editing);
    ASSERT_EQ(runtime.screen.effectiveFocus(), ssg::FocusTarget::Panel);

    ASSERT_EQ(type("z").outcome, ssg::ClientInputOutcome::Dispatched);
    state = runtime.tree.searchState(ssg::TreeProviderId{"search"});
    ASSERT_TRUE(state->editing);
    ASSERT_EQ(key(ssg::KeyCode::Enter).outcome,
              ssg::ClientInputOutcome::Dispatched);
    ASSERT_TRUE(runtime.workspaceSearchPending());
    ASSERT_TRUE(ssg::activeTreeProvider(runtime.tree.viewState())->nodes.empty());
    ASSERT_TRUE(runtime.dispatch("panel.show_files").accepted());
    ASSERT_FALSE(runtime.workspaceSearchPending());
    state = runtime.tree.searchState(ssg::TreeProviderId{"search"});
    ASSERT_FALSE(state->submittedQuery.has_value());
    ASSERT_FALSE(state->searching);

    ASSERT_TRUE(runtime.dispatch("panel.show_search").accepted());
    state->query = ssg::PromptEditState{};
    state->editing = true;
    ASSERT_TRUE(runtime.tree.setSearchState(
        ssg::TreeProviderId{"search"}, *state));
    ASSERT_EQ(key(ssg::KeyCode::Enter).outcome,
              ssg::ClientInputOutcome::Dispatched);
    ASSERT_FALSE(runtime.workspaceSearchPending());
    state = runtime.tree.searchState(ssg::TreeProviderId{"search"});
    ASSERT_FALSE(state->submittedQuery.has_value());
    std::filesystem::remove_all(root);
}

TEST(searchPanelActivatesTheSelectedResultAtItsMatchColumn) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::ofstream{workspace / "target.txt"} << "zero\nalpha here\n";
    auto created = ssg::createEditor(
        {workspace, root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch("panel.show_search").accepted());
    ASSERT_EQ(runtime.input(ssg::ClientKeyInput{{}, "pha"}).outcome,
              ssg::ClientInputOutcome::Dispatched);
    const auto activation = runtime.input(ssg::ClientKeyInput{
        ssg::KeyStroke{.code = ssg::KeyCode::Enter}, {}});
    ASSERT_EQ(activation.outcome, ssg::ClientInputOutcome::Dispatched);
    while (runtime.workspaceSearchPending()) runtime.advanceWorkspaceSearch();
    ASSERT_EQ(runtime.input(ssg::ClientKeyInput{
                  ssg::KeyStroke{.code = ssg::KeyCode::ArrowDown}, {}}).outcome,
              ssg::ClientInputOutcome::Dispatched);
    const auto resultActivation = runtime.input(ssg::ClientKeyInput{
        ssg::KeyStroke{.code = ssg::KeyCode::Enter}, {}});
    ASSERT_EQ(resultActivation.outcome, ssg::ClientInputOutcome::ViewOwned);
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"zero\nalpha here\n"});
    ASSERT_EQ(runtime.selection.selections.primary().active.byteOffset,
              ssg::ByteOffset{7});
    ASSERT_EQ(runtime.screen.effectiveFocus(), ssg::FocusTarget::Editor);
    std::filesystem::remove_all(root);
}

TEST(searchPanelPointerActivationRevealsTheMatch) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::string text;
    for (int line = 0; line < 30; ++line) text += "line\n";
    text += "needle\n";
    std::ofstream{workspace / "target.txt"} << text;
    auto created = ssg::createEditor(
        {workspace, root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ssg::GridPresenter presenter;
    ASSERT_TRUE(presenter.project(runtime, {{40, 8}, {}}).has_value());
    ASSERT_TRUE(runtime.dispatch("panel.show_search").accepted());
    ASSERT_EQ(runtime.input(ssg::ClientKeyInput{{}, "needle"}).outcome,
              ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(runtime.input(ssg::ClientKeyInput{
                  ssg::KeyStroke{.code = ssg::KeyCode::Enter}, {}}).outcome,
              ssg::ClientInputOutcome::Dispatched);
    while (runtime.workspaceSearchPending()) runtime.advanceWorkspaceSearch();
    const auto tree = runtime.tree.viewState();
    const auto* provider = ssg::activeTreeProvider(tree);
    ASSERT_TRUE(provider != nullptr && provider->nodes.size() == 1);
    if (provider == nullptr || provider->nodes.size() != 1) return;

    const auto activation =
        runtime.input(ssg::TreePointerInput{provider->nodes.front().node.id});
    ASSERT_EQ(activation.outcome, ssg::ClientInputOutcome::ViewOwned);
    ASSERT_TRUE(activation.command && activation.command->viewAction);
    if (!activation.command || !activation.command->viewAction) return;
    auto frame = presenter.project(runtime, {{40, 8}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    ASSERT_EQ(frame->selections.primary().active.line, ssg::LineIndex{30});
    ASSERT_TRUE(frame->viewport.totalVisualRows > 30);
    ASSERT_TRUE(frame->viewport.scrollbar.viewportRows < 30);
    ASSERT_TRUE(frame->viewport.firstVisualRow > 0);
    ASSERT_TRUE(
        presenter.apply(*activation.command->viewAction, *frame).accepted());
    frame = presenter.project(runtime, {{40, 8}, {}});
    ASSERT_TRUE(frame.has_value());
    if (frame) ASSERT_TRUE(frame->viewport.firstVisualRow > 0);
    std::filesystem::remove_all(root);
}

TEST(findUpdateQueryProjectsMatchesAndPromptAndNextCycles) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "hits.txt"} << "cat cat cat";

    auto created = ssg::createEditor({
        workspace, root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"hits.txt"}).accepted());

    ASSERT_TRUE(runtime.dispatch("find.open").accepted());
    ASSERT_TRUE(runtime.updateFindQuery(ssg::test::promptText("cat")).accepted());

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

    ASSERT_EQ(snapshot->prompt.activeKind,
              std::optional{ssg::PromptKind::Find});

    const auto activeAfter = [&](int advances) -> std::size_t {
        for (int i = 0; i < advances; ++i) {
            (void)runtime.dispatch("find.next");
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
        workspace, root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    // No document is open: find.close (and next/previous) must not be rejected by
    // the active-document guard, so a find opened before the last tab closed can
    // still be dismissed.
    ASSERT_TRUE(runtime.dispatch("find.close").accepted());
    ASSERT_TRUE(runtime.dispatch("find.next").accepted());
    ASSERT_TRUE(runtime.dispatch("find.previous").accepted());
}

TEST(findCloseDoesNotCancelAnUnrelatedPrompt) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "doc.txt"} << "hello";
    auto created = ssg::createEditor({
        workspace, root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"doc.txt"}).accepted());

    ASSERT_TRUE(runtime.dispatch("palette.open").accepted());
    auto before = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(before.has_value());
    if (before) {
        ASSERT_TRUE(before->prompt.activeKind.has_value());
        ASSERT_EQ(*before->prompt.activeKind,
                  ssg::PromptKind::Palette);
    }

    // A find.close while the palette prompt is active must leave the palette
    // prompt intact (it only owns the find prompt).
    ASSERT_TRUE(runtime.dispatch("find.close").accepted());
    auto after = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(after.has_value());
    if (after) {
        ASSERT_TRUE(after->prompt.activeKind.has_value());
        ASSERT_EQ(*after->prompt.activeKind,
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
        workspace, root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"a.txt"}).accepted());
    ASSERT_TRUE(runtime.dispatch("find.open").accepted());
    ASSERT_TRUE(runtime.updateFindQuery(ssg::test::promptText("cat")).accepted());
    {
        auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) ASSERT_EQ(snap->findReplace.matches.size(), std::size_t{3});
    }

    // Switching to another freshly opened document (which shares revision 1 with
    // a.txt) must dismiss find: identity, not revision equality, binds the state.
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"b.txt"}).accepted());
    auto after = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(after.has_value());
    if (after) {
        ASSERT_FALSE(after->findReplace.open);
        ASSERT_FALSE(after->prompt.activeKind.has_value());
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
        workspace, root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"tall.txt"}).accepted());

    // Baseline: the viewport starts at the top.
    {
        auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) ASSERT_EQ(snap->viewport.firstVisualRow, std::uint32_t{0});
    }

    ASSERT_TRUE(runtime.dispatch("find.open").accepted());
    ASSERT_TRUE(runtime.updateFindQuery(ssg::test::promptText("target")).accepted());

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
        workspace, root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"r.txt"}).accepted());
    ASSERT_TRUE(runtime.dispatch("replace.open").accepted());
    ASSERT_TRUE(runtime.updateFindQuery(ssg::test::promptText("cat")).accepted());
    ASSERT_TRUE(runtime.updateReplacement(ssg::test::promptText("dog")).accepted());

    {
        auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) {
            ASSERT_EQ(snap->findReplace.replacement, std::string{"dog"});
            ASSERT_EQ(snap->findReplace.matches.size(), std::size_t{3});
            ASSERT_EQ(snap->prompt.activeKind,
                      std::optional{ssg::PromptKind::Replace});
        }
    }

    ASSERT_TRUE(runtime.dispatch("replace.current").accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"dog cat cat"});
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
        workspace, root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"r.txt"}).accepted());
    ASSERT_TRUE(runtime.dispatch("replace.open").accepted());
    ASSERT_TRUE(runtime.updateFindQuery(ssg::test::promptText("cat")).accepted());
    ASSERT_TRUE(runtime.updateReplacement(ssg::test::promptText("dog")).accepted());
    ASSERT_TRUE(runtime.dispatch("replace.all").accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"dog dog dog"});
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
        workspace, root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"r.txt"}).accepted());

    // A find prompt (not replace) is open: replace commands must be benign
    // success no-ops that do not mutate the document or controller state.
    ASSERT_TRUE(runtime.dispatch("find.open").accepted());
    ASSERT_TRUE(runtime.updateFindQuery(ssg::test::promptText("cat")).accepted());
    ASSERT_TRUE(runtime.updateReplacement(ssg::test::promptText("dog")).accepted());
    ASSERT_TRUE(runtime.dispatch("replace.current").accepted());
    ASSERT_TRUE(runtime.dispatch("replace.all").accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"cat cat cat"});
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
        workspace, root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"m.txt"}).accepted());

    // No find/replace prompt yet: find.toggle_case must be a benign no-op that
    // leaves the (default) options untouched.
    ASSERT_TRUE(runtime.dispatch("find.toggle_case").accepted());
    {
        auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) ASSERT_FALSE(snap->findReplace.options.caseSensitive);
    }

    ASSERT_TRUE(runtime.dispatch("find.open").accepted());
    ASSERT_TRUE(runtime.updateFindQuery(ssg::test::promptText("cat")).accepted());
    {
        // Case-insensitive (default): all three "cat"s match.
        auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) ASSERT_EQ(snap->findReplace.matches.size(), std::size_t{3});
    }

    // Toggle case sensitivity: now only the lowercase "cat" matches.
    ASSERT_TRUE(runtime.dispatch("find.toggle_case").accepted());
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
        workspace, root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"m.txt"}).accepted());
    ASSERT_TRUE(runtime.dispatch("find.open").accepted());
    ASSERT_TRUE(runtime.updateFindQuery(ssg::test::promptText("cat")).accepted());
    ASSERT_TRUE(runtime.dispatch("find.toggle_case").accepted());
    // Case-sensitive find matched only the lowercase "cat".
    {
        auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) ASSERT_EQ(snap->findReplace.matches.size(), std::size_t{1});
    }
    // Opening replace must NOT widen the match population: options carry over so
    // replace.all acts on exactly what the user reviewed.
    ASSERT_TRUE(runtime.dispatch("replace.open").accepted());
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
        workspace, root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"m.txt"}).accepted());
    ASSERT_TRUE(runtime.dispatch("replace.open").accepted());
    {
        auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) {
            ASSERT_TRUE(snap->findReplace.open);
            ASSERT_TRUE(snap->prompt.activeKind.has_value());
        }
    }
    // A single find.close must close the controller AND dismiss the replace
    // prompt (no stale prompt requiring a second cancel).
    ASSERT_TRUE(runtime.dispatch("find.close").accepted());
    auto snap = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snap.has_value());
    if (snap) {
        ASSERT_FALSE(snap->findReplace.open);
        ASSERT_FALSE(snap->prompt.activeKind.has_value());
    }
}

TEST(pointerSelectionCommandsFocusTheEditorKeyboardMotionDoesNot) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor({root / "workspace", root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"edit.txt"}).accepted());
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
            ASSERT_TRUE(runtime.dispatch("panel.toggle").accepted());
        }
        ASSERT_TRUE(runtime.dispatch("panel.focus").accepted());
    };

    // A pointer click-to-caret from panel focus moves focus to the editor.
    focusPanel();
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);
    ASSERT_TRUE(ssg::test::clickDocument(runtime, 1).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Editor);

    // A pointer drag likewise focuses the editor.
    focusPanel();
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);
    ASSERT_TRUE(ssg::test::dragDocument(runtime, 0, 2).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Editor);

    // A pointer Alt+click add-caret focuses the editor too.
    focusPanel();
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);
    ASSERT_TRUE(ssg::test::clickDocument(runtime, 1, true).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Editor);

    // A pointer Alt+drag add-range focuses the editor too.
    focusPanel();
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);
    ASSERT_TRUE(ssg::test::dragDocument(runtime, 0, 2, true).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Editor);

    // A KEYBOARD caret motion (a different SelectionCommand) does NOT change focus:
    // dispatched from panel focus, the caret moves but the keyboard stays on the panel.
    focusPanel();
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);
    ASSERT_TRUE(runtime.dispatch("cursor.left").accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);
    ASSERT_TRUE(runtime.dispatch("select.line_down").accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);
}

TEST(panelFocusShortcutTogglesBetweenPanelAndEditor) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor(
        {root / "workspace", root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch("panel.toggle_focus").accepted());
    ASSERT_EQ(runtime.screen.effectiveFocus(), ssg::FocusTarget::Panel);
    ASSERT_TRUE(runtime.dispatch("panel.toggle_focus").accepted());
    ASSERT_EQ(runtime.screen.effectiveFocus(), ssg::FocusTarget::Editor);
    std::filesystem::remove_all(root);
}

TEST(panelWidthCommandsResizeAndClampTheSidebar) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor(
        {root / "workspace", root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_FALSE(projectFrame(runtime, {80, 24})->panel.has_value());
    ASSERT_TRUE(runtime.dispatch("panel.grow").accepted());
    const auto initial = projectFrame(runtime, {80, 24});
    ASSERT_TRUE(initial && initial->panel);
    if (!initial || !initial->panel) return;
    const auto initialWidth = initial->panel->rect.width;
    ASSERT_TRUE(runtime.dispatch("panel.grow").accepted());
    const auto grown = projectFrame(runtime, {80, 24});
    ASSERT_TRUE(grown && grown->panel);
    if (!grown || !grown->panel) return;
    ASSERT_EQ(grown->panel->rect.width, initialWidth + 1);

    runtime.focusEditor();
    ASSERT_EQ(runtime.screen.effectiveFocus(), ssg::FocusTarget::Editor);
    ASSERT_TRUE(runtime.dispatch("panel.shrink").accepted());
    ASSERT_EQ(runtime.screen.effectiveFocus(), ssg::FocusTarget::Editor);

    runtime.screen.toggleDistractionFree();
    ASSERT_FALSE(projectFrame(runtime, {80, 24})->panel.has_value());
    ASSERT_TRUE(runtime.dispatch("panel.grow").accepted());
    ASSERT_TRUE(projectFrame(runtime, {80, 24})->panel.has_value());

    for (int width = 0; width < initialWidth + 10; ++width) {
        ASSERT_TRUE(runtime.dispatch("panel.shrink").accepted());
    }
    const auto clamped = projectFrame(runtime, {80, 24});
    ASSERT_TRUE(clamped && clamped->panel);
    if (clamped && clamped->panel) {
        ASSERT_EQ(clamped->panel->rect.width, ssg::kPanelMinimumWidth);
    }
    std::filesystem::remove_all(root);
}

TEST(tabKeyInsertsATabInTheEditor) {
    auto root = uniqueRoot();
    std::filesystem::create_directories(root / "workspace");
    std::ofstream{root / "workspace" / "tab.txt"};
    auto created = ssg::createEditor(
        {root / "workspace", root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"tab.txt"}).accepted());
    const auto tab = runtime.input(ssg::ClientKeyInput{
        ssg::KeyStroke{.code = ssg::KeyCode::Tab}, {}});
    ASSERT_EQ(tab.outcome, ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"\t"});
    std::filesystem::remove_all(root);
}

TEST(editRevealsThePrimaryCaretFreeScrollDoesNotAndFollowsPrimary) {
    // A document taller than the pane, one single-cell line per row.
    auto root = testRuntimePath("runtime_editing_reveal");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "recovery");
    std::string text;
    for (int i = 0; i < 100; ++i) text += "a\n";  // line L starts at byte L*2
    std::ofstream{root / "workspace" / "tall.txt", std::ios::binary} << text;
    auto created = ssg::createEditor({root / "workspace", root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"tall.txt"}).accepted());

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
    ASSERT_TRUE(grid.input(runtime, ssg::ScrollLinesInput{
                                       {ssg::ScrollTarget::Document, 40}}).accepted());
    ASSERT_EQ(firstRow(), 40U);   // caret (line 0) is now off-screen above
    ASSERT_EQ(firstRow(), 40U);   // a second read without an edit stays put

    // Typing at the (off-screen) caret reveals it: minimal offset to show line 0.
    ASSERT_TRUE(ssg::test::typeText(runtime, "x").accepted());
    ASSERT_EQ(firstRow(), 0U);

    // Move the caret to the last line (navigation reveals it to the bottom), then
    // free-scroll to the top so the caret is off-screen below.
    auto doc = ssg::test::activeDocumentText(runtime);
    auto endPos = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{static_cast<std::uint32_t>(doc.size())});
    ASSERT_TRUE(endPos.has_value());
    ASSERT_TRUE(ssg::test::setSelections(
        runtime,
        {{endPos->byteOffset.value(), endPos->byteOffset.value()}}).accepted());
    ASSERT_TRUE(grid.input(runtime, ssg::ScrollLinesInput{
                                       {ssg::ScrollTarget::Document, -200}}).accepted());
    ASSERT_EQ(firstRow(), 0U);
    // An edit at the bottom caret reveals it to the maximum offset (last line shown).
    ASSERT_TRUE(ssg::test::typeText(runtime, "y").accepted());
    ASSERT_EQ(firstRow(), maximum());

    // Two cursors: secondary near the top (line 0), PRIMARY near the bottom (the
    // back selection).
    doc = ssg::test::activeDocumentText(runtime);
    auto top = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{0});
    auto bottomLineStart = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{static_cast<std::uint32_t>(doc.size()) - 2});
    ASSERT_TRUE(top.has_value());
    ASSERT_TRUE(bottomLineStart.has_value());
    ASSERT_TRUE(ssg::test::setSelections(
        runtime,
        {{top->byteOffset.value(), top->byteOffset.value()},
         {bottomLineStart->byteOffset.value(),
          bottomLineStart->byteOffset.value()}}).accepted());
    // Free-scroll to the top so the primary (bottom) caret is off-screen below.
    ASSERT_TRUE(grid.input(runtime, ssg::ScrollLinesInput{
                                       {ssg::ScrollTarget::Document, -200}}).accepted());
    ASSERT_EQ(firstRow(), 0U);
    // A multi-cursor insert reveals the PRIMARY caret (bottom), not the secondary
    // (top): the offset jumps down far enough to show it, rather than staying at 0.
    ASSERT_TRUE(ssg::test::typeText(runtime, "z").accepted());
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
    std::filesystem::create_directories(root / "recovery");
    std::string text;
    for (int i = 0; i < 100; ++i) text += "a\n";
    std::ofstream{root / "workspace" / "tall.txt", std::ios::binary} << text;
    auto created = ssg::createEditor({root / "workspace", root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"tall.txt"}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    ssg::test::GridTestView grid{dims};
    auto firstRow = [&] {
        auto snap = grid.present(runtime);
        return snap ? snap->viewport.firstVisualRow : 0U;
    };
    ASSERT_EQ(firstRow(), 0U);

    // Type a character (caret at top), then scroll away and UNDO: undo reveals.
    ASSERT_TRUE(ssg::test::typeText(runtime, "x").accepted());
    ASSERT_TRUE(grid.input(runtime, ssg::ScrollLinesInput{
                                       {ssg::ScrollTarget::Document, 40}}).accepted());
    ASSERT_EQ(firstRow(), 40U);
    ASSERT_TRUE(runtime.dispatch("edit.undo").accepted());
    ASSERT_EQ(firstRow(), 0U);

    // Copy a line, collapse the caret to the top, scroll away, and PASTE: the
    // paste inserts a duplicate (a real document mutation) and reveals the caret.
    // (Pasting over the same selection would reproduce identical bytes — a no-op
    // that correctly does not mutate or reveal.)
    ASSERT_TRUE(runtime.dispatch("select.line_down").accepted());
    ASSERT_TRUE(runtime.dispatch("clipboard.copy").accepted());
    ASSERT_TRUE(ssg::test::setSelections(runtime, {{0, 0}}).accepted());
    ASSERT_TRUE(grid.input(runtime, ssg::ScrollLinesInput{
                                       {ssg::ScrollTarget::Document, 40}}).accepted());
    ASSERT_EQ(firstRow(), 40U);
    ASSERT_TRUE(runtime.dispatch("clipboard.paste").accepted());
    // The pasted "a\n" pushes the caret to line 1; revealing from row 40 scrolls
    // up so the caret's row sits at the viewport top (first_row == its row).
    ASSERT_EQ(firstRow(), 1U);
    std::filesystem::remove_all(root);
}

TEST(multiCursorPastePreservesAllCursors) {
    auto root = testRuntimePath("runtime_editing_mcpaste");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "m.txt", std::ios::binary} << "aaa\nbbb\nccc\n";
    auto created = ssg::createEditor({root / "workspace", root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"m.txt"}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    auto selectionCount = [&] {
        auto snap = projectFrame(runtime, dims);
        return snap ? snap->selections.items().size() : std::size_t{0};
    };

    // Build two cursors (top of line 0 and top of line 1), copy, then paste. The
    // paste must not collapse the multi-cursor set to a single caret.
    auto doc = ssg::test::activeDocumentText(runtime);
    auto p0 = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{0});
    auto p1 = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{4});
    ASSERT_TRUE(p0.has_value() && p1.has_value());
    ASSERT_TRUE(ssg::test::setSelections(
        runtime,
        {{p0->byteOffset.value(), p0->byteOffset.value()},
         {p1->byteOffset.value(), p1->byteOffset.value()}}).accepted());
    ASSERT_EQ(selectionCount(), std::size_t{2});
    ASSERT_TRUE(runtime.dispatch("select.line_end").accepted());
    ASSERT_TRUE(runtime.dispatch("clipboard.copy").accepted());
    ASSERT_TRUE(runtime.dispatch("clipboard.paste").accepted());
    ASSERT_EQ(selectionCount(), std::size_t{2});
    std::filesystem::remove_all(root);
}

TEST(multiCursorTypingReplacesEachSelectionAndKeepsAllCursors) {
    auto root = testRuntimePath("runtime_editing_mctype");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "m.txt", std::ios::binary}
        << "aaa\nbbb\nccc\n";
    auto created = ssg::createEditor(
        {root / "workspace", root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"m.txt"}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    auto selectionCount = [&] {
        auto snap = projectFrame(runtime, dims);
        return snap ? snap->selections.items().size()
                    : std::size_t{0};
    };

    // Two RANGE selections over "aaa" and "bbb" (as Alt+d would build over a
    // repeated word).
    auto doc = ssg::test::activeDocumentText(runtime);
    auto p0 = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{0});
    auto p3 = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{3});
    auto p4 = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{4});
    auto p7 = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{7});
    ASSERT_TRUE(p0 && p3 && p4 && p7);
    ASSERT_TRUE(ssg::test::setSelections(
        runtime,
        {{p0->byteOffset.value(), p3->byteOffset.value()},
         {p4->byteOffset.value(), p7->byteOffset.value()}}).accepted());
    ASSERT_EQ(selectionCount(), std::size_t{2});

    // Typing replaces EACH selection and leaves a caret at each edit -- the
    // multi-cursor must survive (regression: it used to collapse to one).
    ASSERT_TRUE(ssg::test::typeText(runtime, "X").accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"X\nX\nccc\n"});
    ASSERT_EQ(selectionCount(), std::size_t{2});

    // Continuing to type inserts at BOTH carets, so multi-cursor editing works
    // across successive keystrokes.
    ASSERT_TRUE(ssg::test::typeText(runtime, "Y").accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"XY\nXY\nccc\n"});
    ASSERT_EQ(selectionCount(), std::size_t{2});

    // Undo and redo across the multi-cursor edits keep all cursors too (the
    // bindHistory clamp must preserve the set, not collapse it).
    ASSERT_TRUE(runtime.dispatch("edit.undo").accepted());
    ASSERT_EQ(selectionCount(), std::size_t{2});
    ASSERT_TRUE(runtime.dispatch("edit.redo").accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"XY\nXY\nccc\n"});
    ASSERT_EQ(selectionCount(), std::size_t{2});
    std::filesystem::remove_all(root);
}

TEST(replaceAllRevealsTheCaretWhenNoMatchRemains) {
    auto root = testRuntimePath("runtime_editing_replacereveal");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "recovery");
    // A tall doc with the only match near the bottom.
    std::string text;
    for (int i = 0; i < 90; ++i) text += "filler\n";
    text += "needle\n";
    std::ofstream{root / "workspace" / "t.txt", std::ios::binary} << text;
    auto created = ssg::createEditor({root / "workspace", root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"t.txt"}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    ssg::test::GridTestView grid{dims};
    auto firstRow = [&] {
        auto snap = grid.present(runtime);
        return snap ? snap->viewport.firstVisualRow : 0U;
    };
    ASSERT_EQ(firstRow(), 0U);  // caret at top; the match is off-screen far below

    ASSERT_TRUE(runtime.dispatch("replace.open").accepted());
    ASSERT_TRUE(runtime.updateFindQuery(ssg::test::promptText("needle")).accepted());
    ASSERT_TRUE(runtime.updateReplacement(ssg::test::promptText("pin")).accepted());
    ASSERT_TRUE(runtime.dispatch("replace.all").accepted());
    // No match remains, but the caret (now at the replaced text near the bottom)
    // is revealed rather than left off-screen.
    ASSERT_TRUE(firstRow() > 0U);
    std::filesystem::remove_all(root);
}

TEST(promptCommandsFulfillFindReplaceByActiveKind) {
    auto root = testRuntimePath("runtime_editing_prompt_fulfillment");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "f.txt"} << "cat cat cat";
    auto created = ssg::createEditor(
        {root / "workspace", root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"f.txt"})
                    .accepted());

    ASSERT_TRUE(runtime.dispatch("find.open")
                    .accepted());
    ASSERT_TRUE(runtime.updateFindQuery(ssg::test::promptText("cat"))
                    .accepted());
    ASSERT_TRUE(runtime.dispatch("prompt.submit")
                    .accepted());
    auto findAfterSubmit =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(findAfterSubmit.has_value());
    if (findAfterSubmit) {
        ASSERT_TRUE(findAfterSubmit->findReplace.open);
        ASSERT_EQ(findAfterSubmit->findReplace.activeMatch,
                  std::optional<std::size_t>{1});
        ASSERT_TRUE(findAfterSubmit->prompt.activeKind.has_value());
        ASSERT_EQ(*findAfterSubmit->prompt.activeKind,
                  ssg::PromptKind::Find);
    }

    ASSERT_TRUE(runtime.dispatch("prompt.previous")
                    .accepted());
    auto findAfterPrevious =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(findAfterPrevious.has_value());
    if (findAfterPrevious) {
        ASSERT_EQ(findAfterPrevious->findReplace.activeMatch,
                  std::optional<std::size_t>{0});
    }

    ASSERT_TRUE(runtime.dispatch("replace.open")
                    .accepted());
    ASSERT_TRUE(runtime.updateReplacement(ssg::test::promptText("dog"))
                    .accepted());
    ASSERT_TRUE(runtime.dispatch("prompt.submit")
                    .accepted());
    ASSERT_EQ(ssg::test::activeDocumentText(runtime), std::string{"dog cat cat"});
    auto replaceAfterSubmit =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(replaceAfterSubmit.has_value());
    std::uint64_t generationBeforeCancel = 0;
    if (replaceAfterSubmit) {
        generationBeforeCancel = replaceAfterSubmit->findReplace.generation;
        ASSERT_TRUE(replaceAfterSubmit->prompt.activeKind.has_value());
        ASSERT_EQ(*replaceAfterSubmit->prompt.activeKind,
                  ssg::PromptKind::Replace);
    }

    ASSERT_TRUE(runtime.dispatch("prompt.cancel")
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
        ASSERT_FALSE(afterCancel->prompt.activeKind.has_value());
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
    return ssg::createEditor({workspace, root / "recovery", root / "archive"});
}
}  // namespace

TEST(findWordUnderCursorSeedsTheCaretWordAndFindsEveryOccurrence) {
    auto root = uniqueRoot();
    auto created = openWith(root, "w.txt", "alpha beta alpha");
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"w.txt"}).accepted());

    // The caret starts at offset 0, inside "alpha".
    ASSERT_TRUE(runtime.dispatch("find.word_under_cursor").accepted());
    auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& find = snapshot->findReplace;
    ASSERT_TRUE(find.open);
    ASSERT_EQ(find.query, std::string{"alpha"});
    ASSERT_EQ(find.matches.size(), std::size_t{2});
    ASSERT_FALSE(find.options.regex);
    ASSERT_EQ(snapshot->prompt.activeKind,
              std::optional{ssg::PromptKind::Find});
    // The query is live, not just prompt text: next moves to the second "alpha".
    ASSERT_TRUE(runtime.dispatch("find.next").accepted());
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
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"w.txt"}).accepted());
    // Move the caret to offset 5 -- the space, immediately past "alpha".
    ASSERT_TRUE(runtime.dispatch("cursor.word_right").accepted());

    ASSERT_TRUE(runtime.dispatch("find.word_under_cursor").accepted());
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
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"w.txt"}).accepted());
    // Select the first three bytes, "a.b", spanning a word boundary the caret
    // word would never include.
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(runtime.dispatch("select.right").accepted());
    }
    // Turn regex AND selection-only ON first; the command must force both off so
    // the seeded word is searched literally across the whole document.
    ASSERT_TRUE(runtime.dispatch("find.open").accepted());
    ASSERT_TRUE(runtime.dispatch("find.toggle_regex").accepted());
    ASSERT_TRUE(runtime.dispatch("find.toggle_selection").accepted());

    ASSERT_TRUE(runtime.dispatch("find.word_under_cursor").accepted());
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
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"w.txt"}).accepted());
    // Move the caret to offset 2, between the two spaces: no word on either side.
    ASSERT_TRUE(runtime.dispatch("cursor.right").accepted());
    ASSERT_TRUE(runtime.dispatch("cursor.right").accepted());

    // Reported success, but no find controller and no prompt were opened.
    ASSERT_TRUE(runtime.dispatch("find.word_under_cursor").accepted());
    auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_FALSE(snapshot->findReplace.open);
    ASSERT_FALSE(snapshot->prompt.activeKind.has_value());
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
        workspace, root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"hits.txt"}).accepted());

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
    ASSERT_TRUE(runtime.dispatch("palette.open").accepted());
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
    ASSERT_TRUE(runtime.dispatch("palette.close").accepted());
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
    ASSERT_TRUE(runtime.dispatch("find.open").accepted());
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

TEST(liveInlineDiffPointerSelectionCopiesOnlyCurrentContent) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor(
        {root / "workspace", root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::applyGitDiffScan(
                    runtime,
                    {.revision = std::uint64_t{90},
                     .baselineIdentity = "head-copy:index-1",
                     .files = {
                         {.id = ssg::DiffFileId{"inline-copy"},
                          .path = "inline.txt",
                          .baselineContent =
                              std::string{"gamma original line two\n"},
                          .workingContent =
                              std::string{"gamma modified line two\n"}}}})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch("panel.show_git_status").accepted());
    ASSERT_TRUE(runtime.dispatch("tree.select_next").accepted());
    ASSERT_TRUE(runtime.dispatch("tree.activate").accepted());

    auto frame = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    const auto projected = frame->viewport.projectedRow(0);
    const auto* row = std::get_if<ssg::RealRow>(&projected);
    ASSERT_TRUE(row != nullptr);
    if (!row) return;
    ASSERT_FALSE(row->mergedSegments.empty());

    const auto targetAt = [&](std::uint32_t byteOffset) {
        return std::find_if(
            frame->viewport.hitTargets.begin(), frame->viewport.hitTargets.end(),
            [&](const ssg::CellHitTarget& target) {
                return !target.ghost && target.byteOffset == byteOffset;
            });
    };
    const auto start = targetAt(6);
    const auto end = targetAt(14);
    ASSERT_TRUE(start != frame->viewport.hitTargets.end());
    ASSERT_TRUE(end != frame->viewport.hitTargets.end());
    if (start == frame->viewport.hitTargets.end() ||
        end == frame->viewport.hitTargets.end()) {
        return;
    }
    ASSERT_TRUE(start->viewportColumn > start->byteOffset);
    ASSERT_TRUE(std::any_of(
        frame->viewport.hitTargets.begin(), frame->viewport.hitTargets.end(),
        [&](const ssg::CellHitTarget& target) {
            return target.ghost &&
                   target.viewportColumn < start->viewportColumn;
        }));

    auto press =
        runtime.input(ssg::DocumentPointerInput{ssg::ByteOffset{start->byteOffset}});
    ASSERT_TRUE(press.command && press.command->accepted());
    auto move = runtime.input(ssg::DocumentPointerInput{
        ssg::ByteOffset{end->byteOffset}, false, false,
        ssg::InputPointerButton::Primary, ssg::InputPointerPhase::Move});
    ASSERT_TRUE(move.command && move.command->accepted());
    frame = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    ASSERT_EQ(frame->selections.primary().anchor.byteOffset,
              ssg::ByteOffset{6});
    ASSERT_EQ(frame->selections.primary().active.byteOffset,
              ssg::ByteOffset{14});

    ASSERT_TRUE(runtime.dispatch("clipboard.copy").accepted());
    ASSERT_EQ(runtime.clipboard.plainText(), std::string{"modified"});
    ASSERT_TRUE(runtime.clipboard.plainText().find("original") ==
                std::string::npos);
    std::filesystem::remove_all(root);
}

} // namespace

SSG_TEST_SUITE(test_session_editing) {
    RUN(runtimeTextSelectionAndHistoryMatchFeatureOperations);
    RUN(typingUndoBreaksOnWordAndLineBoundaries);
    RUN(searchPanelEditsSubmitsPublishesAndCancelsWithoutEagerWork);
    RUN(searchPanelActivatesTheSelectedResultAtItsMatchColumn);
    RUN(searchPanelPointerActivationRevealsTheMatch);
    RUN(pointerSelectionCommandsFocusTheEditorKeyboardMotionDoesNot);
    RUN(panelFocusShortcutTogglesBetweenPanelAndEditor);
    RUN(panelWidthCommandsResizeAndClampTheSidebar);
    RUN(tabKeyInsertsATabInTheEditor);
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
    RUN(liveInlineDiffPointerSelectionCopiesOnlyCurrentContent);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
