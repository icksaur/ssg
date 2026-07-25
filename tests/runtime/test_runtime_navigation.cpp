#include "../test_helpers.h"

#include <ssg/EditorRuntime.h>
#include <ssg/GraphemeLayout.h>
#include <ssg/TextInputCommands.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace {

std::filesystem::path uniqueRoot() {
    auto root = std::filesystem::current_path() / "runtime_navigation";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "needle.txt"} << "alpha needle omega";
    return root;
}

const ssg::TreeProviderView* findProvider(const ssg::TreeViewState& tree,
                                          ssg::TreeProviderKind kind) {
    for (const auto& provider : tree.providers) {
        if (provider.kind == kind) {
            return &provider;
        }
    }
    return nullptr;
}

std::map<std::string, ssg::GitTreeStatus> gitProviderStatuses(
    const ssg::TreeProviderView& provider) {
    std::map<std::string, ssg::GitTreeStatus> statuses;
    for (const auto& node : provider.nodes) {
        ASSERT_TRUE(node.node.workspacePath.has_value());
        ASSERT_TRUE(node.node.gitStatus.has_value());
        if (!node.node.workspacePath || !node.node.gitStatus) {
            continue;
        }
        statuses.emplace(*node.node.workspacePath, *node.node.gitStatus);
    }
    return statuses;
}

std::size_t countTabsOfKind(const ssg::TabViewState& tabs, ssg::TabKind kind) {
    return static_cast<std::size_t>(std::count_if(
        tabs.tabs.begin(), tabs.tabs.end(), [&](const ssg::TabState& tab) {
            return tab.kind == kind;
        }));
}

ssg::FollowMode followMode(ssg::EditorRuntime& runtime) {
    auto snapshot =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) {
        return ssg::FollowMode::Paused;
    }
    return snapshot->sections().followEdits.mode;
}

std::unique_ptr<ssg::EditorRuntime> followPauseRuntime(std::string text) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "needle.txt"} << text;
    std::ofstream{root / "workspace" / "other.txt"} << "other\n";
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) {
        return nullptr;
    }
    auto runtime = std::move(created.runtime);
    ASSERT_TRUE(runtime
                    ->attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                             ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    ->attach({ssg::ClientId{2}, ssg::InvocationOrigin::Lua},
                             ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    ->attach({ssg::ClientId{3}, ssg::InvocationOrigin::System},
                             ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"file.open", runtime->revision(),
                                std::string{"needle.txt"}})
                    .accepted());
    ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Following);
    return runtime;
}

TEST(searchTreeDiffAndFollowSectionsUseRuntimeState) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"search.workspace", runtime.revision(), std::string{"needle"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"follow_edits.pause", runtime.revision(), {}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_FALSE(snapshot->sections().search.results.empty());
    ASSERT_FALSE(snapshot->sections().tree.providers.empty());
    ASSERT_EQ(snapshot->sections().followEdits.mode, ssg::FollowMode::Paused);
}

TEST(externalDiffBurstRevealsOnlyNewestFileWithoutPausingFollow) {
    auto root = uniqueRoot();
    std::string middle;
    for (int line = 0; line < 40; ++line) {
        middle += "line " + std::to_string(line) + "\n";
    }

    std::ofstream{root / "workspace" / "a.txt"} << "a\n";
    std::ofstream{root / "workspace" / "b.txt"} << "b\n";
    std::ofstream{root / "workspace" / "c.txt"} << middle;

    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1},
                             ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .applyExternalDiffBurst(
                        {{{.kind = ssg::NonGitDiffEventKind::Create,
                           .id = ssg::DiffFileId{"a.txt"},
                           .path = "a.txt",
                           .baselineContent = "",
                           .targetContent = "a\n"},
                          ssg::Revision{1}},
                         {{.kind = ssg::NonGitDiffEventKind::Create,
                           .id = ssg::DiffFileId{"b.txt"},
                           .path = "b.txt",
                           .baselineContent = "",
                           .targetContent = "b\n"},
                          ssg::Revision{2}},
                         {{.kind = ssg::NonGitDiffEventKind::Create,
                           .id = ssg::DiffFileId{"c.txt"},
                           .path = "c.txt",
                           .baselineContent = "",
                           .targetContent = middle},
                          ssg::Revision{3}}})
                    .accepted());

    const ssg::ViewportDimensions dimensions{20, 6};
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, dimensions);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().document.diffFileIdentity,
              std::optional<std::string>{"c.txt"});
    ASSERT_EQ(snapshot->sections().followEdits.activeTarget->id,
              ssg::DiffFileId{"c.txt"});
    ASSERT_EQ(snapshot->sections().followEdits.mode, ssg::FollowMode::Following);

    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"cursor.line_up", runtime.revision(), {}})
                    .accepted());
    snapshot = runtime.snapshot(ssg::ClientId{1}, dimensions);
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) {
        ASSERT_EQ(snapshot->sections().followEdits.mode,
                  ssg::FollowMode::Paused);
    }
}

TEST(gitDiffScanUpdatesDiffAndRejectsStaleBatches) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "a.txt"} << "a\n";
    std::ofstream{root / "workspace" / "b.txt"} << "b\n";

    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());

    ssg::GitDiffScan scan{
        .revision = ssg::Revision{10},
        .baselineIdentity = "head-1:index-1",
        .files = {
            {.id = ssg::DiffFileId{"a.txt"},
             .path = "a.txt",
             .baselineContent = std::string{"a\n"},
             .workingContent = std::string{"a changed\n"}},
            {.id = ssg::DiffFileId{"b.txt"},
             .path = "b.txt",
             .baselineContent = std::string{"b\n"},
             .workingContent = std::string{"b changed\n"}},
        }};
    ASSERT_TRUE(runtime.applyGitDiffScan(scan).accepted());

    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().diff.files.size(), std::size_t{2});
    ASSERT_TRUE(snapshot->sections().followEdits.activeTarget.has_value());
    if (snapshot->sections().followEdits.activeTarget) {
        ASSERT_EQ(snapshot->sections().followEdits.activeTarget->id,
                  ssg::DiffFileId{"b.txt"});
    }

    ssg::GitDiffScan stale{
        .revision = ssg::Revision{10},
        .baselineIdentity = "head-1:index-2",
        .files = {{.id = ssg::DiffFileId{"a.txt"},
                   .path = "a.txt",
                   .baselineContent = std::string{"a\n"},
                   .workingContent = std::string{"a changed again\n"}}}};
    auto staleResult = runtime.applyGitDiffScan(std::move(stale));
    ASSERT_FALSE(staleResult.accepted());
    ASSERT_EQ(staleResult.error, ssg::GitDiffScanError::DiffRejected);
}

TEST(gitDiffSelectionUsesDiffIdentityIndependentOfDocumentRevision) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"needle.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"follow_edits.pause", runtime.revision(), {}})
                    .accepted());

    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = ssg::Revision{20},
                         .baselineIdentity = "head-2:index-1",
                         .files = {{.id = ssg::DiffFileId{"needle.txt"},
                                    .path = "needle.txt",
                                    .baselineContent =
                                        std::string{"alpha needle omega"},
                                    .workingContent =
                                        std::string{"alpha NEEDLE omega"}}}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"text.insert", runtime.revision(),
                               ssg::TextInputArguments{"!"}})
                    .accepted());

    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().document.diffFileIdentity, std::nullopt);
    ASSERT_NE(snapshot->sections().document.revision,
              snapshot->sections().diff.revision);
    const auto byIdentity = std::find_if(
        snapshot->sections().diff.files.begin(),
        snapshot->sections().diff.files.end(),
        [](const ssg::DiffFileView& file) {
            return file.id == ssg::DiffFileId{"needle.txt"};
        });
    ASSERT_TRUE(byIdentity != snapshot->sections().diff.files.end());
}

TEST(gitDiffScanRefreshesGitTreeProviderFromDiffAndOnSecondScan) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());

    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = ssg::Revision{20},
                         .baselineIdentity = "head-1:index-0",
                         .files = {}})
                    .accepted());
    auto emptyFirst =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(emptyFirst.has_value());
    if (!emptyFirst) return;
    auto* emptyFirstGit =
        findProvider(emptyFirst->sections().tree, ssg::TreeProviderKind::Git);
    ASSERT_TRUE(emptyFirstGit != nullptr);
    if (!emptyFirstGit) return;
    ASSERT_TRUE(gitProviderStatuses(*emptyFirstGit).empty());
    const std::uint64_t emptyFirstRevision =
        emptyFirst->sections().tree.revision.value();

    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = ssg::Revision{21},
                         .baselineIdentity = "head-1:index-0",
                         .files = {}})
                    .accepted());
    auto emptySecond =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(emptySecond.has_value());
    if (!emptySecond) return;
    auto* emptySecondGit =
        findProvider(emptySecond->sections().tree, ssg::TreeProviderKind::Git);
    ASSERT_TRUE(emptySecondGit != nullptr);
    if (!emptySecondGit) return;
    ASSERT_TRUE(gitProviderStatuses(*emptySecondGit).empty());
    ASSERT_TRUE(emptySecond->sections().tree.revision.value() >
                emptyFirstRevision);

    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = ssg::Revision{22},
                         .baselineIdentity = "head-1:index-1",
                         .files =
                             {
                                 {.id = ssg::DiffFileId{"added.txt"},
                                  .path = "added.txt",
                                  .baselineContent = std::nullopt,
                                  .workingContent = std::string{"added\n"}},
                                 {.id = ssg::DiffFileId{"modified.txt"},
                                  .path = "modified.txt",
                                  .baselineContent = std::string{"before\n"},
                                  .workingContent = std::string{"after\n"}},
                                 {.id = ssg::DiffFileId{"deleted.txt"},
                                  .path = "deleted.txt",
                                  .baselineContent = std::string{"gone\n"},
                                  .workingContent = std::nullopt},
                                 {.id = ssg::DiffFileId{"renamed.txt"},
                                  .path = "renamed.txt",
                                  .previousPath = std::filesystem::path{"old-name.txt"},
                                  .baselineContent = std::string{"same\n"},
                                  .workingContent = std::string{"same\n"}},
                             }})
                    .accepted());

    auto first = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(first.has_value());
    if (!first) return;
    auto* firstGit =
        findProvider(first->sections().tree, ssg::TreeProviderKind::Git);
    ASSERT_TRUE(firstGit != nullptr);
    if (!firstGit) return;

    const auto firstStatuses = gitProviderStatuses(*firstGit);
    ASSERT_EQ(firstStatuses.size(), std::size_t{4});
    ASSERT_EQ(firstStatuses.at("added.txt"), ssg::GitTreeStatus::Added);
    ASSERT_EQ(firstStatuses.at("modified.txt"), ssg::GitTreeStatus::Modified);
    ASSERT_EQ(firstStatuses.at("deleted.txt"), ssg::GitTreeStatus::Deleted);
    ASSERT_EQ(firstStatuses.at("renamed.txt"), ssg::GitTreeStatus::Renamed);

    std::uint64_t firstRevision = first->sections().tree.revision.value();

    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = ssg::Revision{23},
                         .baselineIdentity = "head-1:index-2",
                         .files =
                             {
                                 {.id = ssg::DiffFileId{"modified.txt"},
                                  .path = "modified.txt",
                                  .baselineContent = std::string{"after\n"},
                                  .workingContent = std::string{"after again\n"}},
                                 {.id = ssg::DiffFileId{"renamed.txt"},
                                  .path = "renamed.txt",
                                  .previousPath = std::filesystem::path{"old-name.txt"},
                                  .baselineContent = std::string{"same\n"},
                                  .workingContent = std::string{"same\n"}},
                             }})
                    .accepted());

    auto second = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(second.has_value());
    if (!second) return;
    auto* secondGit =
        findProvider(second->sections().tree, ssg::TreeProviderKind::Git);
    ASSERT_TRUE(secondGit != nullptr);
    if (!secondGit) return;

    const auto secondStatuses = gitProviderStatuses(*secondGit);
    ASSERT_EQ(secondStatuses.size(), std::size_t{2});
    ASSERT_EQ(secondStatuses.at("modified.txt"), ssg::GitTreeStatus::Modified);
    ASSERT_EQ(secondStatuses.at("renamed.txt"), ssg::GitTreeStatus::Renamed);
    ASSERT_TRUE(second->sections().tree.revision.value() > firstRevision);
}

TEST(gitStatusActivationOpensLiveDiffTabAndReusesIt) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "coexist.txt"} << "disk\n";
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"coexist.txt"}})
                    .accepted());

    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = ssg::Revision{30},
                         .baselineIdentity = "head-x:index-1",
                         .files = {{.id = ssg::DiffFileId{"coexist-id"},
                                    .path = "coexist.txt",
                                    .baselineContent = std::string{"before\n"},
                                    .workingContent = std::string{"after\n"}}}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"panel.show_git_status", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tree.select_next", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tree.activate", runtime.revision(), {}})
                    .accepted());

    auto first = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(first.has_value());
    if (!first) return;
    ASSERT_EQ(countTabsOfKind(first->sections().tabs, ssg::TabKind::Document),
              std::size_t{1});
    ASSERT_EQ(countTabsOfKind(first->sections().tabs, ssg::TabKind::LiveDiff),
              std::size_t{1});
    std::optional<ssg::TabId> liveDiffId;
    for (const auto& tab : first->sections().tabs.tabs) {
        if (tab.kind == ssg::TabKind::LiveDiff) {
            liveDiffId = tab.id;
            ASSERT_EQ(tab.contentIdentity, std::string{"coexist-id"});
        }
    }
    ASSERT_TRUE(liveDiffId.has_value());
    if (!liveDiffId) return;

    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tree.activate", runtime.revision(), {}})
                    .accepted());
    auto second =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(second.has_value());
    if (!second) return;
    ASSERT_EQ(countTabsOfKind(second->sections().tabs, ssg::TabKind::Document),
              std::size_t{1});
    ASSERT_EQ(countTabsOfKind(second->sections().tabs, ssg::TabKind::LiveDiff),
              std::size_t{1});
    ASSERT_EQ(second->sections().tabs.active, liveDiffId);
}

TEST(documentAndLiveDiffTabsCloseIndependently) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "coexist.txt"} << "disk\n";
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"coexist.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = ssg::Revision{32},
                         .baselineIdentity = "head-z:index-1",
                         .files = {{.id = ssg::DiffFileId{"coexist-id"},
                                    .path = "coexist.txt",
                                    .baselineContent = std::string{"before\n"},
                                    .workingContent = std::string{"after\n"}}}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"panel.show_git_status", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tree.select_next", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tree.activate", runtime.revision(), {}})
                    .accepted());

    auto first = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(first.has_value());
    if (!first) return;
    std::optional<ssg::TabId> documentTab;
    std::optional<ssg::TabId> liveDiffTab;
    for (const auto& tab : first->sections().tabs.tabs) {
        if (tab.kind == ssg::TabKind::Document) {
            documentTab = tab.id;
        } else if (tab.kind == ssg::TabKind::LiveDiff) {
            liveDiffTab = tab.id;
        }
    }
    ASSERT_TRUE(documentTab.has_value());
    ASSERT_TRUE(liveDiffTab.has_value());
    if (!documentTab || !liveDiffTab) return;

    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tab.activate", runtime.revision(),
                               *documentTab})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tab.close", runtime.revision(),
                               *documentTab})
                    .accepted());
    auto afterDocumentClose =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(afterDocumentClose.has_value());
    if (!afterDocumentClose) return;
    ASSERT_EQ(countTabsOfKind(afterDocumentClose->sections().tabs,
                              ssg::TabKind::Document),
              std::size_t{0});
    ASSERT_EQ(countTabsOfKind(afterDocumentClose->sections().tabs,
                              ssg::TabKind::LiveDiff),
              std::size_t{1});
    ASSERT_EQ(afterDocumentClose->sections().document.diffFileIdentity,
              std::optional<std::string>{"coexist-id"});

    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"coexist.txt"}})
                    .accepted());
    auto reopened =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(reopened.has_value());
    if (!reopened) return;
    ASSERT_EQ(countTabsOfKind(reopened->sections().tabs, ssg::TabKind::Document),
              std::size_t{1});
    ASSERT_EQ(countTabsOfKind(reopened->sections().tabs, ssg::TabKind::LiveDiff),
              std::size_t{1});

    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tab.activate", runtime.revision(),
                               *liveDiffTab})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tab.close", runtime.revision(),
                               *liveDiffTab})
                    .accepted());
    auto afterLiveDiffClose =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(afterLiveDiffClose.has_value());
    if (!afterLiveDiffClose) return;
    ASSERT_EQ(countTabsOfKind(afterLiveDiffClose->sections().tabs,
                              ssg::TabKind::Document),
              std::size_t{1});
    ASSERT_EQ(countTabsOfKind(afterLiveDiffClose->sections().tabs,
                              ssg::TabKind::LiveDiff),
              std::size_t{0});
    ASSERT_EQ(afterLiveDiffClose->sections().document.diffFileIdentity,
              std::nullopt);
}

TEST(gitStatusActivationOpensDeletedLiveDiffWithoutDiskFile) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "gone.txt"} << "gone\n";
    std::filesystem::remove(root / "workspace" / "gone.txt");
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());
    ASSERT_FALSE(std::filesystem::exists(root / "workspace" / "gone.txt"));

    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = ssg::Revision{31},
                         .baselineIdentity = "head-x:index-2",
                         .files = {{.id = ssg::DiffFileId{"deleted-id"},
                                    .path = "gone.txt",
                                    .baselineContent = std::string{"gone\n"},
                                    .workingContent = std::nullopt}}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"panel.show_git_status", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tree.select_next", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tree.activate", runtime.revision(), {}})
                    .accepted());

    auto snapshot =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(countTabsOfKind(snapshot->sections().tabs, ssg::TabKind::LiveDiff),
              std::size_t{1});
    ASSERT_EQ(snapshot->sections().document.diffFileIdentity,
              std::optional<std::string>{"deleted-id"});
    // A deleted file has no real document content -- its removed lines are
    // represented entirely as phantom rows (Viewport's removedBlocks
    // projection), derived from diff.files' hunks below, not as literal
    // document text. Synthesizing the baseline here as "current" text would
    // duplicate every removed line: once as a real row, once as its phantom.
    ASSERT_EQ(snapshot->sections().document.text, std::string{});
    const auto deleted = std::find_if(
        snapshot->sections().diff.files.begin(),
        snapshot->sections().diff.files.end(),
        [](const ssg::DiffFileView& file) {
            return file.id == ssg::DiffFileId{"deleted-id"};
        });
    ASSERT_TRUE(deleted != snapshot->sections().diff.files.end());
    if (deleted == snapshot->sections().diff.files.end()) return;
    ASSERT_TRUE(deleted->deleted);
    ASSERT_FALSE(deleted->hunks.empty());
}

TEST(liveDiffOpenClassificationPausesOnlyForUserActivation) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());

    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"follow_edits.pause", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = ssg::Revision{40},
                         .baselineIdentity = "head-y:index-1",
                         .files = {{.id = ssg::DiffFileId{"programmatic-id"},
                                    .path = "needle.txt",
                                    .baselineContent =
                                        std::string{"alpha needle omega"},
                                    .workingContent =
                                        std::string{"alpha NEEDLE omega"}}}})
                    .accepted());
    auto beforeResume =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(beforeResume.has_value());
    if (!beforeResume) return;
    ASSERT_EQ(beforeResume->sections().followEdits.mode,
              ssg::FollowMode::Paused);
    ASSERT_EQ(countTabsOfKind(beforeResume->sections().tabs,
                              ssg::TabKind::LiveDiff),
              std::size_t{0});

    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"follow_edits.resume", runtime.revision(), {}})
                    .accepted());
    auto afterProgrammatic =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(afterProgrammatic.has_value());
    if (!afterProgrammatic) return;
    ASSERT_EQ(afterProgrammatic->sections().followEdits.mode,
              ssg::FollowMode::Following);
    ASSERT_EQ(countTabsOfKind(afterProgrammatic->sections().tabs,
                              ssg::TabKind::LiveDiff),
              std::size_t{1});

    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"panel.show_git_status", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tree.select_next", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tree.activate", runtime.revision(), {}})
                    .accepted());
    auto afterUser =
        runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(afterUser.has_value());
    if (!afterUser) return;
    ASSERT_EQ(afterUser->sections().followEdits.mode, ssg::FollowMode::Paused);
}

TEST(tabSwitchPausesFollowViaNavigationPath) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "other.txt"} << "other\n";
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"needle.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"other.txt"}})
                    .accepted());
    ASSERT_EQ(followMode(runtime), ssg::FollowMode::Following);

    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tab.previous", runtime.revision(), {}})
                    .accepted());
    ASSERT_EQ(followMode(runtime), ssg::FollowMode::Paused);
}

TEST(followToggleMatchesPauseAndResumeIncludingQueuedTargetResolution) {
    const auto makeRuntime = []() -> std::unique_ptr<ssg::EditorRuntime> {
        auto root = uniqueRoot();
        auto created = ssg::EditorRuntime::create(
            {root / "workspace", root / "scratch", root / "recovery"});
        ASSERT_TRUE(created.accepted());
        if (!created.accepted()) return nullptr;
        auto runtime = std::move(created.runtime);
        ASSERT_TRUE(runtime
                        ->attach({ssg::ClientId{1},
                                  ssg::InvocationOrigin::InProcess},
                                 ssg::ViewId{1})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"file.open", runtime->revision(),
                                    std::string{"needle.txt"}})
                        .accepted());
        return runtime;
    };
    const auto followState = [](ssg::EditorRuntime& runtime) {
        auto snapshot =
            runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return ssg::FollowEditsViewState();
        return snapshot->sections().followEdits;
    };

    auto pauseResume = makeRuntime();
    auto togglePath = makeRuntime();
    ASSERT_TRUE(pauseResume != nullptr);
    ASSERT_TRUE(togglePath != nullptr);
    if (!pauseResume || !togglePath) return;

    ASSERT_TRUE(pauseResume
                    ->dispatch(ssg::ClientId{1},
                               {"follow_edits.pause", pauseResume->revision(), {}})
                    .accepted());
    ASSERT_TRUE(togglePath
                    ->dispatch(ssg::ClientId{1},
                               {"follow_edits.toggle", togglePath->revision(), {}})
                    .accepted());

    const auto pausedWithPause = followState(*pauseResume);
    const auto pausedWithToggle = followState(*togglePath);
    ASSERT_EQ(pausedWithPause.mode, ssg::FollowMode::Paused);
    ASSERT_EQ(pausedWithToggle, pausedWithPause);

    ssg::GitDiffScan scan{
        .revision = ssg::Revision{2},
        .baselineIdentity = "head-1:index-1",
        .files = {{.id = ssg::DiffFileId{"needle.txt"},
                   .path = "needle.txt",
                   .baselineContent = std::string{"alpha needle omega"},
                   .workingContent = std::string{"alpha needle omega plus"}}}};
    ASSERT_TRUE(pauseResume->applyGitDiffScan(scan).accepted());
    ASSERT_TRUE(togglePath->applyGitDiffScan(scan).accepted());

    const auto pausedQueuedWithPause = followState(*pauseResume);
    const auto pausedQueuedWithToggle = followState(*togglePath);
    ASSERT_EQ(pausedQueuedWithPause.mode, ssg::FollowMode::Paused);
    ASSERT_FALSE(pausedQueuedWithPause.queuedTargets.empty());
    ASSERT_EQ(pausedQueuedWithToggle, pausedQueuedWithPause);

    ASSERT_TRUE(pauseResume
                    ->dispatch(ssg::ClientId{1},
                               {"follow_edits.resume", pauseResume->revision(),
                                {}})
                    .accepted());
    ASSERT_TRUE(togglePath
                    ->dispatch(ssg::ClientId{1},
                               {"follow_edits.toggle", togglePath->revision(),
                                {}})
                    .accepted());

    const auto resumedWithResume = followState(*pauseResume);
    const auto resumedWithToggle = followState(*togglePath);
    ASSERT_EQ(resumedWithResume.mode, ssg::FollowMode::Following);
    ASSERT_TRUE(resumedWithResume.activeTarget.has_value());
    ASSERT_TRUE(resumedWithResume.queuedTargets.empty());
    ASSERT_EQ(resumedWithToggle, resumedWithResume);
}

TEST(followPauseOnEditTransitionTable) {
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"text.insert", runtime->revision(),
                                    ssg::TextInputArguments{"x"}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"select.right", runtime->revision(), {}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"clipboard.cut", runtime->revision(), {}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"follow_edits.resume", runtime->revision(), {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Following);
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"clipboard.paste", runtime->revision(), {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"text.insert", runtime->revision(),
                                    ssg::TextInputArguments{"x"}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"follow_edits.resume", runtime->revision(), {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Following);
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"edit.undo", runtime->revision(), {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"text.insert", runtime->revision(),
                                    ssg::TextInputArguments{"x"}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"follow_edits.resume", runtime->revision(), {}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"edit.undo", runtime->revision(), {}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"follow_edits.resume", runtime->revision(), {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Following);
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"edit.redo", runtime->revision(), {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"replace.open", runtime->revision(), {}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"find.update_query", runtime->revision(),
                                    ssg::FindQueryArguments{"needle"}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"replace.update_replacement",
                                    runtime->revision(),
                                    ssg::FindQueryArguments{"pin"}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Following);
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"replace.current", runtime->revision(), {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha\nbeta\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"select.add_cursor_down",
                                    runtime->revision(), {}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"text.insert", runtime->revision(),
                                    ssg::TextInputArguments{"x"}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{2},
                                   {"text.insert", runtime->revision(),
                                    ssg::TextInputArguments{"x"}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Following);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        // Recovery/replay-originated edits execute under a System principal.
        ASSERT_TRUE(
            runtime
                ->dispatch(ssg::ClientId{3},
                           {"text.insert", runtime->revision(),
                            ssg::TextInputArguments{"x"}})
                .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Following);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"pane.next", runtime->revision(), {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"view.scroll_lines", runtime->revision(),
                                    ssg::ScrollLinesArguments{1}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"cursor.right", runtime->revision(), {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
}

TEST(paletteOpenEntersPromptFocusAndPublishesCandidates) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.open", runtime.revision(), {}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().shell.focus, ssg::FocusTarget::Prompt);
    ASSERT_FALSE(snapshot->sections().palette.candidates.empty());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.close", runtime.revision(), {}}).accepted());
    auto closed = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(closed.has_value());
    if (!closed) return;
    ASSERT_EQ(closed->sections().shell.focus, ssg::FocusTarget::Editor);
}

// The open-picker kind is derived from the prompt after every dispatch rather
// than cleared at each close path.  Pin that across every way a picker closes:
// a stale kind would make the NEXT open publish the previous picker's mode and
// candidates, which is invisible while only one picker exists and wrong the
// moment a second one lands.
TEST(everyPaletteClosePathLeavesNoOpenPickerBehind) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    auto candidatesAfter = [&](std::string const& closeCommand) {
        ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.open", runtime.revision(), {}}).accepted());
        auto open = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(open.has_value());
        if (open) ASSERT_FALSE(open->sections().palette.candidates.empty());
        (void)runtime.dispatch(ssg::ClientId{1}, {closeCommand, runtime.revision(), {}});
        auto shut = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(shut.has_value());
        if (shut) ASSERT_TRUE(shut->sections().palette.candidates.empty());
    };

    candidatesAfter("palette.close");
    candidatesAfter("prompt.cancel");

    // A successful palette.execute cancels the prompt as part of executing; the
    // picker must not survive into the next open.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.open", runtime.revision(), {}}).accepted());
    (void)runtime.dispatch(
        ssg::ClientId{1},
        {"palette.execute", runtime.revision(), ssg::PaletteExecuteArguments{"edit.undo"}});
    auto executed = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(executed.has_value());
    if (executed) ASSERT_TRUE(executed->sections().palette.candidates.empty());
}

TEST(paletteExecuteValidatesCandidateMembership) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"needle.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.open", runtime.revision(), {}}).accepted());

    // A command outside the published candidate set is rejected before dispatch.
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1}, {"palette.execute", runtime.revision(), ssg::PaletteExecuteArguments{"not.a.command"}}).accepted());
    // Missing the id payload is rejected.
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1}, {"palette.execute", runtime.revision(), {}}).accepted());
    // A published command id validates, executes server-side, and closes the palette.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.execute", runtime.revision(), ssg::PaletteExecuteArguments{"file.save"}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().shell.focus, ssg::FocusTarget::Editor);
}

TEST(paletteCandidatesCarryLabelsAndKeyDetail) {
    auto root = uniqueRoot();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.open", runtime.revision(), {}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    const auto& candidates = snapshot->sections().palette.candidates;
    ASSERT_FALSE(candidates.empty());

    const ssg::PaletteCandidate* save = nullptr;
    const ssg::PaletteCandidate* undo = nullptr;
    for (const auto& candidate : candidates) {
        // Every candidate carries a human label, never the raw dotted id.
        ASSERT_NE(candidate.label, candidate.id);
        ASSERT_FALSE(candidate.label.empty());
        if (candidate.id == "file.save") save = &candidate;
        if (candidate.id == "edit.undo") undo = &candidate;
    }
    ASSERT_TRUE(save != nullptr);
    ASSERT_TRUE(undo != nullptr);
    if (save) {
        ASSERT_EQ(save->label, std::string{"Save File"});
        ASSERT_EQ(save->detail, std::string{"Esc S"});  // Its bound chord.
    }
    if (undo) {
        ASSERT_EQ(undo->detail, std::string{"Esc Z"});
    }

    // An unbound command shows a label but no key detail.
    const ssg::PaletteCandidate* unbound = nullptr;
    for (const auto& candidate : candidates) {
        if (candidate.id == "edit.sort_lines") unbound = &candidate;
    }
    ASSERT_TRUE(unbound != nullptr);
    if (unbound) ASSERT_TRUE(unbound->detail.empty());
}

TEST(treeScrollsToKeepSelectionVisibleInAShortPanel) {
    auto root = std::filesystem::current_path() / "runtime_nav_treescroll";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    // 40 top-level files -> a tree far taller than a short panel.
    for (int i = 0; i < 40; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "file-%02d.txt", i);
        std::ofstream{root / "workspace" / name} << "x";
    }
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    // Show the panel; a 12-row terminal gives a panel content height of ~9.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    // Select the workspace root and expand it so its 40 files become visible.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_next", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.activate", runtime.revision(), {}}).accepted());
    const ssg::ViewportDimensions dims{80, 12};

    // Baseline: selection at the top (root), window pinned to the top with a live
    // thumb.
    {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        ASSERT_TRUE(snap.has_value());
        if (!snap) return;
        auto const& p = snap->sections().tree.providers.front();
        ASSERT_TRUE(p.nodes.size() >= 40);
        ASSERT_EQ(p.firstVisible, std::uint32_t{0});
        ASSERT_TRUE(p.scrollbar.maximumFirstRow > 0);          // scrollable
        ASSERT_TRUE(p.scrollbar.thumbSize < p.scrollbar.viewportRows);
        ASSERT_EQ(p.visibleNodeIds.size(),
                  std::size_t{p.scrollbar.viewportRows});       // window bound
        ASSERT_EQ(p.visibleNodeIds.front(), p.nodes.front().node.id);
    }

    // Move the selection to the bottom: the window scrolls to keep it shown.
    for (int i = 0; i < 60; ++i) {
        ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_next", runtime.revision(), {}}).accepted());
    }
    std::uint32_t deepFirst = 0;
    {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        ASSERT_TRUE(snap.has_value());
        if (!snap) return;
        auto const& p = snap->sections().tree.providers.front();
        ASSERT_TRUE(p.selected.has_value());
        // The selected node's absolute index lies within the visible window.
        std::optional<std::uint32_t> selIndex;
        for (std::uint32_t i = 0; i < p.nodes.size(); ++i) {
            if (p.nodes[i].node.id == *p.selected) { selIndex = i; break; }
        }
        ASSERT_TRUE(selIndex.has_value());
        ASSERT_TRUE(p.firstVisible > 0);
        ASSERT_TRUE(*selIndex >= p.firstVisible &&
                    *selIndex < p.firstVisible + p.visibleNodeIds.size());
        // The hit map maps each viewport row to the correct on-screen node id.
        for (std::size_t row = 0; row < p.visibleNodeIds.size(); ++row) {
            ASSERT_EQ(p.visibleNodeIds[row],
                      p.nodes[p.firstVisible + row].node.id);
        }
        deepFirst = p.firstVisible;
    }
    ASSERT_TRUE(deepFirst > 0);

    // Move back up to the top: the window scrolls back to first_visible == 0.
    for (int i = 0; i < 40; ++i) {
        ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_previous", runtime.revision(), {}}).accepted());
    }
    {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        ASSERT_TRUE(snap.has_value());
        if (!snap) return;
        ASSERT_EQ(snap->sections().tree.providers.front().firstVisible, std::uint32_t{0});
    }
    std::filesystem::remove_all(root);
}

TEST(treeSelectSetsSelectionToANodeAndRejectsUnknownIds) {
    auto root = std::filesystem::current_path() / "runtime_nav_treeselect";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    for (int i = 0; i < 6; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "file-%02d.txt", i);
        std::ofstream{root / "workspace" / name} << "x";
    }
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    // Expand the workspace root so its files become visible/selectable nodes.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_next", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.activate", runtime.revision(), {}}).accepted());

    const ssg::ViewportDimensions dims{80, 24};
    auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(snap.has_value());
    if (!snap) return;
    auto const& nodes = snap->sections().tree.providers.front().nodes;
    ASSERT_TRUE(nodes.size() >= 4);
    if (nodes.size() < 4) return;
    // Pick a node that is NOT already selected (the third visible node).
    auto const target = nodes[2].node.id;

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"tree.select", runtime.revision(),
                                  ssg::TreeSelectArguments{target}}).accepted());
    auto after = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(after.has_value());
    if (!after) return;
    ASSERT_TRUE(after->sections().tree.providers.front().selected.has_value());
    ASSERT_EQ(*after->sections().tree.providers.front().selected, target);

    // An id absent from the active provider is rejected; a missing payload too.
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1},
                                  {"tree.select", runtime.revision(),
                                   ssg::TreeSelectArguments{ssg::TreeNodeId{"nope"}}}).accepted());
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1},
                                  {"tree.select", runtime.revision(), {}}).accepted());
    // The selection is unchanged after the rejected attempts.
    auto again = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(again.has_value());
    if (!again) return;
    ASSERT_EQ(*again->sections().tree.providers.front().selected, target);
    std::filesystem::remove_all(root);
}

TEST(treeSelectFocusesThePanelAndTheClickPairNetsExpectedFocus) {
    auto root = std::filesystem::current_path() / "runtime_nav_treefocus";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace" / "dir");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "dir" / "inner.txt"} << "x";
    std::ofstream{root / "workspace" / "top.txt"} << "hello";
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    auto focus = [&] {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        return snap ? snap->sections().shell.focus : ssg::FocusTarget::Editor;
    };
    // Showing the panel now focuses it (QOL); expand the root so a directory node
    // and a file node are both visible/selectable.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_next", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.activate", runtime.revision(), {}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);

    auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(snap.has_value());
    if (!snap) return;
    std::optional<ssg::TreeNodeId> dirId;
    std::optional<ssg::TreeNodeId> fileId;
    for (auto const& view : snap->sections().tree.providers.front().nodes) {
        if (view.node.expandable && !dirId) dirId = view.node.id;
        if (!view.node.expandable && view.node.workspacePath && !fileId) fileId = view.node.id;
    }
    ASSERT_TRUE(dirId.has_value());
    ASSERT_TRUE(fileId.has_value());
    if (!dirId || !fileId) return;

    // The file click pair [tree.select, tree.activate] ends on the editor (the
    // file opens, so tree.activate's focus_editor wins over tree.select's panel).
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select", runtime.revision(), ssg::TreeSelectArguments{*fileId}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.activate", runtime.revision(), {}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Editor);

    // From editor focus, tree.select alone moves keyboard focus to the panel.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select", runtime.revision(), ssg::TreeSelectArguments{*fileId}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);

    // The directory click pair ends on the panel (tree.select focuses the panel,
    // tree.activate toggles the directory and leaves focus alone).
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select", runtime.revision(), ssg::TreeSelectArguments{*dirId}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.activate", runtime.revision(), {}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);
    std::filesystem::remove_all(root);
}

TEST(treeScrollMovesTheViewportWithoutMovingTheSelection) {
    auto root = std::filesystem::current_path() / "runtime_nav_treescroll_wheel";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    for (int i = 0; i < 40; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "file-%02d.txt", i);
        std::ofstream{root / "workspace" / name} << "x";
    }
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    // Expand the root so the 40 files become a tree taller than a short panel.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_next", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.activate", runtime.revision(), {}}).accepted());
    const ssg::ViewportDimensions dims{80, 12};

    auto baseline = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(baseline.has_value());
    if (!baseline) return;
    auto const& p0 = baseline->sections().tree.providers.front();
    ASSERT_EQ(p0.firstVisible, std::uint32_t{0});
    ASSERT_TRUE(p0.scrollbar.maximumFirstRow > 0);
    auto const selectedBefore = p0.selected;

    // Wheel down: the viewport offset advances, but the selection does not move.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"tree.scroll", runtime.revision(),
                                  ssg::ScrollLinesArguments{3}}).accepted());
    auto scrolled = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(scrolled.has_value());
    if (!scrolled) return;
    auto const& p1 = scrolled->sections().tree.providers.front();
    ASSERT_EQ(p1.firstVisible, std::uint32_t{3});
    ASSERT_EQ(p1.selected, selectedBefore);  // selection unchanged

    // Wheel up past the top clamps at 0.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"tree.scroll", runtime.revision(),
                                  ssg::ScrollLinesArguments{-99}}).accepted());
    auto topped = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(topped.has_value());
    if (!topped) return;
    ASSERT_EQ(topped->sections().tree.providers.front().firstVisible, std::uint32_t{0});

    // Wheel down past the bottom clamps at maximum_first_row.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"tree.scroll", runtime.revision(),
                                  ssg::ScrollLinesArguments{999}}).accepted());
    auto bottomed = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(bottomed.has_value());
    if (!bottomed) return;
    auto const& p3 = bottomed->sections().tree.providers.front();
    ASSERT_EQ(p3.firstVisible, p3.scrollbar.maximumFirstRow);

    // A missing payload is rejected.
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1},
                                  {"tree.scroll", runtime.revision(), {}}).accepted());
    std::filesystem::remove_all(root);
}

} // namespace

namespace {

// M12 VP-H: word-wrap-off horizontal caret reveal. A long line whose caret moves
// past the pane width scrolls horizontally so the caret stays visible; returning
// to the line start resets the offset. A short (fitting) line never scrolls.
TEST(wordWrapOffRevealsCaretHorizontally) {
    auto root = std::filesystem::current_path() / "runtime_hscroll";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    // A single 60-cell line, far wider than the test pane.
    std::ofstream{root / "workspace" / "long.txt"} << std::string(60, 'a') << "\n";
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"long.txt"}}).accepted());

    ssg::ViewportDimensions const dims{24, 6};
    // Prime the pane-size cache the reveal path reads.
    auto primed = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(primed.has_value());
    if (!primed) return;
    ASSERT_EQ(primed->client().viewport.firstVisualColumn, std::uint32_t{0});

    // Move the caret to the end of the long line: it is past the pane width, so
    // the viewport scrolls horizontally to keep it visible.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"cursor.line_end", runtime.revision(), {}})
                    .accepted());
    auto scrolled = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(scrolled.has_value());
    if (!scrolled) return;
    auto const offset = scrolled->client().viewport.firstVisualColumn;
    ASSERT_TRUE(offset > 0);
    // The caret's cell (60) is within the visible horizontal window.
    ASSERT_TRUE(60u >= offset);
    ASSERT_TRUE(60u < offset + dims.columns);

    // Returning to the line start resets the horizontal offset to zero.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"cursor.line_start", runtime.revision(), {}})
                    .accepted());
    auto reset = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(reset.has_value());
    if (!reset) return;
    ASSERT_EQ(reset->client().viewport.firstVisualColumn, std::uint32_t{0});
    std::filesystem::remove_all(root);
}

// VP-3 regression: the word-wrap-ON path stays EXACT — a long line still wraps to
// multiple visual rows through the runtime — while word-wrap-OFF clips it to one
// row and scrolls horizontally.  Locks both directions of the wrap gate so the
// M12 projection can never silently disable wrapping.
TEST(wordWrapOnWrapsLongLinesOffClipsThem) {
    auto root = std::filesystem::current_path() / "runtime_wrap_gate";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    // One 200-cell line (far wider than the 80-col pane) plus a short line.
    std::ofstream{root / "workspace" / "wide.txt"}
        << std::string(200, 'b') << "\nshort\n";
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"wide.txt"}}).accepted());
    ssg::ViewportDimensions const dims{80, 24};

    // Word wrap OFF (default): three logical lines (the trailing newline yields a
    // final empty line) -> three visual rows total; the 200-cell line is ONE
    // clipped visual row.
    auto off = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(off.has_value());
    if (!off) return;
    ASSERT_EQ(off->client().viewport.totalVisualRows, std::uint32_t{3});
    std::uint32_t offRowsForLine0 = 0;
    for (auto const& row : off->client().viewport.visibleRows) {
        if (row.logicalLine == 0) ++offRowsForLine0;
    }
    ASSERT_EQ(offRowsForLine0, std::uint32_t{1});  // clipped, not wrapped

    // Word wrap ON: the 200-cell line wraps into ceil(200/80) = 3 visual rows, so
    // the total exceeds the OFF total and logical line 0 spans >1 row.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"view.toggle_word_wrap", runtime.revision(), {}})
                    .accepted());
    auto on = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(on.has_value());
    if (!on) return;
    ASSERT_TRUE(on->client().viewport.totalVisualRows > 3u);  // wrapped
    std::uint32_t onRowsForLine0 = 0;
    for (auto const& row : on->client().viewport.visibleRows) {
        if (row.logicalLine == 0) ++onRowsForLine0;
    }
    ASSERT_EQ(onRowsForLine0, std::uint32_t{3});  // 200 cells / 80 -> 3 rows
    // Wrapped lines never scroll horizontally.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"cursor.line_end", runtime.revision(), {}})
                    .accepted());
    auto wrappedEnd = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(wrappedEnd.has_value());
    if (!wrappedEnd) return;
    ASSERT_EQ(wrappedEnd->client().viewport.firstVisualColumn, std::uint32_t{0});
    std::filesystem::remove_all(root);
}

// M12 VP-2b (INV-viewport-bounded-work): with word wrap OFF, a caret navigation
// segments only the visible + moved lines — bounded and INDEPENDENT of document
// length — not the whole document. Proven by the compute_cell_run counter: the
// per-move segmentation count is identical for a 50-line and a 20000-line file.
TEST(wordWrapOffNavigationIsViewportBounded) {
    auto root = std::filesystem::current_path() / "runtime_navbound";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    auto makeDoc = [](std::size_t lines) {
        std::string text;
        for (std::size_t i = 0; i < lines; ++i) {
            text += "line " + std::to_string(i) + " content\n";
        }
        return text;
    };
    std::ofstream{root / "workspace" / "small.txt"} << makeDoc(50);
    std::ofstream{root / "workspace" / "big.txt"} << makeDoc(20000);
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ssg::ViewportDimensions const dims{80, 24};

    auto navSegmentations = [&](std::string const& file) -> std::uint64_t {
        (void)runtime.dispatch(
            ssg::ClientId{1}, {"file.open", runtime.revision(), file});
        (void)runtime.snapshot(ssg::ClientId{1}, dims);  // prime pane cache
        ssg::GraphemeLayout::resetCellRunCalls();
        for (int i = 0; i < 4; ++i) {
            (void)runtime.dispatch(
                ssg::ClientId{1},
                {"cursor.line_down", runtime.revision(), {}});
        }
        return ssg::GraphemeLayout::cellRunCalls();
    };

    auto const smallCalls = navSegmentations("small.txt");
    auto const bigCalls = navSegmentations("big.txt");

    ASSERT_TRUE(smallCalls > 0);
    // Bounded (~ per move: visible rows + the moved line), and NOT proportional to
    // the 400x-larger document.
    ASSERT_TRUE(smallCalls < 200);
    ASSERT_EQ(smallCalls, bigCalls);
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    RUN(searchTreeDiffAndFollowSectionsUseRuntimeState);
    RUN(externalDiffBurstRevealsOnlyNewestFileWithoutPausingFollow);
    RUN(gitDiffScanUpdatesDiffAndRejectsStaleBatches);
    RUN(gitDiffSelectionUsesDiffIdentityIndependentOfDocumentRevision);
    RUN(gitDiffScanRefreshesGitTreeProviderFromDiffAndOnSecondScan);
    RUN(gitStatusActivationOpensLiveDiffTabAndReusesIt);
    RUN(documentAndLiveDiffTabsCloseIndependently);
    RUN(gitStatusActivationOpensDeletedLiveDiffWithoutDiskFile);
    RUN(liveDiffOpenClassificationPausesOnlyForUserActivation);
    RUN(tabSwitchPausesFollowViaNavigationPath);
    RUN(followToggleMatchesPauseAndResumeIncludingQueuedTargetResolution);
    RUN(followPauseOnEditTransitionTable);
    RUN(paletteOpenEntersPromptFocusAndPublishesCandidates);
    RUN(everyPaletteClosePathLeavesNoOpenPickerBehind);
    RUN(paletteExecuteValidatesCandidateMembership);
    RUN(paletteCandidatesCarryLabelsAndKeyDetail);
    RUN(treeScrollsToKeepSelectionVisibleInAShortPanel);
    RUN(treeSelectSetsSelectionToANodeAndRejectsUnknownIds);
    RUN(treeScrollMovesTheViewportWithoutMovingTheSelection);
    RUN(treeSelectFocusesThePanelAndTheClickPairNetsExpectedFocus);
    RUN(wordWrapOffRevealsCaretHorizontally);
    RUN(wordWrapOnWrapsLongLinesOffClipsThem);
    RUN(wordWrapOffNavigationIsViewportBounded);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
