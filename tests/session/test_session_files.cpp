#include "../test_helpers.h"
#include "../grid_test_view.h"
#include "../grid_test_frame.h"

#include <ssg/EditorSession.h>
#include <ssg/FileCommands.h>
#include <ssg/GitDiffSource.h>
#include <ssg/HitTester.h>
#include <ssg/Keymap.h>
#include <ssg/Settings.h>
#include <ssg/session_snapshot.h>
#include <ssg/TextCodec.h>
#include <ssg/TextInputCommands.h>

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::filesystem::path uniqueRoot(std::string_view name) {
    auto root = std::filesystem::current_path() / ("runtime_files_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    return root;
}

ssg::EditorSessionConfig configFor(const std::filesystem::path& root) {
    ssg::EditorSessionConfig config{
        root / "workspace", root / "scratch", root / "recovery"};
    config.enableGitDiffWorker = false;
    config.enableFilesystemWatcher = false;
    return config;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void writeBytes(const std::filesystem::path& path, std::initializer_list<std::uint8_t> bytes) {
    std::ofstream output{path, std::ios::binary};
    for (auto byte : bytes) output.put(static_cast<char>(byte));
}

bool activeTabDirty(ssg::EditorSession& runtime) {
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
    if (!snapshot) return false;
    const auto& tabs = snapshot->sections().tabs;
    if (!tabs.active) return false;
    for (const auto& tab : tabs.tabs) {
        if (tab.id == *tabs.active) return tab.dirty;
    }
    return false;
}

bool activeTabIsLiveDiff(ssg::EditorSession& runtime) {
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
    if (!snapshot) return false;
    const auto& tabs = snapshot->sections().tabs;
    if (!tabs.active) return false;
    for (const auto& tab : tabs.tabs) {
        if (tab.id == *tabs.active) return tab.kind == ssg::TabKind::LiveDiff;
    }
    return false;
}

std::vector<std::filesystem::path> archivedDrafts(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> result;
    const auto dir = root / "draft-archive";
    std::error_code code;
    for (std::filesystem::directory_iterator it{dir, code}, end;
         !code && it != end; it.increment(code)) {
        if (it->is_regular_file()) result.push_back(it->path());
    }
    return result;
}

bool hasNoticeBar(const ssg::GridPresentation& snapshot) {
    return snapshot.layout().find(
               ssg::UiNodeId{std::string{ssg::kNoticeNodeId}}) != nullptr;
}

bool statusMentions(ssg::EditorSession& runtime, std::string_view needle) {
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
    if (!snapshot) return false;
    for (const auto& item : snapshot->sections().promptStatus.status.items) {
        if (item.accessibleLabel.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Edit note.txt to a dirty draft, flush it, then drop the runtime — leaving a
// recoverable draft in `root/scratch`. Returns the drafted buffer text.
std::string leaveDirtyDraft(const std::filesystem::path& root) {
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << "hi\n";
    auto created = ssg::EditorSession::create(configFor(root));
    if (!created.accepted()) return {};
    auto& runtime = *created.session;
    (void)runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                         ssg::ViewId{1});
    (void)runtime.dispatch(ssg::ClientId{1},
                           {"file.open", runtime.revision(), std::string{"note.txt"}});
    (void)runtime.dispatch(ssg::ClientId{1},
                           {"text.insert", runtime.revision(),
                            ssg::TextInputArguments{"!"}});
    const auto draft = runtime.activeDocumentText();
    (void)runtime.flushDueAutosaveDrafts();
    return draft;
}

ssg::CommandResult reopenNote(ssg::EditorSession& runtime) {
    (void)runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                         ssg::ViewId{1});
    return runtime.dispatch(
        ssg::ClientId{1},
        {"file.open", runtime.revision(), std::string{"note.txt"}});
}

TEST(reopeningADirtyDraftRestoresTheEditsWhenDiskIsUnchanged) {
    auto root = uniqueRoot("draft_restore");
    const auto draft = leaveDirtyDraft(root);
    ASSERT_TRUE(!draft.empty());

    // Disk untouched since the edits branched: recover the draft as a dirty
    // buffer with a "restored" notice, no conflict.
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    ASSERT_EQ(runtime.activeDocumentText(), draft);
    ASSERT_TRUE(activeTabDirty(runtime));
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorSession::DraftReopenNotice::Restored);
}

TEST(reopeningAConvergedDraftDropsItAndOpensClean) {
    auto root = uniqueRoot("draft_converged");
    const auto draft = leaveDirtyDraft(root);
    ASSERT_TRUE(!draft.empty());
    // The file on disk now already holds exactly the drafted content: the edits
    // converged. There is nothing unsaved to recover.
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << draft;

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    ASSERT_EQ(runtime.activeDocumentText(), draft);
    ASSERT_FALSE(activeTabDirty(runtime));
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorSession::DraftReopenNotice::None);
    // The draft was dropped, so the clean buffer has nothing to flush.
    ASSERT_EQ(runtime.flushAllAutosaveDrafts(), std::size_t{0});
}

TEST(reopeningADraftAfterAnExternalChangeFlagsConflict) {
    auto root = uniqueRoot("draft_conflict");
    const auto draft = leaveDirtyDraft(root);
    ASSERT_TRUE(!draft.empty());
    // Something other than SSG rewrote the file since the edits branched.
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary}
        << "changed externally\n";

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    // The draft still loads as a dirty buffer (never a blind blocking choice)...
    ASSERT_EQ(runtime.activeDocumentText(), draft);
    ASSERT_TRUE(activeTabDirty(runtime));
    // ...but the conflict notice fires because disk changed.
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorSession::DraftReopenNotice::Conflict);
}

TEST(editingAndSavingARestoredDraftRoundTripsCoherently) {
    // Guards the decoded/document coherence seam: a restored draft's `decoded`
    // must match the buffer so a later edit's terminator bookkeeping and a save
    // do not desynchronize. The disk file uses CRLF so the terminator convention
    // is non-trivial.
    auto root = uniqueRoot("draft_edit_after_restore");
    {
        std::ofstream{root / "workspace" / "note.txt", std::ios::binary}
            << "one\r\ntwo\r\n";
        auto created = ssg::EditorSession::create(configFor(root));
        ASSERT_TRUE(created.accepted());
        auto& runtime = *created.session;
        ASSERT_TRUE(reopenNote(runtime).accepted());
        ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                     {"text.insert", runtime.revision(),
                                      ssg::TextInputArguments{"X"}})
                        .accepted());
        ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{1});
    }
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorSession::DraftReopenNotice::Restored);
    const auto restored = runtime.activeDocumentText();
    // A further edit after restore must apply cleanly and stay dirty...
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"text.insert", runtime.revision(),
                                  ssg::TextInputArguments{"Y"}})
                    .accepted());
    ASSERT_EQ(runtime.activeDocumentText(), "Y" + restored);
    ASSERT_TRUE(activeTabDirty(runtime));
    // ...and a save must write CRLF back to disk, proving decoded's terminator
    // convention survived the restore.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.save", runtime.revision(), {}})
                    .accepted());
    ASSERT_FALSE(activeTabDirty(runtime));
    ASSERT_NE(readText(root / "workspace" / "note.txt").find("\r\n"),
              std::string::npos);
}

TEST(reactivatingAnOpenTabDoesNotReapplyItsDraft) {
    // The freshlyOpened gate: with a draft still present in scratch, switching
    // back to an already-open tab must not re-run reconcile and clobber the live
    // buffer with the stale draft.
    auto root = uniqueRoot("draft_no_reclobber");
    const auto draft = leaveDirtyDraft(root);
    ASSERT_TRUE(!draft.empty());
    std::ofstream{root / "workspace" / "other.txt", std::ios::binary} << "other\n";

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    ASSERT_EQ(runtime.activeDocumentText(), draft);
    // Edit past the recovered draft, open another file, then re-open note.txt:
    // openFile short-circuits to the already-open document, so reconcile must not
    // run again and must leave the live edited buffer intact.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"text.insert", runtime.revision(),
                                  ssg::TextInputArguments{"Z"}})
                    .accepted());
    const auto live = runtime.activeDocumentText();
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"other.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"note.txt"}})
                    .accepted());
    ASSERT_EQ(runtime.activeDocumentText(), live);
    ASSERT_TRUE(activeTabDirty(runtime));
}

TEST(draftDiffOnAConflictShowsDraftAgainstDiskHunks) {
    auto root = uniqueRoot("draft_diff_conflict");
    const auto draft = leaveDirtyDraft(root);  // "!hi\n"
    ASSERT_TRUE(!draft.empty());
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary}
        << "changed externally\n";

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorSession::DraftReopenNotice::Conflict);

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"draft.diff", runtime.revision(), {}})
                    .accepted());
    // The diff is its own derived tab, distinct from the still-dirty draft.
    ASSERT_TRUE(activeTabIsLiveDiff(runtime));
    // The merged diff view carries the target (draft) content; the disk-only
    // baseline line is projected as a phantom removed row, so the draft's own
    // line is what the diff document text holds.
    const auto diffText = runtime.activeDocumentText();
    ASSERT_NE(diffText.find("!hi"), std::string::npos);
}

TEST(draftDiffWithDiskMissingDiffsDraftAgainstEmpty) {
    auto root = uniqueRoot("draft_diff_missing");
    const auto draft = leaveDirtyDraft(root);
    ASSERT_TRUE(!draft.empty());
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary}
        << "changed externally\n";

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    // The file vanishes after the draft is open: draft.diff must still succeed,
    // diffing the draft against empty rather than failing.
    std::filesystem::remove(root / "workspace" / "note.txt");
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"draft.diff", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(activeTabIsLiveDiff(runtime));
    ASSERT_NE(runtime.activeDocumentText().find("!hi"), std::string::npos);
}

TEST(draftDiffSurvivesAGitScanThatDoesNotMentionTheFile) {
    // A git rescan reconciles only its own entries; the non-git draft-vs-disk
    // entry must not be evicted just because the scan does not list it.
    auto root = uniqueRoot("draft_diff_git_scan");
    const auto draft = leaveDirtyDraft(root);
    ASSERT_TRUE(!draft.empty());
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary}
        << "changed externally\n";

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"draft.diff", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(activeTabIsLiveDiff(runtime));
    // Pause follow-edits so the scan does not auto-navigate to the git-changed
    // file; this test is about the draft entry surviving, not follow behavior.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"follow_edits.pause", runtime.revision(), {}})
                    .accepted());

    // A git scan that finds unrelated changes (not note.txt) arrives.
    ssg::GitDiffScan scan;
    scan.revision = ssg::Revision{1000};
    scan.baselineIdentity = "index-a";
    scan.currentBranch = "main";
    scan.files.push_back({ssg::DiffFileId{"other.cpp"}, "other.cpp",
                          std::nullopt, std::string{"x\n"},
                          std::string{"y\n"}});
    ASSERT_TRUE(runtime.applyGitDiffScan(std::move(scan)).accepted());

    // The draft-vs-disk diff tab is still active and still shows the draft.
    ASSERT_TRUE(activeTabIsLiveDiff(runtime));
    ASSERT_NE(runtime.activeDocumentText().find("!hi"), std::string::npos);
}

TEST(draftDiscardArchivesTheDraftAndLoadsDiskContent) {
    auto root = uniqueRoot("draft_discard");
    const auto draft = leaveDirtyDraft(root);  // "!hi\n"
    ASSERT_TRUE(!draft.empty());
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary}
        << "changed externally\n";

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorSession::DraftReopenNotice::Conflict);

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"draft.discard", runtime.revision(), {}})
                    .accepted());

    // (a)+(b): the buffer now holds disk content, is clean, notice cleared.
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"changed externally\n"});
    ASSERT_FALSE(activeTabDirty(runtime));
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorSession::DraftReopenNotice::None);

    // (a)+(c): the discarded edits were archived (reversible), byte-for-byte.
    const auto archived = archivedDrafts(root);
    ASSERT_EQ(archived.size(), std::size_t{1});
    ASSERT_EQ(readText(archived.front()), draft);

    // (d): the user's file on disk was not written by the discard.
    ASSERT_EQ(readText(root / "workspace" / "note.txt"),
              std::string{"changed externally\n"});
}

TEST(discardedDraftIsRemovedFromScratchSoReopenIsClean) {
    auto root = uniqueRoot("draft_discard_removed");
    const auto draft = leaveDirtyDraft(root);
    ASSERT_TRUE(!draft.empty());
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary}
        << "changed externally\n";
    {
        auto created = ssg::EditorSession::create(configFor(root));
        ASSERT_TRUE(created.accepted());
        auto& runtime = *created.session;
        ASSERT_TRUE(reopenNote(runtime).accepted());
        ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                     {"draft.discard", runtime.revision(), {}})
                        .accepted());
    }
    // A fresh session over the same store must find no draft to recover.
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"changed externally\n"});
    ASSERT_FALSE(activeTabDirty(runtime));
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorSession::DraftReopenNotice::None);
}

TEST(draftDiscardRefusesACleanSavedDocument) {
    auto root = uniqueRoot("draft_discard_clean");
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << "hi\n";
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    (void)runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                         ssg::ViewId{1});
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"note.txt"}})
                    .accepted());
    // Nothing unsaved: discard must refuse rather than archive an empty change.
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1},
                                  {"draft.discard", runtime.revision(), {}})
                     .accepted());
    ASSERT_TRUE(archivedDrafts(root).empty());
}

TEST(draftDiscardArchivesADeeplyNestedPathWithoutExceedingNameLimits) {
    // Regression guard: the archive filename must not be the flattened full
    // relative path, which for a legal deep path can exceed a filesystem's
    // 255-byte per-component limit and make discard fail. The path below flattens
    // to well over 255 bytes.
    auto root = uniqueRoot("draft_discard_deep");
    std::filesystem::path rel;
    for (int i = 0; i < 6; ++i) {
        rel /= std::string(50, 'd') + std::to_string(i);
    }
    rel /= "note.txt";
    std::filesystem::create_directories((root / "workspace" / rel).parent_path());
    std::ofstream{root / "workspace" / rel, std::ios::binary} << "hi\n";

    const auto relKey = rel.generic_string();
    {
        auto created = ssg::EditorSession::create(configFor(root));
        ASSERT_TRUE(created.accepted());
        auto& runtime = *created.session;
        (void)runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                             ssg::ViewId{1});
        ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                     {"file.open", runtime.revision(), relKey})
                        .accepted());
        ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                     {"text.insert", runtime.revision(),
                                      ssg::TextInputArguments{"!"}})
                        .accepted());
        ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{1});
    }
    std::ofstream{root / "workspace" / rel, std::ios::binary} << "changed\n";

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    (void)runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                         ssg::ViewId{1});
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(), relKey})
                    .accepted());
    // Discard must succeed and produce exactly one bounded-name archive entry.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"draft.discard", runtime.revision(), {}})
                    .accepted());
    const auto archived = archivedDrafts(root);
    ASSERT_EQ(archived.size(), std::size_t{1});
    ASSERT_TRUE(archived.front().filename().string().size() <= std::size_t{255});
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"changed\n"});
}

TEST(conflictNoticeIsPresentOnlyForAConflictReopen) {
    const ssg::ViewportDimensions dims{80, 24};
    {
        auto root = uniqueRoot("notice_conflict");
        leaveDirtyDraft(root);
        std::ofstream{root / "workspace" / "note.txt", std::ios::binary}
            << "changed externally\n";
        auto created = ssg::EditorSession::create(configFor(root));
        ASSERT_TRUE(created.accepted());
        auto& runtime = *created.session;
        ASSERT_TRUE(reopenNote(runtime).accepted());
        ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                    ssg::EditorSession::DraftReopenNotice::Conflict);
        ASSERT_TRUE(hasNoticeBar(*ssg::test::projectGridFrame(
            runtime, ssg::ClientId{1}, ssg::ViewId{1}, dims)));
    }
    {
        // Restored (disk unchanged): a quieter state, no yellow notice.
        auto root = uniqueRoot("notice_restored");
        leaveDirtyDraft(root);  // disk still "hi\n"
        auto created = ssg::EditorSession::create(configFor(root));
        ASSERT_TRUE(created.accepted());
        auto& runtime = *created.session;
        ASSERT_TRUE(reopenNote(runtime).accepted());
        ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                    ssg::EditorSession::DraftReopenNotice::Restored);
        ASSERT_FALSE(hasNoticeBar(*ssg::test::projectGridFrame(
            runtime, ssg::ClientId{1}, ssg::ViewId{1}, dims)));
    }
}

// The semantic NoticeView projection is present exactly on a draft-conflict reopen
// (nullopt otherwise), so a native/web client raises the notice without the grid.
TEST(noticeViewIsPresentOnlyOnADraftConflict) {
    const ssg::ViewportDimensions dims{80, 24};
    {
        auto root = uniqueRoot("semantic_notice_conflict");
        leaveDirtyDraft(root);
        std::ofstream{root / "workspace" / "note.txt", std::ios::binary}
            << "changed externally\n";
        auto created = ssg::EditorSession::create(configFor(root));
        ASSERT_TRUE(created.accepted());
        auto& runtime = *created.session;
        ASSERT_TRUE(reopenNote(runtime).accepted());
        auto snapshot = runtime.snapshot(ssg::ClientId{1});
        ASSERT_TRUE(snapshot.has_value());
        const auto& notice = snapshot->sections().noticeView;
        ASSERT_TRUE(notice.has_value());
        ASSERT_FALSE(notice->text.empty());
        ASSERT_EQ(notice->actions.size(), std::size_t{3});
    }
    {
        auto root = uniqueRoot("semantic_notice_restored");
        leaveDirtyDraft(root);  // disk unchanged -> Restored, no notice
        auto created = ssg::EditorSession::create(configFor(root));
        ASSERT_TRUE(created.accepted());
        auto& runtime = *created.session;
        ASSERT_TRUE(reopenNote(runtime).accepted());
        auto snapshot = runtime.snapshot(ssg::ClientId{1});
        ASSERT_TRUE(snapshot.has_value());
        ASSERT_FALSE(snapshot->sections().noticeView.has_value());
    }
}

// The solved grid notice and semantic NoticeView come from the same resolver:
// whenever one raises the notice the other does too, with the same action ids.
TEST(theGridNoticeAndSemanticNoticeComeFromTheOneResolver) {
    const ssg::ViewportDimensions dims{80, 24};
    auto root = uniqueRoot("one_resolver_conflict");
    leaveDirtyDraft(root);
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary}
        << "changed externally\n";
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    auto frame = ssg::test::projectGridFrame(
        runtime, ssg::ClientId{1}, ssg::ViewId{1}, dims);
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;

    // Both projections agree that a notice is raised.
    ASSERT_TRUE(hasNoticeBar(*frame));
    const auto& notice = frame->sections().noticeView;
    ASSERT_TRUE(notice.has_value());

    // Both projections carry the same semantic action identities. Commands remain
    // library-owned and are resolved only after input returns to the session.
    std::vector<std::string> gridActions;
    const auto* noticeNode = frame->layout().find(
        ssg::UiNodeId{std::string{ssg::kNoticeNodeId}});
    ASSERT_TRUE(noticeNode != nullptr);
    if (!noticeNode) return;
    const auto solved =
        ssg::solveNoticeSurface(*notice, noticeNode->rect);
    for (const auto& action : solved.actions) {
        const auto hit =
            ssg::HitTester{*frame}.at(action.rect.x, action.rect.y);
        ASSERT_TRUE(hit.fieldId.has_value());
        ASSERT_FALSE(hit.commandId.has_value());
        if (hit.fieldId) gridActions.push_back(*hit.fieldId);
    }
    std::vector<std::string> semanticActions;
    for (const auto& action : notice->actions)
        semanticActions.push_back(action.id);
    std::sort(gridActions.begin(), gridActions.end());
    std::sort(semanticActions.begin(), semanticActions.end());
    ASSERT_EQ(gridActions.size(), semanticActions.size());
    ASSERT_TRUE(gridActions == semanticActions);

    // Dismiss clears BOTH projections together.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"draft.dismiss", runtime.revision(), {}})
                    .accepted());
    auto cleared = ssg::test::projectGridFrame(
        runtime, ssg::ClientId{1}, ssg::ViewId{1}, dims);
    ASSERT_TRUE(cleared.has_value());
    ASSERT_FALSE(hasNoticeBar(*cleared));
    ASSERT_FALSE(cleared->sections().noticeView.has_value());
}

TEST(conflictNoticeReservesChromeWithoutPerturbingTheDocument) {
    // Non-perturbation: the notice is a reserved chrome row, not stolen document
    // row 0. Two sessions with the IDENTICAL buffer ("!hi\n") -- one Conflict
    // (notice up), one Restored (no notice) -- must share the document's own
    // coordinate space: the pane content only loses one row from the TOP for the
    // reserved notice; its width is unchanged and the same viewport-origin cell
    // maps to the same document byte.
    const ssg::ViewportDimensions dims{80, 24};

    auto conflictRoot = uniqueRoot("notice_perturb_conflict");
    leaveDirtyDraft(conflictRoot);
    std::ofstream{conflictRoot / "workspace" / "note.txt", std::ios::binary}
        << "changed externally\n";
    auto conflictCreated = ssg::EditorSession::create(configFor(conflictRoot));
    ASSERT_TRUE(conflictCreated.accepted());
    auto& conflict = *conflictCreated.session;
    ASSERT_TRUE(reopenNote(conflict).accepted());
    auto conflictFrame = ssg::test::projectGridFrame(
        conflict, ssg::ClientId{1}, ssg::ViewId{1}, dims);
    ASSERT_TRUE(conflictFrame.has_value());
    if (!conflictFrame) return;
    ASSERT_TRUE(hasNoticeBar(*conflictFrame));

    auto restoredRoot = uniqueRoot("notice_perturb_restored");
    leaveDirtyDraft(restoredRoot);  // disk unchanged -> Restored, no notice
    auto restoredCreated = ssg::EditorSession::create(configFor(restoredRoot));
    ASSERT_TRUE(restoredCreated.accepted());
    auto& restored = *restoredCreated.session;
    ASSERT_TRUE(reopenNote(restored).accepted());
    auto restoredFrame = ssg::test::projectGridFrame(
        restored, ssg::ClientId{1}, ssg::ViewId{1}, dims);
    ASSERT_TRUE(restoredFrame.has_value());
    if (!restoredFrame) return;
    ASSERT_FALSE(hasNoticeBar(*restoredFrame));

    ASSERT_EQ(conflict.activeDocumentText(), restored.activeDocumentText());
    ASSERT_TRUE(conflictFrame->document().has_value());
    ASSERT_TRUE(restoredFrame->document().has_value());
    if (!conflictFrame->document() || !restoredFrame->document()) return;
    const auto& withNotice = conflictFrame->document()->content;
    const auto& without = restoredFrame->document()->content;
    // Reserved from the top: same left edge and width, top pushed down one, one
    // fewer content row -- the document is not shifted, it just shows one less
    // row (exactly like the prompt reservation costs a row from the bottom).
    ASSERT_EQ(withNotice.x, without.x);
    ASSERT_EQ(withNotice.width, without.width);
    ASSERT_EQ(withNotice.y, without.y + 1);
    ASSERT_EQ(withNotice.height, without.height - 1);
    // The same viewport-origin cell resolves to the same document byte offset in
    // both: the document's internal coordinate space is untouched.
    const auto withHit =
        ssg::HitTester{*conflictFrame}.at(withNotice.x, withNotice.y);
    const auto withoutHit =
        ssg::HitTester{*restoredFrame}.at(without.x, without.y);
    ASSERT_EQ(withHit.byteOffset, withoutHit.byteOffset);
}

TEST(clickingNoticeActionsDispatchesTheirCommands) {
    const ssg::ViewportDimensions dims{80, 24};
    auto root = uniqueRoot("notice_click");
    leaveDirtyDraft(root);
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary}
        << "changed externally\n";
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    auto frame = ssg::test::projectGridFrame(
        runtime, ssg::ClientId{1}, ssg::ViewId{1}, dims);
    ASSERT_TRUE(frame.has_value());
    if (!frame) return;
    const auto* noticeNode = frame->layout().find(
        ssg::UiNodeId{std::string{ssg::kNoticeNodeId}});
    ASSERT_TRUE(noticeNode != nullptr);
    if (!noticeNode || !frame->sections().noticeView) return;
    const auto solved = ssg::solveNoticeSurface(
        *frame->sections().noticeView, noticeNode->rect);

    // Each solved action hit-tests to its semantic identity, not its command.
    for (const auto& id :
         {"draft.notice.diff", "draft.notice.use_disk",
          "draft.notice.dismiss"}) {
        const auto action = std::find_if(
           solved.actions.begin(), solved.actions.end(),
           [&](const ssg::SolvedNoticeAction& candidate) {
               return candidate.id == id;
           });
        ASSERT_TRUE(action != solved.actions.end());
        if (action == solved.actions.end()) continue;
        const auto hit =
           ssg::HitTester{*frame}.at(action->rect.x, action->rect.y);
        ASSERT_EQ(hit.fieldId, std::optional<std::string>{id});
        ASSERT_FALSE(hit.commandId.has_value());
    }

    const auto revisionBeforeInvalid = runtime.revision();
    auto invalid = runtime.input(
        ssg::ClientId{1},
        ssg::NoticeActionPointerInput{{revisionBeforeInvalid},
                                      "draft.notice.missing"});
    ASSERT_EQ(invalid.outcome, ssg::ClientInputOutcome::Rejected);
    ASSERT_EQ(runtime.revision(), revisionBeforeInvalid);

    // The semantic action identity is resolved against the current notice.
    auto dismiss = runtime.input(
        ssg::ClientId{1},
        ssg::NoticeActionPointerInput{{runtime.revision()},
                                      "draft.notice.dismiss"});
    ASSERT_TRUE(dismiss.command.has_value() && dismiss.command->accepted());
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorSession::DraftReopenNotice::None);
    ASSERT_FALSE(hasNoticeBar(*ssg::test::projectGridFrame(
        runtime, ssg::ClientId{1}, ssg::ViewId{1}, dims)));
}

TEST(dismissRefusesWhenThereIsNoConflictNotice) {
    const ssg::ViewportDimensions dims{80, 24};
    // A Restored reopen shows no notice, so dismiss must refuse rather than
    // silently mutate the (non-notice) restored state.
    auto root = uniqueRoot("notice_dismiss_restored");
    leaveDirtyDraft(root);  // disk unchanged -> Restored
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorSession::DraftReopenNotice::Restored);
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1},
                                  {"draft.dismiss", runtime.revision(), {}})
                     .accepted());
    // The restored state is untouched.
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorSession::DraftReopenNotice::Restored);
}

TEST(liveDiffVirtualDocumentIsNotAutosavedAsADraft) {
    // A live-diff tab's virtual document (DocumentMode::Diff) is untitled and
    // non-empty, so without a mode guard it would be flushed as a spurious
    // untitled scratch draft. It must never be an autosave candidate.
    auto root = uniqueRoot("diff_candidate_clean");
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << "hi\n";
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"note.txt"}})
                    .accepted());
    // A clean file with a live-diff tab open: nothing dirty to draft. The diff
    // virtual doc must NOT be flushed.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"draft.diff", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(activeTabIsLiveDiff(runtime));
    ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{0});
    ASSERT_EQ(runtime.flushAllAutosaveDrafts(), std::size_t{0});
}

TEST(liveDiffTabDoesNotInflateTheDirtyDocumentFlushCount) {
    // With a genuinely dirty saved document AND a live-diff tab open, exactly one
    // draft flushes (the real document); the diff virtual doc is excluded.
    auto root = uniqueRoot("diff_candidate_dirty");
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << "hi\n";
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"note.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"text.insert", runtime.revision(),
                                  ssg::TextInputArguments{"!"}})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"draft.diff", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(activeTabIsLiveDiff(runtime));
    // Exactly the real document, not the diff virtual doc too.
    ASSERT_EQ(runtime.flushAllAutosaveDrafts(), std::size_t{1});
}

TEST(binaryDiskReplacementRaisesConflictNotSilentDraftLoss) {
    // Hardening (M15 p7): a file that had a text draft is externally replaced by
    // binary/non-UTF-8 content. restoreDraft cannot load the draft into the now
    // read-only binary buffer, but the draft must NOT be silently dropped: the
    // conflict notice is raised so the user is warned and the draft stays in
    // scratch.
    auto root = uniqueRoot("draft_binary_disk");
    leaveDirtyDraft(root);  // draft "!hi\n", disk "hi\n"
    writeBytes(root / "workspace" / "note.txt", {0x00, 0x01, 0x02, 0x00, 0xff});

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    // Not silently None: the conflict is surfaced (old behaviour left it None).
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorSession::DraftReopenNotice::Conflict);
    ASSERT_TRUE(hasNoticeBar(*ssg::test::projectGridFrame(
        runtime, ssg::ClientId{1}, ssg::ViewId{1}, {80, 24})));
}

TEST(oversizedBufferIsNotAutosavedAndIsReportedOnce) {
    // Hardening (M15 p7): a buffer larger than the per-draft cap gets NO draft
    // (writing a giant draft every tick would blow the scratch quota), reported
    // rather than silently written or partially drafted.
    auto root = uniqueRoot("draft_oversize");
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << "hi\n";
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    runtime.setAutosaveDraftByteCapForTests(4);  // "hi\n" + edits exceed it
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"note.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"text.insert", runtime.revision(),
                                  ssg::TextInputArguments{"ab"}})
                    .accepted());
    // Over cap: not persisted (flush count 0) and reported once.
    ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{0});
    ASSERT_TRUE(statusMentions(runtime, "too large to autosave"));
    // A fresh session over the same store finds no draft (no false partial).
    auto reCreated = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(reCreated.accepted());
    auto& reopened = *reCreated.session;
    ASSERT_TRUE(reopenNote(reopened).accepted());
    ASSERT_TRUE(reopened.activeDraftReopenNotice() ==
                ssg::EditorSession::DraftReopenNotice::None);
    ASSERT_FALSE(activeTabDirty(reopened));
}

TEST(loweringAutosaveDebounceMsEnablesAFlushTheDefaultSuppresses) {
    // Hardening (M15 p7): the flush-interval setting is read and applied each
    // tick. With the default 10s interval a second edit is debounced (no flush);
    // lowering AutosaveDebounceMs lets that same pending edit flush.
    auto root = uniqueRoot("draft_debounce_setting");
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << "hi\n";
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"note.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"text.insert", runtime.revision(),
                                  ssg::TextInputArguments{"a"}})
                    .accepted());
    ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{1});  // eager first flush

    // A further edit within the default 10s interval is debounced.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"text.insert", runtime.revision(),
                                  ssg::TextInputArguments{"b"}})
                    .accepted());
    ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{0});

    // Lower the interval to its minimum; after a short wait the pending edit
    // flushes -- proving the setting is read and applied.
    auto lower = ssg::SettingSetArguments{ssg::SettingScope::User,
                                          ssg::SettingKey::AutosaveDebounceMs,
                                          ssg::SettingValue{std::uint32_t{1}}};
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"settings.set", runtime.revision(), lower})
                    .accepted());
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{1});
}

TEST(touchingTheFileWithIdenticalBytesIsNotAFalseConflict) {
    // Acceptance (M15): a touched-but-identical file (new mtime, same content)
    // must NOT be a conflict -- content is the authority, not the clock.
    auto root = uniqueRoot("draft_touch");
    const auto draft = leaveDirtyDraft(root);  // disk "hi\n"
    ASSERT_TRUE(!draft.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << "hi\n";

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    ASSERT_TRUE(reopenNote(runtime).accepted());
    // Unchanged, not Conflict: the draft is restored quietly, no yellow notice.
    ASSERT_EQ(runtime.activeDocumentText(), draft);
    ASSERT_TRUE(activeTabDirty(runtime));
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorSession::DraftReopenNotice::Restored);
    ASSERT_FALSE(hasNoticeBar(*ssg::test::projectGridFrame(
        runtime, ssg::ClientId{1}, ssg::ViewId{1}, {80, 24})));
}

TEST(draftDiffRefusesWhenTheActiveDocumentIsNotASavedFile) {
    // An untitled scratch buffer has no disk side to diff against.
    auto root = uniqueRoot("draft_diff_untitled");
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << "hi\n";
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    (void)runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                         ssg::ViewId{1});
    // The session starts on an empty untitled buffer; draft.diff must refuse it.
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1},
                                  {"draft.diff", runtime.revision(), {}})
                     .accepted());
}

TEST(openingAFileRevealsTheCaretResettingAStaleScroll) {
    // Reveal-policy audit: opening a document must show the
    // caret, not inherit the previous document's scroll offset. Two tall files.
    auto root = uniqueRoot("open_reveal");
    std::string tall;
    for (int i = 0; i < 100; ++i) tall += "line\n";
    std::ofstream{root / "workspace" / "a.txt", std::ios::binary} << tall;
    std::ofstream{root / "workspace" / "b.txt", std::ios::binary} << tall;
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    ssg::test::GridTestView grid{ssg::ClientId{1}, ssg::ViewId{1}, dims};
    auto firstRow = [&] {
        auto snap = grid.present(runtime);
        return snap ? snap->presentation().viewport.firstVisualRow : 0U;
    };

    // Open A and scroll far down (free scroll leaves the caret off-screen above).
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"a.txt"}}).accepted());
    ASSERT_TRUE(grid.dispatch(runtime, {"view.scroll_lines", runtime.revision(), ssg::ScrollLinesArguments{50}}).accepted());
    ASSERT_EQ(firstRow(), 50U);

    // Opening B resets the view so B's caret (its document start) is visible: the
    // stale offset of 50 must not carry over.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"b.txt"}}).accepted());
    ASSERT_EQ(firstRow(), 0U);
}

TEST(openEditSaveRoundTripsRealDiskBytes) {
    auto root = uniqueRoot("round_trip");
    {
        std::ofstream output{root / "workspace" / "note.txt", std::ios::binary};
        output << "hello";
    }

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    auto open = runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"note.txt"}});
    ASSERT_TRUE(open.accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"hello"});

    auto insert = runtime.dispatch(ssg::ClientId{1}, {"text.insert", runtime.revision(), ssg::TextInputArguments{"!"}});
    ASSERT_TRUE(insert.accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"!hello"});

    auto save = runtime.dispatch(ssg::ClientId{1}, {"file.save", runtime.revision(), {}});
    ASSERT_TRUE(save.accepted());
    ASSERT_EQ(readText(root / "workspace" / "note.txt"), std::string{"!hello"});
}

TEST(droppedContentRequiresRealCapability) {
    auto root = uniqueRoot("drop");
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    auto denied = runtime.dispatch(ssg::ClientId{1}, {"file.open_dropped_content", runtime.revision(), ssg::DroppedContentArguments{{'a'}, "a.txt"}});
    ASSERT_FALSE(denied.accepted());

    ASSERT_TRUE(runtime.attach({ssg::ClientId{2}, ssg::InvocationOrigin::InProcess, {ssg::CapabilityId{"local_file_drop"}}}, ssg::ViewId{1}).accepted());
    auto accepted = runtime.dispatch(ssg::ClientId{2}, {"file.open_dropped_content", runtime.revision(), ssg::DroppedContentArguments{{'a'}, "a.txt"}});
    ASSERT_TRUE(accepted.accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"a"});
}

TEST(encodingDispatchMatchesEncodeOracleAndSavedBytes) {
    auto root = uniqueRoot("encoding_save");
    {
        std::ofstream output{root / "workspace" / "note.txt", std::ios::binary};
        output << "one\ntwo";
    }

    auto original = readBytes(root / "workspace" / "note.txt");
    auto decoded = ssg::TextCodec{}.decode(original);
    ASSERT_TRUE(decoded.accepted());
    decoded.text->utf8 = "one\ntwo\n";
    decoded.text->lineTerminators = {ssg::LineTerminator::Crlf, ssg::LineTerminator::Crlf};
    decoded.text->status.encoding = ssg::TextEncoding::Utf8Bom;
    decoded.text->status.hadBom = true;
    decoded.text->status.lineEnding = ssg::LineEnding::Crlf;
    decoded.text->status.finalNewline = true;
    auto expected = ssg::TextCodec{}.encode(*decoded.text);
    ASSERT_TRUE(expected.accepted());

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"note.txt"}}).accepted());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.set_encoding", runtime.revision(), ssg::SetEncodingArguments{ssg::TextEncoding::Utf8Bom}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.set_line_ending", runtime.revision(), ssg::SetLineEndingArguments{ssg::LineEnding::Crlf}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.set_final_newline", runtime.revision(), ssg::SetFinalNewlineArguments{true}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().textEncoding.status, decoded.text->status);

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.save", runtime.revision(), {}}).accepted());
    ASSERT_EQ(readBytes(root / "workspace" / "note.txt"), expected.bytes);
}

TEST(reopenWithEncodingDispatchRedecodesRealFileBytes) {
    auto root = uniqueRoot("reopen_encoding");
    writeBytes(root / "workspace" / "latin.txt", {0xe9, 0x0d});

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"latin.txt"}}).accepted());

    auto reopened = runtime.dispatch(ssg::ClientId{1}, {"file.reopen_with_encoding", runtime.revision(), ssg::ReopenWithEncodingArguments{ssg::TextEncoding::Iso88591}});
    ASSERT_TRUE(reopened.accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"\xC3\xA9\n"});
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().textEncoding.status.encoding, ssg::TextEncoding::Iso88591);
    ASSERT_EQ(snapshot->sections().textEncoding.status.lineEnding, ssg::LineEnding::Cr);
}

TEST(closingTheLastTabClearsTheEditorDocument) {
    auto root = uniqueRoot("close_last_tab");
    std::ofstream{root / "workspace" / "a.txt", std::ios::binary} << "alpha";
    std::ofstream{root / "workspace" / "b.txt", std::ios::binary} << "beta";

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());

    auto tabCount = [&] {
        return runtime.snapshot(ssg::ClientId{1})->sections().tabs.tabs.size();
    };

    // Open two files: two tabs, the active document shows content.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"a.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"b.txt"}}).accepted());
    ASSERT_EQ(tabCount(), std::size_t{2});
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"beta"});

    // Closing one tab switches to the remaining tab's document (still shown).
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tab.close", runtime.revision(), {}}).accepted());
    ASSERT_EQ(tabCount(), std::size_t{1});
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"alpha"});

    // Closing the last tab must clear the editor document (empty state), not
    // leave a phantom document with no tab.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tab.close", runtime.revision(), {}}).accepted());
    ASSERT_EQ(tabCount(), std::size_t{0});
    ASSERT_TRUE(runtime.activeDocumentText().empty());
    auto snapshot = runtime.snapshot(ssg::ClientId{1});
    ASSERT_TRUE(snapshot.has_value());
    if (snapshot) ASSERT_TRUE(snapshot->sections().document.text.empty());
}

TEST(tabActivateFocusesTheEditor) {
    auto root = uniqueRoot("tab_activate_focus");
    std::ofstream{root / "workspace" / "a.txt", std::ios::binary} << "alpha";
    std::ofstream{root / "workspace" / "b.txt", std::ios::binary} << "beta";

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"a.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"b.txt"}}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    auto focus = [&] {
        auto snap = runtime.snapshot(ssg::ClientId{1});
        return snap ? snap->sections().uiFrame.effectiveFocus()
                    : ssg::FocusTarget::Editor;
    };

    // Move focus to the panel, then activating a tab (a tab click) returns focus
    // to the editor.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.focus", runtime.revision(), {}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Panel);

    auto first = runtime.snapshot(ssg::ClientId{1})->sections().tabs.tabs.front().id;
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tab.activate", runtime.revision(), first}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::Editor);
}

TEST(switchingTabsRevealsTheNewDocumentsCaret) {
    // Reveal-policy: switching to a different tab shows that document's caret
    // instead of inheriting the previous tab's scroll offset.
    auto root = uniqueRoot("tab_switch_reveal");
    std::string tall;
    for (int i = 0; i < 100; ++i) tall += "line\n";
    std::ofstream{root / "workspace" / "a.txt", std::ios::binary} << tall;
    std::ofstream{root / "workspace" / "b.txt", std::ios::binary} << tall;
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    ssg::test::GridTestView grid{ssg::ClientId{1}, ssg::ViewId{1}, dims};
    auto firstRow = [&] {
        auto snap = grid.present(runtime);
        return snap ? snap->presentation().viewport.firstVisualRow : 0U;
    };
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"a.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"b.txt"}}).accepted());
    // B is active; scroll it far down (free scroll leaves B's caret off-screen).
    ASSERT_TRUE(grid.dispatch(runtime, {"view.scroll_lines", runtime.revision(), ssg::ScrollLinesArguments{50}}).accepted());
    ASSERT_EQ(firstRow(), 50U);

    // Switch to A (previous tab): its caret (top) is revealed, not B's stale 50.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tab.previous", runtime.revision(), {}}).accepted());
    ASSERT_EQ(firstRow(), 0U);

    // Moving a tab keeps the SAME active document and must NOT snap the scroll:
    // switch back to B, scroll away, move the tab, and the offset stays put.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tab.next", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(grid.dispatch(runtime, {"view.scroll_lines", runtime.revision(), ssg::ScrollLinesArguments{50}}).accepted());
    ASSERT_EQ(firstRow(), 50U);
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tab.move_left", runtime.revision(), {}}).accepted());
    ASSERT_EQ(firstRow(), 50U);  // same document -> no reveal snap
}

TEST(closingNonActiveDirtyTabReopensItsOwnContentWithNewDocumentId) {
    auto root = uniqueRoot("close_non_active_dirty");
    std::ofstream{root / "workspace" / "a.txt", std::ios::binary} << "alpha";
    std::ofstream{root / "workspace" / "b.txt", std::ios::binary} << "beta";

    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());

    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"a.txt"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"text.insert", runtime.revision(),
                               ssg::TextInputArguments{"!"}})
                    .accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"!alpha"});
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"b.txt"}})
                    .accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"beta"});

    auto beforeClose = runtime.snapshot(ssg::ClientId{1});
    ASSERT_TRUE(beforeClose.has_value());
    if (!beforeClose.has_value()) return;
    std::optional<ssg::TabId> tabA;
    std::optional<ssg::FileDocumentId> documentA;
    for (auto const& tab : beforeClose->sections().tabs.tabs) {
        if (tab.label == "a.txt") {
            tabA = tab.id;
            documentA = tab.document;
            break;
        }
    }
    ASSERT_TRUE(tabA.has_value());
    ASSERT_TRUE(documentA.has_value());
    if (!tabA || !documentA) return;

    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tab.close", runtime.revision(), *tabA})
                    .accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"beta"});
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tab.reopen_closed", runtime.revision(), {}})
                    .accepted());
    ASSERT_EQ(runtime.activeDocumentText(), std::string{"!alpha"});

    auto afterReopen = runtime.snapshot(ssg::ClientId{1});
    ASSERT_TRUE(afterReopen.has_value());
    if (!afterReopen.has_value()) return;
    std::optional<ssg::FileDocumentId> reopenedDocumentA;
    for (auto const& tab : afterReopen->sections().tabs.tabs) {
        if (tab.label == "a.txt") {
            reopenedDocumentA = tab.document;
            break;
        }
    }
    ASSERT_TRUE(reopenedDocumentA.has_value());
    ASSERT_TRUE(*reopenedDocumentA != *documentA);
}

TEST(autosaveFlushesADirtyDocumentEagerlyThenDebounces) {
    auto root = uniqueRoot("autosave_eager");
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << "hi\n";
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"note.txt"}}).accepted());

    // A clean, just-opened document is not flushed.
    ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{0});

    // Editing makes it dirty; the first tick flushes eagerly (bounds the crash
    // window to one tick).
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"text.insert", runtime.revision(), ssg::TextInputArguments{"!"}}).accepted());
    ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{1});

    // An immediate second tick with unchanged content is debounced (default
    // interval is 10s, and nothing changed anyway).
    ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{0});
}

TEST(autosaveFlushesNothingWhenNoDocumentIsDirty) {
    auto root = uniqueRoot("autosave_clean");
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << "hi\n";
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"note.txt"}}).accepted());
    // Edit then save -> clean again -> no autosave.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"text.insert", runtime.revision(), ssg::TextInputArguments{"!"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.save", runtime.revision(), {}}).accepted());
    ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{0});
}

TEST(autosaveFlushAllForcesADirtyDocumentAfterAnEagerFlush) {
    auto root = uniqueRoot("autosave_exit");
    std::ofstream{root / "workspace" / "note.txt", std::ios::binary} << "hi\n";
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"note.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"text.insert", runtime.revision(), ssg::TextInputArguments{"!"}}).accepted());
    ASSERT_EQ(runtime.flushDueAutosaveDrafts(), std::size_t{1});
    // A clean-exit flush ignores the debounce and writes the dirty draft again,
    // capturing any edits newer than the last tick.
    ASSERT_EQ(runtime.flushAllAutosaveDrafts(), std::size_t{1});
    // A clean document is still skipped by the exit flush.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.save", runtime.revision(), {}}).accepted());
    ASSERT_EQ(runtime.flushAllAutosaveDrafts(), std::size_t{0});
}

} // namespace

int main() {
    RUN(openEditSaveRoundTripsRealDiskBytes);
    RUN(openingAFileRevealsTheCaretResettingAStaleScroll);
    RUN(droppedContentRequiresRealCapability);
    RUN(encodingDispatchMatchesEncodeOracleAndSavedBytes);
    RUN(reopenWithEncodingDispatchRedecodesRealFileBytes);
    RUN(closingTheLastTabClearsTheEditorDocument);
    RUN(tabActivateFocusesTheEditor);
    RUN(switchingTabsRevealsTheNewDocumentsCaret);
    RUN(closingNonActiveDirtyTabReopensItsOwnContentWithNewDocumentId);
    RUN(autosaveFlushesADirtyDocumentEagerlyThenDebounces);
    RUN(autosaveFlushesNothingWhenNoDocumentIsDirty);
    RUN(autosaveFlushAllForcesADirtyDocumentAfterAnEagerFlush);
    RUN(reopeningADirtyDraftRestoresTheEditsWhenDiskIsUnchanged);
    RUN(reopeningAConvergedDraftDropsItAndOpensClean);
    RUN(reopeningADraftAfterAnExternalChangeFlagsConflict);
    RUN(editingAndSavingARestoredDraftRoundTripsCoherently);
    RUN(reactivatingAnOpenTabDoesNotReapplyItsDraft);
    RUN(draftDiffOnAConflictShowsDraftAgainstDiskHunks);
    RUN(draftDiffWithDiskMissingDiffsDraftAgainstEmpty);
    RUN(draftDiffSurvivesAGitScanThatDoesNotMentionTheFile);
    RUN(draftDiffRefusesWhenTheActiveDocumentIsNotASavedFile);
    RUN(draftDiscardArchivesTheDraftAndLoadsDiskContent);
    RUN(discardedDraftIsRemovedFromScratchSoReopenIsClean);
    RUN(draftDiscardRefusesACleanSavedDocument);
    RUN(draftDiscardArchivesADeeplyNestedPathWithoutExceedingNameLimits);
    RUN(conflictNoticeIsPresentOnlyForAConflictReopen);
    RUN(noticeViewIsPresentOnlyOnADraftConflict);
    RUN(theGridNoticeAndSemanticNoticeComeFromTheOneResolver);
    RUN(conflictNoticeReservesChromeWithoutPerturbingTheDocument);
    RUN(clickingNoticeActionsDispatchesTheirCommands);
    RUN(dismissRefusesWhenThereIsNoConflictNotice);
    RUN(binaryDiskReplacementRaisesConflictNotSilentDraftLoss);
    RUN(oversizedBufferIsNotAutosavedAndIsReportedOnce);
    RUN(loweringAutosaveDebounceMsEnablesAFlushTheDefaultSuppresses);
    RUN(touchingTheFileWithIdenticalBytesIsNotAFalseConflict);
    RUN(liveDiffVirtualDocumentIsNotAutosavedAsADraft);
    RUN(liveDiffTabDoesNotInflateTheDirtyDocumentFlushCount);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
