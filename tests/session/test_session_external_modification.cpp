// External-modification backend oracles (7A Phase 5a). Seam kind: runtime seam.
#include "../grid_test_frame.h"
// These drive the REAL runtime wiring the flow's unit tests cannot reach: the
// watcher-event reconcile, the shared-DiffModel revision allocation, the command
// handlers that resolve a published external id back to an open document, save
// correlation, adopt-rename, and the durable watcher-availability view-model field.
//
// The workspace is deliberately non-git and the git-diff worker is disabled, so a
// test drives ingress deterministically through the runtime-thread test hook rather
// than depending on inotify timing.
#include "../test_helpers.h"
#include "../editor_test_support.h"

#include <ssg/FileCommands.h>
#include <ssg/FilesystemWatcher.h>
#include <ssg/TextCodec.h>
#include <ssg/TextInputCommands.h>
#include <ssg/CompiledKeymap.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>
#include <algorithm>
#include <array>

namespace {

std::filesystem::path uniqueRoot(std::string_view name) {
    auto root = testRuntimePath("runtime_extmod_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "recovery");
    std::filesystem::create_directories(root / "archive");
    return root;
}

ssg::EditorConfig configFor(const std::filesystem::path& root) {
    ssg::EditorConfig config{root / "workspace", root / "recovery",
                             root / "archive"};
    // Non-git workspace, git worker AND the filesystem watcher off: ingress is
    // driven through the test hook, so the reconcile runs deterministically without
    // depending on inotify timing, and watcherAvailable starts false. The watcher
    // and git worker are independent toggles (Decision 1).
    config.enableGitDiffWorker = false;
    config.enableFilesystemWatcher = false;
    return config;
}

void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::ofstream out{path, std::ios::binary};
    out << content;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in},
            std::istreambuf_iterator<char>{}};
}

ssg::WatchEvent watchEvent(ssg::WatchEventKind kind, std::string path,
                           std::uint64_t sequence,
                           std::optional<std::string> previousPath = {}) {
    ssg::WatchEvent event;
    event.kind = kind;
    event.path = std::move(path);
    if (previousPath) event.previousPath = std::filesystem::path{*previousPath};
    event.sequence = sequence;
    event.origin = ssg::WatchEventOrigin::External;
    return event;
}

// A running runtime with note.txt open. `dirty` inserts a character so the buffer
// diverges from disk (the flow raises actions only for a dirty buffer).
struct Session {
    std::filesystem::path root;
    ssg::EditorCreateResult created;
    ssg::Editor* runtime = nullptr;

    static Session open(std::string_view name, std::string_view diskContent,
                        bool dirty) {
        Session session;
        session.root = uniqueRoot(name);
        writeFile(session.root / "workspace" / "note.txt", diskContent);
        session.created = ssg::createEditor(configFor(session.root));
        session.runtime = session.created.session.get();
        (void)ssg::applyFilePathCompletion(*session.runtime,
                                          ssg::PromptCompletion::FileOpen,
                                          "note.txt");
        if (dirty) {
            (void)ssg::test::typeText(*session.runtime, "!");
        }
        return session;
    }

    std::filesystem::path workspacePath(std::string_view rel) const {
        return root / "workspace" / std::filesystem::path{std::string{rel}};
    }
};

std::vector<ssg::ExternalDocumentView> externalFiles(
    ssg::Editor& runtime) {
    return runtime.external.viewState().files;
}

bool activeTabIsLiveDiff(ssg::Editor& runtime) {
    auto snapshot = ssg::test::projectGridFrame(runtime);
    if (!snapshot) return false;
    const auto& tabs = snapshot->tabs;
    if (!tabs.active) return false;
    for (const auto& tab : tabs.tabs) {
        if (tab.id == *tabs.active) return tab.kind == ssg::TabKind::LiveDiff;
    }
    return false;
}

ssg::CommandResult externalAction(
    ssg::Editor& runtime, ssg::ExternalActionInvocation invocation) {
    auto result = runtime.input(ssg::ExternalActionPointerInput{invocation});
    if (result.command) return std::move(*result.command);
    return {ssg::CommandError::HandlerFailed,
            "external action did not produce a result", {}};
}

TEST(anOpenDocumentChangedOnDiskPopulatesTheExternalSection) {
    auto session = Session::open("changed_populates", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});

    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyModified);
    ASSERT_EQ(files[0].id, ssg::DiffFileId{"external:note.txt"});
}

TEST(anOpenDocumentRemovedOnDiskPublishesRemovedStatusAndItsActions) {
    auto session = Session::open("removed_status", "hi\n", true);
    std::filesystem::remove(session.workspacePath("note.txt"));
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Remove, "note.txt", 1)});

    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
    ASSERT_EQ(files[0].actions.size(), 2U);
    ASSERT_EQ(files[0].statusLabel, std::string{"D"});
    ASSERT_EQ(files[0].actions[0],
              ssg::externalActionAffordance(
                  ssg::ExternalAction::KeepBuffer));
    ASSERT_EQ(files[0].actions[1],
              ssg::externalActionAffordance(ssg::ExternalAction::OpenDiff));
}

TEST(aChangeToANonOpenFileRaisesNoExternalSection) {
    auto session = Session::open("non_open", "hi\n", true);
    writeFile(session.workspacePath("other.txt"), "other\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "other.txt", 1)});

    ASSERT_TRUE(externalFiles(*session.runtime).empty());
}

TEST(aCleanOpenDocumentChangedOnDiskAutoReloadsWithoutRaisingActions) {
    auto session = Session::open("clean_autoreload", "hi\n", false);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});

    ASSERT_TRUE(externalFiles(*session.runtime).empty());
    ASSERT_EQ(ssg::test::activeDocumentText(*session.runtime), "external\n");
}

TEST(externalReloadCommitsDiskIntoTheWorkspaceAndClearsTheSection) {
    auto session = Session::open("reload_commits", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);

    ASSERT_TRUE(externalAction(
                    *session.runtime,
                    {files[0].id, ssg::ExternalAction::Reload})
                    .accepted());

    ASSERT_TRUE(externalFiles(*session.runtime).empty());
    ASSERT_EQ(ssg::test::activeDocumentText(*session.runtime), "external\n");
    ASSERT_EQ(session.runtime->activeSyntaxView()->revision(),
              session.runtime->activeDocument()->revision());
}

TEST(externalKeepBufferClearsTheSectionWithoutTouchingTheBuffer) {
    auto session = Session::open("keep_buffer", "hi\n", true);
    const auto buffer = ssg::test::activeDocumentText(*session.runtime);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);

    ASSERT_TRUE(externalAction(
                    *session.runtime,
                    {files[0].id, ssg::ExternalAction::KeepBuffer})
                    .accepted());

    ASSERT_TRUE(externalFiles(*session.runtime).empty());
    ASSERT_EQ(ssg::test::activeDocumentText(*session.runtime), buffer);
}

TEST(externalOpenDiffOpensALiveDiffTabForThatFile) {
    auto session = Session::open("open_diff", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);

    ASSERT_TRUE(externalAction(
                    *session.runtime,
                    {files[0].id, ssg::ExternalAction::OpenDiff})
                    .accepted());

    ASSERT_TRUE(activeTabIsLiveDiff(*session.runtime));
}

TEST(externalActionOnAnUnknownIdIsARejectedNoOp) {
    auto session = Session::open("unknown_id", "hi\n", true);

    const auto result = externalAction(
        *session.runtime,
        {ssg::DiffFileId{"external:missing.txt"},
         ssg::ExternalAction::KeepBuffer});

    ASSERT_FALSE(result.accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
}

TEST(exmdStaleIdDoesNotActOnThePreviousSelection) {
    auto session = Session::open("stale_select", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    const auto present = files[0].id;

    // external.select on an id absent from the section is REJECTED, so the host's
    // accepted()-gate skips the follow-up action. selectFile's own bool cannot be
    // the success signal: it is false for an absent id AND for an already-selected
    // id. The selection stays on the previously selected present file, and because
    // select failed no action runs on it.
    const auto stale = ssg::test::dispatchInput(
        *session.runtime,
        ssg::ExternalActionPointerInput{
            {ssg::DiffFileId{"external:not-a-real-file.txt"},
             ssg::ExternalAction::Reload}});
    ASSERT_FALSE(stale.accepted());
    const auto external = session.runtime->external.viewState();
    ASSERT_TRUE(external.selected.has_value());
    ASSERT_EQ(*external.selected, present);
}

TEST(exmdOnAnAlreadySelectedPresentIdStillActsOnIt) {
    auto session = Session::open("reselect", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    const auto present = files[0].id;

    // Selecting the CURRENT selection is a valid no-op: the id names a present
    // file, so external.select SUCCEEDS and the host lets the action run on it.
    // (selectFile returns false here because the selection did not move, which is
    // why presence -- not selectFile's bool -- decides command success.)
    const auto reselect = ssg::test::dispatchInput(
        *session.runtime,
        ssg::ExternalActionPointerInput{{present, ssg::ExternalAction::Reload}});
    ASSERT_TRUE(reselect.accepted());
}

TEST(anSsgSaveIsCorrelatedAndRaisesNoExternalNotice) {
    auto session = Session::open("own_save", "hi\n", true);
    // A prior external change raised a pending conflict for the file.
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    ASSERT_EQ(externalFiles(*session.runtime).size(), 1U);
    // SSG writes the file itself; the save primitive records the expectation.
    ASSERT_TRUE(session.runtime
                    ->dispatch("file.save")
                    .accepted());
    // The watcher reports the write with no state supplied; the reconcile stats the
    // (unchanged-since-save) file, matches the expectation, consumes it, AND clears
    // the pending conflict -- a successful self-save resolves the external state.
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 2)});

    ASSERT_TRUE(externalFiles(*session.runtime).empty());
}

TEST(aGenuineExternalEditAfterASelfSaveIsNotSuppressed) {
    auto session = Session::open("edit_after_save", "hi\n", true);
    // A self-save consumes its expectation (and clears any pending conflict).
    ASSERT_TRUE(session.runtime
                    ->dispatch("file.save")
                    .accepted());
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
    ASSERT_FALSE(session.runtime->diff
                     .file(ssg::DiffFileId{"external:note.txt"})
                     .has_value());

    // A genuine external edit follows. Because the expectation was consumed rather
    // than left to accumulate, it is NOT suppressed and raises actions.
    ASSERT_TRUE(ssg::test::typeText(*session.runtime, "x").accepted());
    writeFile(session.workspacePath("note.txt"), "genuinely-external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 2)});

    ASSERT_EQ(externalFiles(*session.runtime).size(), 1U);
}

TEST(aFailedDirtyRenameAdoptionDoesNotPublishAnUnresolvableNewPathEntry) {
    // note.txt is dirty AND other.txt is open, so adopting note.txt's rename onto
    // other.txt's path must fail (the destination is already open).
    auto session = Session::open("rename_adopt_fail", "hi\n", true);
    writeFile(session.workspacePath("other.txt"), "other\n");
    ASSERT_TRUE(ssg::applyFilePathCompletion(*session.runtime,
                    ssg::PromptCompletion::FileOpen, "other.txt").accepted);

    std::filesystem::rename(session.workspacePath("note.txt"),
                            session.workspacePath("other.txt"));
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Rename, "other.txt", 1, "note.txt")});

    // The adoption failed, so the flow published nothing under the new-path id: a
    // conflict keyed to a path no document owns would be an unresolvable action.
    const auto files = externalFiles(*session.runtime);
    for (const auto& file : files) {
        ASSERT_FALSE(file.id == ssg::DiffFileId{"external:other.txt"});
    }
    ASSERT_TRUE(files.empty());
}

TEST(aRenameRetiresThePendingEntryKeyedByThePreviousPath) {
    auto session = Session::open("rename_pending", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    ASSERT_EQ(externalFiles(*session.runtime).size(), 1U);

    std::filesystem::rename(session.workspacePath("note.txt"),
                            session.workspacePath("renamed.txt"));
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Rename, "renamed.txt", 2, "note.txt")});

    // The old-path entry was retired: exactly one pending entry, keyed by the new
    // path -- not a stale pair.
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].id, ssg::DiffFileId{"external:renamed.txt"});
}

TEST(aRenamedOpenDocumentFollowsItsFileWithoutASpuriousRemove) {
    auto session = Session::open("rename_follows", "hi\n", false);
    std::filesystem::rename(session.workspacePath("note.txt"),
                            session.workspacePath("renamed.txt"));
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Rename, "renamed.txt", 1, "note.txt")});

    // No spurious removed/created pair, and the open document followed its file.
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
    const auto state =
        session.runtime->workspace.state(*session.runtime->activeDocumentId());
    ASSERT_TRUE(state.has_value());
    ASSERT_EQ(state->key.savedPath(), "renamed.txt");
    ASSERT_EQ(ssg::test::activeDocumentText(*session.runtime), "hi\n");
}

TEST(theExternalIdNeverCollidesWithAGitPathId) {
    auto session = Session::open("no_collision", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});

    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    // The published id is namespaced, so it can never equal a git entry keyed by
    // the bare path.
    ASSERT_EQ(files[0].id, ssg::DiffFileId{"external:note.txt"});
    ASSERT_FALSE(files[0].id == ssg::DiffFileId{"note.txt"});
}

TEST(aSecondExternalChangeWhileActionsArePendingUpdatesNotDuplicates) {
    auto session = Session::open("second_change", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external-1\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    ASSERT_EQ(externalFiles(*session.runtime).size(), 1U);

    writeFile(session.workspacePath("note.txt"), "external-2\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 2)});

    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyModified);
}

TEST(anOverflowResyncsOpenDocumentsSoAChangeDuringTheOverflowRaisesItsConflict) {
    // The buffer is dirty, so a disk change must raise a conflict rather than
    // auto-reload. The change happens while the watcher is overflowed: its Modify
    // event is lost, and only the Overflow arrives.
    auto session = Session::open("overflow_resync", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.reconcileAllOpenDocumentsAgainstDisk();

    // The overflow re-scanned open documents against disk, found note.txt changed,
    // and raised its conflict -- the modification was not lost with the events.
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].id, ssg::DiffFileId{"external:note.txt"});
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyModified);
}

TEST(aFailedRenameLeavesNoOrphanDiffEntry) {
    // note.txt is dirty AND other.txt is open, so adopting note.txt's rename onto
    // other.txt's path is rejected. The reconcile seeds the new-path diff entry
    // before driving the flow (openDiff needs a file); a rejected event must roll
    // that seed back rather than orphan a diff entry no pending action owns.
    auto session = Session::open("rename_orphan", "hi\n", true);
    writeFile(session.workspacePath("other.txt"), "other\n");
    ASSERT_TRUE(ssg::applyFilePathCompletion(*session.runtime,
                    ssg::PromptCompletion::FileOpen, "other.txt").accepted);

    std::filesystem::rename(session.workspacePath("note.txt"),
                            session.workspacePath("other.txt"));
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Rename, "other.txt", 1, "note.txt")});

    ASSERT_FALSE(session.runtime->diff
                     .file(ssg::DiffFileId{"external:other.txt"})
                     .has_value());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
}

TEST(anOverflowDoesNotResurrectAConflictDismissedByKeepBuffer) {
    auto session = Session::open("keep_overflow", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    const auto raised = externalFiles(*session.runtime);
    ASSERT_EQ(raised.size(), 1U);

    ASSERT_TRUE(externalAction(
                    *session.runtime,
                    {raised[0].id, ssg::ExternalAction::KeepBuffer})
                    .accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    // An overflow resync re-derives events from disk. Disk still differs from the
    // buffer's baseline, but the user already dismissed this state, so it must NOT
    // re-raise the conflict.
    session.runtime->external.reconcileAllOpenDocumentsAgainstDisk();
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    // A genuinely NEW disk change after the acknowledgement is a fresh question and
    // still raises the conflict.
    writeFile(session.workspacePath("note.txt"), "external again\n");
    session.runtime->external.reconcileAllOpenDocumentsAgainstDisk();
    const auto reraised = externalFiles(*session.runtime);
    ASSERT_EQ(reraised.size(), 1U);
    ASSERT_EQ(reraised[0].id, ssg::DiffFileId{"external:note.txt"});
    ASSERT_EQ(reraised[0].status, ssg::ExternalDocumentStatus::ExternallyModified);
}

TEST(keepBufferAdvancesTheExternalBaselineToTheDismissedDiskState) {
    auto session = Session::open("keep_advances", "hi\n", true);
    const auto buffer = ssg::test::activeDocumentText(*session.runtime);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);

    ASSERT_TRUE(externalAction(
                    *session.runtime,
                    {files[0].id, ssg::ExternalAction::KeepBuffer})
                    .accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    // The baseline now equals the dismissed disk state, so a duplicate ordinary
    // event carrying that SAME state is a no-op, and the buffer is preserved.
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 2)});
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
    ASSERT_EQ(ssg::test::activeDocumentText(*session.runtime), buffer);
}

TEST(keepBufferOnARemovedFileSetsTheExternalBaselineMissing) {
    auto session = Session::open("keep_removed", "hi\n", true);
    const auto buffer = ssg::test::activeDocumentText(*session.runtime);
    std::filesystem::remove(session.workspacePath("note.txt"));
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Remove, "note.txt", 1)});
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);

    ASSERT_TRUE(externalAction(
                    *session.runtime,
                    {files[0].id, ssg::ExternalAction::KeepBuffer})
                    .accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    // The baseline is Missing (not a bogus empty-bytes baseline), so an overflow
    // resync with the file still absent does not re-raise the dismissed removal.
    session.runtime->external.reconcileAllOpenDocumentsAgainstDisk();
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
    ASSERT_EQ(ssg::test::activeDocumentText(*session.runtime), buffer);
}

TEST(anEventObservingANonRegularOrUnreadablePathAlwaysRaises) {
    // A file replaced by a directory: the reconcile can neither read regular bytes
    // nor see a clean absence. Such an Unknown observation must never match a
    // stored baseline and always raises, on BOTH the ordinary and overflow paths.
    {
        auto session = Session::open("unknown_dir_ordinary", "hi\n", true);
        std::filesystem::remove(session.workspacePath("note.txt"));
        std::filesystem::create_directory(session.workspacePath("note.txt"));
        session.runtime->external.ingest(
            {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
        const auto files = externalFiles(*session.runtime);
        ASSERT_EQ(files.size(), 1U);
        ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
    }
    {
        auto session = Session::open("unknown_dir_overflow", "hi\n", true);
        std::filesystem::remove(session.workspacePath("note.txt"));
        std::filesystem::create_directory(session.workspacePath("note.txt"));
        // The overflow path formerly collapsed this to a bare Remove, which could
        // match a Missing baseline and be suppressed; it must route through the one
        // Unknown-detection chokepoint and raise like the ordinary path.
        session.runtime->external.reconcileAllOpenDocumentsAgainstDisk();
        const auto files = externalFiles(*session.runtime);
        ASSERT_EQ(files.size(), 1U);
        ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
    }
}

TEST(aBrokenSymlinkObservationRaisesOnTheOverflowPath) {
    auto session = Session::open("unknown_symlink_overflow", "hi\n", true);
    std::filesystem::remove(session.workspacePath("note.txt"));
    std::error_code linkCode;
    std::filesystem::create_symlink(
        "does-not-exist", session.workspacePath("note.txt"), linkCode);
    if (linkCode) {
        SKIP("symlink creation is unavailable: " + linkCode.message());
    }
    session.runtime->external.reconcileAllOpenDocumentsAgainstDisk();
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
}

TEST(anUnknownObservationRaisesEvenAfterAMissingBaselineOnBothPaths) {
    // A removed file dismissed with keep_buffer sets a Missing baseline. If the
    // path then reappears as a NON-regular/unreadable entry, that is an Unknown
    // observation: it must raise (never be collapsed into a Remove that the Missing
    // baseline would silently suppress), on BOTH the ordinary and overflow paths.
    auto dismissRemoval = [](Session& session) {
        std::filesystem::remove(session.workspacePath("note.txt"));
        session.runtime->external.ingest(
            {watchEvent(ssg::WatchEventKind::Remove, "note.txt", 1)});
        auto files = externalFiles(*session.runtime);
        ASSERT_EQ(files.size(), 1U);
        ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
        ASSERT_TRUE(externalAction(
                        *session.runtime,
                        {files[0].id, ssg::ExternalAction::KeepBuffer})
                        .accepted());
        ASSERT_TRUE(externalFiles(*session.runtime).empty());
    };
    {
        auto session = Session::open("missing_then_unknown_ordinary", "hi\n", true);
        dismissRemoval(session);
        std::filesystem::create_directory(session.workspacePath("note.txt"));
        session.runtime->external.ingest(
            {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 2)});
        const auto files = externalFiles(*session.runtime);
        ASSERT_EQ(files.size(), 1U);
        ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
    }
    {
        auto session = Session::open("missing_then_unknown_overflow", "hi\n", true);
        dismissRemoval(session);
        std::filesystem::create_directory(session.workspacePath("note.txt"));
        session.runtime->external.reconcileAllOpenDocumentsAgainstDisk();
        const auto files = externalFiles(*session.runtime);
        ASSERT_EQ(files.size(), 1U);
        ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
    }
}

TEST(aStaleOrdinaryRemoveWhosePathReappearedNonRegularRaises) {
    // A kept-removed file (Missing baseline). A watcher emits an ordinary Remove,
    // but before the reconcile processes it the path reappears as a directory. The
    // stale Remove must NOT match the Missing baseline and skip: re-observing sees a
    // present non-regular entry (Unknown) and RAISES.
    auto session = Session::open("stale_remove_reappeared", "hi\n", true);
    std::filesystem::remove(session.workspacePath("note.txt"));
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Remove, "note.txt", 1)});
    auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_TRUE(externalAction(
                    *session.runtime,
                    {files[0].id, ssg::ExternalAction::KeepBuffer})
                    .accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    // The path reappears as a directory; a stale ORDINARY Remove now arrives.
    std::filesystem::create_directory(session.workspacePath("note.txt"));
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Remove, "note.txt", 2)});
    files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
}

TEST(aStaleOrdinaryRemoveWhosePathReappearedRegularRaisesAsModified) {
    // The same stale-Remove race, but the path reappears as a REGULAR file with new
    // content: that is a real change, not an absence, so it must raise as modified.
    auto session = Session::open("stale_remove_regular", "hi\n", true);
    std::filesystem::remove(session.workspacePath("note.txt"));
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Remove, "note.txt", 1)});
    auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_TRUE(externalAction(
                    *session.runtime,
                    {files[0].id, ssg::ExternalAction::KeepBuffer})
                    .accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    writeFile(session.workspacePath("note.txt"), "reappeared\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Remove, "note.txt", 2)});
    files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyModified);
}

TEST(aStatusErrorOnAMissingBaselineRaisesOnTheOverflowPath) {
#ifdef _WIN32
    return;
#endif
    // A kept-removed file (Missing baseline) whose path can no longer be stat-ed
    // (its parent directory loses search permission) is a status ERROR -- Unknown,
    // not a clean absence. The overflow resync must route it through the
    // Unknown-detection chokepoint and RAISE, never treat it as a Missing-matchable
    // absence that the resync suppresses.
    auto root = uniqueRoot("status_error_overflow");
    std::filesystem::create_directories(root / "workspace" / "sub");
    writeFile(root / "workspace" / "sub" / "note.txt", "hi\n");
    auto created = ssg::createEditor(configFor(root));
    auto* runtime = created.session.get();
    (void)ssg::applyFilePathCompletion(*runtime, ssg::PromptCompletion::FileOpen, "sub/note.txt");
    (void)ssg::test::typeText(*runtime, "!");

    std::filesystem::remove(root / "workspace" / "sub" / "note.txt");
    runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Remove, "sub/note.txt", 1)});
    const auto snapshot0 = runtime->external.viewState();
    ASSERT_EQ(snapshot0.files.size(), 1U);
    ASSERT_TRUE(externalAction(
                    *runtime,
                    {snapshot0.files[0].id,
                     ssg::ExternalAction::KeepBuffer})
                    .accepted());

    // Deny search permission on the parent: symlink_status of the child now errors.
    std::filesystem::permissions(root / "workspace" / "sub",
                                 std::filesystem::perms::none);
    runtime->external.reconcileAllOpenDocumentsAgainstDisk();
    const auto snapshot1 = runtime->external.viewState();
    const auto& raised = snapshot1.files;
    // Restore permission before asserting so the test dir is always cleanable.
    std::filesystem::permissions(root / "workspace" / "sub",
                                 std::filesystem::perms::owner_all);
    ASSERT_EQ(raised.size(), 1U);
    ASSERT_EQ(raised[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
}

TEST(anOrdinaryDuplicateEventMatchingTheBaselineDoesNotResurrectTheConflict) {
    auto session = Session::open("ordinary_duplicate", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_TRUE(externalAction(
                    *session.runtime,
                    {files[0].id, ssg::ExternalAction::KeepBuffer})
                    .accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    // A coalesced/duplicate ORDINARY watcher event carrying the SAME dismissed disk
    // state matches the advanced baseline and must not resurrect the conflict (the
    // regression the side-table caused).
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 2)});
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
}

TEST(aRealChangeAfterKeepBufferStillRaises) {
    auto session = Session::open("real_change", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_TRUE(externalAction(
                    *session.runtime,
                    {files[0].id, ssg::ExternalAction::KeepBuffer})
                    .accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    // A genuinely NEW disk state differs from the advanced baseline and raises
    // afresh through the ordinary path.
    writeFile(session.workspacePath("note.txt"), "external again\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 2)});
    files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyModified);
}

TEST(keepBufferLeavesTheBufferAndEncodingUntouched) {
    auto session = Session::open("keep_untouched", "hi\n", true);
    const auto buffer = ssg::test::activeDocumentText(*session.runtime);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);

    ASSERT_TRUE(externalAction(
                    *session.runtime,
                    {files[0].id, ssg::ExternalAction::KeepBuffer})
                    .accepted());
    // The dismissal moves only the branched-from baseline: the visible buffer (and
    // hence the document's live text and encoding) is untouched.
    ASSERT_EQ(ssg::test::activeDocumentText(*session.runtime), buffer);
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
}

TEST(anExternalActionAppliesOnlyAnOfferedActionForTheSelectedFile) {
    auto session = Session::open("offered_guard", "hi\n", true);
    const auto buffer = ssg::test::activeDocumentText(*session.runtime);
    std::filesystem::remove(session.workspacePath("note.txt"));
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Remove, "note.txt", 1)});
    auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
    // A removed file offers KeepBuffer/OpenDiff but NOT Reload.
    ASSERT_TRUE(std::none_of(
        files[0].actions.begin(), files[0].actions.end(),
        [](ssg::ExternalActionAffordance const& action) {
            return action.action == ssg::ExternalAction::Reload;
        }));

    auto rejectedAction = session.runtime->input(ssg::ExternalActionPointerInput{
            {files[0].id, ssg::ExternalAction::Reload}});
    ASSERT_TRUE(rejectedAction.command.has_value());
    ASSERT_TRUE(rejectedAction.command->accepted());
    ASSERT_EQ(externalFiles(*session.runtime).size(), 1U);

    // The unoffered action is a guarded no-op: the section is untouched and the
    // buffer preserved.
    files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
    ASSERT_EQ(ssg::test::activeDocumentText(*session.runtime), buffer);

    // The offered KeepBuffer, by contrast, resolves the selected file.
    auto offeredAction = session.runtime->input(ssg::ExternalActionPointerInput{
            {files[0].id, ssg::ExternalAction::KeepBuffer}});
    ASSERT_TRUE(offeredAction.command.has_value());
    ASSERT_TRUE(offeredAction.command->accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
}

TEST(externalSelectionRefreshesDuringIngest) {
    auto session = Session::open("drain_refresh", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});

    const auto external = session.runtime->external.viewState();
    ASSERT_EQ(external.files.size(), 1U);
    // The library-owned selection is populated before any later dispatch.
    ASSERT_TRUE(external.selected.has_value());
    ASSERT_EQ(*external.selected, external.files[0].id);
}

TEST(aHostRoutesExternalKeysInTheExternalContextWhenExternalFocusHeld) {
    auto session = Session::open("host_ext_context", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->external.ingest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    ASSERT_TRUE(session.runtime->dispatch("external.focus").accepted());
    ssg::KeyStroke down;
    down.code = ssg::KeyCode::ArrowDown;
    const auto input = session.runtime->input(ssg::ClientKeyInput{down, {}});
    ASSERT_EQ(input.outcome, ssg::ClientInputOutcome::Dispatched);
}

}  // namespace

SSG_TEST_SUITE(test_session_external_modification) {
    RUN(anOpenDocumentChangedOnDiskPopulatesTheExternalSection);
    RUN(anOpenDocumentRemovedOnDiskPublishesRemovedStatusAndItsActions);
    RUN(aChangeToANonOpenFileRaisesNoExternalSection);
    RUN(aCleanOpenDocumentChangedOnDiskAutoReloadsWithoutRaisingActions);
    RUN(externalReloadCommitsDiskIntoTheWorkspaceAndClearsTheSection);
    RUN(externalKeepBufferClearsTheSectionWithoutTouchingTheBuffer);
    RUN(externalOpenDiffOpensALiveDiffTabForThatFile);
    RUN(externalActionOnAnUnknownIdIsARejectedNoOp);
    RUN(exmdStaleIdDoesNotActOnThePreviousSelection);
    RUN(exmdOnAnAlreadySelectedPresentIdStillActsOnIt);
    RUN(anSsgSaveIsCorrelatedAndRaisesNoExternalNotice);
    RUN(aGenuineExternalEditAfterASelfSaveIsNotSuppressed);
    RUN(aRenameRetiresThePendingEntryKeyedByThePreviousPath);
    RUN(aFailedDirtyRenameAdoptionDoesNotPublishAnUnresolvableNewPathEntry);
    RUN(aFailedRenameLeavesNoOrphanDiffEntry);
    RUN(anOverflowDoesNotResurrectAConflictDismissedByKeepBuffer);
    RUN(keepBufferAdvancesTheExternalBaselineToTheDismissedDiskState);
    RUN(keepBufferOnARemovedFileSetsTheExternalBaselineMissing);
    RUN(anEventObservingANonRegularOrUnreadablePathAlwaysRaises);
    RUN(aBrokenSymlinkObservationRaisesOnTheOverflowPath);
    RUN(anUnknownObservationRaisesEvenAfterAMissingBaselineOnBothPaths);
    RUN(aStaleOrdinaryRemoveWhosePathReappearedNonRegularRaises);
    RUN(aStaleOrdinaryRemoveWhosePathReappearedRegularRaisesAsModified);
    RUN(aStatusErrorOnAMissingBaselineRaisesOnTheOverflowPath);
    RUN(anOrdinaryDuplicateEventMatchingTheBaselineDoesNotResurrectTheConflict);
    RUN(aRealChangeAfterKeepBufferStillRaises);
    RUN(keepBufferLeavesTheBufferAndEncodingUntouched);
    RUN(aRenamedOpenDocumentFollowsItsFileWithoutASpuriousRemove);
    RUN(theExternalIdNeverCollidesWithAGitPathId);
    RUN(aSecondExternalChangeWhileActionsArePendingUpdatesNotDuplicates);
    RUN(anOverflowResyncsOpenDocumentsSoAChangeDuringTheOverflowRaisesItsConflict);
    RUN(anExternalActionAppliesOnlyAnOfferedActionForTheSelectedFile);
    RUN(externalSelectionRefreshesDuringIngest);
    RUN(aHostRoutesExternalKeysInTheExternalContextWhenExternalFocusHeld);
    return failed == 0 ? 0 : 1;
}
