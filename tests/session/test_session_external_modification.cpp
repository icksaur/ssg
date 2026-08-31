// External-modification backend oracles (7A Phase 5a). Seam kind: runtime seam.
// These drive the REAL runtime wiring the flow's unit tests cannot reach: the
// watcher-event reconcile, the shared-DiffModel revision allocation, the command
// handlers that resolve a published external id back to an open document, save
// correlation, adopt-rename, and the durable watcher-availability view-model field.
//
// The workspace is deliberately non-git and the git-diff worker is disabled, so a
// test drives ingress deterministically through the runtime-thread test hook rather
// than depending on inotify timing.
#include "../test_helpers.h"

#include <ssg/EditorSession.h>
#include <ssg/FileCommands.h>
#include <ssg/FilesystemWatcher.h>
#include <ssg/TextCodec.h>
#include <ssg/TextInputCommands.h>
#include <ssg/CompiledKeymap.h>
#include <ssg/session_snapshot.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>
#include <algorithm>
#include <array>

namespace {

std::filesystem::path uniqueRoot(std::string_view name) {
    auto root =
        std::filesystem::current_path() / ("runtime_extmod_" + std::string{name});
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    return root;
}

ssg::EditorSessionConfig configFor(const std::filesystem::path& root) {
    ssg::EditorSessionConfig config{root / "workspace", root / "scratch",
                                    root / "recovery"};
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
    ssg::EditorSessionCreateResult created;
    ssg::EditorSession* runtime = nullptr;

    static Session open(std::string_view name, std::string_view diskContent,
                        bool dirty) {
        Session session;
        session.root = uniqueRoot(name);
        writeFile(session.root / "workspace" / "note.txt", diskContent);
        session.created = ssg::EditorSession::create(configFor(session.root));
        session.runtime = session.created.session.get();
        (void)session.runtime->attach(
            {ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1});
        (void)session.runtime->dispatch(
            ssg::ClientId{1},
            {"file.open", session.runtime->revision(), std::string{"note.txt"}});
        if (dirty) {
            (void)session.runtime->dispatch(
                ssg::ClientId{1}, {"text.insert", session.runtime->revision(),
                                   ssg::TextInputArguments{"!"}});
        }
        return session;
    }

    std::filesystem::path workspacePath(std::string_view rel) const {
        return root / "workspace" / std::filesystem::path{std::string{rel}};
    }
};

std::vector<ssg::ExternalDocumentView> externalFiles(
    ssg::EditorSession& runtime) {
    auto snapshot = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    if (!snapshot) return {};
    return snapshot->semantic().sections().externalModification.files;
}

bool watcherAvailable(ssg::EditorSession& runtime) {
    auto snapshot = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    return snapshot && snapshot->semantic().sections().watcherAvailable;
}

std::string activeTabLabel(ssg::EditorSession& runtime) {
    auto snapshot = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    if (!snapshot) return {};
    const auto& tabs = snapshot->semantic().sections().tabs;
    if (!tabs.active) return {};
    for (const auto& tab : tabs.tabs) {
        if (tab.id == *tabs.active) return tab.label;
    }
    return {};
}

bool activeTabIsLiveDiff(ssg::EditorSession& runtime) {
    auto snapshot = runtime.present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    if (!snapshot) return false;
    const auto& tabs = snapshot->semantic().sections().tabs;
    if (!tabs.active) return false;
    for (const auto& tab : tabs.tabs) {
        if (tab.id == *tabs.active) return tab.kind == ssg::TabKind::LiveDiff;
    }
    return false;
}

TEST(anOpenDocumentChangedOnDiskPopulatesTheExternalSection) {
    auto session = Session::open("changed_populates", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});

    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyModified);
    ASSERT_EQ(files[0].id, ssg::DiffFileId{"external:note.txt"});
}

TEST(anOpenDocumentRemovedOnDiskPublishesRemovedStatusAndItsActions) {
    auto session = Session::open("removed_status", "hi\n", true);
    std::filesystem::remove(session.workspacePath("note.txt"));
    session.runtime->reconcileExternalWatchEventsForTest(
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
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "other.txt", 1)});

    ASSERT_TRUE(externalFiles(*session.runtime).empty());
}

TEST(aCleanOpenDocumentChangedOnDiskAutoReloadsWithoutRaisingActions) {
    auto session = Session::open("clean_autoreload", "hi\n", false);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});

    ASSERT_TRUE(externalFiles(*session.runtime).empty());
    ASSERT_EQ(session.runtime->activeDocumentText(), "external\n");
}

TEST(externalReloadCommitsDiskIntoTheWorkspaceAndClearsTheSection) {
    auto session = Session::open("reload_commits", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);

    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"external.reload", session.runtime->revision(),
                                files[0].id})
                    .accepted());

    ASSERT_TRUE(externalFiles(*session.runtime).empty());
    ASSERT_EQ(session.runtime->activeDocumentText(), "external\n");
}

TEST(externalKeepBufferClearsTheSectionWithoutTouchingTheBuffer) {
    auto session = Session::open("keep_buffer", "hi\n", true);
    const auto buffer = session.runtime->activeDocumentText();
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);

    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"external.keep_buffer",
                                session.runtime->revision(), files[0].id})
                    .accepted());

    ASSERT_TRUE(externalFiles(*session.runtime).empty());
    ASSERT_EQ(session.runtime->activeDocumentText(), buffer);
}

TEST(externalOpenDiffOpensALiveDiffTabForThatFile) {
    auto session = Session::open("open_diff", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);

    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"external.open_diff",
                                session.runtime->revision(), files[0].id})
                    .accepted());

    ASSERT_TRUE(activeTabIsLiveDiff(*session.runtime));
}

TEST(externalActionOnAnUnknownIdIsARejectedNoOp) {
    auto session = Session::open("unknown_id", "hi\n", true);

    const auto result = session.runtime->dispatch(
        ssg::ClientId{1}, {"external.keep_buffer", session.runtime->revision(),
                           ssg::DiffFileId{"external:missing.txt"}});

    ASSERT_FALSE(result.accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
}

TEST(exmdStaleIdDoesNotActOnThePreviousSelection) {
    auto session = Session::open("stale_select", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    const auto present = files[0].id;

    // external.select on an id absent from the section is REJECTED, so the host's
    // accepted()-gate skips the follow-up action. selectFile's own bool cannot be
    // the success signal: it is false for an absent id AND for an already-selected
    // id. The selection stays on the previously selected present file, and because
    // select failed no action runs on it.
    const auto stale = session.runtime->dispatch(
        ssg::ClientId{1},
        {"external.select", session.runtime->revision(),
         ssg::DiffFileId{"external:not-a-real-file.txt"}});
    ASSERT_FALSE(stale.accepted());
    auto snapshot =
        session.runtime->present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    const auto& external = snapshot->semantic().sections().externalModification;
    ASSERT_TRUE(external.selected.has_value());
    ASSERT_EQ(*external.selected, present);
}

TEST(exmdOnAnAlreadySelectedPresentIdStillActsOnIt) {
    auto session = Session::open("reselect", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    const auto present = files[0].id;

    // Selecting the CURRENT selection is a valid no-op: the id names a present
    // file, so external.select SUCCEEDS and the host lets the action run on it.
    // (selectFile returns false here because the selection did not move, which is
    // why presence -- not selectFile's bool -- decides command success.)
    const auto reselect = session.runtime->dispatch(
        ssg::ClientId{1},
        {"external.select", session.runtime->revision(), present});
    ASSERT_TRUE(reselect.accepted());
}

TEST(anSsgSaveIsCorrelatedAndRaisesNoExternalNotice) {
    auto session = Session::open("own_save", "hi\n", true);
    // A prior external change raised a pending conflict for the file.
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    ASSERT_EQ(externalFiles(*session.runtime).size(), 1U);
    // SSG writes the file itself; the save primitive records the expectation.
    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"file.save", session.runtime->revision(), {}})
                    .accepted());
    // The watcher reports the write with no state supplied; the reconcile stats the
    // (unchanged-since-save) file, matches the expectation, consumes it, AND clears
    // the pending conflict -- a successful self-save resolves the external state.
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 2)});

    ASSERT_TRUE(externalFiles(*session.runtime).empty());
}

TEST(aGenuineExternalEditAfterASelfSaveIsNotSuppressed) {
    auto session = Session::open("edit_after_save", "hi\n", true);
    // A self-save consumes its expectation (and clears any pending conflict).
    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"file.save", session.runtime->revision(), {}})
                    .accepted());
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    // A genuine external edit follows. Because the expectation was consumed rather
    // than left to accumulate, it is NOT suppressed and raises actions.
    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"text.insert", session.runtime->revision(),
                                ssg::TextInputArguments{"x"}})
                    .accepted());
    writeFile(session.workspacePath("note.txt"), "genuinely-external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 2)});

    ASSERT_EQ(externalFiles(*session.runtime).size(), 1U);
}

TEST(aCleanExternalReloadDecodesNonUtf8BytesThroughTheDocumentsEncoding) {
    auto session = Session::open("nonutf8_reload", std::string{"\xe9\n"}, false);
    ASSERT_TRUE(
        session.runtime
            ->dispatch(ssg::ClientId{1},
                       {"file.reopen_with_encoding", session.runtime->revision(),
                        ssg::ReopenWithEncodingArguments{
                            ssg::TextEncoding::Iso88591}})
            .accepted());
    // An external writer replaces the file with more Latin-1 bytes.
    writeFile(session.workspacePath("note.txt"), std::string{"\xe9\xe9\n"});
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});

    // The clean auto-reload decoded the raw disk bytes through the document's
    // encoding (each 0xE9 -> U+00E9 -> UTF-8 "\xC3\xA9"), never assuming UTF-8, so
    // the buffer is neither corrupted nor emptied.
    ASSERT_EQ(session.runtime->activeDocumentText(),
              std::string{"\xC3\xA9\xC3\xA9\n"});
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
}

TEST(aFailedDirtyRenameAdoptionDoesNotPublishAnUnresolvableNewPathEntry) {
    // note.txt is dirty AND other.txt is open, so adopting note.txt's rename onto
    // other.txt's path must fail (the destination is already open).
    auto session = Session::open("rename_adopt_fail", "hi\n", true);
    writeFile(session.workspacePath("other.txt"), "other\n");
    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"file.open", session.runtime->revision(),
                                std::string{"other.txt"}})
                    .accepted());

    std::filesystem::rename(session.workspacePath("note.txt"),
                            session.workspacePath("other.txt"));
    session.runtime->reconcileExternalWatchEventsForTest(
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
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    ASSERT_EQ(externalFiles(*session.runtime).size(), 1U);

    std::filesystem::rename(session.workspacePath("note.txt"),
                            session.workspacePath("renamed.txt"));
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Rename, "renamed.txt", 2, "note.txt")});

    // The old-path entry was retired: exactly one pending entry, keyed by the new
    // path -- not a stale pair.
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].id, ssg::DiffFileId{"external:renamed.txt"});
}

TEST(aRuntimeWatcherAvailabilityTransitionAdvancesTheRevision) {
    auto session = Session::open("watcher_transition", "hi\n", false);
    // The watcher is disabled by configFor, so it starts unavailable.
    ASSERT_FALSE(watcherAvailable(*session.runtime));

    const auto before = session.runtime->revision().value();
    session.runtime->reportWatcherAvailabilityForTest(true);
    ASSERT_TRUE(watcherAvailable(*session.runtime));
    ASSERT_TRUE(before < session.runtime->revision().value());

    const auto mid = session.runtime->revision().value();
    session.runtime->reportWatcherAvailabilityForTest(false);
    ASSERT_FALSE(watcherAvailable(*session.runtime));
    ASSERT_TRUE(mid < session.runtime->revision().value());

    // An unchanged report advances nothing (one revision per edge).
    const auto stable = session.runtime->revision().value();
    session.runtime->reportWatcherAvailabilityForTest(false);
    ASSERT_EQ(stable, session.runtime->revision().value());
}

TEST(watcherUnavailabilityIsPublishedAsDurableRuntimeState) {
    // The watcher is disabled, so this session has no watcher: external change is
    // not observed, and the durable semantic field says so.
    auto session = Session::open("watcher_unavailable", "hi\n", false);
    ASSERT_FALSE(watcherAvailable(*session.runtime));
}

TEST(aRenamedOpenDocumentFollowsItsFileWithoutASpuriousRemove) {
    auto session = Session::open("rename_follows", "hi\n", false);
    std::filesystem::rename(session.workspacePath("note.txt"),
                            session.workspacePath("renamed.txt"));
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Rename, "renamed.txt", 1, "note.txt")});

    // No spurious removed/created pair, and the open document followed its file.
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
    ASSERT_EQ(activeTabLabel(*session.runtime), "renamed.txt");
    ASSERT_EQ(session.runtime->activeDocumentText(), "hi\n");
}

TEST(theExternalIdNeverCollidesWithAGitPathId) {
    auto session = Session::open("no_collision", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
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
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    ASSERT_EQ(externalFiles(*session.runtime).size(), 1U);

    writeFile(session.workspacePath("note.txt"), "external-2\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 2)});

    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyModified);
}

TEST(aCleanExternalReloadDecodesUtf16BytesWithNulThroughTheDocumentsEncoding) {
    // Establish a clean UTF-16LE document. The initial bytes are a valid UTF-16LE
    // unit with no NUL, so reopen-with-encoding adopts them cleanly (updating the
    // persisted status, unlike set-encoding which would leave the buffer dirty).
    auto session = Session::open("utf16_reload", std::string{'\x41', '\x42'}, false);
    ASSERT_TRUE(
        session.runtime
            ->dispatch(ssg::ClientId{1},
                       {"file.reopen_with_encoding", session.runtime->revision(),
                        ssg::ReopenWithEncodingArguments{
                            ssg::TextEncoding::Utf16le}})
            .accepted());
    // An external writer replaces the file with real UTF-16LE bytes. Each ASCII
    // code unit carries a trailing NUL, which the raw-bytes binary guard would
    // wrongly reject -- the reload must decode through the document's UTF-16
    // encoding and reject only on a genuine decode failure.
    writeFile(session.workspacePath("note.txt"),
              std::string{'\x68', '\x00', '\x69', '\x00', '\x21', '\x00',
                          '\x0a', '\x00'});
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});

    // The clean auto-reload decoded the UTF-16LE disk bytes through the document's
    // encoding ("hi!\n"), never treating the legitimate NUL bytes as binary, so
    // the buffer is neither corrupted nor emptied.
    ASSERT_EQ(session.runtime->activeDocumentText(), std::string{"hi!\n"});
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
}

TEST(anOverflowResyncsOpenDocumentsSoAChangeDuringTheOverflowRaisesItsConflict) {
    // The buffer is dirty, so a disk change must raise a conflict rather than
    // auto-reload. The change happens while the watcher is overflowed: its Modify
    // event is lost, and only the Overflow arrives.
    auto session = Session::open("overflow_resync", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Overflow, "", 1)});

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
    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"file.open", session.runtime->revision(),
                                std::string{"other.txt"}})
                    .accepted());

    std::filesystem::rename(session.workspacePath("note.txt"),
                            session.workspacePath("other.txt"));
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Rename, "other.txt", 1, "note.txt")});

    ASSERT_FALSE(session.runtime->diffModelHasFileForTest(
        ssg::DiffFileId{"external:other.txt"}));
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
}

TEST(aRejectedSeededEventAdvancesTheSessionRevisionWithTheDiffModel) {
    // Same failed rename as above: the reconcile seeds the new-path diff entry, the
    // adoption is rejected, and the seed is rolled back. That net-advances the
    // shared DiffModel revision the published diff section is keyed to, so the
    // session revision MUST advance too -- otherwise a delta client would miss or
    // mis-order the diff removal the rollback published.
    auto session = Session::open("rejected_revision", "hi\n", true);
    writeFile(session.workspacePath("other.txt"), "other\n");
    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"file.open", session.runtime->revision(),
                                std::string{"other.txt"}})
                    .accepted());

    std::filesystem::rename(session.workspacePath("note.txt"),
                            session.workspacePath("other.txt"));
    const auto revisionBefore = session.runtime->revision();
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Rename, "other.txt", 1, "note.txt")});

    // No orphan entry survives AND the session revision advanced with the rolled-back
    // DiffModel revision, so the published state stays revision-consistent.
    ASSERT_FALSE(session.runtime->diffModelHasFileForTest(
        ssg::DiffFileId{"external:other.txt"}));
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
    ASSERT_TRUE(revisionBefore < session.runtime->revision());
}

TEST(anOverflowDoesNotResurrectAConflictDismissedByKeepBuffer) {
    auto session = Session::open("keep_overflow", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    const auto raised = externalFiles(*session.runtime);
    ASSERT_EQ(raised.size(), 1U);

    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"external.keep_buffer",
                                session.runtime->revision(), raised[0].id})
                    .accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    // An overflow resync re-derives events from disk. Disk still differs from the
    // buffer's baseline, but the user already dismissed this state, so it must NOT
    // re-raise the conflict.
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Overflow, "", 2)});
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    // A genuinely NEW disk change after the acknowledgement is a fresh question and
    // still raises the conflict.
    writeFile(session.workspacePath("note.txt"), "external again\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Overflow, "", 3)});
    const auto reraised = externalFiles(*session.runtime);
    ASSERT_EQ(reraised.size(), 1U);
    ASSERT_EQ(reraised[0].id, ssg::DiffFileId{"external:note.txt"});
    ASSERT_EQ(reraised[0].status, ssg::ExternalDocumentStatus::ExternallyModified);
}

TEST(keepBufferAdvancesTheExternalBaselineToTheDismissedDiskState) {
    auto session = Session::open("keep_advances", "hi\n", true);
    const auto buffer = session.runtime->activeDocumentText();
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);

    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"external.keep_buffer",
                                session.runtime->revision(), files[0].id})
                    .accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    // The baseline now equals the dismissed disk state, so a duplicate ordinary
    // event carrying that SAME state is a no-op, and the buffer is preserved.
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 2)});
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
    ASSERT_EQ(session.runtime->activeDocumentText(), buffer);
}

TEST(keepBufferOnARemovedFileSetsTheExternalBaselineMissing) {
    auto session = Session::open("keep_removed", "hi\n", true);
    const auto buffer = session.runtime->activeDocumentText();
    std::filesystem::remove(session.workspacePath("note.txt"));
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Remove, "note.txt", 1)});
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);

    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"external.keep_buffer",
                                session.runtime->revision(), files[0].id})
                    .accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    // The baseline is Missing (not a bogus empty-bytes baseline), so an overflow
    // resync with the file still absent does not re-raise the dismissed removal.
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Overflow, "", 2)});
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
    ASSERT_EQ(session.runtime->activeDocumentText(), buffer);
}

TEST(anEventObservingANonRegularOrUnreadablePathAlwaysRaises) {
    // A file replaced by a directory: the reconcile can neither read regular bytes
    // nor see a clean absence. Such an Unknown observation must never match a
    // stored baseline and always raises, on BOTH the ordinary and overflow paths.
    {
        auto session = Session::open("unknown_dir_ordinary", "hi\n", true);
        std::filesystem::remove(session.workspacePath("note.txt"));
        std::filesystem::create_directory(session.workspacePath("note.txt"));
        session.runtime->reconcileExternalWatchEventsForTest(
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
        session.runtime->reconcileExternalWatchEventsForTest(
            {watchEvent(ssg::WatchEventKind::Overflow, "", 1)});
        const auto files = externalFiles(*session.runtime);
        ASSERT_EQ(files.size(), 1U);
        ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
    }
    // A broken symlink where a regular file was: exists() follows the link and is
    // false, indistinguishable there from a true absence, so the overflow path must
    // detect the present symlink entry (lstat) and still raise.
    {
        auto session = Session::open("unknown_symlink_overflow", "hi\n", true);
        std::filesystem::remove(session.workspacePath("note.txt"));
        std::error_code linkCode;
        std::filesystem::create_symlink("does-not-exist",
                                        session.workspacePath("note.txt"), linkCode);
        ASSERT_FALSE(static_cast<bool>(linkCode));
        session.runtime->reconcileExternalWatchEventsForTest(
            {watchEvent(ssg::WatchEventKind::Overflow, "", 1)});
        const auto files = externalFiles(*session.runtime);
        ASSERT_EQ(files.size(), 1U);
        ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
    }
}

TEST(anUnknownObservationRaisesEvenAfterAMissingBaselineOnBothPaths) {
    // A removed file dismissed with keep_buffer sets a Missing baseline. If the
    // path then reappears as a NON-regular/unreadable entry, that is an Unknown
    // observation: it must raise (never be collapsed into a Remove that the Missing
    // baseline would silently suppress), on BOTH the ordinary and overflow paths.
    auto dismissRemoval = [](Session& session) {
        std::filesystem::remove(session.workspacePath("note.txt"));
        session.runtime->reconcileExternalWatchEventsForTest(
            {watchEvent(ssg::WatchEventKind::Remove, "note.txt", 1)});
        auto files = externalFiles(*session.runtime);
        ASSERT_EQ(files.size(), 1U);
        ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
        ASSERT_TRUE(session.runtime
                        ->dispatch(ssg::ClientId{1},
                                   {"external.keep_buffer",
                                    session.runtime->revision(), files[0].id})
                        .accepted());
        ASSERT_TRUE(externalFiles(*session.runtime).empty());
    };
    {
        auto session = Session::open("missing_then_unknown_ordinary", "hi\n", true);
        dismissRemoval(session);
        std::filesystem::create_directory(session.workspacePath("note.txt"));
        session.runtime->reconcileExternalWatchEventsForTest(
            {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 2)});
        const auto files = externalFiles(*session.runtime);
        ASSERT_EQ(files.size(), 1U);
        ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
    }
    {
        auto session = Session::open("missing_then_unknown_overflow", "hi\n", true);
        dismissRemoval(session);
        std::filesystem::create_directory(session.workspacePath("note.txt"));
        session.runtime->reconcileExternalWatchEventsForTest(
            {watchEvent(ssg::WatchEventKind::Overflow, "", 2)});
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
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Remove, "note.txt", 1)});
    auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"external.keep_buffer",
                                session.runtime->revision(), files[0].id})
                    .accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    // The path reappears as a directory; a stale ORDINARY Remove now arrives.
    std::filesystem::create_directory(session.workspacePath("note.txt"));
    session.runtime->reconcileExternalWatchEventsForTest(
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
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Remove, "note.txt", 1)});
    auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"external.keep_buffer",
                                session.runtime->revision(), files[0].id})
                    .accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    writeFile(session.workspacePath("note.txt"), "reappeared\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Remove, "note.txt", 2)});
    files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyModified);
}

TEST(aStatusErrorOnAMissingBaselineRaisesOnTheOverflowPath) {
    // A kept-removed file (Missing baseline) whose path can no longer be stat-ed
    // (its parent directory loses search permission) is a status ERROR -- Unknown,
    // not a clean absence. The overflow resync must route it through the
    // Unknown-detection chokepoint and RAISE, never treat it as a Missing-matchable
    // absence that the resync suppresses.
    auto root = uniqueRoot("status_error_overflow");
    std::filesystem::create_directories(root / "workspace" / "sub");
    writeFile(root / "workspace" / "sub" / "note.txt", "hi\n");
    auto created = ssg::EditorSession::create(configFor(root));
    auto* runtime = created.session.get();
    (void)runtime->attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                          ssg::ViewId{1});
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"sub/note.txt"}});
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"text.insert", runtime->revision(), ssg::TextInputArguments{"!"}});

    std::filesystem::remove(root / "workspace" / "sub" / "note.txt");
    runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Remove, "sub/note.txt", 1)});
    auto snapshot0 =
        runtime->present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_EQ(snapshot0->semantic().sections().externalModification.files.size(), 1U);
    ASSERT_TRUE(
        runtime
            ->dispatch(ssg::ClientId{1},
                       {"external.keep_buffer", runtime->revision(),
                        snapshot0->semantic().sections().externalModification.files[0].id})
            .accepted());

    // Deny search permission on the parent: symlink_status of the child now errors.
    std::filesystem::permissions(root / "workspace" / "sub",
                                 std::filesystem::perms::none);
    runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Overflow, "", 2)});
    auto snapshot1 =
        runtime->present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    const auto& raised = snapshot1->semantic().sections().externalModification.files;
    // Restore permission before asserting so the test dir is always cleanable.
    std::filesystem::permissions(root / "workspace" / "sub",
                                 std::filesystem::perms::owner_all);
    ASSERT_EQ(raised.size(), 1U);
    ASSERT_EQ(raised[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
}

TEST(anOrdinaryDuplicateEventMatchingTheBaselineDoesNotResurrectTheConflict) {
    auto session = Session::open("ordinary_duplicate", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"external.keep_buffer",
                                session.runtime->revision(), files[0].id})
                    .accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    // A coalesced/duplicate ORDINARY watcher event carrying the SAME dismissed disk
    // state matches the advanced baseline and must not resurrect the conflict (the
    // regression the side-table caused).
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 2)});
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
}

TEST(aRealChangeAfterKeepBufferStillRaises) {
    auto session = Session::open("real_change", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"external.keep_buffer",
                                session.runtime->revision(), files[0].id})
                    .accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());

    // A genuinely NEW disk state differs from the advanced baseline and raises
    // afresh through the ordinary path.
    writeFile(session.workspacePath("note.txt"), "external again\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 2)});
    files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyModified);
}

TEST(keepBufferLeavesTheBufferAndEncodingUntouched) {
    auto session = Session::open("keep_untouched", "hi\n", true);
    const auto buffer = session.runtime->activeDocumentText();
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    const auto files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);

    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"external.keep_buffer",
                                session.runtime->revision(), files[0].id})
                    .accepted());
    // The dismissal moves only the branched-from baseline: the visible buffer (and
    // hence the document's live text and encoding) is untouched.
    ASSERT_EQ(session.runtime->activeDocumentText(), buffer);
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
}

TEST(aDraftPersistedBeforeKeepBufferReopensWithoutResurrectingTheConflict) {
    auto root = uniqueRoot("draft_keep");
    writeFile(root / "workspace" / "note.txt", "hi\n");
    std::string draft;
    {
        auto created = ssg::EditorSession::create(configFor(root));
        ASSERT_TRUE(created.accepted());
        auto& runtime = *created.session;
        (void)runtime.attach(
            {ssg::ClientId{1}, ssg::InvocationOrigin::InProcess}, ssg::ViewId{1});
        (void)runtime.dispatch(
            ssg::ClientId{1},
            {"file.open", runtime.revision(), std::string{"note.txt"}});
        (void)runtime.dispatch(ssg::ClientId{1},
                               {"text.insert", runtime.revision(),
                                ssg::TextInputArguments{"!"}});
        draft = runtime.activeDocumentText();
        // Persist a draft whose journal baseline is the ORIGINAL disk state.
        ASSERT_EQ(runtime.flushAllAutosaveDrafts(), std::size_t{1});

        // An external change raises a conflict; dismissing it must refresh the
        // already-persisted draft record's baseline to the dismissed disk state, so
        // a crash-reopen does not re-raise the conflict via draft recovery.
        writeFile(root / "workspace" / "note.txt", "external\n");
        runtime.reconcileExternalWatchEventsForTest(
            {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
        const auto files = externalFiles(runtime);
        ASSERT_EQ(files.size(), 1U);
        ASSERT_TRUE(runtime
                        .dispatch(ssg::ClientId{1},
                                  {"external.keep_buffer", runtime.revision(),
                                   files[0].id})
                        .accepted());
    }

    // Crash-reopen over the same scratch store: the persisted draft now branches
    // from the dismissed disk state, so it classifies Restored, not Conflict.
    auto created = ssg::EditorSession::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    auto& runtime = *created.session;
    (void)runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                         ssg::ViewId{1});
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"note.txt"}})
                    .accepted());
    ASSERT_EQ(runtime.activeDocumentText(), draft);
    ASSERT_TRUE(runtime.activeDraftReopenNotice() ==
                ssg::EditorSession::DraftReopenNotice::Restored);
}

TEST(anExternalActionAppliesOnlyAnOfferedActionForTheSelectedFile) {
    auto session = Session::open("offered_guard", "hi\n", true);
    const auto buffer = session.runtime->activeDocumentText();
    std::filesystem::remove(session.workspacePath("note.txt"));
    session.runtime->reconcileExternalWatchEventsForTest(
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

    auto rejectedAction = session.runtime->input(
        ssg::ClientId{1},
        ssg::ExternalActionPointerInput{
            {session.runtime->revision()},
            {files[0].id, ssg::ExternalAction::Reload}});
    ASSERT_TRUE(rejectedAction.command.has_value());
    ASSERT_TRUE(rejectedAction.command->accepted());
    ASSERT_EQ(externalFiles(*session.runtime).size(), 1U);

    // The unoffered action is a guarded no-op: the section is untouched and the
    // buffer preserved.
    files = externalFiles(*session.runtime);
    ASSERT_EQ(files.size(), 1U);
    ASSERT_EQ(files[0].status, ssg::ExternalDocumentStatus::ExternallyRemoved);
    ASSERT_EQ(session.runtime->activeDocumentText(), buffer);

    // The offered KeepBuffer, by contrast, resolves the selected file.
    auto offeredAction = session.runtime->input(
        ssg::ClientId{1},
        ssg::ExternalActionPointerInput{
            {session.runtime->revision()},
            {files[0].id, ssg::ExternalAction::KeepBuffer}});
    ASSERT_TRUE(offeredAction.command.has_value());
    ASSERT_TRUE(offeredAction.command->accepted());
    ASSERT_TRUE(externalFiles(*session.runtime).empty());
}

TEST(externalPresenceAndSelectionRefreshInTheWatcherDrainNotOnlyOnDispatch) {
    auto session = Session::open("drain_refresh", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    // Only a watcher event is drained -- no command is dispatched afterwards.
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});

    auto snapshot =
        session.runtime->present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    const auto& external = snapshot->semantic().sections().externalModification;
    ASSERT_EQ(external.files.size(), 1U);
    // The library-owned selection homed to the raised file in the watcher drain,
    // not on a later dispatch: it is already populated in the very first snapshot.
    ASSERT_TRUE(external.selected.has_value());
    ASSERT_EQ(*external.selected, external.files[0].id);
}

TEST(aHostRoutesExternalKeysInTheExternalContextWhenExternalFocusHeld) {
    // The library keymap authors external.select_next in the "external" context.
    // A host resolves directly from the authoritative frame endpoint, or the
    // external select/action keys fall through to the editor.
    auto session = Session::open("host_ext_context", "hi\n", true);
    writeFile(session.workspacePath("note.txt"), "external\n");
    session.runtime->reconcileExternalWatchEventsForTest(
        {watchEvent(ssg::WatchEventKind::Modify, "note.txt", 1)});
    ASSERT_EQ(externalFiles(*session.runtime).size(), 1U);
    ASSERT_TRUE(session.runtime
                    ->dispatch(ssg::ClientId{1},
                               {"external.focus", session.runtime->revision(), {}})
                    .accepted());

    auto snapshot =
        session.runtime->present(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    const auto& sections = snapshot->semantic().sections();
    ASSERT_TRUE(sections.uiFrame.effectiveFocus() ==
                ssg::FocusTarget::ExternalModification);

    ssg::CompiledKeymap keymap{sections.keymap, *session.runtime->commandCatalog()};
    ssg::KeyStroke down;
    down.code = ssg::KeyCode::ArrowDown;
    const auto stroke = ssg::CompiledKeymap::compile(down);

    const auto effective = keymap.resolve(
        std::array{stroke}, sections.uiFrame.effectiveFocus());
    ASSERT_TRUE(effective.kind == ssg::KeymapMatchKind::Resolved);
    ASSERT_TRUE(effective.command.name() == std::string_view{"external.select_next"});
}

}  // namespace

int main() {
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
    RUN(aCleanExternalReloadDecodesNonUtf8BytesThroughTheDocumentsEncoding);
    RUN(aCleanExternalReloadDecodesUtf16BytesWithNulThroughTheDocumentsEncoding);
    RUN(aRenameRetiresThePendingEntryKeyedByThePreviousPath);
    RUN(aFailedDirtyRenameAdoptionDoesNotPublishAnUnresolvableNewPathEntry);
    RUN(aFailedRenameLeavesNoOrphanDiffEntry);
    RUN(aRejectedSeededEventAdvancesTheSessionRevisionWithTheDiffModel);
    RUN(anOverflowDoesNotResurrectAConflictDismissedByKeepBuffer);
    RUN(keepBufferAdvancesTheExternalBaselineToTheDismissedDiskState);
    RUN(keepBufferOnARemovedFileSetsTheExternalBaselineMissing);
    RUN(anEventObservingANonRegularOrUnreadablePathAlwaysRaises);
    RUN(anUnknownObservationRaisesEvenAfterAMissingBaselineOnBothPaths);
    RUN(aStaleOrdinaryRemoveWhosePathReappearedNonRegularRaises);
    RUN(aStaleOrdinaryRemoveWhosePathReappearedRegularRaisesAsModified);
    RUN(aStatusErrorOnAMissingBaselineRaisesOnTheOverflowPath);
    RUN(anOrdinaryDuplicateEventMatchingTheBaselineDoesNotResurrectTheConflict);
    RUN(aRealChangeAfterKeepBufferStillRaises);
    RUN(keepBufferLeavesTheBufferAndEncodingUntouched);
    RUN(aDraftPersistedBeforeKeepBufferReopensWithoutResurrectingTheConflict);
    RUN(aRuntimeWatcherAvailabilityTransitionAdvancesTheRevision);
    RUN(aRenamedOpenDocumentFollowsItsFileWithoutASpuriousRemove);
    RUN(theExternalIdNeverCollidesWithAGitPathId);
    RUN(aSecondExternalChangeWhileActionsArePendingUpdatesNotDuplicates);
    RUN(anOverflowResyncsOpenDocumentsSoAChangeDuringTheOverflowRaisesItsConflict);
    RUN(watcherUnavailabilityIsPublishedAsDurableRuntimeState);
    RUN(anExternalActionAppliesOnlyAnOfferedActionForTheSelectedFile);
    RUN(externalPresenceAndSelectionRefreshInTheWatcherDrainNotOnlyOnDispatch);
    RUN(aHostRoutesExternalKeysInTheExternalContextWhenExternalFocusHeld);
    return failed == 0 ? 0 : 1;
}
