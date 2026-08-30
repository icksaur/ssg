#include "../grid_test_view.h"
#include "../test_helpers.h"

#include <ssg/EditorSession.h>
#include <ssg/CommandCatalog.h>
#include <ssg/GraphemeLayout.h>
#include <ssg/Keymap.h>
#include <ssg/PromptSurface.h>
#include <ssg/StatusQueue.h>
#include <ssg/TextInputCommands.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

std::vector<std::string_view> keyboardRoutes(
    std::type_identity<ssg::ClientKeyInput>) {
    return {};
}
std::vector<std::string_view> keyboardRoutes(
    std::type_identity<ssg::TabPointerInput>) {
    return {"tab.next", "tab.previous", "tab.close"};
}
std::vector<std::string_view> keyboardRoutes(
    std::type_identity<ssg::TreePointerInput>) {
    return {"panel.focus", "tree.activate"};
}
std::vector<std::string_view> keyboardRoutes(
    std::type_identity<ssg::PickerPointerInput>) {
    return {"prompt.submit"};
}
std::vector<std::string_view> keyboardRoutes(
    std::type_identity<ssg::PromptControlPointerInput>) {
    return {"prompt.focus_next_control"};
}
std::vector<std::string_view> keyboardRoutes(
    std::type_identity<ssg::ExternalActionPointerInput>) {
    return {"external.focus", "external.reload", "external.keep_buffer",
            "external.open_diff"};
}
std::vector<std::string_view> keyboardRoutes(
    std::type_identity<ssg::StatusActionPointerInput>) {
    return {"palette.open"};
}
std::vector<std::string_view> keyboardRoutes(
    std::type_identity<ssg::PublishedUiActionPointerInput>) {
    return {"palette.open"};
}
std::vector<std::string_view> keyboardRoutes(
    std::type_identity<ssg::NoticeActionPointerInput>) {
    return {"draft.discard"};
}
std::vector<std::string_view> keyboardRoutes(
    std::type_identity<ssg::DocumentPointerInput>) {
    return {"cursor.left", "cursor.right", "cursor.line_up",
            "cursor.line_down", "select.left", "select.right",
            "select.line_up", "select.line_down", "select.add_cursor_up",
            "select.add_cursor_down"};
}
std::vector<std::string_view> keyboardRoutes(
    std::type_identity<ssg::ScrollLinesInput>) {
    return {"cursor.page_up", "cursor.page_down", "tree.select_next",
            "tree.select_previous"};
}
std::vector<std::string_view> keyboardRoutes(
    std::type_identity<ssg::ScrollFractionInput>) {
    return {"cursor.document_start", "cursor.document_end",
            "tree.select_next", "tree.select_previous"};
}
std::vector<std::string_view> keyboardRoutes(
    std::type_identity<ssg::ViewNavigationInput>) {
    return {};
}
std::vector<std::string_view> keyboardRoutes(
    std::type_identity<ssg::ResolvedPaneFocusInput>) {
    return {};
}

template <std::size_t... Index>
std::vector<std::string_view> allKeyboardRoutes(
    std::index_sequence<Index...>) {
    std::vector<std::string_view> routes;
    const auto append = [&](auto type) {
        auto variantRoutes = keyboardRoutes(type);
        routes.insert(routes.end(), variantRoutes.begin(),
                      variantRoutes.end());
    };
    (append(std::type_identity<
            std::variant_alternative_t<Index, ssg::ClientInput>>{}),
     ...);
    return routes;
}

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
        statuses.emplace(*node.node.workspacePath,
                         node.node.gitStatus->status);
    }
    return statuses;
}

std::size_t countTabsOfKind(const ssg::TabViewState& tabs, ssg::TabKind kind) {
    return static_cast<std::size_t>(std::count_if(
        tabs.tabs.begin(), tabs.tabs.end(), [&](const ssg::TabState& tab) {
            return tab.kind == kind;
        }));
}

std::optional<ssg::SearchMode> activePickerMode(
    const ssg::PaletteViewState& palette) {
    return palette.activePicker
               ? std::optional<ssg::SearchMode>{palette.activePicker->mode}
               : std::nullopt;
}

ssg::PickerSubmitArguments pickerSubmit(
    ssg::EditorSession& runtime, ssg::SearchMode mode, std::string candidateId) {
    auto snapshot =
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    const auto activation =
        snapshot && snapshot->sections().palette.activePicker
            ? snapshot->sections().palette.activePicker->id
            : ssg::PickerActivationId{};
    return {{mode, activation}, std::move(candidateId)};
}

std::string overDiffLineBudget(char value) {
    std::string text;
    text.reserve(400'002);
    for (std::size_t line = 0; line < 200'001; ++line) {
        text.push_back(value);
        text.push_back('\n');
    }
    return text;
}

ssg::FollowMode followMode(ssg::EditorSession& runtime) {
    auto snapshot =
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) {
        return ssg::FollowMode::Paused;
    }
    return snapshot->sections().followEdits.mode;
}

std::unique_ptr<ssg::EditorSession> followPauseRuntime(std::string text) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "needle.txt"} << text;
    std::ofstream{root / "workspace" / "other.txt"} << "other\n";
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) {
        return nullptr;
    }
    auto runtime = std::move(created.session);
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
    auto created = ssg::EditorSession::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"search.workspace", runtime.revision(), std::string{"needle"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"follow_edits.pause", runtime.revision(), {}}).accepted());
    auto snapshot = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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

    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
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
    auto snapshot = runtime.present(ssg::ClientId{1}, dimensions);
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
    snapshot = runtime.present(ssg::ClientId{1}, dimensions);
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) {
        ASSERT_EQ(snapshot->sections().followEdits.mode,
                  ssg::FollowMode::Paused);
    }
}

TEST(attachedClientsShareFollowPauseQueueAndResumeState) {
    auto runtime = followPauseRuntime("alpha\nbeta\n");
    if (!runtime) return;
    ssg::test::GridTestView firstGrid{
        ssg::ClientId{1}, ssg::ViewId{1}, {80, 20}};
    ssg::test::GridTestView secondGrid{
        ssg::ClientId{2}, ssg::ViewId{1}, {80, 20}};

    ASSERT_TRUE(firstGrid
                    .dispatch(*runtime,
                              {"view.scroll_lines", runtime->revision(),
                               ssg::ScrollLinesArguments{3}})
                    .accepted());
    ASSERT_TRUE(secondGrid
                    .dispatch(*runtime,
                              {"view.scroll_lines", runtime->revision(),
                               ssg::ScrollLinesArguments{10}})
                    .accepted());

    auto paused = runtime->present(ssg::ClientId{1},
                                    ssg::ViewportDimensions{80, 20});
    ASSERT_TRUE(paused.has_value());
    if (!paused) return;
    ASSERT_EQ(paused->sections().followEdits.mode, ssg::FollowMode::Paused);
    ASSERT_EQ(paused->sections().followEdits.clients.size(), std::size_t{3});

    auto const sourceRevision = runtime->revision().value();
    ASSERT_TRUE(runtime
                    ->applyExternalDiffBurst(
                        {{{.kind = ssg::NonGitDiffEventKind::Create,
                           .id = ssg::DiffFileId{"watched-a"},
                           .path = "watched-a.txt",
                           .baselineContent = "",
                           .targetContent = "first\nbeta\n"},
                          ssg::Revision{sourceRevision + 1}},
                         {{.kind = ssg::NonGitDiffEventKind::Create,
                           .id = ssg::DiffFileId{"watched-b"},
                           .path = "watched-b.txt",
                           .baselineContent = "",
                           .targetContent = "newest\n"},
                          ssg::Revision{sourceRevision + 2}}})
                    .accepted());

    paused = runtime->present(ssg::ClientId{2},
                               ssg::ViewportDimensions{80, 20});
    ASSERT_TRUE(paused.has_value());
    if (!paused) return;
    ASSERT_EQ(paused->sections().followEdits.mode, ssg::FollowMode::Paused);
    ASSERT_EQ(paused->sections().followEdits.queuedTargets.size(),
              std::size_t{2});

    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{2},
                               {"follow_edits.resume", runtime->revision(), {}})
                    .accepted());
    auto first = runtime->present(ssg::ClientId{1},
                                   ssg::ViewportDimensions{80, 20});
    auto second = runtime->present(ssg::ClientId{2},
                                    ssg::ViewportDimensions{80, 20});
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    if (!first || !second) return;
    ASSERT_EQ(first->sections().followEdits.mode, ssg::FollowMode::Following);
    ASSERT_EQ(second->sections().followEdits.mode, ssg::FollowMode::Following);
    ASSERT_TRUE(first->sections().followEdits.activeTarget.has_value());
    if (!first->sections().followEdits.activeTarget) return;
    ASSERT_EQ(first->sections().followEdits.activeTarget->id,
              ssg::DiffFileId{"watched-b"});
    ASSERT_EQ(first->sections().followEdits.activeTarget,
              second->sections().followEdits.activeTarget);
}

TEST(gitDiffScanUpdatesDiffAndRejectsStaleBatches) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "a.txt"} << "a\n";
    std::ofstream{root / "workspace" / "b.txt"} << "b\n";

    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
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

    auto snapshot = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
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

    auto snapshot = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
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
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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

    auto first = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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

    auto second = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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

TEST(gitStatusSurvivesDetailedDiffWorkLimit) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "large.txt"} << "working file\n";
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());

    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = ssg::Revision{24},
                         .baselineIdentity = "head-limit:index-1",
                         .files =
                             {
                                 {.id = ssg::DiffFileId{"large-id"},
                                  .path = "large.txt",
                                  .baselineContent = overDiffLineBudget('a'),
                                  .workingContent = overDiffLineBudget('b')},
                                 {.id = ssg::DiffFileId{"small-id"},
                                  .path = "small.txt",
                                  .baselineContent = std::string{"before\n"},
                                  .workingContent = std::string{"after\n"}},
                             }})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"panel.show_git_status", runtime.revision(), {}})
                    .accepted());

    auto snapshot =
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto* git =
        findProvider(snapshot->sections().tree, ssg::TreeProviderKind::Git);
    ASSERT_TRUE(git != nullptr);
    if (!git) return;
    ASSERT_EQ(gitProviderStatuses(*git).size(), std::size_t{2});
    ASSERT_EQ(snapshot->sections().diff.files.size(), std::size_t{1});
    ASSERT_EQ(snapshot->sections().diff.files.front().id,
              ssg::DiffFileId{"small-id"});
    const auto liveDiffsBeforeActivation =
        countTabsOfKind(snapshot->sections().tabs, ssg::TabKind::LiveDiff);

    std::optional<ssg::TreeNodeId> largeNode;
    for (const auto& node : git->nodes) {
        if (node.node.workspacePath == std::optional<std::string>{"large.txt"}) {
            largeNode = node.node.id;
        }
    }
    ASSERT_TRUE(largeNode.has_value());
    if (!largeNode) return;
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tree.activate_node", runtime.revision(),
                               ssg::TreeSelectArguments{*largeNode}})
                    .accepted());
    auto opened =
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(opened.has_value());
    if (!opened) return;
    ASSERT_EQ(countTabsOfKind(opened->sections().tabs, ssg::TabKind::Document),
              std::size_t{1});
    ASSERT_EQ(countTabsOfKind(opened->sections().tabs, ssg::TabKind::LiveDiff),
              liveDiffsBeforeActivation);
    ASSERT_EQ(opened->sections().document.text, std::string{"working file\n"});

    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = ssg::Revision{25},
                         .baselineIdentity = "head-limit:index-2",
                         .files =
                             {
                                 {.id = ssg::DiffFileId{"large-id"},
                                  .path = "large.txt",
                                  .baselineContent = overDiffLineBudget('a'),
                                  .workingContent = overDiffLineBudget('b')},
                                 {.id = ssg::DiffFileId{"small-id"},
                                  .path = "small.txt",
                                  .baselineContent = overDiffLineBudget('a'),
                                  .workingContent = overDiffLineBudget('b')},
                             }})
                    .accepted());
    auto transitioned =
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(transitioned.has_value());
    if (!transitioned) return;
    auto* transitionedGit =
        findProvider(transitioned->sections().tree, ssg::TreeProviderKind::Git);
    ASSERT_TRUE(transitionedGit != nullptr);
    if (!transitionedGit) return;
    ASSERT_EQ(gitProviderStatuses(*transitionedGit).size(), std::size_t{2});
    ASSERT_TRUE(transitioned->sections().diff.files.empty());
    ASSERT_EQ(
        countTabsOfKind(transitioned->sections().tabs, ssg::TabKind::LiveDiff),
        std::size_t{0});
}

TEST(gitStatusActivationOpensLiveDiffTabAndReusesIt) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "coexist.txt"} << "disk\n";
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
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

    auto first = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
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

    auto first = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
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
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
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
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(afterUser.has_value());
    if (!afterUser) return;
    ASSERT_EQ(afterUser->sections().followEdits.mode, ssg::FollowMode::Paused);
}

TEST(tabSwitchPausesFollowViaNavigationPath) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "other.txt"} << "other\n";
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
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
    const auto makeRuntime = []() -> std::unique_ptr<ssg::EditorSession> {
        auto root = uniqueRoot();
        auto created = ssg::EditorSession::create(
            {root / "workspace", root / "scratch", root / "recovery"});
        ASSERT_TRUE(created.accepted());
        if (!created.accepted()) return nullptr;
        auto runtime = std::move(created.session);
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
    const auto followState = [](ssg::EditorSession& runtime) {
        auto snapshot =
            runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
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
        ssg::test::GridTestView grid{
            ssg::ClientId{1}, ssg::ViewId{1}, {80, 20}};
        ASSERT_TRUE(grid
                        .dispatch(*runtime,
                                  {"pane.next", runtime->revision(), {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ssg::test::GridTestView grid{
            ssg::ClientId{1}, ssg::ViewId{1}, {80, 20}};
        ASSERT_TRUE(grid
                        .dispatch(*runtime,
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
    auto created = ssg::EditorSession::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.open", runtime.revision(), {}}).accepted());
    auto snapshot = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().focus, ssg::FocusTarget::Prompt);
    ASSERT_FALSE(snapshot->sections().palette.commandCandidates.empty());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.close", runtime.revision(), {}}).accepted());
    auto closed = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(closed.has_value());
    if (!closed) return;
    ASSERT_EQ(closed->sections().focus, ssg::FocusTarget::Editor);
}

// The open-picker kind is derived from the prompt after every dispatch rather
// than cleared at each close path.  Pin that across every way a picker closes:
// a stale kind would make the NEXT open publish the previous picker's mode and
// candidates, which is invisible while only one picker exists and wrong the
// moment a second one lands.
TEST(everyPaletteClosePathLeavesNoOpenPickerBehind) {
    auto root = uniqueRoot();
    auto created = ssg::EditorSession::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    auto pickerStateAfter = [&](std::string const& closeCommand) {
        ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.open", runtime.revision(), {}}).accepted());
        auto open = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(open.has_value());
        if (open) {
            ASSERT_EQ(activePickerMode(open->sections().palette),
                      std::optional<ssg::SearchMode>{ssg::SearchMode::Command});
        }
        (void)runtime.dispatch(ssg::ClientId{1}, {closeCommand, runtime.revision(), {}});
        auto shut = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(shut.has_value());
        if (shut) {
            ASSERT_FALSE(shut->sections().palette.activePicker.has_value());
        }
    };

    pickerStateAfter("palette.close");
    pickerStateAfter("prompt.cancel");

    // A successful palette.execute cancels the prompt as part of executing; the
    // picker must not survive into the next open.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.open", runtime.revision(), {}}).accepted());
    (void)runtime.dispatch(
        ssg::ClientId{1},
        {"palette.execute", runtime.revision(), ssg::PaletteExecuteArguments{"edit.undo"}});
    auto executed = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(executed.has_value());
    if (executed) {
        ASSERT_FALSE(executed->sections().palette.activePicker.has_value());
    }
}

// The file picker publishes paths, not command ids, so palette.execute must
// refuse to run while it is open.  Before the picker existed, the guard checked
// only that a Palette-kind prompt was active, which every picker satisfies.
TEST(filePickerPublishesWorkspaceFilesAndRejectsPaletteExecute) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace / "src");
    std::ofstream{workspace / "alpha.txt"} << "a\n";
    std::ofstream{workspace / "src" / "beta.cpp"} << "b\n";
    // The test root lives inside SSG's own repository, whose .gitignore covers
    // it; give the workspace its own repository so the picker's ignore rules are
    // the fixture's, not the enclosing checkout's.
    ASSERT_EQ(std::system(("git -C \"" + workspace.string() + "\" init -q >/dev/null 2>&1").c_str()), 0);

    auto created = ssg::EditorSession::create({workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    auto closed =
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(closed.has_value());
    if (!closed) return;
    ASSERT_FALSE(closed->sections().palette.activePicker.has_value());
    ASSERT_FALSE(closed->sections().palette.fileCandidates.empty());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file_finder.open", runtime.revision(), {}}).accepted());
    auto snapshot = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    auto const& palette = snapshot->sections().palette;
    ASSERT_EQ(activePickerMode(palette),
              std::optional<ssg::SearchMode>{ssg::SearchMode::File});
    std::set<std::string> paths;
    for (auto const& candidate : palette.fileCandidates) paths.insert(candidate.id);
    ASSERT_TRUE(paths.contains("alpha.txt"));
    ASSERT_TRUE(paths.contains("src/beta.cpp"));

    // Candidate ids are paths here; executing one as a command must be refused
    // even though a Palette prompt is genuinely open.
    auto rejected = runtime.dispatch(
        ssg::ClientId{1},
        {"palette.execute", runtime.revision(),
         ssg::PaletteExecuteArguments{"src/beta.cpp"}});
    ASSERT_FALSE(rejected.accepted());
    // Not even a real command id is executable through the file picker.
    auto alsoRejected = runtime.dispatch(
        ssg::ClientId{1},
        {"palette.execute", runtime.revision(),
         ssg::PaletteExecuteArguments{"edit.undo"}});
    ASSERT_FALSE(alsoRejected.accepted());
    std::filesystem::remove_all(root);
}

// Toggling the setting with the picker already open must re-walk; otherwise the
// command appears to do nothing until the picker is closed and reopened.
TEST(togglingGitignoreRebuildsTheOpenFilePickerIndex) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace / "build");
    std::ofstream{workspace / ".gitignore"} << "build/\n";
    std::ofstream{workspace / "kept.txt"} << "k\n";
    std::ofstream{workspace / "build" / "hidden.o"} << "h\n";
    ASSERT_EQ(std::system(("git -C \"" + workspace.string() + "\" init -q >/dev/null 2>&1").c_str()), 0);

    auto created = ssg::EditorSession::create({workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    auto candidateIds = [&] {
        auto snapshot = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        std::set<std::string> paths;
        if (snapshot) {
            for (auto const& candidate :
                 snapshot->sections().palette.fileCandidates) {
                paths.insert(candidate.id);
            }
        }
        return paths;
    };

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file_finder.open", runtime.revision(), {}}).accepted());
    auto before = candidateIds();
    ASSERT_TRUE(before.contains("kept.txt"));
    ASSERT_FALSE(before.contains("build/hidden.o"));

    // No reopen in between: the SAME open picker must change.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file_finder.toggle_gitignore", runtime.revision(), {}}).accepted());
    auto after = candidateIds();
    ASSERT_TRUE(after.contains("build/hidden.o"));
    std::filesystem::remove_all(root);
}

TEST(workerFilesystemRefreshPublishesChangedFileCandidates) {
    auto root = uniqueRoot();
    ASSERT_EQ(std::system(("git -C \"" + (root / "workspace").string() +
                           "\" init -q >/dev/null 2>&1")
                              .c_str()),
              0);
    auto created = ssg::EditorSession::create(
        {.cwd = root / "workspace",
         .scratchRoot = root / "scratch",
         .recoveryRoot = root / "recovery",
         .enableGitDiffWorker = false,
         .enableFilesystemWatcher = false});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(
        runtime
            .attach({ssg::ClientId{1}, ssg::InvocationOrigin::Websocket},
                    ssg::ViewId{1})
            .accepted());
    const auto before = runtime.revision();
    std::ofstream{root / "workspace" / "arrived.txt"} << "new\n";

    runtime.refreshFilesystemForTest();

    ASSERT_TRUE(runtime.revision() > before);
    auto snapshot =
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_TRUE(std::ranges::any_of(
        snapshot->sections().palette.fileCandidates,
        [](const auto& candidate) { return candidate.id == "arrived.txt"; }));
}

// Submitting from the file picker closes it ONLY when the open succeeds, so a
// file removed between the walk and the submit leaves the picker up with its
// query intact rather than silently dropping the user back to the editor.
// Server-owned so keyboard and pointer submits cannot drift apart.
TEST(filePickerClosesOnSuccessfulOpenAndStaysOpenOnFailure) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "present.txt"} << "p\n";
    ASSERT_EQ(std::system(("git -C \"" + workspace.string() + "\" init -q >/dev/null 2>&1").c_str()), 0);

    auto created = ssg::EditorSession::create({workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    auto pickerIsOpen = [&] {
        auto snapshot = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        return snapshot &&
               activePickerMode(snapshot->sections().palette) ==
                   std::optional<ssg::SearchMode>{ssg::SearchMode::File};
    };

    // A rejected open leaves the picker up.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file_finder.open", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(pickerIsOpen());
    auto missing = runtime.dispatch(
        ssg::ClientId{1},
        {"picker.submit", runtime.revision(),
         pickerSubmit(runtime, ssg::SearchMode::File, "gone.txt")});
    ASSERT_FALSE(missing.accepted());
    ASSERT_TRUE(pickerIsOpen());

    // A successful open dismisses it.
    ASSERT_TRUE(
        runtime
            .dispatch(ssg::ClientId{1},
                      {"picker.submit", runtime.revision(),
                       pickerSubmit(runtime, ssg::SearchMode::File,
                                    "present.txt")})
            .accepted());
    ASSERT_FALSE(pickerIsOpen());
    std::filesystem::remove_all(root);
}

TEST(websocketPickerSubmissionRequiresAndClosesTheAuthoritativePicker) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::ofstream{workspace / "present.txt"} << "present\n";
    ASSERT_EQ(std::system(("git -C \"" + workspace.string() +
                           "\" init -q >/dev/null 2>&1")
                              .c_str()),
              0);
    auto created = ssg::EditorSession::create(
        {workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .attach({ssg::ClientId{1}, ssg::InvocationOrigin::Websocket},
                            ssg::ViewId{1})
                    .accepted());

    // A transport cannot submit against an inventory without first opening the
    // matching authoritative picker.
    ASSERT_FALSE(
        runtime
            .dispatch(ssg::ClientId{1},
                      {"picker.submit", runtime.revision(),
                       ssg::PickerSubmitArguments{
                           {ssg::SearchMode::Command,
                            ssg::PickerActivationId{1}},
                           "panel.toggle"}})
            .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"palette.open", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(
        runtime
            .dispatch(ssg::ClientId{1},
                      {"picker.submit", runtime.revision(),
                       pickerSubmit(runtime, ssg::SearchMode::Command,
                                    "panel.toggle")})
            .accepted());
    auto commandSubmitted =
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(commandSubmitted.has_value());
    if (!commandSubmitted) return;
    ASSERT_FALSE(commandSubmitted->sections().palette.activePicker.has_value());
    ASSERT_TRUE(commandSubmitted->sections().focus == ssg::FocusTarget::Panel);

    ASSERT_TRUE(runtime
            .dispatch(ssg::ClientId{1},
                             {"file_finder.open", runtime.revision(), {}})
            .accepted());
    ASSERT_TRUE(
        runtime
            .dispatch(ssg::ClientId{1},
                      {"picker.submit", runtime.revision(),
                       pickerSubmit(runtime, ssg::SearchMode::File,
                                    "present.txt")})
            .accepted());
    auto submitted =
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(submitted.has_value());
    if (!submitted) return;
    ASSERT_FALSE(submitted->sections().palette.activePicker.has_value());
    ASSERT_TRUE(submitted->sections().focus == ssg::FocusTarget::Panel);
}

TEST(commandPickerSubmissionHasOriginParity) {
    for (auto const origin : {ssg::InvocationOrigin::InProcess,
                              ssg::InvocationOrigin::Websocket}) {
        auto root = uniqueRoot();
        auto created = ssg::EditorSession::create(
            {root / "workspace", root / "scratch", root / "recovery"});
        ASSERT_TRUE(created.accepted());
        if (!created.accepted()) return;
        auto& runtime = *created.session;
        ASSERT_TRUE(
            runtime.attach({ssg::ClientId{1}, origin}, ssg::ViewId{1}).accepted());

        ASSERT_TRUE(
            runtime
                .dispatch(ssg::ClientId{1},
                          {"file_finder.open", runtime.revision(), {}})
                .accepted());
        ASSERT_FALSE(
            runtime
                .dispatch(ssg::ClientId{1},
                          {"picker.submit", runtime.revision(),
                           pickerSubmit(runtime, ssg::SearchMode::Command,
                                        "panel.toggle")})
                .accepted());
        auto snapshot =
            runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return;
        ASSERT_TRUE(activePickerMode(snapshot->sections().palette) ==
                    std::optional<ssg::SearchMode>{ssg::SearchMode::File});

        ASSERT_TRUE(
            runtime
                .dispatch(ssg::ClientId{1},
                          {"palette.close", runtime.revision(), {}})
                .accepted());
        ASSERT_TRUE(runtime
                        .dispatch(ssg::ClientId{1},
                                  {"palette.open", runtime.revision(), {}})
                        .accepted());
        ASSERT_TRUE(
            runtime
                .dispatch(ssg::ClientId{1},
                          {"picker.submit", runtime.revision(),
                           pickerSubmit(runtime, ssg::SearchMode::Command,
                                        "panel.toggle")})
                .accepted());
        snapshot =
            runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return;
        ASSERT_FALSE(snapshot->sections().palette.activePicker.has_value());
        std::filesystem::remove_all(root);
    }
}

TEST(commandPickerActionThatOpensPromptDismissesPickerWithoutFailure) {
    auto root = uniqueRoot();
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(
        runtime
            .attach({ssg::ClientId{1}, ssg::InvocationOrigin::Websocket},
                    ssg::ViewId{1})
            .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"palette.open", runtime.revision(), {}})
                    .accepted());

    auto result = runtime.dispatch(
        ssg::ClientId{1},
        {"picker.submit", runtime.revision(),
         pickerSubmit(runtime, ssg::SearchMode::Command, "goto.line")});
    ASSERT_TRUE(result.accepted());
    auto snapshot =
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_FALSE(snapshot->sections().palette.activePicker.has_value());
    ASSERT_TRUE(snapshot->presentation()->prompt.has_value());
    if (snapshot->presentation()->prompt) {
        ASSERT_EQ(snapshot->presentation()->prompt->kind,
                  ssg::PromptKind::CommandArgument);
    }
}

TEST(pickerSubmissionUsesActivationIdentityInsteadOfGlobalRevision) {
    auto root = uniqueRoot();
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    const auto catalog = runtime.commandCatalog();
    const auto commands = catalog->commands();
    const auto stateValidated = std::count_if(
        commands.begin(), commands.end(),
        [](const auto* command) {
            return command->revisionPolicy ==
                   ssg::CommandRevisionPolicy::StateValidated;
        });
    ASSERT_EQ(stateValidated, std::ptrdiff_t{1});
    ASSERT_TRUE(catalog->find("picker.submit")->revisionPolicy ==
                ssg::CommandRevisionPolicy::StateValidated);
    ASSERT_TRUE(
        runtime
            .attach({ssg::ClientId{1}, ssg::InvocationOrigin::Websocket},
                    ssg::ViewId{1})
            .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"palette.open", runtime.revision(), {}})
                    .accepted());
    auto snapshot =
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot || !snapshot->sections().palette.activePicker) return;
    const auto first = *snapshot->sections().palette.activePicker;
    const auto staleSessionRevision = runtime.revision();

    ASSERT_TRUE(
        runtime
            .dispatch(ssg::ClientId{1},
                      {"panel.toggle", runtime.revision(), {}})
            .accepted());
    ASSERT_TRUE(
        runtime
            .dispatch(ssg::ClientId{1},
                      {"picker.submit", staleSessionRevision,
                       ssg::PickerSubmitArguments{first, "panel.toggle"}})
            .accepted());

    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"palette.open", runtime.revision(), {}})
                    .accepted());
    snapshot =
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot || !snapshot->sections().palette.activePicker) return;
    const auto second = *snapshot->sections().palette.activePicker;
    ASSERT_FALSE(second.id == first.id);
    ASSERT_FALSE(
        runtime
            .dispatch(ssg::ClientId{1},
                      {"picker.submit", runtime.revision(),
                       ssg::PickerSubmitArguments{first, "panel.toggle"}})
            .accepted());
    snapshot =
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().palette.activePicker,
              std::optional<ssg::PickerActivation>{second});
}

TEST(simpleSemanticInputsLowerThroughAuthoritativeTransactions) {
    auto root = uniqueRoot();
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    const ssg::ClientId client{1};
    const ssg::ViewportDimensions viewport{80, 24};
    ASSERT_TRUE(runtime
                    .attach({client, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(client, {"file.open", runtime.revision(),
                                       std::string{"needle.txt"}})
                    .accepted());
    auto document = runtime.present(client, viewport);
    ASSERT_TRUE(document.has_value());
    if (!document || !document->sections().tabs.active) return;
    const auto documentTab = *document->sections().tabs.active;

    ASSERT_TRUE(
        runtime.dispatch(client, {"file.new", runtime.revision(), {}}).accepted());
    auto scratch = runtime.present(client, viewport);
    ASSERT_TRUE(scratch.has_value());
    if (!scratch || !scratch->sections().tabs.active) return;
    const auto scratchTab = *scratch->sections().tabs.active;
    auto activate = runtime.input(
        client, ssg::TabPointerInput{{scratch->revision()}, documentTab});
    ASSERT_EQ(activate.outcome, ssg::ClientInputOutcome::Dispatched);
    ASSERT_TRUE(activate.command.has_value() && activate.command->accepted());
    ASSERT_EQ(runtime.present(client, viewport)->sections().tabs.active,
              std::optional<ssg::TabId>{documentTab});

    auto beforeStale = runtime.present(client, viewport);
    ASSERT_TRUE(beforeStale.has_value());
    if (!beforeStale) return;
    auto stale = runtime.input(
        client,
        ssg::TabPointerInput{
            {ssg::Revision{beforeStale->revision().value() - 1}}, scratchTab});
    ASSERT_EQ(stale.outcome, ssg::ClientInputOutcome::Rejected);
    ASSERT_TRUE(stale.command.has_value());
    ASSERT_EQ(stale.command->error, ssg::CommandError::StaleRevision);
    ASSERT_EQ(runtime.present(client, viewport)->sections().tabs.active,
              beforeStale->sections().tabs.active);

    auto closeBasis = runtime.present(client, viewport);
    ASSERT_TRUE(closeBasis.has_value());
    if (!closeBasis) return;
    auto close = runtime.input(
        client,
        ssg::TabPointerInput{{closeBasis->revision()}, scratchTab,
                             ssg::InputPointerButton::Auxiliary});
    ASSERT_TRUE(close.command.has_value() && close.command->accepted());
    auto afterClose = runtime.present(client, viewport);
    ASSERT_TRUE(afterClose.has_value());
    ASSERT_TRUE(std::none_of(
        afterClose->sections().tabs.tabs.begin(),
        afterClose->sections().tabs.tabs.end(),
        [&](const ssg::TabState& tab) { return tab.id == scratchTab; }));

    ASSERT_TRUE(runtime
                    .dispatch(client,
                              {"palette.open", runtime.revision(), {}})
                    .accepted());
    auto palette = runtime.present(client, viewport);
    ASSERT_TRUE(palette.has_value());
    if (!palette || !palette->sections().palette.activePicker) return;
    const auto activation = *palette->sections().palette.activePicker;
    auto submit = runtime.input(
        client,
        ssg::PickerPointerInput{activation, "panel.toggle"});
    ASSERT_TRUE(submit.command.has_value() && submit.command->accepted());
    auto afterSubmit = runtime.present(client, viewport);
    ASSERT_TRUE(afterSubmit.has_value());
    ASSERT_FALSE(afterSubmit->sections().palette.activePicker.has_value());
    ASSERT_TRUE(afterSubmit->presentation()->shell.panel.has_value());

    auto const actionNode = std::find_if(
        afterSubmit->sections().uiState.nodes.begin(),
        afterSubmit->sections().uiState.nodes.end(), [](auto const& node) {
            return node.leaf && node.leaf->command &&
                   !node.leaf->command->empty();
        });
    ASSERT_TRUE(actionNode != afterSubmit->sections().uiState.nodes.end());
    if (actionNode == afterSubmit->sections().uiState.nodes.end()) return;
    const auto revisionBeforeInvalid = runtime.revision();
    auto invalidUiAction = runtime.input(
        client, ssg::PublishedUiActionPointerInput{
                    {revisionBeforeInvalid},
                    afterSubmit->sections().uiState.generation,
                    ssg::UiNodeId{"missing.action"}});
    ASSERT_EQ(invalidUiAction.outcome, ssg::ClientInputOutcome::Rejected);
    ASSERT_EQ(runtime.revision(), revisionBeforeInvalid);

    auto publishedAction = runtime.input(
        client, ssg::PublishedUiActionPointerInput{
                    {runtime.revision()},
                    afterSubmit->sections().uiState.generation,
                    actionNode->id});
    ASSERT_TRUE(publishedAction.command.has_value() &&
                publishedAction.command->accepted());

    for (auto target : {ssg::SemanticScrollTarget::Document,
                        ssg::SemanticScrollTarget::Tree}) {
        auto scroll = runtime.input(
            client, ssg::ScrollLinesInput{
                        {runtime.revision()}, target, 1});
        ASSERT_TRUE(scroll.command.has_value() && scroll.command->accepted());
        auto fraction = runtime.input(
            client, ssg::ScrollFractionInput{
                        {runtime.revision()}, target, 0, 1});
        ASSERT_TRUE(fraction.command.has_value() &&
                    fraction.command->accepted());
    }
    const auto beforeInvalidScroll = runtime.revision();
    auto invalidScroll = runtime.input(
        client, ssg::ScrollFractionInput{
                    {beforeInvalidScroll},
                    ssg::SemanticScrollTarget::Document, 2, 1});
    ASSERT_EQ(invalidScroll.outcome, ssg::ClientInputOutcome::Rejected);
    ASSERT_EQ(runtime.revision(), beforeInvalidScroll);

    ASSERT_TRUE(runtime
                    .dispatch(client,
                              {"replace.open", runtime.revision(), {}})
                    .accepted());
    auto replace = runtime.present(client, viewport);
    ASSERT_TRUE(replace.has_value() &&
                replace->sections().promptView.has_value());
    if (!replace || !replace->sections().promptView) return;
    auto focusReplacement = runtime.input(
        client, ssg::PromptControlPointerInput{
                    {replace->revision()}, "replace.replacement"});
    ASSERT_TRUE(focusReplacement.command.has_value() &&
                focusReplacement.command->accepted());
    auto focused = runtime.present(client, viewport);
    ASSERT_TRUE(focused.has_value() &&
                focused->sections().promptView.has_value());
    if (focused && focused->sections().promptView) {
        ASSERT_EQ(focused->sections().promptView->activeInput, std::size_t{1});
    }
}

TEST(documentPointerInputOwnsSelectionGesturePolicy) {
    auto root = uniqueRoot();
    std::filesystem::create_directories(root / "workspace");
    std::ofstream{root / "workspace" / "words.txt"} << "alpha beta gamma\n";
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    const ssg::ClientId client{1};
    ASSERT_TRUE(runtime
                    .attach({client, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(client, {"file.open", runtime.revision(),
                                       std::string{"words.txt"}})
                    .accepted());

    auto press = runtime.input(
        client, ssg::DocumentPointerInput{
                    {runtime.revision()}, ssg::ByteOffset{1}});
    ASSERT_TRUE(press.command.has_value() && press.command->accepted());
    auto move = runtime.input(
        client, ssg::DocumentPointerInput{
                    {runtime.revision()}, ssg::ByteOffset{9}, false, false,
                    ssg::InputPointerButton::Primary,
                    ssg::InputPointerPhase::Move});
    ASSERT_TRUE(move.command.has_value() && move.command->accepted());
    auto snapshot = runtime.snapshot(client);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().selection.primary().anchor.byteOffset,
              ssg::ByteOffset{1});
    ASSERT_EQ(snapshot->sections().selection.primary().active.byteOffset,
              ssg::ByteOffset{9});

    auto release = runtime.input(
        client, ssg::DocumentPointerInput{
                    {runtime.revision()}, std::nullopt, false, false,
                    ssg::InputPointerButton::Primary,
                    ssg::InputPointerPhase::Release});
    ASSERT_EQ(release.outcome, ssg::ClientInputOutcome::Dispatched);
    auto strayMove = runtime.input(
        client, ssg::DocumentPointerInput{
                    {runtime.revision()}, ssg::ByteOffset{4}, false, false,
                    ssg::InputPointerButton::Primary,
                    ssg::InputPointerPhase::Move});
    ASSERT_EQ(strayMove.outcome, ssg::ClientInputOutcome::Unhandled);

    ASSERT_TRUE(runtime
                    .input(client, ssg::DocumentPointerInput{
                                       {runtime.revision()},
                                       ssg::ByteOffset{3}})
                    .command->accepted());
    auto stalePress = runtime.input(
        client, ssg::DocumentPointerInput{
                    {ssg::Revision{runtime.revision().value() - 1}},
                    ssg::ByteOffset{4}});
    ASSERT_EQ(stalePress.outcome, ssg::ClientInputOutcome::Rejected);
    auto moveAfterStalePress = runtime.input(
        client, ssg::DocumentPointerInput{
                    {runtime.revision()}, ssg::ByteOffset{5}, false, false,
                    ssg::InputPointerButton::Primary,
                    ssg::InputPointerPhase::Move});
    ASSERT_EQ(moveAfterStalePress.outcome,
              ssg::ClientInputOutcome::Unhandled);

    ASSERT_TRUE(runtime
                    .input(client, ssg::DocumentPointerInput{
                                       {runtime.revision()},
                                       ssg::ByteOffset{3}})
                    .command->accepted());
    auto staleCancel = runtime.input(
        client, ssg::DocumentPointerInput{
                    {ssg::Revision{runtime.revision().value() - 1}},
                    std::nullopt, false, false,
                    ssg::InputPointerButton::Primary,
                    ssg::InputPointerPhase::Cancel});
    ASSERT_EQ(staleCancel.outcome, ssg::ClientInputOutcome::Rejected);
    auto moveAfterStaleCancel = runtime.input(
        client, ssg::DocumentPointerInput{
                    {runtime.revision()}, ssg::ByteOffset{5}, false, false,
                    ssg::InputPointerButton::Primary,
                    ssg::InputPointerPhase::Move});
    ASSERT_EQ(moveAfterStaleCancel.outcome,
              ssg::ClientInputOutcome::Unhandled);

    ASSERT_TRUE(runtime
                    .dispatch(client,
                              {"cursor.set_position", runtime.revision(),
                               ssg::SelectionCommandArguments{
                                   ssg::SelectionNavigator::resolvePosition(
                                       runtime.activeDocumentText(),
                                       ssg::ByteOffset{2}),
                                   std::nullopt}})
                    .accepted());
    const auto second = ssg::SelectionNavigator::resolvePosition(
        runtime.activeDocumentText(), ssg::ByteOffset{12});
    ASSERT_TRUE(second.has_value());
    if (!second) return;
    ASSERT_TRUE(runtime
                    .dispatch(client,
                              {"select.add_range", runtime.revision(),
                               ssg::SelectionCommandArguments{
                                   std::nullopt,
                                   ssg::Selection{*second, *second}}})
                    .accepted());
    auto toggle = runtime.input(
        client, ssg::DocumentPointerInput{
                    {runtime.revision()}, ssg::ByteOffset{2}, true});
    ASSERT_TRUE(toggle.command.has_value() && toggle.command->accepted());
    snapshot = runtime.snapshot(client);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().selection.items().size(), std::size_t{1});
    ASSERT_EQ(snapshot->sections().selection.primary().anchor.byteOffset,
              ssg::ByteOffset{12});

    auto word = runtime.input(
        client, ssg::DocumentPointerInput{
                    {runtime.revision()}, ssg::ByteOffset{7}, false, true});
    ASSERT_TRUE(word.command.has_value() && word.command->accepted());
    snapshot = runtime.snapshot(client);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().selection.primary().lower().byteOffset,
              ssg::ByteOffset{6});
    ASSERT_EQ(snapshot->sections().selection.primary().upper().byteOffset,
              ssg::ByteOffset{10});

    ASSERT_TRUE(runtime
                    .input(client, ssg::DocumentPointerInput{
                                       {runtime.revision()},
                                       ssg::ByteOffset{0}})
                    .command->accepted());
    ASSERT_TRUE(runtime
                    .dispatch(client,
                              {"text.insert", runtime.revision(),
                               ssg::TextInputArguments{"X"}})
                    .accepted());
    const auto editedDocumentRevision = runtime.revision();
    auto editedDocumentMove = runtime.input(
        client, ssg::DocumentPointerInput{
                    {editedDocumentRevision}, ssg::ByteOffset{1}, false, false,
                    ssg::InputPointerButton::Primary,
                    ssg::InputPointerPhase::Move});
    ASSERT_EQ(editedDocumentMove.outcome,
              ssg::ClientInputOutcome::Rejected);
    ASSERT_EQ(runtime.revision(), editedDocumentRevision);

    ASSERT_TRUE(runtime
                    .input(client, ssg::DocumentPointerInput{
                                       {runtime.revision()},
                                       ssg::ByteOffset{0}})
                    .command->accepted());
    ASSERT_TRUE(runtime
                    .dispatch(client,
                              {"file.new", runtime.revision(), {}})
                    .accepted());
    const auto changedDocumentRevision = runtime.revision();
    auto changedDocumentMove = runtime.input(
        client, ssg::DocumentPointerInput{
                    {changedDocumentRevision}, ssg::ByteOffset{0}, false, false,
                    ssg::InputPointerButton::Primary,
                    ssg::InputPointerPhase::Move});
    ASSERT_EQ(changedDocumentMove.outcome,
              ssg::ClientInputOutcome::Rejected);
    ASSERT_EQ(runtime.revision(), changedDocumentRevision);
}

TEST(documentEdgeMovesExtendAndRevealInOneAuthoritativeTransition) {
    auto root = uniqueRoot();
    std::filesystem::create_directories(root / "workspace");
    std::ofstream{root / "workspace" / "lines.txt"}
        << "aa\nbb\ncc\ndd\nee\nff\n";
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    const ssg::ClientId client{1};
    ssg::test::GridTestView grid{client, ssg::ViewId{1}, {20, 4}};
    ASSERT_TRUE(runtime
                    .attach({client, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(client, {"file.open", runtime.revision(),
                                       std::string{"lines.txt"}})
                    .accepted());
    ASSERT_TRUE(grid.present(runtime).has_value());
    ASSERT_TRUE(runtime
                    .input(client, ssg::DocumentPointerInput{
                                       {runtime.revision()},
                                       ssg::ByteOffset{1}})
                    .command->accepted());

    for (const auto expected :
         {ssg::ByteOffset{4}, ssg::ByteOffset{7}, ssg::ByteOffset{10}}) {
        auto edge = runtime.input(
            client, ssg::DocumentPointerInput{
                        {runtime.revision()}, std::nullopt, false, false,
                        ssg::InputPointerButton::Primary,
                        ssg::InputPointerPhase::Move,
                        ssg::DocumentPointerEdge::After});
        ASSERT_TRUE(edge.command.has_value() && edge.command->accepted());
        auto snapshot = grid.present(runtime);
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return;
        ASSERT_EQ(snapshot->sections().selection.primary().anchor.byteOffset,
                  ssg::ByteOffset{1});
        ASSERT_EQ(snapshot->sections().selection.primary().active.byteOffset,
                  expected);
    }
    auto snapshot = grid.present(runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) {
        ASSERT_TRUE(
            snapshot->presentation()->viewport.firstVisualRow > 0);
    }
}

TEST(everySemanticPointerRouteHasAnAuthoritativeKeyboardPath) {
    auto root = uniqueRoot();
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    const ssg::ClientId client{1};
    ASSERT_TRUE(runtime
                    .attach({client, ssg::InvocationOrigin::InProcess},
                            ssg::ViewId{1})
                    .accepted());
    auto snapshot = runtime.snapshot(client);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    std::set<std::string> bound;
    for (const auto& binding : snapshot->sections().keymap.bindings) {
        bound.insert(binding.commandId);
    }
    const auto routes = allKeyboardRoutes(std::make_index_sequence<
                                          std::variant_size_v<
                                              ssg::ClientInput>>{});
    for (const auto command : routes) {
        if (!bound.contains(std::string{command})) {
            std::cerr << "  pointer route has no keyboard path: " << command
                      << '\n';
        }
        ASSERT_TRUE(bound.contains(std::string{command}));
    }

    ASSERT_TRUE(runtime
                    .dispatch(client,
                              {"palette.open", runtime.revision(), {}})
                    .accepted());
    snapshot = runtime.present(client, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    std::map<std::string, std::string> paletteCandidates;
    for (const auto& candidate :
         snapshot->sections().palette.commandCandidates) {
        paletteCandidates.emplace(candidate.id, candidate.label);
    }

    ssg::StatusQueue status;
    auto enqueued = status.enqueue(
        {ssg::StatusId{1}, ssg::StatusPriority::Information, "Help available",
         {{"sort", "Sort lines", "edit.sort_lines"}}});
    ASSERT_TRUE(enqueued.accepted);
    const auto statusActions = status.viewState().items.front().actions;
    ASSERT_EQ(statusActions.size(), std::size_t{1});
    for (const auto& action : statusActions) {
        const auto candidate = paletteCandidates.find(action.commandId);
        if (candidate == paletteCandidates.end()) {
            std::cerr << "  status action is not a palette candidate: "
                      << action.commandId << " (" << paletteCandidates.size()
                      << " candidates)\n";
        }
        ASSERT_TRUE(candidate != paletteCandidates.end());
        if (candidate != paletteCandidates.end()) {
            ASSERT_FALSE(candidate->second.empty());
        }
    }

    std::size_t publishedActions = 0;
    const auto inspectNode = [&](const auto& self,
                                 const ssg::UiNode& node) -> void {
        if (const auto* leaf = std::get_if<ssg::UiLeaf>(&node.content)) {
            if (leaf->widget.command) {
                ++publishedActions;
                const auto& command = *leaf->widget.command;
                const auto candidate = paletteCandidates.find(command);
                ASSERT_TRUE(bound.contains(command) ||
                            (candidate != paletteCandidates.end() &&
                             !candidate->second.empty()));
            }
            return;
        }
        for (const auto& child :
             std::get<ssg::UiContainer>(node.content).children) {
            self(self, child);
        }
    };
    inspectNode(inspectNode, snapshot->sections().ui.root);
    ASSERT_TRUE(publishedActions > 0);
}

TEST(failedSelectedCommandLeavesPickerOpenForEveryOrigin) {
    for (auto const origin : {ssg::InvocationOrigin::InProcess,
                              ssg::InvocationOrigin::Websocket}) {
        auto root = uniqueRoot();
        auto created = ssg::EditorSession::create(
            {root / "workspace", root / "scratch", root / "recovery"});
        ASSERT_TRUE(created.accepted());
        if (!created.accepted()) return;
        auto& runtime = *created.session;
        std::optional<ssg::PickerActivation> nestedActivation;
        auto const failureCommand = runtime.registerCommand(
            ssg::CommandSpecBuilder{"test.picker_failure"}
                .owner("test")
                .summary("Picker failure")
                .mutates()
                .handler([](ssg::CommandContext&) {
                    return ssg::CommandHandlerResult::failure("expected failure");
                }));
        ASSERT_TRUE(failureCommand.valid());
        auto const nestedFailure = runtime.registerCommand(
            ssg::CommandSpecBuilder{"test.picker_nested_failure"}
                .owner("test")
                .summary("Picker nested failure")
                .mutates()
                .handler([](ssg::CommandContext&) {
                    return ssg::CommandHandlerResult::failure(
                        "expected nested failure");
                }));
        ASSERT_TRUE(nestedFailure.valid());
        auto const deferringCommand = runtime.registerCommand(
            ssg::CommandSpecBuilder{"test.picker_defers_failure"}
                .owner("test")
                .summary("Picker defers failure")
                .mutates()
                .handler([&runtime](ssg::CommandContext& context) {
                    if (!runtime.deferDispatch(
                            ssg::ClientId{1},
                            {"test.picker_nested_failure", context.revision(),
                             {}})) {
                        return ssg::CommandHandlerResult::failure(
                            "could not defer nested failure");
                    }
                    return ssg::CommandHandlerResult::success();
                }));
        ASSERT_TRUE(deferringCommand.valid());
        auto const nestedSubmit = runtime.registerCommand(
            ssg::CommandSpecBuilder{"test.picker_defers_submit"}
                .owner("test")
                .summary("Picker defers another submit")
                .mutates()
                .handler([&runtime, &nestedActivation](
                             ssg::CommandContext& context) {
                    if (!nestedActivation) {
                        return ssg::CommandHandlerResult::failure(
                            "picker activation was not captured");
                    }
                    if (!runtime.deferDispatch(
                            ssg::ClientId{1},
                            {"picker.submit", context.revision(),
                             ssg::PickerSubmitArguments{
                                 *nestedActivation, "panel.toggle"}})) {
                        return ssg::CommandHandlerResult::failure(
                            "could not defer nested picker submit");
                    }
                    return ssg::CommandHandlerResult::success();
                }));
        ASSERT_TRUE(nestedSubmit.valid());
        ASSERT_TRUE(runtime
                        .attach({ssg::ClientId{1}, origin}, ssg::ViewId{1})
                        .accepted());
        ASSERT_TRUE(runtime
                        .dispatch(ssg::ClientId{1},
                                  {"palette.open", runtime.revision(), {}})
                        .accepted());
        auto opened =
            runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(opened.has_value());
        if (!opened) return;
        nestedActivation = opened->sections().palette.activePicker;
        ASSERT_TRUE(nestedActivation.has_value());
        ASSERT_FALSE(
            runtime
                .dispatch(ssg::ClientId{1},
                          {"picker.submit", runtime.revision(),
                           pickerSubmit(runtime, ssg::SearchMode::Command,
                                        "test.picker_failure")})
                .accepted());
        auto snapshot =
            runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return;
        ASSERT_TRUE(activePickerMode(snapshot->sections().palette) ==
                    std::optional<ssg::SearchMode>{ssg::SearchMode::Command});
        ASSERT_TRUE(snapshot->sections().focus == ssg::FocusTarget::Prompt);

        ASSERT_FALSE(
            runtime
                .dispatch(ssg::ClientId{1},
                          {"picker.submit", runtime.revision(),
                           pickerSubmit(runtime, ssg::SearchMode::Command,
                                        "test.picker_defers_submit")})
                .accepted());
        snapshot =
            runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return;
        ASSERT_TRUE(activePickerMode(snapshot->sections().palette) ==
                    std::optional<ssg::SearchMode>{ssg::SearchMode::Command});
        ASSERT_TRUE(snapshot->sections().focus == ssg::FocusTarget::Prompt);

        ASSERT_FALSE(
            runtime
                .dispatch(ssg::ClientId{1},
                          {"picker.submit", runtime.revision(),
                           pickerSubmit(runtime, ssg::SearchMode::Command,
                                        "test.picker_defers_failure")})
                .accepted());
        snapshot =
            runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return;
        ASSERT_TRUE(activePickerMode(snapshot->sections().palette) ==
                    std::optional<ssg::SearchMode>{ssg::SearchMode::Command});
        ASSERT_TRUE(snapshot->sections().focus == ssg::FocusTarget::Prompt);
    }
}

TEST(selectedCommandThatOpensAnotherPickerKeepsTheNewPicker) {
        auto root = uniqueRoot();
        auto created = ssg::EditorSession::create(
            {root / "workspace", root / "scratch", root / "recovery"});
        ASSERT_TRUE(created.accepted());
        if (!created.accepted()) return;
        auto& runtime = *created.session;
        ASSERT_TRUE(runtime
                        .attach({ssg::ClientId{1},
                                 ssg::InvocationOrigin::Websocket},
                                ssg::ViewId{1})
                        .accepted());
        ASSERT_TRUE(runtime
                        .dispatch(ssg::ClientId{1},
                                  {"file.open", runtime.revision(),
                                   std::string{"needle.txt"}})
                        .accepted());
        ASSERT_TRUE(runtime
                        .dispatch(ssg::ClientId{1},
                                  {"palette.open", runtime.revision(), {}})
                        .accepted());
        ASSERT_TRUE(
            runtime
                .dispatch(ssg::ClientId{1},
                          {"picker.submit", runtime.revision(),
                           pickerSubmit(runtime, ssg::SearchMode::Command,
                                        "file_finder.open")})
                .accepted());
        auto snapshot =
            runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return;
        ASSERT_TRUE(activePickerMode(snapshot->sections().palette) ==
                    std::optional<ssg::SearchMode>{ssg::SearchMode::File});
        ASSERT_TRUE(snapshot->sections().promptStatus.activeKind ==
                    std::optional<ssg::PromptKind>{ssg::PromptKind::Palette});
        ASSERT_TRUE(snapshot->sections().focus == ssg::FocusTarget::Prompt);
}

TEST(selectedCommandThatReopensTheSamePickerKeepsTheNewActivation) {
    auto root = uniqueRoot();
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(
        runtime
            .attach({ssg::ClientId{1}, ssg::InvocationOrigin::Websocket},
                    ssg::ViewId{1})
            .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"palette.open", runtime.revision(), {}})
                    .accepted());
    auto before =
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(before.has_value());
    if (!before || !before->sections().palette.activePicker) return;
    const auto first = *before->sections().palette.activePicker;

    ASSERT_TRUE(
        runtime
            .dispatch(ssg::ClientId{1},
                      {"picker.submit", runtime.revision(),
                       ssg::PickerSubmitArguments{first, "palette.open"}})
            .accepted());
    auto after =
        runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(after.has_value());
    if (!after || !after->sections().palette.activePicker) return;
    ASSERT_TRUE(after->sections().palette.activePicker->mode ==
                ssg::SearchMode::Command);
    ASSERT_FALSE(after->sections().palette.activePicker->id == first.id);
    ASSERT_TRUE(after->sections().focus == ssg::FocusTarget::Prompt);
}

TEST(paletteExecuteValidatesCandidateMembership) {
    auto root = uniqueRoot();
    auto created = ssg::EditorSession::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"needle.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.open", runtime.revision(), {}}).accepted());

    // A command outside the published candidate set is rejected before dispatch.
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1}, {"palette.execute", runtime.revision(), ssg::PaletteExecuteArguments{"not.a.command"}}).accepted());
    // Missing the id payload is rejected.
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1}, {"palette.execute", runtime.revision(), {}}).accepted());
    // A published command id validates, executes server-side, and closes the palette.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.execute", runtime.revision(), ssg::PaletteExecuteArguments{"file.save"}}).accepted());
    auto snapshot = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().focus, ssg::FocusTarget::Editor);
}

TEST(paletteCandidatesCarryLabelsAndKeyDetail) {
    auto root = uniqueRoot();
    auto created = ssg::EditorSession::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.open", runtime.revision(), {}}).accepted());
    auto snapshot = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    const auto& candidates = snapshot->sections().palette.commandCandidates;
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
        ASSERT_EQ(save->detail, std::string{"Alt+s"});  // Its bound chord.
    }
    if (undo) {
        ASSERT_EQ(undo->detail, std::string{"Alt+z"});
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
    auto created = ssg::EditorSession::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    // Show the panel; a 12-row terminal gives a panel content height of ~9.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    // Select the workspace root and expand it so its 40 files become visible.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_next", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.activate", runtime.revision(), {}}).accepted());
    const ssg::ViewportDimensions dims{80, 12};
    ssg::test::GridTestView grid{ssg::ClientId{1}, ssg::ViewId{1}, dims};

    // Baseline: selection at the top (root), window pinned to the top with a live
    // thumb.
    {
        auto snap = grid.present(runtime);
        ASSERT_TRUE(snap.has_value());
        if (!snap) return;
        auto const& p = snap->sections().tree.providers.front();
        auto const& w = snap->presentation()->treeWindows.front();
        ASSERT_TRUE(p.nodes.size() >= 40);
        ASSERT_EQ(w.firstVisible, std::uint32_t{0});
        ASSERT_TRUE(w.scrollbar.maximumFirstRow > 0);          // scrollable
        ASSERT_TRUE(w.scrollbar.thumbSize < w.scrollbar.viewportRows);
        ASSERT_EQ(w.visibleNodeIds.size(),
                  std::size_t{w.scrollbar.viewportRows});       // window bound
        ASSERT_EQ(w.visibleNodeIds.front(), p.nodes.front().node.id);
    }

    // Move the selection to the bottom: the window scrolls to keep it shown.
    for (int i = 0; i < 60; ++i) {
        ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_next", runtime.revision(), {}}).accepted());
    }
    std::uint32_t deepFirst = 0;
    {
        auto snap = grid.present(runtime);
        ASSERT_TRUE(snap.has_value());
        if (!snap) return;
        auto const& p = snap->sections().tree.providers.front();
        auto const& w = snap->presentation()->treeWindows.front();
        ASSERT_TRUE(p.selected.has_value());
        // The selected node's absolute index lies within the visible window.
        std::optional<std::uint32_t> selIndex;
        for (std::uint32_t i = 0; i < p.nodes.size(); ++i) {
            if (p.nodes[i].node.id == *p.selected) { selIndex = i; break; }
        }
        ASSERT_TRUE(selIndex.has_value());
        ASSERT_TRUE(w.firstVisible > 0);
        ASSERT_TRUE(*selIndex >= w.firstVisible &&
                    *selIndex < w.firstVisible + w.visibleNodeIds.size());
        // The hit map maps each viewport row to the correct on-screen node id.
        for (std::size_t row = 0; row < w.visibleNodeIds.size(); ++row) {
            ASSERT_EQ(w.visibleNodeIds[row],
                      p.nodes[w.firstVisible + row].node.id);
        }
        deepFirst = w.firstVisible;
    }
    ASSERT_TRUE(deepFirst > 0);

    // Move back up to the top: the window scrolls back to first_visible == 0.
    for (int i = 0; i < 40; ++i) {
        ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_previous", runtime.revision(), {}}).accepted());
    }
    {
        auto snap = grid.present(runtime);
        ASSERT_TRUE(snap.has_value());
        if (!snap) return;
        ASSERT_EQ(snap->presentation()->treeWindows.front().firstVisible, std::uint32_t{0});
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
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    // Expand the workspace root so its files become visible/selectable nodes.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_next", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.activate", runtime.revision(), {}}).accepted());

    const ssg::ViewportDimensions dims{80, 24};
    auto snap = runtime.present(ssg::ClientId{1}, dims);
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
    auto after = runtime.present(ssg::ClientId{1}, dims);
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
    auto again = runtime.present(ssg::ClientId{1}, dims);
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
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    auto focus = [&] {
        auto snap = runtime.present(ssg::ClientId{1}, dims);
        return snap ? snap->sections().focus : ssg::FocusTarget::Editor;
    };
    // Showing the panel now focuses it (QOL); expand the root so a directory node
    // and a file node are both visible/selectable.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_next", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.activate", runtime.revision(), {}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);

    auto snap = runtime.present(ssg::ClientId{1}, dims);
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
    ASSERT_TRUE(runtime.dispatch(
        ssg::ClientId{1},
        {"tree.activate_node", runtime.revision(),
         ssg::TreeSelectArguments{*fileId}}).accepted());
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
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.attach({ssg::ClientId{2}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{2}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    // Expand the root so the 40 files become a tree taller than a short panel.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_next", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.activate", runtime.revision(), {}}).accepted());
    const ssg::ViewportDimensions dims{80, 12};
    ssg::test::GridTestView firstGrid{
        ssg::ClientId{1}, ssg::ViewId{1}, dims};
    ssg::test::GridTestView secondGrid{
        ssg::ClientId{2}, ssg::ViewId{2}, dims};

    auto baseline = firstGrid.present(runtime);
    auto otherView = secondGrid.present(runtime);
    ASSERT_TRUE(baseline.has_value());
    ASSERT_TRUE(otherView.has_value());
    if (!baseline || !otherView) return;
    auto const& p0 = baseline->sections().tree.providers.front();
    auto const& w0 = baseline->presentation()->treeWindows.front();
    ASSERT_EQ(w0.firstVisible, std::uint32_t{0});
    ASSERT_TRUE(w0.scrollbar.maximumFirstRow > 0);
    auto const selectedBefore = p0.selected;

    // Wheel down: the viewport offset advances, but the selection does not move.
    ASSERT_TRUE(firstGrid.dispatch(runtime,
                                   {"tree.scroll", runtime.revision(),
                                    ssg::ScrollLinesArguments{3}}).accepted());
    auto scrolled = firstGrid.present(runtime);
    ASSERT_TRUE(scrolled.has_value());
    if (!scrolled) return;
    auto const& p1 = scrolled->sections().tree.providers.front();
    ASSERT_EQ(scrolled->presentation()->treeWindows.front().firstVisible, std::uint32_t{3});
    otherView = secondGrid.present(runtime);
    ASSERT_TRUE(otherView.has_value());
    if (!otherView) return;
    ASSERT_EQ(otherView->presentation()->treeWindows.front().firstVisible,
              std::uint32_t{0});
    ASSERT_EQ(p1.selected, selectedBefore);  // selection unchanged

    // Wheel up past the top clamps at 0.
    ASSERT_TRUE(firstGrid.dispatch(runtime,
                                   {"tree.scroll", runtime.revision(),
                                    ssg::ScrollLinesArguments{-99}}).accepted());
    auto topped = firstGrid.present(runtime);
    ASSERT_TRUE(topped.has_value());
    if (!topped) return;
    ASSERT_EQ(topped->presentation()->treeWindows.front().firstVisible, std::uint32_t{0});

    // Wheel down past the bottom clamps at maximum_first_row.
    ASSERT_TRUE(firstGrid.dispatch(runtime,
                                   {"tree.scroll", runtime.revision(),
                                    ssg::ScrollLinesArguments{999}}).accepted());
    auto bottomed = firstGrid.present(runtime);
    ASSERT_TRUE(bottomed.has_value());
    if (!bottomed) return;
    auto const& w3 = bottomed->presentation()->treeWindows.front();
    ASSERT_EQ(w3.firstVisible, w3.scrollbar.maximumFirstRow);

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
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"long.txt"}}).accepted());

    ssg::ViewportDimensions const dims{24, 6};
    ssg::test::GridTestView grid{ssg::ClientId{1}, ssg::ViewId{1}, dims};
    // Prime the pane-size cache the reveal path reads.
    auto primed = grid.present(runtime);
    ASSERT_TRUE(primed.has_value());
    if (!primed) return;
    ASSERT_EQ(primed->presentation()->viewport.firstVisualColumn, std::uint32_t{0});

    // Move the caret to the end of the long line: it is past the pane width, so
    // the viewport scrolls horizontally to keep it visible.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"cursor.line_end", runtime.revision(), {}})
                    .accepted());
    auto scrolled = grid.present(runtime);
    ASSERT_TRUE(scrolled.has_value());
    if (!scrolled) return;
    auto const offset = scrolled->presentation()->viewport.firstVisualColumn;
    ASSERT_TRUE(offset > 0);
    // The caret's cell (60) is within the visible horizontal window.
    ASSERT_TRUE(60u >= offset);
    ASSERT_TRUE(60u < offset + dims.columns);

    // Returning to the line start resets the horizontal offset to zero.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"cursor.line_start", runtime.revision(), {}})
                    .accepted());
    auto reset = grid.present(runtime);
    ASSERT_TRUE(reset.has_value());
    if (!reset) return;
    ASSERT_EQ(reset->presentation()->viewport.firstVisualColumn, std::uint32_t{0});
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
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"wide.txt"}}).accepted());
    ssg::ViewportDimensions const dims{80, 24};

    // Word wrap OFF (default): three logical lines (the trailing newline yields a
    // final empty line) -> three visual rows total; the 200-cell line is ONE
    // clipped visual row.
    auto off = runtime.present(ssg::ClientId{1}, dims);
    ASSERT_TRUE(off.has_value());
    if (!off) return;
    ASSERT_EQ(off->presentation()->viewport.totalVisualRows, std::uint32_t{3});
    std::uint32_t offRowsForLine0 = 0;
    for (auto const& row : off->presentation()->viewport.visibleRows) {
        if (row.logicalLine == 0) ++offRowsForLine0;
    }
    ASSERT_EQ(offRowsForLine0, std::uint32_t{1});  // clipped, not wrapped

    // Word wrap ON: the 200-cell line wraps into ceil(200/80) = 3 visual rows, so
    // the total exceeds the OFF total and logical line 0 spans >1 row.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"view.toggle_word_wrap", runtime.revision(), {}})
                    .accepted());
    auto on = runtime.present(ssg::ClientId{1}, dims);
    ASSERT_TRUE(on.has_value());
    if (!on) return;
    ASSERT_TRUE(on->presentation()->viewport.totalVisualRows > 3u);  // wrapped
    std::uint32_t onRowsForLine0 = 0;
    for (auto const& row : on->presentation()->viewport.visibleRows) {
        if (row.logicalLine == 0) ++onRowsForLine0;
    }
    ASSERT_EQ(onRowsForLine0, std::uint32_t{3});  // 200 cells / 80 -> 3 rows
    // Wrapped lines never scroll horizontally.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"cursor.line_end", runtime.revision(), {}})
                    .accepted());
    auto wrappedEnd = runtime.present(ssg::ClientId{1}, dims);
    ASSERT_TRUE(wrappedEnd.has_value());
    if (!wrappedEnd) return;
    ASSERT_EQ(wrappedEnd->presentation()->viewport.firstVisualColumn, std::uint32_t{0});
    std::filesystem::remove_all(root);
}

// Lever 2 (wrap-mode shaping cache): word-wrap shaping is O(document) -- it
// segments every line to compute wrap positions. The per-document cell runs are
// cached by (revision, documentId), so a snapshot that changes neither re-shapes
// nothing, and an edit (new revision) forces a full re-shape. Proven by the
// compute_cell_run counter: two identical wrap snapshots segment the same
// (small, non-document-scaled) amount; an edit adds a full-document re-shape.
TEST(wordWrapShapingIsCachedUntilTheDocumentRevisionChanges) {
    auto root = std::filesystem::current_path() / "runtime_wrapcache";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::string text;
    for (int i = 0; i < 60; ++i) {
        text += "line " + std::to_string(i) + " content\n";
    }
    std::ofstream{root / "workspace" / "doc.txt"} << text;
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"doc.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"view.toggle_word_wrap", runtime.revision(), {}})
                    .accepted());
    ssg::ViewportDimensions const dims{80, 24};
    (void)runtime.present(ssg::ClientId{1}, dims);  // warm the cache

    // Two identical wrap snapshots: the second re-shapes nothing from the
    // document -- only the constant chrome/prompt shaping remains.
    ssg::GraphemeLayout::resetCellRunCalls();
    (void)runtime.present(ssg::ClientId{1}, dims);
    auto const base = ssg::GraphemeLayout::cellRunCalls();
    ssg::GraphemeLayout::resetCellRunCalls();
    (void)runtime.present(ssg::ClientId{1}, dims);
    ASSERT_EQ(ssg::GraphemeLayout::cellRunCalls(), base);

    // An edit bumps the document revision, so the whole document is re-shaped:
    // the count jumps well past the cached-snapshot baseline.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"text.insert", runtime.revision(),
                                  ssg::TextInputArguments{"z"}})
                    .accepted());
    ssg::GraphemeLayout::resetCellRunCalls();
    (void)runtime.present(ssg::ClientId{1}, dims);
    ASSERT_TRUE(ssg::GraphemeLayout::cellRunCalls() > base);
    std::filesystem::remove_all(root);
}


// Lever 3 (dispatch effects). A dispatch reports, via its CommandResult, whether
// it changed routing state (what a key/paste reads to route) and/or geometry
// (what a pointer/wheel hit-tests). The host uses these to coalesce per-drain
// snapshots. A cursor move or text edit changes geometry but not routing; opening
// a prompt or rebinding a key changes routing; a rejected command changes
// neither. This names the routing/geometry separation the coalescing relies on --
// keying only on the session revision (geometry) would wrongly treat a
// prompt-open as a non-routing change.
TEST(dispatchEffectsSeparateRoutingFromGeometryAcrossRoutes) {
    auto root = std::filesystem::current_path() / "runtime_effects";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::string text;
    for (int i = 0; i < 10; ++i) text += "line " + std::to_string(i) + "\n";
    std::ofstream{root / "workspace" / "doc.txt"} << text;
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    const ssg::ClientId client{1};
    ASSERT_TRUE(runtime.attach({client, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(client, {"file.open", runtime.revision(),
                                          std::string{"doc.txt"}}).accepted());

    // A cursor move: geometry advances (a new revision), routing does not.
    auto down = runtime.dispatch(client, {"cursor.line_down", runtime.revision(), {}});
    ASSERT_TRUE(down.accepted());
    ASSERT_TRUE(down.effects.geometryChanged);
    ASSERT_FALSE(down.effects.routingChanged);

    // Typing text: geometry, not routing.
    auto ins = runtime.dispatch(client, {"text.insert", runtime.revision(),
                                         ssg::TextInputArguments{"x"}});
    ASSERT_TRUE(ins.accepted());
    ASSERT_TRUE(ins.effects.geometryChanged);
    ASSERT_FALSE(ins.effects.routingChanged);

    // Opening the palette changes routing (how the next key is interpreted).
    auto pal = runtime.dispatch(client, {"palette.open", runtime.revision(), {}});
    ASSERT_TRUE(pal.accepted());
    ASSERT_TRUE(pal.effects.routingChanged);
    (void)runtime.dispatch(client, {"palette.close", runtime.revision(), {}});

    // Rebinding a key changes routing even though the catalog revision does not
    // move (binding an existing command registers nothing).
    auto bind = runtime.dispatch(
        client, {"keymap.bind", runtime.revision(),
                 ssg::KeymapBindArguments{"Ctrl+KeyG", "goto.line", "editor"}});
    ASSERT_TRUE(bind.accepted());
    ASSERT_TRUE(bind.effects.routingChanged);

    // A rejected command changes nothing.
    auto bad = runtime.dispatch(client, {"no.such.command", runtime.revision(), {}});
    ASSERT_FALSE(bad.accepted());
    ASSERT_FALSE(bad.effects.routingChanged);
    ASSERT_FALSE(bad.effects.geometryChanged);
    std::filesystem::remove_all(root);
}


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
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ssg::ViewportDimensions const dims{80, 24};

    auto navSegmentations = [&](std::string const& file) -> std::uint64_t {
        (void)runtime.dispatch(
            ssg::ClientId{1}, {"file.open", runtime.revision(), file});
        (void)runtime.present(ssg::ClientId{1}, dims);  // prime pane cache
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

std::unique_ptr<ssg::EditorSession> gotoLineRuntime() {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "lines.txt"}
        << "one\ntwo\nthree\nfour\nfive";
    auto created = ssg::EditorSession::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    if (!created.accepted()) return nullptr;
    auto runtime = std::move(created.session);
    (void)runtime->attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                          ssg::ViewId{1});
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"lines.txt"}});
    return runtime;
}

std::uint32_t gotoCaretLine(ssg::EditorSession& runtime) {
    auto snapshot = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    if (!snapshot) return 0;
    return snapshot->sections().selection.primary().active.line.value();
}

TEST(gotoLineClampsToTheOneBasedLineRange) {
    auto runtime = gotoLineRuntime();
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    // "3" is 1-based, so the caret lands on line index 2.
    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"goto.line", runtime->revision(), std::string{"3"}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 2U);
    // A number past the end clamps to the last line (index 4).
    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"goto.line", runtime->revision(), std::string{"999"}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 4U);
    // "1" is the first line.
    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"goto.line", runtime->revision(), std::string{"1"}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 0U);
    // Below the range clamps to the first line rather than failing: "0" and a
    // negative both go to line 1 (index 0). Move off line 0 between each so a
    // no-op could not masquerade as a successful clamp.
    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"goto.line", runtime->revision(), std::string{"4"}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 3U);
    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"goto.line", runtime->revision(), std::string{"0"}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 0U);
    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"goto.line", runtime->revision(), std::string{"4"}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 3U);
    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"goto.line", runtime->revision(), std::string{"-7"}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 0U);
}

TEST(gotoLineRejectsNonNumericInput) {
    auto runtime = gotoLineRuntime();
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"goto.line", runtime->revision(), std::string{"3"}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 2U);
    for (const auto* bad : {"abc", "2x", "1.5", ""}) {
        ASSERT_FALSE(runtime
                         ->dispatch(ssg::ClientId{1},
                                    {"goto.line", runtime->revision(),
                                     std::string{bad}})
                         .accepted());
    }
    // The rejected inputs never moved the caret.
    ASSERT_EQ(gotoCaretLine(*runtime), 2U);
}

TEST(gotoLineWithoutPayloadOpensACommandArgumentPromptThatJumpsOnSubmit) {
    auto runtime = gotoLineRuntime();
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"goto.line", runtime->revision(), {}})
                    .accepted());
    auto snapshot = runtime->present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto& prompt = snapshot->presentation()->prompt;
    ASSERT_TRUE(prompt.has_value());
    if (!prompt) return;
    ASSERT_TRUE(prompt->kind == ssg::PromptKind::CommandArgument);
    // The generic prompt round-trip re-dispatches goto.line with the typed value.
    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"prompt.update_value", runtime->revision(),
                                ssg::PromptValueArguments{0, "4"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    ->dispatch(ssg::ClientId{1},
                               {"prompt.submit", runtime->revision(), {}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 3U);
}

} // namespace

int main() {
    RUN(searchTreeDiffAndFollowSectionsUseRuntimeState);
    RUN(externalDiffBurstRevealsOnlyNewestFileWithoutPausingFollow);
    RUN(attachedClientsShareFollowPauseQueueAndResumeState);
    RUN(gitDiffScanUpdatesDiffAndRejectsStaleBatches);
    RUN(gitDiffSelectionUsesDiffIdentityIndependentOfDocumentRevision);
    RUN(gitDiffScanRefreshesGitTreeProviderFromDiffAndOnSecondScan);
    RUN(gitStatusSurvivesDetailedDiffWorkLimit);
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
    RUN(filePickerPublishesWorkspaceFilesAndRejectsPaletteExecute);
    RUN(togglingGitignoreRebuildsTheOpenFilePickerIndex);
    RUN(workerFilesystemRefreshPublishesChangedFileCandidates);
    RUN(filePickerClosesOnSuccessfulOpenAndStaysOpenOnFailure);
    RUN(websocketPickerSubmissionRequiresAndClosesTheAuthoritativePicker);
    RUN(commandPickerSubmissionHasOriginParity);
    RUN(pickerSubmissionUsesActivationIdentityInsteadOfGlobalRevision);
    RUN(simpleSemanticInputsLowerThroughAuthoritativeTransactions);
    RUN(documentPointerInputOwnsSelectionGesturePolicy);
    RUN(documentEdgeMovesExtendAndRevealInOneAuthoritativeTransition);
    RUN(everySemanticPointerRouteHasAnAuthoritativeKeyboardPath);
    RUN(failedSelectedCommandLeavesPickerOpenForEveryOrigin);
    RUN(selectedCommandThatOpensAnotherPickerKeepsTheNewPicker);
    RUN(selectedCommandThatReopensTheSamePickerKeepsTheNewActivation);
    RUN(paletteCandidatesCarryLabelsAndKeyDetail);
    RUN(treeScrollsToKeepSelectionVisibleInAShortPanel);
    RUN(treeSelectSetsSelectionToANodeAndRejectsUnknownIds);
    RUN(treeScrollMovesTheViewportWithoutMovingTheSelection);
    RUN(treeSelectFocusesThePanelAndTheClickPairNetsExpectedFocus);
    RUN(wordWrapOffRevealsCaretHorizontally);
    RUN(wordWrapOnWrapsLongLinesOffClipsThem);
    RUN(wordWrapShapingIsCachedUntilTheDocumentRevisionChanges);
    RUN(dispatchEffectsSeparateRoutingFromGeometryAcrossRoutes);
    RUN(wordWrapOffNavigationIsViewportBounded);
    RUN(gotoLineClampsToTheOneBasedLineRange);
    RUN(gotoLineRejectsNonNumericInput);
    RUN(gotoLineWithoutPayloadOpensACommandArgumentPromptThatJumpsOnSubmit);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
