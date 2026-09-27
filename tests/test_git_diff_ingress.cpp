#include "editor_test_support.h"
#include "grid_test_frame.h"
#include "test_helpers.h"

#include <ssg/GitDiffScanStage.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace ssg;

EditorCreateResult makeEditor(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "workspace");
    EditorConfig config{root / "workspace", root / "recovery", root / "archive"};
    config.enableGitDiffWorker = false;
    config.enableFilesystemWatcher = false;
    return createEditor(std::move(config));
}

GitDiffScan scan(std::uint64_t revision, std::string content) {
    return {.revision = revision,
            .baselineIdentity = "head:index",
            .currentBranch = "main",
            .files = {{.id = DiffFileId{"note"},
                       .path = "note.txt",
                       .baselineContent = std::string{"same\nold\n"},
                       .workingContent = std::move(content)}}};
}

TEST(ingressReturnsWorkerDataWithoutEditor) {
    TestRuntimeDirectory root{testRuntimePath("ingress_worker")};
    GitDiffIngress ingress{root.path(), false, false};
    ASSERT_TRUE(ingress.wake() == nullptr);
    const auto batch = ingress.drain();
    ASSERT_TRUE(batch.scans.empty());
    ASSERT_TRUE(batch.watchEvents.empty());
    ASSERT_FALSE(batch.fullReconcile);
}

TEST(scanStagingRejectsPartialDiffWithoutChangingSourceModels) {
    DiffModel diff;
    ASSERT_TRUE(diff
                    .applyNonGitEvent(
                        {.kind = NonGitDiffEventKind::Create,
                         .id = DiffFileId{"collision"},
                         .path = "local.txt",
                         .baselineContent = "",
                         .targetContent = std::string{"local\n"}},
                        1)
                    .accepted());
    FollowEditsModel follow;
    const auto beforeDiff = diff.viewState();
    const auto beforeFollow = follow.viewState();
    auto invalid = scan(2, "same\nnew\n");
    invalid.files.push_back({.id = DiffFileId{"collision"},
                             .path = "local.txt",
                             .workingContent = std::string{"git\n"}});

    auto staged = stageGitDiffScan(std::move(invalid), diff, follow);
    ASSERT_EQ(staged.result.error, DiffIngressError::DiffRejected);
    ASSERT_EQ(diff.viewState(), beforeDiff);
    ASSERT_EQ(follow.viewState(), beforeFollow);
    ASSERT_TRUE(staged.mutated);
}

TEST(statusOnlyScanRemovesDetailedGitEntryWithoutTouchingNonGit) {
    DiffModel diff{DiffConfig{.maximumLineCount = 4,
                              .maximumMatrixCells = 100,
                              .maximumWordMatrixCells = 100}};
    ASSERT_TRUE(diff.updateGitFile(
                        {.id = DiffFileId{"note"},
                         .path = "note.txt",
                         .baselineContent = std::string{"a\n"},
                         .workingContent = std::string{"b\n"}},
                        "head:index", 1)
                    .accepted());
    ASSERT_TRUE(diff
                    .applyNonGitEvent(
                        {.kind = NonGitDiffEventKind::Create,
                         .id = DiffFileId{"local"},
                         .path = "local.txt",
                         .baselineContent = "",
                         .targetContent = std::string{"local\n"}},
                        2)
                    .accepted());
    FollowEditsModel follow;
    const auto before = diff.viewState();
    auto staged = stageGitDiffScan(
        {.revision = 3,
         .baselineIdentity = "head:index",
         .files = {{.id = DiffFileId{"note"},
                    .path = "note.txt",
                    .baselineContent = std::string{"a\n"},
                    .workingContent = std::string{"b\nc\nd\ne\nf\n"}}}},
        diff, follow);

    ASSERT_TRUE(staged.accepted());
    ASSERT_TRUE(staged.mutated);
    ASSERT_EQ(staged.statusOnlyIds, (std::vector<DiffFileId>{DiffFileId{"note"}}));
    ASSERT_FALSE(staged.diff.file(DiffFileId{"note"}).has_value());
    ASSERT_TRUE(staged.diff.file(DiffFileId{"local"}).has_value());
    ASSERT_EQ(diff.viewState(), before);
}

TEST(batchAdoptionPublishesFollowAndRejectsStaleAndInvalidScans) {
    TestRuntimeDirectory root{testRuntimePath("ingress_scans")};
    auto created = makeEditor(root.path());
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& editor = *created.session;

    GitDiffWorkerDrain batch;
    batch.scans.push_back(scan(5, "same\nnew\n"));
    {
        std::lock_guard lock{editor.operationMutex};
        ASSERT_TRUE(editor.adoptGitDiffWorkerDrainLocked(std::move(batch)));
    }
    auto frame = test::projectGridFrame(editor);
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    ASSERT_EQ(frame->diff.files.size(), std::size_t{1});
    ASSERT_EQ(frame->diffFileIdentity, std::optional<std::string>{"note"});
    ASSERT_EQ(frame->documentText, std::string{"same\nnew\n"});
    ASSERT_EQ(frame->selections.primary().active.byteOffset, ByteOffset{5});
    ASSERT_EQ(frame->followMode, FollowMode::Following);
    ASSERT_EQ(test::liveDiffDocumentCount(editor), std::size_t{1});
    const auto beforeDiff = editor.diff.viewState();
    const auto beforeFollow = editor.follow.viewState();
    const auto beforeTree = editor.tree.viewState();
    const auto beforeTabs = editor.tabs.viewState();

    auto stale = scan(5, "stale\n");
    ASSERT_EQ(test::applyGitDiffScan(editor, std::move(stale)).error,
              DiffIngressError::DiffRejected);
    auto invalid = scan(6, "same\nnewer\n");
    invalid.files.push_back({.id = DiffFileId{"invalid"},
                             .path = "",
                             .workingContent = std::string{"invalid\n"}});
    ASSERT_EQ(test::applyGitDiffScan(editor, std::move(invalid)).error,
              DiffIngressError::DiffRejected);
    ASSERT_EQ(editor.diff.viewState(), beforeDiff);
    ASSERT_EQ(editor.follow.viewState(), beforeFollow);
    ASSERT_EQ(editor.tree.viewState(), beforeTree);
    ASSERT_EQ(editor.tabs.viewState(), beforeTabs);
    ASSERT_EQ(test::activeDocumentText(editor), std::string{"same\nnew\n"});

    GitDiffWorkerDrain branchOnly;
    branchOnly.scans.push_back(
        {.revision = 0, .currentBranch = std::string{"feature"}});
    {
        std::lock_guard lock{editor.operationMutex};
        ASSERT_TRUE(editor.adoptGitDiffWorkerDrainLocked(std::move(branchOnly)));
    }
    const auto fields = editor.uiStatusFields();
    ASSERT_TRUE(std::any_of(fields.header.begin(), fields.header.end(),
                            [](const StatusField& field) {
                                return field.id == kBranchStatusFieldId &&
                                       field.value.find("feature") !=
                                           std::string::npos;
                            }));
    ASSERT_EQ(editor.diff.viewState(), beforeDiff);
    ASSERT_EQ(editor.follow.viewState(), beforeFollow);
    ASSERT_EQ(editor.tree.viewState(), beforeTree);

    ASSERT_TRUE(test::applyGitDiffScan(editor, scan(6, "same\nnewer\n"))
                    .accepted());
    ASSERT_EQ(test::activeDocumentText(editor), std::string{"same\nnewer\n"});
    ASSERT_EQ(test::liveDiffDocumentCount(editor), std::size_t{1});
}

TEST(statusOnlyScanClosesLiveTabAndRetainsTreeStatus) {
    TestRuntimeDirectory root{testRuntimePath("ingress_status_only")};
    auto created = makeEditor(root.path());
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& editor = *created.session;
    editor.diff = DiffModel{DiffConfig{.maximumLineCount = 4,
                                        .maximumMatrixCells = 100,
                                        .maximumWordMatrixCells = 100}};
    ASSERT_TRUE(test::applyGitDiffScan(editor, scan(1, "same\nnew\n"))
                    .accepted());
    ASSERT_EQ(test::liveDiffDocumentCount(editor), std::size_t{1});
    const auto liveDocument = test::contentTabDocument(
        editor, TabKind::LiveDiff, "note");
    auto overBudget = scan(2, "one\ntwo\nthree\nfour\nfive\n");
    ASSERT_TRUE(test::applyGitDiffScan(editor, std::move(overBudget))
                    .accepted());

    ASSERT_FALSE(editor.diff.file(DiffFileId{"note"}).has_value());
    ASSERT_EQ(test::liveDiffDocumentCount(editor), std::size_t{0});
    ASSERT_FALSE(test::hasDocumentAssociation(editor, liveDocument));
    const auto tabs = editor.tabs.viewState();
    ASSERT_TRUE(std::none_of(
        tabs.tabs.begin(), tabs.tabs.end(),
        [](const TabState& tab) { return tab.kind == TabKind::LiveDiff; }));
    bool gitStatusPresent = false;
    for (const auto& provider : editor.tree.viewState().providers) {
        if (provider.kind != TreeProviderKind::Git) continue;
        for (const auto& node : provider.nodes) {
            gitStatusPresent |= node.node.workspacePath == "note.txt";
        }
    }
    ASSERT_TRUE(gitStatusPresent);
}

TEST(watcherOnlyBatchAdoptsExternalModifications) {
    TestRuntimeDirectory root{testRuntimePath("ingress_watch")};
    std::filesystem::create_directories(root.path() / "workspace");
    std::ofstream{root.path() / "workspace" / "note.txt"} << "original\n";
    auto created = makeEditor(root.path());
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& editor = *created.session;
    ASSERT_TRUE(test::openFile(editor, "note.txt").accepted());
    ASSERT_TRUE(test::typeText(editor, "!").accepted());
    const auto buffer = test::activeDocumentText(editor);
    std::ofstream{root.path() / "workspace" / "note.txt"} << "external\n";

    GitDiffWorkerDrain batch;
    batch.watchEvents.push_back(
        {.kind = WatchEventKind::Modify, .path = "note.txt", .sequence = 1});
    {
        std::lock_guard lock{editor.operationMutex};
        ASSERT_TRUE(editor.adoptGitDiffWorkerDrainLocked(std::move(batch)));
        ASSERT_FALSE(editor.adoptGitDiffWorkerDrainLocked({}));
    }
    auto frame = test::projectGridFrame(editor);
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    ASSERT_EQ(frame->externalModification.files.size(), std::size_t{1});
    ASSERT_EQ(frame->externalModification.files.front().id,
              DiffFileId{"external:note.txt"});
    ASSERT_EQ(frame->documentText, buffer);
}

TEST(fullReconcileBatchReloadsCleanDocument) {
    TestRuntimeDirectory root{testRuntimePath("ingress_reconcile")};
    std::filesystem::create_directories(root.path() / "workspace");
    std::ofstream{root.path() / "workspace" / "note.txt"} << "original\n";
    auto created = makeEditor(root.path());
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& editor = *created.session;
    ASSERT_TRUE(test::openFile(editor, "note.txt").accepted());
    std::ofstream{root.path() / "workspace" / "note.txt"} << "external\n";

    GitDiffWorkerDrain batch;
    batch.fullReconcile = true;
    {
        std::lock_guard lock{editor.operationMutex};
        ASSERT_TRUE(editor.adoptGitDiffWorkerDrainLocked(std::move(batch)));
    }
    auto frame = test::projectGridFrame(editor);
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    ASSERT_EQ(frame->documentText, std::string{"external\n"});
    ASSERT_TRUE(frame->externalModification.files.empty());
    ASSERT_TRUE(frame->syntax != nullptr);
    if (frame->syntax) {
        ASSERT_EQ(frame->syntax->revision(), frame->documentRevision);
    }
}

} // namespace

SSG_TEST_SUITE(test_git_diff_ingress) {
    RUN(ingressReturnsWorkerDataWithoutEditor);
    RUN(scanStagingRejectsPartialDiffWithoutChangingSourceModels);
    RUN(statusOnlyScanRemovesDetailedGitEntryWithoutTouchingNonGit);
    RUN(batchAdoptionPublishesFollowAndRejectsStaleAndInvalidScans);
    RUN(statusOnlyScanClosesLiveTabAndRetainsTreeStatus);
    RUN(watcherOnlyBatchAdoptsExternalModifications);
    RUN(fullReconcileBatchReloadsCleanDocument);
    return failed == 0 ? 0 : 1;
}
