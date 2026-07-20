#include "ssg/TreeModel.h"
#include "test_helpers.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace ssg;

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("ssg-tree-" + std::to_string(++sequence))) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

    const std::filesystem::path& path() const { return path_; }

private:
    inline static unsigned sequence = 0;
    std::filesystem::path path_;
};

std::vector<std::string> nodeIds(const TreeProviderSnapshot& snapshot) {
    std::vector<std::string> result;
    for (const auto& node : snapshot.nodes()) {
        result.push_back(node.id.value());
    }
    return result;
}

const TreeProviderView& onlyProvider(const TreeViewState& state) {
    ASSERT_EQ(state.providers.size(), std::size_t{1});
    return state.providers.front();
}

TEST(filesystemSnapshotIsStableSortedAndDoesNotFollowSymlinks) {
    TemporaryDirectory temporary;
    std::filesystem::create_directories(temporary.path() / "z-dir");
    std::filesystem::create_directories(temporary.path() / "a-dir");
    std::ofstream(temporary.path() / "z-dir" / "child.txt") << "child";
    std::ofstream(temporary.path() / "b.txt") << "b";
    std::ofstream(temporary.path() / "a.txt") << "a";
    std::error_code symlinkError;
    std::filesystem::create_directory_symlink(
        temporary.path() / "z-dir", temporary.path() / "a-link", symlinkError);

    const auto first = TreeProviderSnapshot::fromFilesystem(
        TreeProviderId{"files"}, temporary.path(), TreeRevision{1});
    std::filesystem::rename(temporary.path() / "a.txt",
                            temporary.path() / "renamed.txt");
    const auto second = TreeProviderSnapshot::fromFilesystem(
        TreeProviderId{"files"}, temporary.path(), TreeRevision{2});

    std::vector<std::string> expected{
        "files:.", "files:a-dir", "files:a-link", "files:a.txt", "files:b.txt",
        "files:z-dir", "files:z-dir/child.txt"};
    if (symlinkError) {
        expected.erase(expected.begin() + 2);
    }
    const auto firstIds = nodeIds(first);
    const auto secondIds = nodeIds(second);
    ASSERT_EQ(firstIds, expected);
    ASSERT_TRUE(std::find(firstIds.begin(), firstIds.end(),
                          "files:a-link/child.txt") == firstIds.end());
    ASSERT_TRUE(std::find(secondIds.begin(), secondIds.end(),
                          "files:a.txt") == secondIds.end());
    ASSERT_TRUE(std::find(secondIds.begin(), secondIds.end(),
                          "files:renamed.txt") != secondIds.end());
    ASSERT_EQ(first.nodes().front().id, second.nodes().front().id);
}

TEST(gitAndSymbolSnapshotsAreDeterministicAndUseStableKeys) {
    const auto git = TreeProviderSnapshot::fromGit(
        TreeProviderId{"git"}, TreeRevision{7},
        {{.workspacePath = "z.cpp", .label = "renamed label",
          .status = GitTreeStatus::Modified},
         {.workspacePath = "a.cpp", .label = "a.cpp",
          .status = GitTreeStatus::Added}});
    ASSERT_EQ(nodeIds(git),
              (std::vector<std::string>{"git:a.cpp", "git:z.cpp"}));
    ASSERT_EQ(git.nodes()[1].id, TreeNodeId{"git:z.cpp"});

    const auto symbols = TreeProviderSnapshot::fromSymbols(
        TreeProviderId{"symbols"}, TreeRevision{8},
        {{.stableKey = "type/Z", .label = "renamed Z"},
         {.stableKey = "type/A", .label = "A"},
         {.stableKey = "type/A/member", .parentKey = "type/A",
          .label = "member"}});
    ASSERT_EQ(nodeIds(symbols),
              (std::vector<std::string>{"symbols:type/A",
                                        "symbols:type/A/member",
                                        "symbols:type/Z"}));
}

TEST(expansionSurvivesRefreshByIdentityAndDisappearingNodesArePruned) {
    TreeModel model;
    model.replaceProvider(TreeProviderSnapshot::fromSymbols(
        TreeProviderId{"symbols"}, TreeRevision{1},
        {{.stableKey = "type/A", .label = "A"},
         {.stableKey = "type/A/member", .parentKey = "type/A",
          .label = "member"}}));

    ASSERT_TRUE(model.toggleExpanded(TreeProviderId{"symbols"},
                                      TreeNodeId{"symbols:type/A"}));
    ASSERT_EQ(onlyProvider(model.viewState()).nodes.size(), std::size_t{2});

    model.replaceProvider(TreeProviderSnapshot::fromSymbols(
        TreeProviderId{"symbols"}, TreeRevision{2},
        {{.stableKey = "type/A", .label = "renamed A"},
         {.stableKey = "type/A/member", .parentKey = "type/A",
          .label = "renamed member"}}));
    ASSERT_TRUE(onlyProvider(model.viewState()).nodes.front().expanded);
    ASSERT_EQ(onlyProvider(model.viewState()).nodes.size(), std::size_t{2});

    model.replaceProvider(TreeProviderSnapshot::fromSymbols(
        TreeProviderId{"symbols"}, TreeRevision{3},
        {{.stableKey = "type/B", .label = "B"}}));
    ASSERT_FALSE(model.isExpanded(TreeProviderId{"symbols"},
                                   TreeNodeId{"symbols:type/A"}));
}

TEST(commandSetIsExactAndInvocationIsProviderDataOnly) {
    const auto commands = treeCommandSet();
    ASSERT_EQ(commands.descriptors().size(), std::size_t{7});
    ASSERT_EQ(commands.descriptors()[0].id, std::string_view{"tree.toggle_expanded"});
    ASSERT_EQ(commands.descriptors()[1].id,
              std::string_view{"tree.invoke_node_command"});
    ASSERT_EQ(commands.descriptors()[2].id, std::string_view{"tree.select"});
    ASSERT_EQ(commands.descriptors()[3].id, std::string_view{"tree.select_next"});
    ASSERT_EQ(commands.descriptors()[4].id,
              std::string_view{"tree.select_previous"});
    ASSERT_EQ(commands.descriptors()[5].id, std::string_view{"tree.activate"});
    ASSERT_EQ(commands.descriptors()[6].id, std::string_view{"tree.scroll"});

    TreeModel model;
    model.replaceProvider(TreeProviderSnapshot::fromSymbols(
        TreeProviderId{"symbols"}, TreeRevision{1},
        {{.stableKey = "type/A",
          .label = "A",
          .commands = {{.id = "symbol.open", .label = "Open symbol"}}}}));
    const auto invocation = model.invokeNodeCommand(
        TreeProviderId{"symbols"}, TreeNodeId{"symbols:type/A"}, "symbol.open");
    ASSERT_TRUE(invocation.has_value());
    ASSERT_EQ(invocation->commandId, std::string{"symbol.open"});
    ASSERT_FALSE(model.invokeNodeCommand(
        TreeProviderId{"symbols"}, TreeNodeId{"symbols:type/A"}, "missing")
                     .has_value());
}

TEST(selectionNavigatesExpandsAndReportsSelectedNode) {
    TreeModel model;
    model.replaceProvider(TreeProviderSnapshot::fromSymbols(
        TreeProviderId{"symbols"}, TreeRevision{1},
        {{.stableKey = "A", .label = "A"},
         {.stableKey = "A/one", .parentKey = "A", .label = "one"},
         {.stableKey = "B", .label = "B"}}));

    // Populating the provider auto-selects its first visible node.
    auto selected = model.selectedNode();
    ASSERT_TRUE(selected.has_value());
    if (selected) ASSERT_EQ(selected->id, TreeNodeId{"symbols:A"});

    // Only roots A and B are visible while A is collapsed; next selects B.
    ASSERT_TRUE(model.selectNext());
    selected = model.selectedNode();
    if (selected) ASSERT_EQ(selected->id, TreeNodeId{"symbols:B"});

    // At the last visible node, next clamps.
    ASSERT_TRUE(model.selectNext());
    selected = model.selectedNode();
    if (selected) ASSERT_EQ(selected->id, TreeNodeId{"symbols:B"});

    // Return to A and expand it, revealing its child.
    ASSERT_TRUE(model.selectPrevious());
    selected = model.selectedNode();
    if (selected) ASSERT_EQ(selected->id, TreeNodeId{"symbols:A"});
    ASSERT_TRUE(model.toggleSelected());
    ASSERT_TRUE(model.selectNext());
    selected = model.selectedNode();
    if (selected) ASSERT_EQ(selected->id, TreeNodeId{"symbols:A/one"});

    // The selection is exposed on the provider view.
    const auto view = model.viewState();
    ASSERT_FALSE(view.providers.empty());
    if (!view.providers.empty()) {
        ASSERT_TRUE(view.providers.front().selected.has_value());
        if (view.providers.front().selected) {
            ASSERT_EQ(*view.providers.front().selected,
                      TreeNodeId{"symbols:A/one"});
        }
    }
}

TEST(selectByIdSetsVisibleSelectionAndRejectsUnknownOrHiddenNodes) {
    TreeModel model;
    model.replaceProvider(TreeProviderSnapshot::fromSymbols(
        TreeProviderId{"symbols"}, TreeRevision{1},
        {{.stableKey = "A", .label = "A"},
         {.stableKey = "A/one", .parentKey = "A", .label = "one"},
         {.stableKey = "B", .label = "B"}}));

    // A is auto-selected; select B directly by id.
    ASSERT_TRUE(model.select(TreeNodeId{"symbols:B"}));
    auto selected = model.selectedNode();
    ASSERT_TRUE(selected.has_value());
    if (selected) ASSERT_EQ(selected->id, TreeNodeId{"symbols:B"});

    // An id that is not a node at all is rejected, leaving the selection intact.
    ASSERT_FALSE(model.select(TreeNodeId{"symbols:missing"}));
    selected = model.selectedNode();
    if (selected) ASSERT_EQ(selected->id, TreeNodeId{"symbols:B"});

    // A/one is hidden while A is collapsed, so selecting it is rejected.
    ASSERT_FALSE(model.select(TreeNodeId{"symbols:A/one"}));
    // Expand A, then it becomes selectable.
    ASSERT_TRUE(model.select(TreeNodeId{"symbols:A"}));
    ASSERT_TRUE(model.toggleSelected());
    ASSERT_TRUE(model.select(TreeNodeId{"symbols:A/one"}));
    selected = model.selectedNode();
    if (selected) ASSERT_EQ(selected->id, TreeNodeId{"symbols:A/one"});
}

TEST(boundedDeltaReplaysToIndependentViewAndRejectsStaleBase) {
    TreeModel model;
    model.replaceProvider(TreeProviderSnapshot::fromSymbols(
        TreeProviderId{"symbols"}, TreeRevision{1},
        {{.stableKey = "A", .label = "A"},
         {.stableKey = "A/one", .parentKey = "A", .label = "one"}}));
    model.toggleExpanded(TreeProviderId{"symbols"}, TreeNodeId{"symbols:A"});
    const auto base = model.viewState();

    model.replaceProvider(TreeProviderSnapshot::fromSymbols(
        TreeProviderId{"symbols"}, TreeRevision{2},
        {{.stableKey = "A", .label = "A"},
         {.stableKey = "A/one", .parentKey = "A", .label = "renamed one"},
         {.stableKey = "A/two", .parentKey = "A", .label = "two"}}));
    const auto target = model.viewState();
    const auto delta = TreeDeltaCodec{}.derive(base, target, 8);
    ASSERT_FALSE(delta.snapshotRequired);
    ASSERT_TRUE(delta.operationCount() <= std::size_t{8});

    const auto replay = TreeDeltaCodec{}.replay(base, delta);
    ASSERT_TRUE(replay.accepted());
    ASSERT_EQ(*replay.state, target);

    auto stale = base;
    stale.revision = TreeRevision{base.revision.value() + 1};
    const auto staleReplay = TreeDeltaCodec{}.replay(stale, delta);
    ASSERT_FALSE(staleReplay.accepted());
    ASSERT_EQ(staleReplay.error, TreeReplayError::StaleRevision);
}

TEST(overBudgetDeltaRequiresSnapshotWithoutPartialOperations) {
    TreeModel model;
    model.replaceProvider(TreeProviderSnapshot::fromSymbols(
        TreeProviderId{"symbols"}, TreeRevision{1},
        {{.stableKey = "A", .label = "A"}}));
    const auto base = model.viewState();
    model.replaceProvider(TreeProviderSnapshot::fromSymbols(
        TreeProviderId{"symbols"}, TreeRevision{2},
        {{.stableKey = "A", .label = "A"},
         {.stableKey = "B", .label = "B"},
         {.stableKey = "C", .label = "C"}}));
    const auto target = model.viewState();

    const auto delta = TreeDeltaCodec{}.derive(base, target, 1);
    ASSERT_TRUE(delta.snapshotRequired);
    ASSERT_TRUE(delta.providers.empty());
    ASSERT_EQ(delta.operationCount(), std::size_t{0});
    const auto replay = TreeDeltaCodec{}.replay(base, delta);
    ASSERT_EQ(replay.error, TreeReplayError::SnapshotRequired);
}

} // namespace

int main() {
    RUN(filesystemSnapshotIsStableSortedAndDoesNotFollowSymlinks);
    RUN(gitAndSymbolSnapshotsAreDeterministicAndUseStableKeys);
    RUN(expansionSurvivesRefreshByIdentityAndDisappearingNodesArePruned);
    RUN(commandSetIsExactAndInvocationIsProviderDataOnly);
    RUN(selectionNavigatesExpandsAndReportsSelectedNode);
    RUN(selectByIdSetsVisibleSelectionAndRejectsUnknownOrHiddenNodes);
    RUN(boundedDeltaReplaysToIndependentViewAndRejectsStaleBase);
    RUN(overBudgetDeltaRequiresSnapshotWithoutPartialOperations);
    return failed == 0 ? 0 : 1;
}
