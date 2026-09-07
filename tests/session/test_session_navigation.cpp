#include "../grid_test_view.h"
#include "../grid_test_frame.h"
#include "../test_helpers.h"

#include <ssg/Editor.h>
#include <ssg/CommandCatalog.h>
#include <ssg/GraphemeLayout.h>
#include <ssg/Keymap.h>
#include <ssg/PromptSurface.h>
#include <ssg/TextInputCommands.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

std::optional<ssg::GridPresentation> projectFrame(
    ssg::Editor& runtime, ssg::ViewportDimensions dimensions) {
    return ssg::test::projectGridFrame(runtime, dimensions);
}

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
    std::type_identity<ssg::ExternalActionPointerInput>) {
    return {"external.focus", "external.reload", "external.keep_buffer",
            "external.open_diff"};
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
    std::type_identity<ssg::ViewTransitionInput>) {
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
    static const auto runtimeName =
        "runtime_navigation_" +
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
    auto root = testRuntimePath(runtimeName);
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

std::map<std::string, ssg::DiffFileStatus> gitProviderStatuses(
    const ssg::TreeProviderView& provider) {
    std::map<std::string, ssg::DiffFileStatus> statuses;
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

std::map<std::string, ssg::DiffFileStatus> diffStatuses(
    const ssg::DiffViewState& diff) {
    std::map<std::string, ssg::DiffFileStatus> statuses;
    for (const auto& file : diff.files) {
        statuses.emplace(file.path.generic_string(), file.status);
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
    ssg::Editor& runtime, ssg::SearchMode mode, std::string candidateId) {
    auto snapshot = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(snapshot.has_value());
    const auto activation =
        snapshot && snapshot->paletteView.activePicker
            ? snapshot->paletteView.activePicker->id
            : ssg::PickerActivationId{};
    return {ssg::PickerActivation{mode, activation}, std::move(candidateId)};
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

ssg::FollowMode followMode(ssg::Editor& runtime) {
    auto snapshot = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) {
        return ssg::FollowMode::Paused;
    }
    return snapshot->followMode;
}

std::unique_ptr<ssg::Editor> followPauseRuntime(std::string text) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "needle.txt"} << text;
    std::ofstream{root / "workspace" / "other.txt"} << "other\n";
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) {
        return nullptr;
    }
    auto runtime = std::move(created.session);
    ASSERT_TRUE(runtime
                    ->dispatch({"file.open",
                                std::string{"needle.txt"}})
                    .accepted());
    ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Following);
    return runtime;
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

    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .applyExternalDiffBurst(
                        {{{.kind = ssg::NonGitDiffEventKind::Create,
                           .id = ssg::DiffFileId{"a.txt"},
                           .path = "a.txt",
                           .baselineContent = "",
                           .targetContent = "a\n"},
                          std::uint64_t{1}},
                         {{.kind = ssg::NonGitDiffEventKind::Create,
                           .id = ssg::DiffFileId{"b.txt"},
                           .path = "b.txt",
                           .baselineContent = "",
                           .targetContent = "b\n"},
                          std::uint64_t{2}},
                         {{.kind = ssg::NonGitDiffEventKind::Create,
                           .id = ssg::DiffFileId{"c.txt"},
                           .path = "c.txt",
                           .baselineContent = "",
                           .targetContent = middle},
                          std::uint64_t{3}}})
                    .accepted());

    const ssg::ViewportDimensions dimensions{20, 6};
    auto snapshot = projectFrame(runtime, dimensions);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->diffFileIdentity,
              std::optional<std::string>{"c.txt"});
    ASSERT_EQ(snapshot->followMode, ssg::FollowMode::Following);

    ssg::test::GridTestView grid{dimensions};
    ASSERT_TRUE(grid
                    .dispatch(runtime,
                              {"cursor.line_up",  {}})
                    .accepted());
    snapshot = projectFrame(runtime, dimensions);
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) {
        ASSERT_EQ(snapshot->followMode,
                  ssg::FollowMode::Paused);
    }
}

TEST(followPauseQueuesMultipleChangesAndResumeAdoptsTheNewest) {
    auto runtime = followPauseRuntime("alpha\nbeta\n");
    if (!runtime) return;
    ssg::test::GridTestView grid{{80, 20}};

    ASSERT_TRUE(grid
                    .dispatch(*runtime,
                              {"view.scroll_lines",
                               ssg::ScrollLinesArguments{3}})
                    .accepted());

    auto paused = ssg::test::projectGridFrame(*runtime);
    ASSERT_TRUE(paused.has_value());
    if (!paused) return;
    ASSERT_EQ(paused->followMode, ssg::FollowMode::Paused);

    auto const sourceRevision = paused->diff.revision;
    ASSERT_TRUE(runtime
                    ->applyExternalDiffBurst(
                        {{{.kind = ssg::NonGitDiffEventKind::Create,
                           .id = ssg::DiffFileId{"watched-a"},
                           .path = "watched-a.txt",
                           .baselineContent = "",
                           .targetContent = "first\nbeta\n"},
                          std::uint64_t{sourceRevision + 1}},
                         {{.kind = ssg::NonGitDiffEventKind::Create,
                           .id = ssg::DiffFileId{"watched-b"},
                           .path = "watched-b.txt",
                           .baselineContent = "",
                           .targetContent = "newest\n"},
                          std::uint64_t{sourceRevision + 2}}})
                    .accepted());

    paused = ssg::test::projectGridFrame(*runtime);
    ASSERT_TRUE(paused.has_value());
    if (!paused) return;
    ASSERT_EQ(paused->followMode, ssg::FollowMode::Paused);

    ASSERT_TRUE(runtime
                    ->dispatch({"follow_edits.resume",  {}})
                    .accepted());
    auto resumed = ssg::test::projectGridFrame(*runtime);
    ASSERT_TRUE(resumed.has_value());
    if (!resumed) return;
    ASSERT_EQ(resumed->followMode, ssg::FollowMode::Following);
    ASSERT_EQ(resumed->diffFileIdentity,
              std::optional<std::string>{"watched-b"});
}

TEST(gitDiffScanUpdatesDiffAndRejectsStaleBatches) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "a.txt"} << "a\n";
    std::ofstream{root / "workspace" / "b.txt"} << "b\n";

    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    ssg::GitDiffScan scan{
        .revision = std::uint64_t{10},
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

    auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->diff.files.size(), std::size_t{2});
    ASSERT_EQ(snapshot->diffFileIdentity,
              std::optional<std::string>{"b.txt"});

    ssg::GitDiffScan stale{
        .revision = std::uint64_t{10},
        .baselineIdentity = "head-1:index-2",
        .files = {{.id = ssg::DiffFileId{"a.txt"},
                   .path = "a.txt",
                   .baselineContent = std::string{"a\n"},
                   .workingContent = std::string{"a changed again\n"}}}};
    auto staleResult = runtime.applyGitDiffScan(std::move(stale));
    ASSERT_FALSE(staleResult.accepted());
    ASSERT_EQ(staleResult.error, ssg::DiffIngressError::DiffRejected);
}

TEST(gitDiffSelectionUsesDiffIdentityIndependentOfDocumentRevision) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                               std::string{"needle.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"follow_edits.pause",  {}})
                    .accepted());

    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = std::uint64_t{20},
                         .baselineIdentity = "head-2:index-1",
                         .files = {{.id = ssg::DiffFileId{"needle.txt"},
                                    .path = "needle.txt",
                                    .baselineContent =
                                        std::string{"alpha needle omega"},
                                    .workingContent =
                                        std::string{"alpha NEEDLE omega"}}}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"text.insert",
                               ssg::TextInputArguments{"!"}})
                    .accepted());

    auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->diffFileIdentity, std::nullopt);
    ASSERT_NE(snapshot->documentRevision,
              snapshot->diff.revision);
    const auto byIdentity = std::find_if(
        snapshot->diff.files.begin(),
        snapshot->diff.files.end(),
        [](const ssg::DiffFileView& file) {
            return file.id == ssg::DiffFileId{"needle.txt"};
        });
    ASSERT_TRUE(byIdentity != snapshot->diff.files.end());
}

TEST(gitStatusActivationOpensLiveDiffTabAndReusesIt) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "coexist.txt"} << "disk\n";
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                               std::string{"coexist.txt"}})
                    .accepted());

    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = std::uint64_t{30},
                         .baselineIdentity = "head-x:index-1",
                         .files = {{.id = ssg::DiffFileId{"coexist-id"},
                                    .path = "coexist.txt",
                                    .baselineContent = std::string{"before\n"},
                                    .workingContent = std::string{"after\n"}}}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"panel.show_git_status",  {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"tree.select_next",  {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"tree.activate",  {}})
                    .accepted());

    auto first = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(first.has_value());
    if (!first) return;
    ASSERT_EQ(countTabsOfKind(first->tabs, ssg::TabKind::Document),
              std::size_t{1});
    ASSERT_EQ(countTabsOfKind(first->tabs, ssg::TabKind::LiveDiff),
              std::size_t{1});
    std::optional<ssg::TabId> liveDiffId;
    for (const auto& tab : first->tabs.tabs) {
        if (tab.kind == ssg::TabKind::LiveDiff) {
            liveDiffId = tab.id;
            ASSERT_EQ(tab.contentIdentity, std::string{"coexist-id"});
        }
    }
    ASSERT_TRUE(liveDiffId.has_value());
    if (!liveDiffId) return;

    ASSERT_TRUE(runtime
                    .dispatch({"tree.activate",  {}})
                    .accepted());
    auto second =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(second.has_value());
    if (!second) return;
    ASSERT_EQ(countTabsOfKind(second->tabs, ssg::TabKind::Document),
              std::size_t{1});
    ASSERT_EQ(countTabsOfKind(second->tabs, ssg::TabKind::LiveDiff),
              std::size_t{1});
    ASSERT_EQ(second->tabs.active, liveDiffId);
}

TEST(documentAndLiveDiffTabsCloseIndependently) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "coexist.txt"} << "disk\n";
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                               std::string{"coexist.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = std::uint64_t{32},
                         .baselineIdentity = "head-z:index-1",
                         .files = {{.id = ssg::DiffFileId{"coexist-id"},
                                    .path = "coexist.txt",
                                    .baselineContent = std::string{"before\n"},
                                    .workingContent = std::string{"after\n"}}}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"panel.show_git_status",  {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"tree.select_next",  {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"tree.activate",  {}})
                    .accepted());

    auto first = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(first.has_value());
    if (!first) return;
    std::optional<ssg::TabId> documentTab;
    std::optional<ssg::TabId> liveDiffTab;
    for (const auto& tab : first->tabs.tabs) {
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
                    .dispatch({"tab.activate",
                               *documentTab})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"tab.close",
                               *documentTab})
                    .accepted());
    auto afterDocumentClose =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(afterDocumentClose.has_value());
    if (!afterDocumentClose) return;
    ASSERT_EQ(countTabsOfKind(afterDocumentClose->tabs,
                              ssg::TabKind::Document),
              std::size_t{0});
    ASSERT_EQ(countTabsOfKind(afterDocumentClose->tabs,
                              ssg::TabKind::LiveDiff),
              std::size_t{1});
    ASSERT_EQ(afterDocumentClose->diffFileIdentity,
              std::optional<std::string>{"coexist-id"});

    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                               std::string{"coexist.txt"}})
                    .accepted());
    auto reopened =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(reopened.has_value());
    if (!reopened) return;
    ASSERT_EQ(countTabsOfKind(reopened->tabs, ssg::TabKind::Document),
              std::size_t{1});
    ASSERT_EQ(countTabsOfKind(reopened->tabs, ssg::TabKind::LiveDiff),
              std::size_t{1});

    ASSERT_TRUE(runtime
                    .dispatch({"tab.activate",
                               *liveDiffTab})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"tab.close",
                               *liveDiffTab})
                    .accepted());
    auto afterLiveDiffClose =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(afterLiveDiffClose.has_value());
    if (!afterLiveDiffClose) return;
    ASSERT_EQ(countTabsOfKind(afterLiveDiffClose->tabs,
                              ssg::TabKind::Document),
              std::size_t{1});
    ASSERT_EQ(countTabsOfKind(afterLiveDiffClose->tabs,
                              ssg::TabKind::LiveDiff),
              std::size_t{0});
    ASSERT_EQ(afterLiveDiffClose->diffFileIdentity,
              std::nullopt);
}

TEST(gitStatusActivationOpensDeletedLiveDiffWithoutDiskFile) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "gone.txt"} << "gone\n";
    std::filesystem::remove(root / "workspace" / "gone.txt");
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_FALSE(std::filesystem::exists(root / "workspace" / "gone.txt"));

    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = std::uint64_t{31},
                         .baselineIdentity = "head-x:index-2",
                         .files = {{.id = ssg::DiffFileId{"deleted-id"},
                                    .path = "gone.txt",
                                    .baselineContent = std::string{"gone\n"},
                                    .workingContent = std::nullopt}}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"panel.show_git_status",  {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"tree.select_next",  {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"tree.activate",  {}})
                    .accepted());

    auto snapshot =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(countTabsOfKind(snapshot->tabs, ssg::TabKind::LiveDiff),
              std::size_t{1});
    ASSERT_EQ(snapshot->diffFileIdentity,
              std::optional<std::string>{"deleted-id"});
    // A deleted file has no real document content -- its removed lines are
    // represented entirely as phantom rows (Viewport's removedBlocks
    // projection), derived from diff.files' hunks below, not as literal
    // document text. Synthesizing the baseline here as "current" text would
    // duplicate every removed line: once as a real row, once as its phantom.
    ASSERT_EQ(snapshot->documentText, std::string{});
    const auto deleted = std::find_if(
        snapshot->diff.files.begin(),
        snapshot->diff.files.end(),
        [](const ssg::DiffFileView& file) {
            return file.id == ssg::DiffFileId{"deleted-id"};
        });
    ASSERT_TRUE(deleted != snapshot->diff.files.end());
    if (deleted == snapshot->diff.files.end()) return;
    ASSERT_TRUE(deleted->deleted);
    ASSERT_FALSE(deleted->hunks.empty());
}

TEST(liveDiffOpenClassificationPausesOnlyForUserActivation) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    ASSERT_TRUE(runtime
                    .dispatch({"follow_edits.pause",  {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = std::uint64_t{40},
                         .baselineIdentity = "head-y:index-1",
                         .files = {{.id = ssg::DiffFileId{"programmatic-id"},
                                    .path = "needle.txt",
                                    .baselineContent =
                                        std::string{"alpha needle omega"},
                                    .workingContent =
                                        std::string{"alpha NEEDLE omega"}}}})
                    .accepted());
    auto beforeResume =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(beforeResume.has_value());
    if (!beforeResume) return;
    ASSERT_EQ(beforeResume->followMode,
              ssg::FollowMode::Paused);
    ASSERT_EQ(countTabsOfKind(beforeResume->tabs,
                              ssg::TabKind::LiveDiff),
              std::size_t{0});

    ASSERT_TRUE(runtime
                    .dispatch({"follow_edits.resume",  {}})
                    .accepted());
    auto afterProgrammatic =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(afterProgrammatic.has_value());
    if (!afterProgrammatic) return;
    ASSERT_EQ(afterProgrammatic->followMode,
              ssg::FollowMode::Following);
    ASSERT_EQ(countTabsOfKind(afterProgrammatic->tabs,
                              ssg::TabKind::LiveDiff),
              std::size_t{1});

    ASSERT_TRUE(runtime
                    .dispatch({"panel.show_git_status",  {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"tree.select_next",  {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"tree.activate",  {}})
                    .accepted());
    auto afterUser =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(afterUser.has_value());
    if (!afterUser) return;
    ASSERT_EQ(afterUser->followMode, ssg::FollowMode::Paused);
}

TEST(tabSwitchPausesFollowViaNavigationPath) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "other.txt"} << "other\n";
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                               std::string{"needle.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                               std::string{"other.txt"}})
                    .accepted());
    ASSERT_EQ(followMode(runtime), ssg::FollowMode::Following);

    ASSERT_TRUE(runtime
                    .dispatch({"tab.previous",  {}})
                    .accepted());
    ASSERT_EQ(followMode(runtime), ssg::FollowMode::Paused);
}

TEST(followToggleMatchesPauseAndResumeIncludingQueuedTargetResolution) {
    const auto makeRuntime = []() -> std::unique_ptr<ssg::Editor> {
        auto root = uniqueRoot();
        auto created = ssg::createEditor(
            {root / "workspace", root / "scratch", root / "recovery"});
        ASSERT_TRUE(created.accepted());
        if (!created.accepted()) return nullptr;
        auto runtime = std::move(created.session);
        ASSERT_TRUE(runtime
                        ->dispatch({"file.open",
                                    std::string{"needle.txt"}})
                        .accepted());
        return runtime;
    };
    const auto followState = [](ssg::Editor& runtime) {
        auto snapshot =
            projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snapshot.has_value());
        return std::pair{
            snapshot ? snapshot->followMode : ssg::FollowMode::Following,
            snapshot ? snapshot->diffFileIdentity
                     : std::optional<std::string>{}};
    };

    auto pauseResume = makeRuntime();
    auto togglePath = makeRuntime();
    ASSERT_TRUE(pauseResume != nullptr);
    ASSERT_TRUE(togglePath != nullptr);
    if (!pauseResume || !togglePath) return;

    ASSERT_TRUE(pauseResume
                    ->dispatch({"follow_edits.pause", {}})
                    .accepted());
    ASSERT_TRUE(togglePath
                    ->dispatch({"follow_edits.toggle", {}})
                    .accepted());

    const auto pausedWithPause = followState(*pauseResume);
    const auto pausedWithToggle = followState(*togglePath);
    ASSERT_EQ(pausedWithPause.first, ssg::FollowMode::Paused);
    ASSERT_EQ(pausedWithToggle, pausedWithPause);

    ssg::GitDiffScan scan{
        .revision = std::uint64_t{2},
        .baselineIdentity = "head-1:index-1",
        .files = {{.id = ssg::DiffFileId{"needle.txt"},
                   .path = "needle.txt",
                   .baselineContent = std::string{"alpha needle omega"},
                   .workingContent = std::string{"alpha needle omega plus"}}}};
    ASSERT_TRUE(pauseResume->applyGitDiffScan(scan).accepted());
    ASSERT_TRUE(togglePath->applyGitDiffScan(scan).accepted());

    const auto pausedQueuedWithPause = followState(*pauseResume);
    const auto pausedQueuedWithToggle = followState(*togglePath);
    ASSERT_EQ(pausedQueuedWithPause.first, ssg::FollowMode::Paused);
    ASSERT_EQ(pausedQueuedWithToggle, pausedQueuedWithPause);

    ASSERT_TRUE(pauseResume
                    ->dispatch({"follow_edits.resume", {}})
                    .accepted());
    ASSERT_TRUE(togglePath
                    ->dispatch({"follow_edits.toggle", {}})
                    .accepted());

    const auto resumedWithResume = followState(*pauseResume);
    const auto resumedWithToggle = followState(*togglePath);
    ASSERT_EQ(resumedWithResume.first, ssg::FollowMode::Following);
    ASSERT_EQ(resumedWithResume.second,
              std::optional<std::string>{"needle.txt"});
    ASSERT_EQ(resumedWithToggle, resumedWithResume);
}

TEST(followPauseOnEditTransitionTable) {
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch({"text.insert",
                                    ssg::TextInputArguments{"x"}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch({"select.right",  {}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch({"clipboard.cut",  {}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch({"follow_edits.resume",  {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Following);
        ASSERT_TRUE(runtime
                        ->dispatch({"clipboard.paste",  {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch({"text.insert",
                                    ssg::TextInputArguments{"x"}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch({"follow_edits.resume",  {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Following);
        ASSERT_TRUE(runtime
                        ->dispatch({"edit.undo",  {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch({"text.insert",
                                    ssg::TextInputArguments{"x"}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch({"follow_edits.resume",  {}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch({"edit.undo",  {}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch({"follow_edits.resume",  {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Following);
        ASSERT_TRUE(runtime
                        ->dispatch({"edit.redo",  {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch({"replace.open",  {}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch({"find.update_query",
                                    ssg::FindQueryArguments{"needle"}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch({"replace.update_replacement",
                                    ssg::FindQueryArguments{"pin"}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Following);
        ASSERT_TRUE(runtime
                        ->dispatch({"replace.current",  {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha\nbeta\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch({"select.add_cursor_down",  {}})
                        .accepted());
        ASSERT_TRUE(runtime
                        ->dispatch({"text.insert",
                                    ssg::TextInputArguments{"x"}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ssg::test::GridTestView grid{{80, 20}};
        ASSERT_TRUE(grid
                        .dispatch(*runtime,
                                  {"pane.next",  {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ssg::test::GridTestView grid{{80, 20}};
        ASSERT_TRUE(grid
                        .dispatch(*runtime,
                                  {"view.scroll_lines",
                                   ssg::ScrollLinesArguments{1}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
    {
        auto runtime = followPauseRuntime("alpha needle omega\n");
        ASSERT_TRUE(runtime != nullptr);
        if (!runtime) return;
        ASSERT_TRUE(runtime
                        ->dispatch({"cursor.right",  {}})
                        .accepted());
        ASSERT_EQ(followMode(*runtime), ssg::FollowMode::Paused);
    }
}

TEST(paletteOpenEntersPromptFocusAndPublishesCandidates) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    ASSERT_TRUE(runtime.dispatch({"palette.open",  {}}).accepted());
    auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(ssg::effectiveUiFocus(snapshot->uiTree),
              ssg::FocusTarget::Prompt);
    ASSERT_FALSE(snapshot->paletteView.commandCandidates.empty());

    ASSERT_TRUE(runtime.dispatch({"palette.close",  {}}).accepted());
    auto closed = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(closed.has_value());
    if (!closed) return;
    ASSERT_EQ(ssg::effectiveUiFocus(closed->uiTree),
              ssg::FocusTarget::Editor);
}

// The open-picker kind is derived from the prompt after every dispatch rather
// than cleared at each close path.  Pin that across every way a picker closes:
// a stale kind would make the NEXT open publish the previous picker's mode and
// candidates, which is invisible while only one picker exists and wrong the
// moment a second one lands.
TEST(everyPaletteClosePathLeavesNoOpenPickerBehind) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    auto pickerStateAfter = [&](std::string const& closeCommand) {
        ASSERT_TRUE(runtime.dispatch({"palette.open",  {}}).accepted());
        auto open = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(open.has_value());
        if (open) {
            ASSERT_EQ(activePickerMode(open->paletteView),
                      std::optional<ssg::SearchMode>{ssg::SearchMode::Command});
        }
        (void)runtime.dispatch({closeCommand,  {}});
        auto shut = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(shut.has_value());
        if (shut) {
            ASSERT_FALSE(shut->paletteView.activePicker.has_value());
        }
    };

    pickerStateAfter("palette.close");
    pickerStateAfter("prompt.cancel");

    // A successful palette.execute cancels the prompt as part of executing; the
    // picker must not survive into the next open.
    ASSERT_TRUE(runtime.dispatch({"palette.open",  {}}).accepted());
    (void)runtime.dispatch({"palette.execute",  ssg::PaletteExecuteArguments{"edit.undo"}});
    auto executed = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(executed.has_value());
    if (executed) {
        ASSERT_FALSE(executed->paletteView.activePicker.has_value());
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

    auto created = ssg::createEditor({workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    auto closed =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(closed.has_value());
    if (!closed) return;
    ASSERT_FALSE(closed->paletteView.activePicker.has_value());
    ASSERT_FALSE(closed->paletteView.fileCandidates.empty());

    ASSERT_TRUE(runtime.dispatch({"file_finder.open",  {}}).accepted());
    auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    auto const& palette = snapshot->paletteView;
    ASSERT_EQ(activePickerMode(palette),
              std::optional<ssg::SearchMode>{ssg::SearchMode::File});
    std::set<std::string> paths;
    for (auto const& candidate : palette.fileCandidates) paths.insert(candidate.id);
    ASSERT_TRUE(paths.contains("alpha.txt"));
    ASSERT_TRUE(paths.contains("src/beta.cpp"));

    // Candidate ids are paths here; executing one as a command must be refused
    // even though a Palette prompt is genuinely open.
    auto rejected = runtime.dispatch({"palette.execute",
         ssg::PaletteExecuteArguments{"src/beta.cpp"}});
    ASSERT_FALSE(rejected.accepted());
    // Not even a real command id is executable through the file picker.
    auto alsoRejected = runtime.dispatch({"palette.execute",
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

    auto created = ssg::createEditor({workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    auto candidateIds = [&] {
        auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        std::set<std::string> paths;
        if (snapshot) {
            for (auto const& candidate :
                 snapshot->paletteView.fileCandidates) {
                paths.insert(candidate.id);
            }
        }
        return paths;
    };

    ASSERT_TRUE(runtime.dispatch({"file_finder.open",  {}}).accepted());
    auto before = candidateIds();
    ASSERT_TRUE(before.contains("kept.txt"));
    ASSERT_FALSE(before.contains("build/hidden.o"));

    // No reopen in between: the SAME open picker must change.
    ASSERT_TRUE(runtime.dispatch({"file_finder.toggle_gitignore",  {}}).accepted());
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
    auto created = ssg::createEditor(
        {.cwd = root / "workspace",
         .scratchRoot = root / "scratch",
         .recoveryRoot = root / "recovery",
         .enableGitDiffWorker = false,
         .enableFilesystemWatcher = false});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    std::ofstream{root / "workspace" / "arrived.txt"} << "new\n";

    runtime.refreshTreeForPublication();

    auto snapshot =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_TRUE(std::ranges::any_of(
        snapshot->paletteView.fileCandidates,
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

    auto created = ssg::createEditor({workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    auto pickerIsOpen = [&] {
        auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        return snapshot &&
               activePickerMode(snapshot->paletteView) ==
                   std::optional<ssg::SearchMode>{ssg::SearchMode::File};
    };

    // A rejected open leaves the picker up.
    ASSERT_TRUE(runtime.dispatch({"file_finder.open",  {}}).accepted());
    ASSERT_TRUE(pickerIsOpen());
    auto missing = runtime.dispatch({"picker.submit",
         pickerSubmit(runtime, ssg::SearchMode::File, "gone.txt")});
    ASSERT_FALSE(missing.accepted());
    ASSERT_TRUE(pickerIsOpen());

    // A successful open dismisses it.
    ASSERT_TRUE(
        runtime
            .dispatch({"picker.submit",
                       pickerSubmit(runtime, ssg::SearchMode::File,
                                    "present.txt")})
            .accepted());
    ASSERT_FALSE(pickerIsOpen());
    std::filesystem::remove_all(root);
}

TEST(pickerSubmissionRequiresAndClosesTheAuthoritativePicker) {
    auto root = uniqueRoot();
    auto workspace = root / "workspace";
    std::ofstream{workspace / "present.txt"} << "present\n";
    ASSERT_EQ(std::system(("git -C \"" + workspace.string() +
                           "\" init -q >/dev/null 2>&1")
                              .c_str()),
              0);
    auto created = ssg::createEditor(
        {workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    // A transport cannot submit against an inventory without first opening the
    // matching authoritative picker.
    ASSERT_FALSE(
        runtime
            .dispatch({"picker.submit",
                       ssg::PickerSubmitArguments{
                           {ssg::SearchMode::Command,
                            ssg::PickerActivationId{1}},
                           "panel.toggle"}})
            .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"palette.open",  {}})
                    .accepted());
    ASSERT_TRUE(
        runtime
            .dispatch({"picker.submit",
                       pickerSubmit(runtime, ssg::SearchMode::Command,
                                    "panel.toggle")})
            .accepted());
    auto commandSubmitted =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(commandSubmitted.has_value());
    if (!commandSubmitted) return;
    ASSERT_FALSE(commandSubmitted->paletteView.activePicker.has_value());
    ASSERT_TRUE(
        ssg::effectiveUiFocus(commandSubmitted->uiTree) ==
        ssg::FocusTarget::Panel);

    ASSERT_TRUE(runtime
            .dispatch({"file_finder.open",  {}})
            .accepted());
    ASSERT_TRUE(
        runtime
            .dispatch({"picker.submit",
                       pickerSubmit(runtime, ssg::SearchMode::File,
                                    "present.txt")})
            .accepted());
    auto submitted =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(submitted.has_value());
    if (!submitted) return;
    ASSERT_FALSE(submitted->paletteView.activePicker.has_value());
    ASSERT_TRUE(ssg::effectiveUiFocus(submitted->uiTree) ==
                ssg::FocusTarget::Panel);
}

TEST(pickerSubmissionRequiresMatchingPickerMode) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    ASSERT_TRUE(
        runtime
            .dispatch({"file_finder.open",  {}})
            .accepted());
    ASSERT_FALSE(
        runtime
            .dispatch({"picker.submit",
                       pickerSubmit(runtime, ssg::SearchMode::Command,
                                    "panel.toggle")})
            .accepted());
    auto snapshot =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_TRUE(activePickerMode(snapshot->paletteView) ==
                std::optional<ssg::SearchMode>{ssg::SearchMode::File});

    ASSERT_TRUE(
        runtime
            .dispatch({"palette.close",  {}})
            .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"palette.open",  {}})
                    .accepted());
    ASSERT_TRUE(
        runtime
            .dispatch({"picker.submit",
                       pickerSubmit(runtime, ssg::SearchMode::Command,
                                    "panel.toggle")})
            .accepted());
    snapshot =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_FALSE(snapshot->paletteView.activePicker.has_value());
    std::filesystem::remove_all(root);
}

TEST(commandPickerActionThatOpensPromptDismissesPickerWithoutFailure) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .dispatch({"palette.open",  {}})
                    .accepted());

    auto result = runtime.dispatch({"picker.submit",
         pickerSubmit(runtime, ssg::SearchMode::Command, "goto.line")});
    ASSERT_TRUE(result.accepted());
    auto snapshot =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_FALSE(snapshot->paletteView.activePicker.has_value());
    ASSERT_EQ(snapshot->promptStatus.activeKind,
              std::optional{ssg::PromptKind::CommandArgument});
}

TEST(pickerSubmissionUsesActivationIdentityInsteadOfGlobalRevision) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .dispatch({"palette.open",  {}})
                    .accepted());
    auto snapshot =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot || !snapshot->paletteView.activePicker) return;
    const auto first = *snapshot->paletteView.activePicker;
    ASSERT_TRUE(
        runtime
            .dispatch({"panel.toggle",  {}})
            .accepted());
    ASSERT_TRUE(
        runtime
            .dispatch({"picker.submit",
                       ssg::PickerSubmitArguments{first, "panel.toggle"}})
            .accepted());

    ASSERT_TRUE(runtime
                    .dispatch({"palette.open",  {}})
                    .accepted());
    snapshot =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot || !snapshot->paletteView.activePicker) return;
    const auto second = *snapshot->paletteView.activePicker;
    ASSERT_FALSE(second.id == first.id);
    ASSERT_FALSE(
        runtime
            .dispatch({"picker.submit",
                       ssg::PickerSubmitArguments{first, "panel.toggle"}})
            .accepted());
    snapshot =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->paletteView.activePicker,
              std::optional<ssg::PickerActivation>{second});
}

TEST(simpleSemanticInputsLowerThroughAuthoritativeTransactions) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    const ssg::ViewportDimensions viewport{80, 24};
    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                                       std::string{"needle.txt"}})
                    .accepted());
    auto document = projectFrame(runtime, viewport);
    ASSERT_TRUE(document.has_value());
    if (!document || !document->tabs.active) return;
    const auto documentTab = *document->tabs.active;

    ASSERT_TRUE(
        runtime.dispatch({"file.new",  {}}).accepted());
    auto scratch = projectFrame(runtime, viewport);
    ASSERT_TRUE(scratch.has_value());
    if (!scratch || !scratch->tabs.active) return;
    const auto scratchTab = *scratch->tabs.active;
    auto activate = runtime.input(ssg::TabPointerInput{documentTab});
    ASSERT_EQ(activate.outcome, ssg::ClientInputOutcome::Dispatched);
    ASSERT_TRUE(activate.command.has_value() && activate.command->accepted());
    ASSERT_EQ(projectFrame(runtime, viewport)->tabs.active,
              std::optional<ssg::TabId>{documentTab});

    auto closeBasis = projectFrame(runtime, viewport);
    ASSERT_TRUE(closeBasis.has_value());
    if (!closeBasis) return;
    auto close = runtime.input(ssg::TabPointerInput{scratchTab,
                             ssg::InputPointerButton::Auxiliary});
    ASSERT_TRUE(close.command.has_value() && close.command->accepted());
    auto afterClose = projectFrame(runtime, viewport);
    ASSERT_TRUE(afterClose.has_value());
    ASSERT_TRUE(std::none_of(
        afterClose->tabs.tabs.begin(),
        afterClose->tabs.tabs.end(),
        [&](const ssg::TabState& tab) { return tab.id == scratchTab; }));

    ASSERT_TRUE(runtime
                    .dispatch({"palette.open",  {}})
                    .accepted());
    auto palette = projectFrame(runtime, viewport);
    ASSERT_TRUE(palette.has_value());
    if (!palette || !palette->paletteView.activePicker) return;
    const auto activation = *palette->paletteView.activePicker;
    auto submit = runtime.input(ssg::PickerPointerInput{activation, "panel.toggle"});
    ASSERT_TRUE(submit.command.has_value() && submit.command->accepted());
    auto afterSubmit = projectFrame(runtime, viewport);
    ASSERT_TRUE(afterSubmit.has_value());
    ASSERT_FALSE(afterSubmit->paletteView.activePicker.has_value());
    ASSERT_TRUE(afterSubmit->panel.has_value());

    const ssg::UiNode* actionNode = nullptr;
    const auto findActionNode = [&](const auto& self,
                                     const ssg::UiNode& node) -> void {
        if (actionNode) return;
        if (node.resolved && node.resolved->command &&
            !node.resolved->command->empty()) {
            actionNode = &node;
            return;
        }
        if (const auto* container =
                std::get_if<ssg::UiContainer>(&node.content)) {
            for (const auto& child : container->children) self(self, child);
        }
    };
    findActionNode(findActionNode,
                    afterSubmit->uiTree.root);
    ASSERT_TRUE(actionNode != nullptr);
    if (!actionNode) return;
    auto invalidUiAction = runtime.dispatch({"ui.activate",
         ssg::UiNodeActivationArguments{
             ssg::UiNodeId{"missing.action"}}});
    ASSERT_FALSE(invalidUiAction.accepted());

    auto publishedAction = runtime.dispatch({"ui.activate",
         ssg::UiNodeActivationArguments{actionNode->id}});
    ASSERT_TRUE(publishedAction.accepted());

    for (auto target : {ssg::ScrollTarget::Document,
                        ssg::ScrollTarget::Tree}) {
        auto scroll = runtime.input(ssg::ScrollLinesInput{
                        {target, 1}});
        ASSERT_TRUE(scroll.command.has_value() && scroll.command->accepted());
        auto fraction = runtime.input(ssg::ScrollFractionInput{
                        {target, 0, 1}});
        ASSERT_TRUE(fraction.command.has_value() &&
                    fraction.command->accepted());
    }

    auto invalidScroll = runtime.input(ssg::ScrollFractionInput{
                    {ssg::ScrollTarget::Document, 2, 1}});
    ASSERT_EQ(invalidScroll.outcome, ssg::ClientInputOutcome::Rejected);

    ASSERT_TRUE(runtime
                    .dispatch({"replace.open",  {}})
                    .accepted());
    auto replace = projectFrame(runtime, viewport);
    ASSERT_TRUE(replace.has_value());
    if (!replace) return;
    const auto replacementNode =
        ssg::footerPromptControlNodeId("replace.replacement");
    auto focusReplacement = runtime.dispatch({"ui.activate",
         ssg::UiNodeActivationArguments{replacementNode}});
    ASSERT_TRUE(focusReplacement.accepted());
    auto focused = projectFrame(runtime, viewport);
    ASSERT_TRUE(focused.has_value());
    if (focused) {
        const auto* replacement = ssg::findUiNode(
            focused->uiTree, replacementNode);
        ASSERT_TRUE(replacement != nullptr);
        if (replacement) {
            ASSERT_TRUE(replacement->resolved.has_value());
            ASSERT_EQ(replacement->resolved->active, std::optional<bool>{true});
        }
        const auto missing = runtime.dispatch({"ui.activate",
             ssg::UiNodeActivationArguments{
                 ssg::UiNodeId{"missing.control"}}});
        ASSERT_FALSE(missing.accepted());
        ASSERT_TRUE(runtime
                        .dispatch({"prompt.cancel",  {}})
                        .accepted());
        const auto hidden = runtime.dispatch({"ui.activate",
             ssg::UiNodeActivationArguments{replacementNode}});
        ASSERT_FALSE(hidden.accepted());
    }
}

TEST(resolvedSelectionInputRejectsMalformedModelIdentity) {
    auto runtime = followPauseRuntime("a\xC3\xA9z\n");
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    auto frame = ssg::test::projectGridFrame(*runtime);
    ASSERT_TRUE(frame && frame->tabs.active);
    if (!frame || !frame->tabs.active) return;
    const auto valid = ssg::ViewTransitionInput{ssg::SelectionTransition{
        *frame->tabs.active, frame->documentRevision,
        {{ssg::ByteOffset{1}, ssg::ByteOffset{3}}}}};
    const auto before = frame->selections;
    const auto expectRejected = [&](ssg::ViewTransitionInput input) {
        ASSERT_EQ(runtime->input(std::move(input)).outcome,
                  ssg::ClientInputOutcome::Rejected);
        auto after = ssg::test::projectGridFrame(*runtime);
        ASSERT_TRUE(after.has_value());
        if (after) ASSERT_EQ(after->selections, before);
    };
    auto wrongTab = valid;
    std::get<ssg::SelectionTransition>(wrongTab.transition).activeTab =
        ssg::TabId{frame->tabs.active->value() + 1};
    expectRejected(wrongTab);
    auto wrongDocument = valid;
    ++std::get<ssg::SelectionTransition>(wrongDocument.transition)
          .documentRevision;
    expectRejected(wrongDocument);
    auto empty = valid;
    std::get<ssg::SelectionTransition>(empty.transition).selections.clear();
    expectRejected(empty);
    auto insideCodePoint = valid;
    std::get<ssg::SelectionTransition>(insideCodePoint.transition)
        .selections.front().active = ssg::ByteOffset{2};
    expectRejected(insideCodePoint);
    ASSERT_EQ(runtime->input(valid).outcome,
              ssg::ClientInputOutcome::Dispatched);
}

TEST(documentPointerInputOwnsSelectionGesturePolicy) {
    auto root = uniqueRoot();
    std::filesystem::create_directories(root / "workspace");
    std::ofstream{root / "workspace" / "words.txt"} << "alpha beta gamma\n";
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open", std::string{"words.txt"}})
                    .accepted());

    auto press = runtime.input(
        ssg::DocumentPointerInput{ssg::ByteOffset{1}});
    ASSERT_TRUE(press.command && press.command->accepted());
    auto move = runtime.input(ssg::DocumentPointerInput{
        ssg::ByteOffset{9}, false, false, ssg::InputPointerButton::Primary,
        ssg::InputPointerPhase::Move});
    ASSERT_TRUE(move.command && move.command->accepted());
    auto frame = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    ASSERT_EQ(frame->selections.primary().anchor.byteOffset,
              ssg::ByteOffset{1});
    ASSERT_EQ(frame->selections.primary().active.byteOffset,
              ssg::ByteOffset{9});

    ASSERT_TRUE(runtime.dispatch(
        {"text.insert", ssg::TextInputArguments{"X"}}).accepted());
    auto changedDocumentMove = runtime.input(ssg::DocumentPointerInput{
        ssg::ByteOffset{1}, false, false, ssg::InputPointerButton::Primary,
        ssg::InputPointerPhase::Move});
    ASSERT_EQ(changedDocumentMove.outcome,
              ssg::ClientInputOutcome::Rejected);
}

TEST(keyInputRoutingBranchesByPromptMode) {
    auto root = uniqueRoot();
    std::filesystem::create_directories(root / "workspace");
    std::ofstream{root / "workspace" / "lines.txt"} << "one\ntwo\nthree\n";
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(
        runtime.dispatch({"file.open", std::string{"lines.txt"}}).accepted());

    const auto key = [&](ssg::KeyCode code, bool alt = false) {
        ssg::KeyStroke stroke;
        stroke.code = code;
        stroke.alt = alt;
        return runtime.input(ssg::ClientKeyInput{stroke, {}});
    };
    const auto activePromptValue = [&]() {
        auto controls = runtime.resolvedPromptControls();
        if (!controls) return std::string{};
        std::size_t index = 0;
        for (auto const& control : controls->controls) {
            if (control.kind != ssg::PromptControlKind::Input) continue;
            if (index++ == controls->activeInput) return control.value;
        }
        return std::string{};
    };

    ASSERT_TRUE(runtime.dispatch({"palette.open", {}}).accepted());
    struct PaletteCase {
        ssg::KeyCode code;
        ssg::ClientOwnedInputKind expected;
    };
    for (auto const test : {
             PaletteCase{ssg::KeyCode::Enter,
                         ssg::ClientOwnedInputKind::Submit},
             PaletteCase{ssg::KeyCode::ArrowDown,
                         ssg::ClientOwnedInputKind::SelectNext},
             PaletteCase{ssg::KeyCode::ArrowUp,
                         ssg::ClientOwnedInputKind::SelectPrevious},
             PaletteCase{ssg::KeyCode::Backspace,
                         ssg::ClientOwnedInputKind::DeleteGraphemeBackward},
         }) {
        auto result = key(test.code);
        ASSERT_EQ(result.outcome, ssg::ClientInputOutcome::ClientOwned);
        ASSERT_TRUE(result.clientOwned.has_value());
        if (result.clientOwned) {
            ASSERT_EQ(result.clientOwned->kind, test.expected);
        }
    }
    auto deleteWord = key(ssg::KeyCode::Backspace, true);
    ASSERT_EQ(deleteWord.outcome, ssg::ClientInputOutcome::ClientOwned);
    ASSERT_TRUE(deleteWord.clientOwned.has_value());
    if (deleteWord.clientOwned) {
        ASSERT_EQ(deleteWord.clientOwned->kind,
                  ssg::ClientOwnedInputKind::DeleteWordBackward);
    }
    auto append = runtime.input(ssg::ClientKeyInput{{}, "query"});
    ASSERT_EQ(append.outcome, ssg::ClientInputOutcome::ClientOwned);
    ASSERT_TRUE(append.clientOwned.has_value());
    if (append.clientOwned) {
        ASSERT_EQ(append.clientOwned->kind,
                  ssg::ClientOwnedInputKind::AppendText);
        ASSERT_EQ(append.clientOwned->text, std::string{"query"});
    }
    ASSERT_EQ(key(ssg::KeyCode::Escape).outcome,
              ssg::ClientInputOutcome::Dispatched);

    struct PromptCase {
        char const* openCommand;
        char const* text;
    };
    for (auto const test :
         {PromptCase{"find.open", "needle"},
          PromptCase{"replace.open", "replacement"},
          PromptCase{"goto.line", "3"}}) {
        ASSERT_TRUE(runtime.dispatch({test.openCommand, {}}).accepted());
        auto typed = runtime.input(ssg::ClientKeyInput{{}, test.text});
        ASSERT_EQ(typed.outcome, ssg::ClientInputOutcome::Dispatched);
        ASSERT_EQ(activePromptValue(), std::string{test.text});
        auto erased = key(ssg::KeyCode::Backspace);
        ASSERT_EQ(erased.outcome, ssg::ClientInputOutcome::Dispatched);
        ASSERT_EQ(activePromptValue(),
                  std::string{test.text}.substr(
                      0, std::string{test.text}.size() - 1));
        ASSERT_EQ(key(ssg::KeyCode::Escape).outcome,
                  ssg::ClientInputOutcome::Dispatched);
    }
}

TEST(viewTransitionsExecuteTypedEditorMutations) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    auto missingNotice =
        runtime.input(ssg::NoticeActionPointerInput{"missing"});
    ASSERT_EQ(missingNotice.outcome, ssg::ClientInputOutcome::Rejected);
    ASSERT_TRUE(missingNotice.command.has_value());
    if (missingNotice.command) {
        ASSERT_EQ(missingNotice.command->message,
                  std::string{"notice action target is not present"});
    }

    ASSERT_TRUE(runtime.dispatch({"panel.toggle", {}}).accepted());
    ASSERT_EQ(runtime.screen.effectiveFocus(), ssg::FocusTarget::Panel);
    ASSERT_TRUE(runtime.dispatch({"palette.open", {}}).accepted());
    ASSERT_EQ(runtime.screen.effectiveFocus(), ssg::FocusTarget::Prompt);

    auto paneFocus = runtime.input(ssg::ViewTransitionInput{
        ssg::PaneFocusTransition{runtime.paneTopology.activePane()}});
    ASSERT_EQ(paneFocus.outcome, ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(runtime.screen.effectiveFocus(), ssg::FocusTarget::Prompt);
    ASSERT_TRUE(runtime.dispatch({"palette.close", {}}).accepted());
    ASSERT_EQ(runtime.screen.effectiveFocus(), ssg::FocusTarget::Editor);

    auto missingPane = runtime.input(ssg::ViewTransitionInput{
        ssg::PaneFocusTransition{ssg::PaneId{999}}});
    ASSERT_EQ(missingPane.outcome, ssg::ClientInputOutcome::Rejected);
}

TEST(documentPointerGestureEndsWhenSelectionCommandIsRejected) {
    auto root = uniqueRoot();
    std::filesystem::create_directories(root / "workspace");
    std::ofstream{root / "workspace" / "words.txt"} << "alpha beta gamma\n";

    const auto makeRuntime = [&](std::string_view name) {
        auto instanceRoot = root / name;
        auto created = ssg::createEditor(
            {root / "workspace", instanceRoot / "scratch",
             instanceRoot / "recovery"});
        if (created.accepted()) {
            (void)created.session->dispatch(
                {"file.open", std::string{"words.txt"}});
        }
        return created;
    };
    const auto rejectSelectionCommand = [](ssg::Editor& runtime,
                                           std::string id) {
        const auto replaced = runtime.commandCatalog().handleFor(id);
        ASSERT_TRUE(replaced.valid());
        if (!replaced.valid()) return;
        std::array retire{replaced};
        (void)runtime.replaceCommandGeneration(
            retire,
            {ssg::CommandSpec{
                .id = std::move(id),
                .owner = "test-oracle",
                .summary = "rejects pointer selection",
                .effect = ssg::CommandEffect::Mutation,
                .binding =
                    ssg::bindWireHandler<ssg::SelectionCommandArguments>(
                        [](ssg::CommandContext&,
                           ssg::SelectionCommandArguments const&) {
                            return ssg::CommandHandlerResult::failure(
                                "selection refused");
                        }),
            }});
    };

    auto pressCreated = makeRuntime("press");
    ASSERT_TRUE(pressCreated.accepted());
    if (!pressCreated.accepted()) return;
    rejectSelectionCommand(*pressCreated.session, "cursor.set_position");
    auto refusedPress = pressCreated.session->input(
        ssg::DocumentPointerInput{ssg::ByteOffset{1}});
    ASSERT_EQ(refusedPress.outcome, ssg::ClientInputOutcome::Rejected);
    ASSERT_FALSE(pressCreated.session->documentPointerGesture.has_value());
    auto moveAfterPress = pressCreated.session->input(ssg::DocumentPointerInput{
        ssg::ByteOffset{2}, false, false, ssg::InputPointerButton::Primary,
        ssg::InputPointerPhase::Move});
    ASSERT_EQ(moveAfterPress.outcome, ssg::ClientInputOutcome::Unhandled);

    auto moveCreated = makeRuntime("move");
    ASSERT_TRUE(moveCreated.accepted());
    if (!moveCreated.accepted()) return;
    auto acceptedPress = moveCreated.session->input(
        ssg::DocumentPointerInput{ssg::ByteOffset{1}});
    ASSERT_TRUE(acceptedPress.command && acceptedPress.command->accepted());
    rejectSelectionCommand(*moveCreated.session, "select.set_range");
    auto refusedMove = moveCreated.session->input(ssg::DocumentPointerInput{
        ssg::ByteOffset{9}, false, false, ssg::InputPointerButton::Primary,
        ssg::InputPointerPhase::Move});
    ASSERT_EQ(refusedMove.outcome, ssg::ClientInputOutcome::Rejected);
    ASSERT_FALSE(moveCreated.session->documentPointerGesture.has_value());
    auto moveAfterRejection =
        moveCreated.session->input(ssg::DocumentPointerInput{
            ssg::ByteOffset{3}, false, false,
            ssg::InputPointerButton::Primary, ssg::InputPointerPhase::Move});
    ASSERT_EQ(moveAfterRejection.outcome, ssg::ClientInputOutcome::Unhandled);
}

TEST(documentEdgeMovesResolveThroughPresenterAndReveal) {
    auto root = uniqueRoot();
    std::filesystem::create_directories(root / "workspace");
    std::ofstream{root / "workspace" / "lines.txt"}
        << "aa\nbb\ncc\ndd\nee\nff\n";
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ssg::GridPresenter presenter{};
    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                                       std::string{"lines.txt"}})
                    .accepted());
    auto frame = presenter.project(runtime, {{20, 4}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    const auto noGesture = runtime.input(ssg::ViewTransitionInput{
            ssg::PointerSelectionTransition{ssg::ByteOffset{1}}});
    ASSERT_EQ(noGesture.outcome, ssg::ClientInputOutcome::Rejected);
    ASSERT_TRUE(runtime
                    .input(ssg::DocumentPointerInput{
                                       ssg::ByteOffset{1}})
                    .command->accepted());
    frame = presenter.project(runtime, {{20, 4}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    const auto invalidEdge = presenter.apply(
        ssg::ContinuePointerEdge{ssg::DocumentPointerEdge::None},
        *frame);
    ASSERT_FALSE(invalidEdge.accepted());

    for (const auto expected :
         {ssg::ByteOffset{4}, ssg::ByteOffset{7}, ssg::ByteOffset{10}}) {
        auto edge = runtime.input(ssg::DocumentPointerInput{
                        std::nullopt, false, false,
                        ssg::InputPointerButton::Primary,
                        ssg::InputPointerPhase::Move,
                        ssg::DocumentPointerEdge::After});
        ASSERT_EQ(edge.outcome, ssg::ClientInputOutcome::ViewOwned);
        ASSERT_TRUE(edge.command.has_value() && edge.command->viewAction);
        if (!edge.command || !edge.command->viewAction) return;
        const auto expectedAction = ssg::ViewAction{
            ssg::ContinuePointerEdge{
                ssg::DocumentPointerEdge::After}};
        ASSERT_EQ(*edge.command->viewAction, expectedAction);
        auto applied = presenter.apply(*edge.command->viewAction, *frame);
        ASSERT_TRUE(applied.accepted() && applied.transition.has_value());
        ASSERT_TRUE(
            applied.transition &&
            std::holds_alternative<ssg::PointerSelectionTransition>(
                applied.transition->transition));
        ASSERT_FALSE(
            presenter.apply(*edge.command->viewAction, *frame).accepted());
        if (!applied.transition) return;
        ASSERT_EQ(runtime.input(*applied.transition).outcome,
                  ssg::ClientInputOutcome::Dispatched);
        auto snapshot = presenter.project(runtime, {{20, 4}, {}});
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return;
        ASSERT_EQ(snapshot->selections.primary().anchor.byteOffset,
                  ssg::ByteOffset{1});
        ASSERT_EQ(snapshot->selections.primary().active.byteOffset,
                  expected);
        frame = std::move(snapshot);
    }
    auto beforeEdge = runtime.input(ssg::DocumentPointerInput{
                    std::nullopt, false, false,
                    ssg::InputPointerButton::Primary,
                    ssg::InputPointerPhase::Move,
                    ssg::DocumentPointerEdge::Before});
    ASSERT_TRUE(beforeEdge.command && beforeEdge.command->viewAction);
    if (!beforeEdge.command || !beforeEdge.command->viewAction) return;
    const auto beforeAction = ssg::ViewAction{
        ssg::ContinuePointerEdge{ssg::DocumentPointerEdge::Before}};
    ASSERT_EQ(*beforeEdge.command->viewAction, beforeAction);
    auto beforeApplied =
        presenter.apply(*beforeEdge.command->viewAction, *frame);
    ASSERT_TRUE(beforeApplied.transition.has_value());
    if (!beforeApplied.transition) return;
    ASSERT_EQ(runtime.input(*beforeApplied.transition).outcome,
              ssg::ClientInputOutcome::Dispatched);
    auto snapshot = presenter.project(runtime, {{20, 4}, {}});
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) {
        ASSERT_EQ(snapshot->selections.primary().active.byteOffset,
                  ssg::ByteOffset{7});
        ASSERT_TRUE(
            snapshot->viewport.firstVisualRow > 0);
    }
}

TEST(documentEdgeContinuationPreservesAdditiveBaseline) {
    auto root = uniqueRoot();
    std::filesystem::create_directories(root / "workspace");
    std::ofstream{root / "workspace" / "lines.txt"}
        << "aa\nbb\ncc\ndd\nee\nff\n";
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                                       std::string{"lines.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .input(ssg::DocumentPointerInput{
                                       ssg::ByteOffset{1}})
                    .command->accepted());
    ASSERT_EQ(
        runtime
            .input(ssg::DocumentPointerInput{
                       std::nullopt, false, false,
                       ssg::InputPointerButton::Primary,
                       ssg::InputPointerPhase::Release})
            .outcome,
        ssg::ClientInputOutcome::Dispatched);
    const auto second = ssg::resolveSelectionPosition(
        runtime.activeDocumentText(), ssg::ByteOffset{7});
    ASSERT_TRUE(second.has_value());
    if (!second) return;
    ASSERT_TRUE(runtime
                    .dispatch({"select.add_range",
                               ssg::SelectionCommandArguments{
                                   std::nullopt,
                                   ssg::Selection{*second, *second}}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .input(ssg::DocumentPointerInput{
                               ssg::ByteOffset{10}, true})
                    .command->accepted());

    ssg::GridPresenter presenter{};
    auto frame = presenter.project(runtime, {{20, 4}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    const auto baseline = frame->selections.items();
    ASSERT_EQ(baseline.size(), std::size_t{3});
    auto edge = runtime.input(ssg::DocumentPointerInput{
                    std::nullopt, false, false,
                    ssg::InputPointerButton::Primary,
                    ssg::InputPointerPhase::Move,
                    ssg::DocumentPointerEdge::After});
    ASSERT_TRUE(edge.command && edge.command->viewAction);
    if (!edge.command || !edge.command->viewAction) return;
    auto applied = presenter.apply(*edge.command->viewAction, *frame);
    ASSERT_TRUE(applied.transition.has_value());
    if (!applied.transition) return;
    ASSERT_EQ(runtime.input(*applied.transition).outcome,
              ssg::ClientInputOutcome::Dispatched);
    frame = presenter.project(runtime, {{20, 4}, {}});
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    const auto& selections = frame->selections.items();
    ASSERT_EQ(selections.size(), baseline.size());
    ASSERT_EQ(selections[0], baseline[0]);
    ASSERT_EQ(selections[1], baseline[1]);
    ASSERT_EQ(selections[2].anchor.byteOffset, ssg::ByteOffset{10});
    ASSERT_EQ(selections[2].active.byteOffset, ssg::ByteOffset{13});
}

TEST(failedSelectedCommandLeavesPickerOpenForEveryOrigin) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    std::optional<ssg::PickerActivation> nestedActivation;
    auto const failureCommand = runtime.registerCommand(ssg::CommandSpec{
        .id = "test.picker_failure",
        .owner = "test",
        .summary = "Picker failure",
        .effect = ssg::CommandEffect::Mutation,
        .binding = ssg::bindNoArgumentHandler([](ssg::CommandContext&) {
            return ssg::CommandHandlerResult::failure("expected failure");
        }),
    });
    ASSERT_TRUE(failureCommand.valid());
    auto const nestedFailure = runtime.registerCommand(ssg::CommandSpec{
        .id = "test.picker_nested_failure",
        .owner = "test",
        .summary = "Picker nested failure",
        .effect = ssg::CommandEffect::Mutation,
        .binding = ssg::bindNoArgumentHandler([](ssg::CommandContext&) {
            return ssg::CommandHandlerResult::failure(
                "expected nested failure");
        }),
    });
    ASSERT_TRUE(nestedFailure.valid());
    auto const deferringCommand = runtime.registerCommand(ssg::CommandSpec{
        .id = "test.picker_defers_failure",
        .owner = "test",
        .summary = "Picker defers failure",
        .effect = ssg::CommandEffect::Mutation,
        .binding = ssg::bindNoArgumentHandler(
            [&runtime](ssg::CommandContext& context) {
                if (!runtime.deferDispatch(
                        {"test.picker_nested_failure",
                         {}})) {
                    return ssg::CommandHandlerResult::failure(
                        "could not defer nested failure");
                }
                return ssg::CommandHandlerResult::success();
            }),
    });
    ASSERT_TRUE(deferringCommand.valid());
    auto const nestedSubmit = runtime.registerCommand(ssg::CommandSpec{
        .id = "test.picker_defers_submit",
        .owner = "test",
        .summary = "Picker defers another submit",
        .effect = ssg::CommandEffect::Mutation,
        .binding = ssg::bindNoArgumentHandler(
            [&runtime, &nestedActivation](ssg::CommandContext& context) {
                if (!nestedActivation) {
                    return ssg::CommandHandlerResult::failure(
                        "picker activation was not captured");
                }
                if (!runtime.deferDispatch(
                        {"picker.submit",
                         ssg::PickerSubmitArguments{*nestedActivation,
                                                    "panel.toggle"}})) {
                    return ssg::CommandHandlerResult::failure(
                        "could not defer nested picker submit");
                }
                return ssg::CommandHandlerResult::success();
            }),
    });
    ASSERT_TRUE(nestedSubmit.valid());
    ASSERT_TRUE(runtime
                    .dispatch({"palette.open",  {}})
                    .accepted());
    auto opened =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(opened.has_value());
    if (!opened) return;
    nestedActivation = opened->paletteView.activePicker;
    ASSERT_TRUE(nestedActivation.has_value());
    ASSERT_FALSE(
        runtime
            .dispatch({"picker.submit",
                       pickerSubmit(runtime, ssg::SearchMode::Command,
                                    "test.picker_failure")})
            .accepted());
    auto snapshot =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_TRUE(activePickerMode(snapshot->paletteView) ==
                std::optional<ssg::SearchMode>{ssg::SearchMode::Command});
    ASSERT_TRUE(ssg::effectiveUiFocus(snapshot->uiTree) ==
                ssg::FocusTarget::Prompt);

    ASSERT_FALSE(
        runtime
            .dispatch({"picker.submit",
                       pickerSubmit(runtime, ssg::SearchMode::Command,
                                    "test.picker_defers_submit")})
            .accepted());
    snapshot =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_TRUE(activePickerMode(snapshot->paletteView) ==
                std::optional<ssg::SearchMode>{ssg::SearchMode::Command});
    ASSERT_TRUE(ssg::effectiveUiFocus(snapshot->uiTree) ==
                ssg::FocusTarget::Prompt);

    ASSERT_FALSE(
        runtime
            .dispatch({"picker.submit",
                       pickerSubmit(runtime, ssg::SearchMode::Command,
                                    "test.picker_defers_failure")})
            .accepted());
    snapshot =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_TRUE(activePickerMode(snapshot->paletteView) ==
                std::optional<ssg::SearchMode>{ssg::SearchMode::Command});
    ASSERT_TRUE(ssg::effectiveUiFocus(snapshot->uiTree) ==
                ssg::FocusTarget::Prompt);

}

TEST(selectedCommandThatOpensAnotherPickerKeepsTheNewPicker) {
        auto root = uniqueRoot();
        auto created = ssg::createEditor(
            {root / "workspace", root / "scratch", root / "recovery"});
        ASSERT_TRUE(created.accepted());
        if (!created.accepted()) return;
        auto& runtime = *created.session;
        ASSERT_TRUE(runtime
                        .dispatch({"file.open",
                                   std::string{"needle.txt"}})
                        .accepted());
        ASSERT_TRUE(runtime
                        .dispatch({"palette.open",  {}})
                        .accepted());
        ASSERT_TRUE(
            runtime
                .dispatch({"picker.submit",
                           pickerSubmit(runtime, ssg::SearchMode::Command,
                                        "file_finder.open")})
                .accepted());
        auto snapshot =
            projectFrame(runtime, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return;
        ASSERT_TRUE(activePickerMode(snapshot->paletteView) ==
                    std::optional<ssg::SearchMode>{ssg::SearchMode::File});
        ASSERT_TRUE(snapshot->promptStatus.activeKind ==
                    std::optional<ssg::PromptKind>{ssg::PromptKind::Palette});
        ASSERT_TRUE(ssg::effectiveUiFocus(snapshot->uiTree) ==
                    ssg::FocusTarget::Prompt);
}

TEST(selectedCommandThatReopensTheSamePickerKeepsTheNewActivation) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .dispatch({"palette.open",  {}})
                    .accepted());
    auto before =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(before.has_value());
    if (!before || !before->paletteView.activePicker) return;
    const auto first = *before->paletteView.activePicker;

    ASSERT_TRUE(
        runtime
            .dispatch({"picker.submit",
                       ssg::PickerSubmitArguments{first, "palette.open"}})
            .accepted());
    auto after =
        projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(after.has_value());
    if (!after || !after->paletteView.activePicker) return;
    ASSERT_TRUE(after->paletteView.activePicker->mode ==
                ssg::SearchMode::Command);
    ASSERT_FALSE(after->paletteView.activePicker->id == first.id);
    ASSERT_TRUE(ssg::effectiveUiFocus(after->uiTree) ==
                ssg::FocusTarget::Prompt);
}

TEST(paletteExecuteValidatesCandidateMembership) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    ASSERT_TRUE(runtime.dispatch({"file.open",  std::string{"needle.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"palette.open",  {}}).accepted());

    // A command outside the published candidate set is rejected before dispatch.
    ASSERT_FALSE(runtime.dispatch({"palette.execute",  ssg::PaletteExecuteArguments{"not.a.command"}}).accepted());
    // Missing the id payload is rejected.
    ASSERT_FALSE(runtime.dispatch({"palette.execute",  {}}).accepted());
    // A published command id validates, executes server-side, and closes the palette.
    ASSERT_TRUE(runtime.dispatch({"palette.execute",  ssg::PaletteExecuteArguments{"file.save"}}).accepted());
    auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(ssg::effectiveUiFocus(snapshot->uiTree),
              ssg::FocusTarget::Editor);
}

TEST(paletteCandidatesCarryLabelsAndKeyDetail) {
    auto root = uniqueRoot();
    auto created = ssg::createEditor({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"palette.open",  {}}).accepted());
    auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    const auto& candidates = snapshot->paletteView.commandCandidates;
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
    auto root = testRuntimePath("runtime_nav_treescroll");
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
    auto created = ssg::createEditor({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    // Show the panel; a 12-row terminal gives a panel content height of ~9.
    ASSERT_TRUE(runtime.dispatch({"panel.toggle",  {}}).accepted());
    // Select the workspace root and expand it so its 40 files become visible.
    ASSERT_TRUE(runtime.dispatch({"tree.select_next",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"tree.activate",  {}}).accepted());
    const ssg::ViewportDimensions dims{80, 12};
    ssg::test::GridTestView grid{dims};

    // Baseline: selection at the top (root), window pinned to the top with a live
    // thumb.
    {
        auto snap = grid.present(runtime);
        ASSERT_TRUE(snap.has_value());
        if (!snap) return;
        ASSERT_TRUE(snap->panel.has_value());
        if (!snap->panel) return;
        auto const& w = *snap->panel;
        ASSERT_TRUE(w.scrollbar.totalRows >= 40);
        ASSERT_EQ(w.firstVisible, std::uint32_t{0});
        ASSERT_TRUE(w.scrollbar.maximumFirstRow > 0);          // scrollable
        ASSERT_TRUE(w.scrollbar.thumbSize < w.scrollbar.viewportRows);
        ASSERT_EQ(w.rows.size(),
                  std::size_t{w.scrollbar.viewportRows});       // window bound
    }

    // Move the selection to the bottom: the window scrolls to keep it shown.
    for (int i = 0; i < 60; ++i) {
        ASSERT_TRUE(runtime.dispatch({"tree.select_next",  {}}).accepted());
    }
    std::uint32_t deepFirst = 0;
    {
        auto snap = grid.present(runtime);
        ASSERT_TRUE(snap.has_value());
        if (!snap) return;
        ASSERT_TRUE(snap->panel.has_value());
        if (!snap->panel) return;
        auto const& w = *snap->panel;
        ASSERT_TRUE(std::ranges::any_of(
            w.rows, [](const ssg::SolvedPanelRow& row) {
                return row.selected;
            }));
        ASSERT_TRUE(w.firstVisible > 0);
        deepFirst = w.firstVisible;
    }
    ASSERT_TRUE(deepFirst > 0);

    // Move back up to the top: the window scrolls back to first_visible == 0.
    for (int i = 0; i < 40; ++i) {
        ASSERT_TRUE(runtime.dispatch({"tree.select_previous",  {}}).accepted());
    }
    {
        auto snap = grid.present(runtime);
        ASSERT_TRUE(snap.has_value());
        if (!snap) return;
        ASSERT_TRUE(snap->panel.has_value());
        if (snap->panel) {
            ASSERT_EQ(snap->panel->firstVisible, std::uint32_t{0});
        }
    }
    std::filesystem::remove_all(root);
}

TEST(treeSelectSetsSelectionToANodeAndRejectsUnknownIds) {
    auto root = testRuntimePath("runtime_nav_treeselect");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    for (int i = 0; i < 6; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "file-%02d.txt", i);
        std::ofstream{root / "workspace" / name} << "x";
    }
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"panel.toggle",  {}}).accepted());
    // Expand the workspace root so its files become visible/selectable nodes.
    ASSERT_TRUE(runtime.dispatch({"tree.select_next",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"tree.activate",  {}}).accepted());

    const ssg::ViewportDimensions dims{80, 24};
    auto snap = projectFrame(runtime, dims);
    ASSERT_TRUE(snap.has_value());
    if (!snap) return;
    ASSERT_TRUE(snap->panel.has_value());
    if (!snap->panel) return;
    auto const& nodes = snap->panel->rows;
    ASSERT_TRUE(nodes.size() >= 4);
    if (nodes.size() < 4) return;
    // Pick a node that is NOT already selected (the third visible node).
    auto const target = nodes[2].nodeId;

    ASSERT_TRUE(runtime.dispatch({"tree.select",
                                  ssg::TreeSelectArguments{target}}).accepted());
    auto after = projectFrame(runtime, dims);
    ASSERT_TRUE(after.has_value());
    if (!after) return;
    ASSERT_TRUE(after->panel.has_value());
    if (!after->panel) return;
    ASSERT_TRUE(std::ranges::any_of(
        after->panel->rows, [&](const ssg::SolvedPanelRow& row) {
            return row.nodeId == target && row.selected;
        }));

    // An id absent from the active provider is rejected; a missing payload too.
    ASSERT_FALSE(runtime.dispatch({"tree.select",
                                   ssg::TreeSelectArguments{ssg::TreeNodeId{"nope"}}}).accepted());
    ASSERT_FALSE(runtime.dispatch({"tree.select",  {}}).accepted());
    // The selection is unchanged after the rejected attempts.
    auto again = projectFrame(runtime, dims);
    ASSERT_TRUE(again.has_value());
    if (!again) return;
    ASSERT_TRUE(again->panel.has_value());
    if (!again->panel) return;
    ASSERT_TRUE(std::ranges::any_of(
        again->panel->rows, [&](const ssg::SolvedPanelRow& row) {
            return row.nodeId == target && row.selected;
        }));
    std::filesystem::remove_all(root);
}

TEST(treeSelectFocusesThePanelAndTheClickPairNetsExpectedFocus) {
    auto root = testRuntimePath("runtime_nav_treefocus");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace" / "dir");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "dir" / "inner.txt"} << "x";
    std::ofstream{root / "workspace" / "top.txt"} << "hello";
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    const ssg::ViewportDimensions dims{80, 24};
    auto focus = [&] {
        auto snap = projectFrame(runtime, dims);
        return snap ? ssg::effectiveUiFocus(snap->uiTree)
                    : ssg::FocusTarget::Editor;
    };
    // Showing the panel now focuses it (QOL); the CWD's entries are top-level, so
    // "dir" and "top.txt" are both immediately visible/selectable.
    ASSERT_TRUE(runtime.dispatch({"panel.toggle",  {}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);

    auto snap = projectFrame(runtime, dims);
    ASSERT_TRUE(snap.has_value());
    if (!snap) return;
    std::optional<ssg::TreeNodeId> dirId;
    std::optional<ssg::TreeNodeId> fileId;
    ASSERT_TRUE(snap->panel.has_value());
    if (!snap->panel) return;
    for (auto const& row : snap->panel->rows) {
        if (row.text.find("dir") != std::string::npos) dirId = row.nodeId;
        if (row.text.find("top.txt") != std::string::npos) fileId = row.nodeId;
    }
    ASSERT_TRUE(dirId.has_value());
    ASSERT_TRUE(fileId.has_value());
    if (!dirId || !fileId) return;

    // The file click pair [tree.select, tree.activate] ends on the editor (the
    // file opens, so tree.activate's focus_editor wins over tree.select's panel).
    ASSERT_TRUE(runtime.dispatch({"tree.activate_node",
         ssg::TreeSelectArguments{*fileId}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Editor);

    // From editor focus, tree.select alone moves keyboard focus to the panel.
    ASSERT_TRUE(runtime.dispatch({"tree.select",  ssg::TreeSelectArguments{*fileId}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);

    // The directory click pair ends on the panel (tree.select focuses the panel,
    // tree.activate toggles the directory and leaves focus alone).
    ASSERT_TRUE(runtime.dispatch({"tree.select",  ssg::TreeSelectArguments{*dirId}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"tree.activate",  {}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);
    std::filesystem::remove_all(root);
}

TEST(treeScrollMovesTheViewportWithoutMovingTheSelection) {
    auto root = testRuntimePath("runtime_nav_treescroll_wheel");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    for (int i = 0; i < 40; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "file-%02d.txt", i);
        std::ofstream{root / "workspace" / name} << "x";
    }
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"panel.toggle",  {}}).accepted());
    // Expand the root so the 40 files become a tree taller than a short panel.
    ASSERT_TRUE(runtime.dispatch({"tree.select_next",  {}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"tree.activate",  {}}).accepted());
    const ssg::ViewportDimensions dims{80, 12};
    ssg::test::GridTestView firstGrid{dims};

    auto baseline = firstGrid.present(runtime);
    ASSERT_TRUE(baseline.has_value());
    if (!baseline) return;
    ASSERT_TRUE(baseline->panel.has_value());
    if (!baseline->panel) return;
    auto const& w0 = *baseline->panel;
    ASSERT_EQ(w0.firstVisible, std::uint32_t{0});
    ASSERT_TRUE(w0.scrollbar.maximumFirstRow > 0);

    // Wheel down: the viewport offset advances, but the selection does not move.
    ASSERT_TRUE(firstGrid.dispatch(runtime,
                                   {"tree.scroll",
                                    ssg::ScrollLinesArguments{3}}).accepted());
    auto scrolled = firstGrid.present(runtime);
    ASSERT_TRUE(scrolled.has_value());
    if (!scrolled) return;
    ASSERT_TRUE(scrolled->panel.has_value());
    if (!scrolled->panel) return;
    ASSERT_EQ(scrolled->panel->firstVisible, std::uint32_t{3});

    // Wheel up past the top clamps at 0.
    ASSERT_TRUE(firstGrid.dispatch(runtime,
                                   {"tree.scroll",
                                    ssg::ScrollLinesArguments{-99}}).accepted());
    auto topped = firstGrid.present(runtime);
    ASSERT_TRUE(topped.has_value());
    if (!topped) return;
    ASSERT_TRUE(topped->panel.has_value());
    if (!topped->panel) return;
    ASSERT_EQ(topped->panel->firstVisible, std::uint32_t{0});

    // Wheel down past the bottom clamps at maximum_first_row.
    ASSERT_TRUE(firstGrid.dispatch(runtime,
                                   {"tree.scroll",
                                    ssg::ScrollLinesArguments{999}}).accepted());
    auto bottomed = firstGrid.present(runtime);
    ASSERT_TRUE(bottomed.has_value());
    if (!bottomed) return;
    ASSERT_TRUE(bottomed->panel.has_value());
    if (!bottomed->panel) return;
    auto const& w3 = *bottomed->panel;
    ASSERT_EQ(w3.firstVisible, w3.scrollbar.maximumFirstRow);
    ASSERT_TRUE(w3.scrollbar.totalRows >= w3.rows.size());

    for (int i = 0; i < 60; ++i) {
        ASSERT_TRUE(runtime
                        .dispatch({"tree.select_next",  {}})
                        .accepted());
    }
    auto selectedAtBottom = firstGrid.present(runtime);
    ASSERT_TRUE(selectedAtBottom.has_value());
    if (!selectedAtBottom || !selectedAtBottom->panel) return;
    const auto retainedFirst = selectedAtBottom->panel->firstVisible;
    ASSERT_TRUE(retainedFirst > 0);

    firstGrid.resize({31, 12});
    auto hidden = firstGrid.present(runtime);
    ASSERT_TRUE(hidden.has_value());
    if (!hidden) return;
    ASSERT_FALSE(hidden->panel.has_value());
    ASSERT_TRUE(firstGrid.dispatch(
        runtime, {"tree.scroll",
                  ssg::ScrollLinesArguments{-999}})
                    .accepted());

    firstGrid.resize(dims);
    auto restored = firstGrid.present(runtime);
    ASSERT_TRUE(restored.has_value());
    if (!restored || !restored->panel) return;
    ASSERT_EQ(restored->panel->firstVisible, retainedFirst);
    ASSERT_TRUE(std::ranges::any_of(
        restored->panel->rows,
        [](const ssg::SolvedPanelRow& row) { return row.selected; }));

    // A missing payload is rejected.
    ASSERT_FALSE(runtime.dispatch({"tree.scroll",  {}}).accepted());
    std::filesystem::remove_all(root);
}

} // namespace

namespace {

// M12 VP-H: word-wrap-off horizontal caret reveal. A long line whose caret moves
// past the pane width scrolls horizontally so the caret stays visible; returning
// to the line start resets the offset. A short (fitting) line never scrolls.
TEST(wordWrapOffRevealsCaretHorizontally) {
    auto root = testRuntimePath("runtime_hscroll");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    // A single 60-cell line, far wider than the test pane.
    std::ofstream{root / "workspace" / "long.txt"} << std::string(60, 'a') << "\n";
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",
                                  std::string{"long.txt"}}).accepted());

    ssg::ViewportDimensions const dims{24, 6};
    ssg::test::GridTestView grid{dims};
    // Prime the pane-size cache the reveal path reads.
    auto primed = grid.present(runtime);
    ASSERT_TRUE(primed.has_value());
    if (!primed) return;
    ASSERT_EQ(primed->viewport.firstVisualColumn, std::uint32_t{0});

    // Move the caret to the end of the long line: it is past the pane width, so
    // the viewport scrolls horizontally to keep it visible.
    ASSERT_TRUE(runtime.dispatch({"cursor.line_end",  {}})
                    .accepted());
    auto scrolled = grid.present(runtime);
    ASSERT_TRUE(scrolled.has_value());
    if (!scrolled) return;
    auto const offset = scrolled->viewport.firstVisualColumn;
    ASSERT_TRUE(offset > 0);
    // The caret's cell (60) is within the visible horizontal window.
    ASSERT_TRUE(60u >= offset);
    ASSERT_TRUE(60u < offset + dims.columns);

    // Returning to the line start resets the horizontal offset to zero.
    ASSERT_TRUE(runtime.dispatch({"cursor.line_start",  {}})
                    .accepted());
    auto reset = grid.present(runtime);
    ASSERT_TRUE(reset.has_value());
    if (!reset) return;
    ASSERT_EQ(reset->viewport.firstVisualColumn, std::uint32_t{0});
    std::filesystem::remove_all(root);
}

// VP-3 regression: the word-wrap-ON path stays EXACT — a long line still wraps to
// multiple visual rows through the runtime — while word-wrap-OFF clips it to one
// row and scrolls horizontally.  Locks both directions of the wrap gate so the
// M12 projection can never silently disable wrapping.
TEST(wordWrapOnWrapsLongLinesOffClipsThem) {
    auto root = testRuntimePath("runtime_wrap_gate");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    // One 200-cell line (far wider than the 80-col pane) plus a short line.
    std::ofstream{root / "workspace" / "wide.txt"}
        << std::string(200, 'b') << "\nshort\n";
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",
                                  std::string{"wide.txt"}}).accepted());
    ssg::ViewportDimensions const dims{80, 24};

    // Word wrap OFF (default): three logical lines (the trailing newline yields a
    // final empty line) -> three visual rows total; the 200-cell line is ONE
    // clipped visual row.
    auto off = projectFrame(runtime, dims);
    ASSERT_TRUE(off.has_value());
    if (!off) return;
    ASSERT_EQ(off->viewport.totalVisualRows, std::uint32_t{3});
    std::uint32_t offRowsForLine0 = 0;
    for (auto const& row : off->viewport.visibleRows) {
        if (row.logicalLine == 0) ++offRowsForLine0;
    }
    ASSERT_EQ(offRowsForLine0, std::uint32_t{1});  // clipped, not wrapped

    // Word wrap ON: the 200-cell line wraps into ceil(200/80) = 3 visual rows, so
    // the total exceeds the OFF total and logical line 0 spans >1 row.
    ASSERT_TRUE(runtime.dispatch({"view.toggle_word_wrap",  {}})
                    .accepted());
    auto on = projectFrame(runtime, dims);
    ASSERT_TRUE(on.has_value());
    if (!on) return;
    ASSERT_TRUE(on->viewport.totalVisualRows > 3u);  // wrapped
    std::uint32_t onRowsForLine0 = 0;
    for (auto const& row : on->viewport.visibleRows) {
        if (row.logicalLine == 0) ++onRowsForLine0;
    }
    ASSERT_EQ(onRowsForLine0, std::uint32_t{3});  // 200 cells / 80 -> 3 rows
    // Wrapped lines never scroll horizontally.
    ASSERT_TRUE(runtime.dispatch({"cursor.line_end",  {}})
                    .accepted());
    auto wrappedEnd = projectFrame(runtime, dims);
    ASSERT_TRUE(wrappedEnd.has_value());
    if (!wrappedEnd) return;
    ASSERT_EQ(wrappedEnd->viewport.firstVisualColumn, std::uint32_t{0});
    std::filesystem::remove_all(root);
}

// Lever 2 (wrap-mode shaping cache): word-wrap shaping is O(document) -- it
// segments every line to compute wrap positions. The per-document cell runs are
// cached by (revision, documentId), so a snapshot that changes neither re-shapes
// nothing, and an edit (new revision) forces a full re-shape. Proven by the
// compute_cell_run counter: two identical wrap snapshots segment the same
// (small, non-document-scaled) amount; an edit adds a full-document re-shape.
TEST(wordWrapShapingIsCachedUntilTheDocumentRevisionChanges) {
    auto root = testRuntimePath("runtime_wrapcache");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::string text;
    for (int i = 0; i < 60; ++i) {
        text += "line " + std::to_string(i) + " content\n";
    }
    std::ofstream{root / "workspace" / "doc.txt"} << text;
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.dispatch({"file.open",
                                  std::string{"doc.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch({"view.toggle_word_wrap",  {}})
                    .accepted());
    ssg::ViewportDimensions const dims{80, 24};
    ssg::test::GridTestView grid{dims};
    (void)grid.present(runtime);

    // Two identical wrap snapshots: the second re-shapes nothing from the
    // document -- only the constant chrome/prompt shaping remains.
    ssg::resetCellRunCalls();
    (void)grid.present(runtime);
    auto const base = ssg::cellRunCalls();
    ssg::resetCellRunCalls();
    (void)grid.present(runtime);
    ASSERT_EQ(ssg::cellRunCalls(), base);

    // An edit bumps the document revision, so the whole document is re-shaped:
    // the count jumps well past the cached-snapshot baseline.
    ASSERT_TRUE(runtime.dispatch({"text.insert",
                                  ssg::TextInputArguments{"z"}})
                    .accepted());
    ssg::resetCellRunCalls();
    (void)grid.present(runtime);
    ASSERT_TRUE(ssg::cellRunCalls() > base);
    std::filesystem::remove_all(root);
}


// segments only the visible + moved lines — bounded and INDEPENDENT of document
// length — not the whole document. Proven by the compute_cell_run counter: the
// per-move segmentation count is identical for a 50-line and a 20000-line file.
TEST(wordWrapOffNavigationIsViewportBounded) {
    auto root = testRuntimePath("runtime_navbound");
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
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ssg::ViewportDimensions const dims{80, 24};

    auto navSegmentations = [&](std::string const& file) -> std::uint64_t {
        ssg::test::GridTestView grid{dims};
        (void)runtime.dispatch({"file.open",  file});
        (void)grid.present(runtime);
        ssg::resetCellRunCalls();
        for (int i = 0; i < 4; ++i) {
            (void)grid.dispatch(
                runtime, {"cursor.line_down",  {}});
        }
        return ssg::cellRunCalls();
    };

    auto const smallCalls = navSegmentations("small.txt");
    auto const bigCalls = navSegmentations("big.txt");

    ASSERT_TRUE(smallCalls > 0);
    // Bounded (~ per move: visible rows + the moved line), and NOT proportional to
    // the 400x-larger document.
    ASSERT_TRUE(smallCalls < 200);
    ASSERT_TRUE(bigCalls > 0);
    ASSERT_TRUE(bigCalls < 200);
    std::filesystem::remove_all(root);
}

std::unique_ptr<ssg::Editor> gotoLineRuntime() {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "lines.txt"}
        << "one\ntwo\nthree\nfour\nfive";
    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    if (!created.accepted()) return nullptr;
    auto runtime = std::move(created.session);
    (void)runtime->dispatch({"file.open",  std::string{"lines.txt"}});
    return runtime;
}

std::uint32_t gotoCaretLine(ssg::Editor& runtime) {
    auto snapshot = projectFrame(runtime, ssg::ViewportDimensions{80, 24});
    if (!snapshot) return 0;
    return snapshot->selections.primary().active.line.value();
}

TEST(gotoLineClampsToTheOneBasedLineRange) {
    auto runtime = gotoLineRuntime();
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    // "3" is 1-based, so the caret lands on line index 2.
    ASSERT_TRUE(runtime
                    ->dispatch({"goto.line",  std::string{"3"}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 2U);
    // A number past the end clamps to the last line (index 4).
    ASSERT_TRUE(runtime
                    ->dispatch({"goto.line",  std::string{"999"}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 4U);
    // "1" is the first line.
    ASSERT_TRUE(runtime
                    ->dispatch({"goto.line",  std::string{"1"}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 0U);
    // Below the range clamps to the first line rather than failing: "0" and a
    // negative both go to line 1 (index 0). Move off line 0 between each so a
    // no-op could not masquerade as a successful clamp.
    ASSERT_TRUE(runtime
                    ->dispatch({"goto.line",  std::string{"4"}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 3U);
    ASSERT_TRUE(runtime
                    ->dispatch({"goto.line",  std::string{"0"}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 0U);
    ASSERT_TRUE(runtime
                    ->dispatch({"goto.line",  std::string{"4"}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 3U);
    ASSERT_TRUE(runtime
                    ->dispatch({"goto.line",  std::string{"-7"}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 0U);
}

TEST(gotoLineRejectsNonNumericInput) {
    auto runtime = gotoLineRuntime();
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(runtime
                    ->dispatch({"goto.line",  std::string{"3"}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 2U);
    for (const auto* bad : {"abc", "2x", "1.5", ""}) {
        ASSERT_FALSE(runtime
                         ->dispatch({"goto.line",
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
                    ->dispatch({"goto.line",  {}})
                    .accepted());
    auto snapshot = ssg::test::projectGridFrame(*runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->promptStatus.activeKind,
              std::optional{ssg::PromptKind::CommandArgument});
    // The generic prompt round-trip re-dispatches goto.line with the typed value.
    ASSERT_TRUE(runtime
                    ->dispatch({"prompt.update_value",
                                ssg::PromptValueArguments{0, "4"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    ->dispatch({"prompt.submit",  {}})
                    .accepted());
    ASSERT_EQ(gotoCaretLine(*runtime), 3U);
}

} // namespace

SSG_TEST_SUITE(test_session_navigation) {
    RUN(externalDiffBurstRevealsOnlyNewestFileWithoutPausingFollow);
    RUN(followPauseQueuesMultipleChangesAndResumeAdoptsTheNewest);
    RUN(gitDiffScanUpdatesDiffAndRejectsStaleBatches);
    RUN(gitDiffSelectionUsesDiffIdentityIndependentOfDocumentRevision);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}

SSG_TEST_SUITE(test_session_follow) {
    RUN(gitStatusActivationOpensLiveDiffTabAndReusesIt);
    RUN(documentAndLiveDiffTabsCloseIndependently);
    RUN(gitStatusActivationOpensDeletedLiveDiffWithoutDiskFile);
    RUN(liveDiffOpenClassificationPausesOnlyForUserActivation);
    RUN(tabSwitchPausesFollowViaNavigationPath);
    RUN(followToggleMatchesPauseAndResumeIncludingQueuedTargetResolution);
    RUN(followPauseOnEditTransitionTable);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}

SSG_TEST_SUITE(test_session_pickers) {
    RUN(paletteOpenEntersPromptFocusAndPublishesCandidates);
    RUN(everyPaletteClosePathLeavesNoOpenPickerBehind);
    RUN(paletteExecuteValidatesCandidateMembership);
    RUN(filePickerPublishesWorkspaceFilesAndRejectsPaletteExecute);
    RUN(togglingGitignoreRebuildsTheOpenFilePickerIndex);
    RUN(workerFilesystemRefreshPublishesChangedFileCandidates);
    RUN(filePickerClosesOnSuccessfulOpenAndStaysOpenOnFailure);
    RUN(pickerSubmissionRequiresAndClosesTheAuthoritativePicker);
    RUN(pickerSubmissionRequiresMatchingPickerMode);
    RUN(commandPickerActionThatOpensPromptDismissesPickerWithoutFailure);
    RUN(pickerSubmissionUsesActivationIdentityInsteadOfGlobalRevision);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}

SSG_TEST_SUITE(test_session_interaction) {
    RUN(simpleSemanticInputsLowerThroughAuthoritativeTransactions);
    RUN(resolvedSelectionInputRejectsMalformedModelIdentity);
    RUN(keyInputRoutingBranchesByPromptMode);
    RUN(viewTransitionsExecuteTypedEditorMutations);
    RUN(documentPointerInputOwnsSelectionGesturePolicy);
    RUN(documentPointerGestureEndsWhenSelectionCommandIsRejected);
    RUN(documentEdgeMovesResolveThroughPresenterAndReveal);
    RUN(documentEdgeContinuationPreservesAdditiveBaseline);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}

SSG_TEST_SUITE(test_session_layout) {
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
    RUN(wordWrapOffNavigationIsViewportBounded);
    RUN(gotoLineClampsToTheOneBasedLineRange);
    RUN(gotoLineRejectsNonNumericInput);
    RUN(gotoLineWithoutPayloadOpensACommandArgumentPromptThatJumpsOnSubmit);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
