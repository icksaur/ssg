#include "ssg/TreeModel.h"
#include "test_helpers.h"

#include "../src/tree_model_detail.h"

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

TEST(filesystemSnapshotIgnoresAnEntryThatDisappearsDuringInspection) {
    TemporaryDirectory temporary;
    const auto transient = temporary.path() / "transient.txt";
    std::ofstream(transient) << "temporary";
    const std::filesystem::directory_entry entry{transient};
    std::filesystem::remove(transient);

    ASSERT_FALSE(ssg::detail::inspectFilesystemTreeEntry(
                     TreeProviderId{"files"}, temporary.path(), entry)
                     .has_value());
    ASSERT_TRUE(ssg::detail::filesystemTreeEntryDisappeared(
        std::make_error_code(std::errc::no_such_file_or_directory)));
    ASSERT_FALSE(ssg::detail::filesystemTreeEntryDisappeared(
        std::make_error_code(std::errc::permission_denied)));
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
    ASSERT_TRUE(git.nodes()[0].gitStatus.has_value());
    ASSERT_EQ(git.nodes()[0].gitStatus->shortLabel, std::string{"A"});
    ASSERT_EQ(git.nodes()[0].gitStatus->role, SemanticRole::DiffAdded);
    ASSERT_TRUE(git.nodes()[1].gitStatus.has_value());
    ASSERT_EQ(git.nodes()[1].gitStatus->shortLabel, std::string{"M"});
    ASSERT_EQ(git.nodes()[1].gitStatus->role, SemanticRole::DiffModified);

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

TEST(nodeCommandInvocationIsProviderDataOnly) {
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

TEST(treeDeltaPublishesActiveBindingAndPreservesCompatibilityOrder) {
    TemporaryDirectory directory;
    TreeModel model;
    model.replaceProvider(TreeProviderSnapshot::fromFilesystem(
        TreeProviderId{"filesystem"}, directory.path(), TreeRevision{1}));
    model.replaceProvider(TreeProviderSnapshot::fromGit(
        TreeProviderId{"git"}, TreeRevision{2},
        {{.workspacePath = "changed.txt", .label = "changed.txt",
          .status = GitTreeStatus::Modified}}));
    const auto base = model.viewState();
    ASSERT_TRUE((base.activeBinding ==
                 TreeProviderBinding{TreeProviderId{"filesystem"},
                                     TreeProviderKind::Filesystem}));
    ASSERT_EQ(base.providers.front().providerId, TreeProviderId{"filesystem"});

    ASSERT_TRUE(model.activateProvider(TreeProviderId{"git"}));
    const auto target = model.viewState();
    ASSERT_TRUE((target.activeBinding ==
                 TreeProviderBinding{TreeProviderId{"git"},
                                     TreeProviderKind::Git}));
    ASSERT_EQ(target.providers.front().providerId, TreeProviderId{"git"});

    const auto delta = TreeDeltaCodec{}.derive(base, target, 8);
    ASSERT_FALSE(delta.snapshotRequired);
    const auto replay = TreeDeltaCodec{}.replay(base, delta);
    ASSERT_TRUE(replay.accepted());
    ASSERT_EQ(*replay.state, target);

    auto legacyDelta = delta;
    legacyDelta.activeBinding.reset();
    const auto legacyReplay = TreeDeltaCodec{}.replay(base, legacyDelta);
    ASSERT_TRUE(legacyReplay.accepted());
    ASSERT_EQ(*legacyReplay.state, target);
}

TEST(treeViewStateRejectsMissingMismatchedAndDuplicateActiveBindings) {
    TreeModel model;
    model.replaceProvider(TreeProviderSnapshot::fromSymbols(
        TreeProviderId{"symbols"}, TreeRevision{1},
        {{.stableKey = "A", .label = "A"}}));
    const auto valid = model.viewState();
    ASSERT_TRUE(isValidTreeViewState(valid));
    ASSERT_TRUE(activeTreeProvider(valid) != nullptr);

    auto missing = valid;
    missing.activeBinding.reset();
    ASSERT_FALSE(isValidTreeViewState(missing));

    auto mismatched = valid;
    mismatched.activeBinding->kind = TreeProviderKind::Git;
    ASSERT_FALSE(isValidTreeViewState(mismatched));

    auto duplicate = valid;
    duplicate.providers.push_back(duplicate.providers.front());
    ASSERT_FALSE(isValidTreeViewState(duplicate));

    auto malformedDelta = TreeDeltaCodec{}.derive(valid, valid, 8);
    malformedDelta.activeBinding =
        TreeProviderBinding{TreeProviderId{"missing"}, TreeProviderKind::Symbols};
    const auto replay = TreeDeltaCodec{}.replay(valid, malformedDelta);
    ASSERT_FALSE(replay.accepted());
    ASSERT_EQ(replay.error, TreeReplayError::MalformedDelta);
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

TEST(activateOrCreateLazilyCreatesGitAndSymbolsButNeverFilesystem) {
    TemporaryDirectory directory;
    TreeModel model;
    model.replaceProvider(TreeProviderSnapshot::fromFilesystem(
        TreeProviderId{"filesystem"}, directory.path(), TreeRevision{1}));

    // Counts how often the model asks for a create-revision, so the test can pin
    // that the source is consumed ONLY on the create path.
    int revisionRequests = 0;
    auto revision = [&](std::uint64_t value) {
        return [&revisionRequests, value] {
            ++revisionRequests;
            return TreeRevision{value};
        };
    };

    // A git binding with no git provider yet: created at the supplied revision,
    // activated, and reported as the active provider.
    ASSERT_TRUE(model.activateOrCreate(
        TreeProviderBinding{TreeProviderId{"git"}, TreeProviderKind::Git},
        revision(7)));
    ASSERT_EQ(revisionRequests, 1);
    {
        const auto view = model.viewState();
        const auto& active = view.providers.front();
        ASSERT_TRUE(active.providerId == TreeProviderId{"git"});
        ASSERT_TRUE(active.kind == TreeProviderKind::Git);
    }

    // A symbols binding: the same lazy-create path for the second creatable kind.
    ASSERT_TRUE(model.activateOrCreate(
        TreeProviderBinding{TreeProviderId{"symbols"},
                            TreeProviderKind::Symbols},
        revision(9)));
    ASSERT_EQ(revisionRequests, 2);
    {
        const auto view = model.viewState();
        const auto& active = view.providers.front();
        ASSERT_TRUE(active.providerId == TreeProviderId{"symbols"});
        ASSERT_TRUE(active.kind == TreeProviderKind::Symbols);
    }

    // Re-activating an existing provider is a plain activate: it becomes active
    // again, the view revision does NOT advance a second time (no spurious
    // replace), and the create-revision source is NOT consumed.
    ASSERT_TRUE(model.activateOrCreate(
        TreeProviderBinding{TreeProviderId{"git"}, TreeProviderKind::Git},
        revision(99)));
    const auto afterFirst = model.viewState().revision;
    const auto requestsAfterFirst = revisionRequests;
    ASSERT_TRUE(model.activateOrCreate(
        TreeProviderBinding{TreeProviderId{"git"}, TreeProviderKind::Git},
        revision(99)));
    ASSERT_TRUE(model.viewState().revision == afterFirst);
    ASSERT_EQ(revisionRequests, requestsAfterFirst);
    ASSERT_TRUE(model.viewState().providers.front().providerId ==
                TreeProviderId{"git"});

    ASSERT_FALSE(model.activateOrCreate(
        TreeProviderBinding{TreeProviderId{"git"}, TreeProviderKind::Symbols},
        revision(101)));
    ASSERT_EQ(revisionRequests, requestsAfterFirst);
    ASSERT_TRUE((model.activeProviderBinding() ==
                 TreeProviderBinding{TreeProviderId{"git"},
                                     TreeProviderKind::Git}));

    // A filesystem binding is NEVER created here (it is seeded at construction);
    // a missing one is a genuine failure that creates nothing and asks for no
    // revision.
    TreeModel empty;
    int emptyRequests = 0;
    ASSERT_FALSE(empty.activateOrCreate(
        TreeProviderBinding{TreeProviderId{"filesystem"},
                            TreeProviderKind::Filesystem},
        [&emptyRequests] {
            ++emptyRequests;
            return TreeRevision{3};
        }));
    ASSERT_TRUE(empty.viewState().providers.empty());
    ASSERT_EQ(emptyRequests, 0);

    // An empty revision source is a misuse that fails loudly with a descriptive
    // error, not an opaque std::bad_function_call on the create path.
    ASSERT_THROWS(
        model.activateOrCreate(
            TreeProviderBinding{TreeProviderId{"git"}, TreeProviderKind::Git},
            std::function<TreeRevision()>{}),
        std::invalid_argument);
}

// The memoization invariant: visibility is a pure function of (snapshot,
// expanded). It must recompute when the snapshot revision changes OR the
// expanded set changes, and must NOT recompute for navigation (selection) that
// changes neither. Keying on the snapshot revision alone would miss the
// expand/collapse case, since toggleExpanded does not change that revision --
// this test names that load-bearing half.
TEST(visibleNodesRecomputesOnlyOnRevisionOrExpandedChangeNeverOnNavigation) {
    TreeModel model;
    model.replaceProvider(TreeProviderSnapshot::fromSymbols(
        TreeProviderId{"symbols"}, TreeRevision{1},
        {{.stableKey = "type/A", .label = "A"},
         {.stableKey = "type/A/member", .parentKey = "type/A",
          .label = "member"},
         {.stableKey = "type/Z", .label = "Z"}}));

    // A repeated identical viewState() (nothing changed) adds no recompute:
    // the second call is served entirely from cache.
    TreeModel::resetVisibleNodesRecomputeCount();
    (void)model.viewState();
    const auto afterFirst = TreeModel::visibleNodesRecomputeCount();
    (void)model.viewState();
    ASSERT_EQ(TreeModel::visibleNodesRecomputeCount(), afterFirst);

    // Navigation changes neither the tree revision nor the expanded set, so it
    // adds no recompute on top of an already-warm cache.
    (void)model.viewState();  // warm
    TreeModel::resetVisibleNodesRecomputeCount();
    ASSERT_TRUE(model.selectNext());
    (void)model.viewState();
    ASSERT_EQ(TreeModel::visibleNodesRecomputeCount(), std::uint64_t{0});

    // Expand/collapse mutates the expanded set but NOT the snapshot revision;
    // keying on the revision alone would wrongly serve the stale cache here.
    TreeModel::resetVisibleNodesRecomputeCount();
    ASSERT_TRUE(model.toggleExpanded(TreeProviderId{"symbols"},
                                     TreeNodeId{"symbols:type/A"}));
    (void)model.viewState();
    ASSERT_TRUE(TreeModel::visibleNodesRecomputeCount() >= std::uint64_t{1});

    // A provider replacement bumps the snapshot revision.
    TreeModel::resetVisibleNodesRecomputeCount();
    model.replaceProvider(TreeProviderSnapshot::fromSymbols(
        TreeProviderId{"symbols"}, TreeRevision{2},
        {{.stableKey = "type/A", .label = "renamed A"}}));
    (void)model.viewState();
    ASSERT_TRUE(TreeModel::visibleNodesRecomputeCount() >= std::uint64_t{1});
}

SSG_TEST_SUITE(test_tree) {
    RUN(filesystemSnapshotIsStableSortedAndDoesNotFollowSymlinks);
    RUN(filesystemSnapshotIgnoresAnEntryThatDisappearsDuringInspection);
    RUN(gitAndSymbolSnapshotsAreDeterministicAndUseStableKeys);
    RUN(expansionSurvivesRefreshByIdentityAndDisappearingNodesArePruned);
    RUN(nodeCommandInvocationIsProviderDataOnly);
    RUN(selectionNavigatesExpandsAndReportsSelectedNode);
    RUN(selectByIdSetsVisibleSelectionAndRejectsUnknownOrHiddenNodes);
    RUN(boundedDeltaReplaysToIndependentViewAndRejectsStaleBase);
    RUN(treeDeltaPublishesActiveBindingAndPreservesCompatibilityOrder);
    RUN(treeViewStateRejectsMissingMismatchedAndDuplicateActiveBindings);
    RUN(overBudgetDeltaRequiresSnapshotWithoutPartialOperations);
    RUN(activateOrCreateLazilyCreatesGitAndSymbolsButNeverFilesystem);
    RUN(visibleNodesRecomputesOnlyOnRevisionOrExpandedChangeNeverOnNavigation);
    return failed == 0 ? 0 : 1;
}
