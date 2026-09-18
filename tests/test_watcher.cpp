#include <ssg/FilesystemWatcher.h>
#include <ssg/GitMetadataWatcher.h>
#include "test_helpers.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using ssg::FileIdentity;
using ssg::NativeWatchAction;
using ssg::NativeWatchEvent;
using ssg::WatchEvent;
using ssg::WatchEventKind;
using ssg::WatchEventNormalizer;
using ssg::WatchEventOrigin;
using ssg::WatchFileState;
using ssg::WatcherConfig;
using ssg::WorkspaceEntry;
using ssg::WorkspaceScan;

const auto kStart = ssg::WatchTimePoint{};

WatchFileState state(std::uint64_t id, std::uint64_t size = 1,
                     std::int64_t modified = 1) {
    return {{1, {id, 0}}, size, modified};
}

NativeWatchEvent raw(NativeWatchAction action, std::string path,
                     std::optional<WatchFileState> observed = std::nullopt,
                     std::uint64_t renameToken = 0) {
    return {action, std::move(path), renameToken, observed};
}

WatcherConfig config(std::size_t maxEvents = 16) {
    WatcherConfig result;
    result.debounce = 10ms;
    result.rescanRetry = 100ms;
    result.maxQueuedEvents = maxEvents;
    result.maxPendingRenames = 4;
    result.maxSaveExpectations = 4;
    result.maxRescanEntries = 16;
    return result;
}

WorkspaceScan unchangedScan(std::size_t) {
    return {{}, true};
}

std::vector<WatchEvent> finish(WatchEventNormalizer& normalizer) {
    return normalizer.takeReady(kStart + 11ms);
}

TEST(createModifyDeleteScriptsAreDeterministic) {
    WatchEventNormalizer normalizer(config(), {}, unchangedScan);
    normalizer.push(raw(NativeWatchAction::Create, "new.txt", state(1)), kStart);
    normalizer.push(raw(NativeWatchAction::Modify, "new.txt", state(1, 2)), kStart + 1ms);

    auto events = finish(normalizer);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Create);
    ASSERT_EQ(events[0].path, std::filesystem::path{"new.txt"});
    ASSERT_EQ(events[0].size, std::optional<std::uint64_t>{2});

    WatchEventNormalizer vanished(config(), {}, unchangedScan);
    vanished.push(raw(NativeWatchAction::Create, "gone.txt", state(2)), kStart);
    vanished.push(raw(NativeWatchAction::Remove, "gone.txt"), kStart + 1ms);
    ASSERT_TRUE(finish(vanished).empty());

    WatchEventNormalizer deleted(
        config(), {WorkspaceEntry{"old.txt", state(3, 4)}}, unchangedScan);
    deleted.push(raw(NativeWatchAction::Modify, "old.txt", state(3, 5)), kStart);
    deleted.push(raw(NativeWatchAction::Remove, "old.txt"), kStart + 1ms);
    events = finish(deleted);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Remove);
    ASSERT_EQ(events[0].identity, std::optional<FileIdentity>{state(3).identity});
}

TEST(renamePairingPreservesIdentityAndFinalPath) {
    WatchEventNormalizer normalizer(
        config(), {WorkspaceEntry{"before.txt", state(7, 3)}}, unchangedScan);
    normalizer.push(raw(NativeWatchAction::RenameFrom, "before.txt",
                        std::nullopt, 42), kStart);
    normalizer.push(raw(NativeWatchAction::RenameTo, "after.txt",
                        state(7, 3), 42), kStart + 1ms);
    normalizer.push(raw(NativeWatchAction::Modify, "after.txt",
                        state(7, 8)), kStart + 2ms);

    const auto events = normalizer.takeReady(kStart + 13ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Rename);
    ASSERT_EQ(events[0].previousPath,
              std::optional<std::filesystem::path>{"before.txt"});
    ASSERT_EQ(events[0].path, std::filesystem::path{"after.txt"});
    ASSERT_EQ(events[0].identity, std::optional<FileIdentity>{state(7).identity});
    ASSERT_EQ(events[0].size, std::optional<std::uint64_t>{8});
}

TEST(renameThenDeleteReportsTheOriginalPath) {
    WatchEventNormalizer normalizer(
        config(), {WorkspaceEntry{"before.txt", state(8)}}, unchangedScan);
    normalizer.push(raw(NativeWatchAction::RenameFrom, "before.txt",
                        std::nullopt, 50), kStart);
    normalizer.push(raw(NativeWatchAction::RenameTo, "after.txt",
                        state(8), 50), kStart + 1ms);
    normalizer.push(
        raw(NativeWatchAction::Remove, "after.txt"), kStart + 2ms);
    const auto events = normalizer.takeReady(kStart + 13ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Remove);
    ASSERT_EQ(events[0].path, std::filesystem::path{"before.txt"});
    ASSERT_FALSE(events[0].previousPath.has_value());
}

TEST(newSourceRenamedToExistingPathIsOneModify) {
    WatchEventNormalizer normalizer(
        config(), {WorkspaceEntry{"target.txt", state(30, 1)}}, unchangedScan);
    normalizer.registerSave({"target.txt", state(31, 8, 4)});
    normalizer.push(
        raw(NativeWatchAction::Create, "temp.txt", state(31, 8, 4)), kStart);
    normalizer.push(raw(NativeWatchAction::RenameFrom, "temp.txt",
                        std::nullopt, 60), kStart + 1ms);
    normalizer.push(raw(NativeWatchAction::RenameTo, "target.txt",
                        state(31, 8, 4), 60), kStart + 2ms);

    const auto events = normalizer.takeReady(kStart + 13ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Modify);
    ASSERT_EQ(events[0].path, std::filesystem::path{"target.txt"});
    ASSERT_EQ(events[0].origin, WatchEventOrigin::SsgSave);
}

TEST(newSourceRenamedToNewPathIsOneCreate) {
    WatchEventNormalizer normalizer(config(), {}, unchangedScan);
    normalizer.push(
        raw(NativeWatchAction::Create, "temp.txt", state(32)), kStart);
    normalizer.push(raw(NativeWatchAction::RenameFrom, "temp.txt",
                        std::nullopt, 61), kStart + 1ms);
    normalizer.push(raw(NativeWatchAction::RenameTo, "target.txt",
                        state(32), 61), kStart + 2ms);

    const auto events = normalizer.takeReady(kStart + 13ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Create);
    ASSERT_EQ(events[0].path, std::filesystem::path{"target.txt"});
}

TEST(identityReuseAtAnotherPathKeepsBothEvents) {
    WatchEventNormalizer normalizer(
        config(), {WorkspaceEntry{"old.txt", state(12)}}, unchangedScan);
    normalizer.push(raw(NativeWatchAction::Remove, "old.txt"), kStart);
    normalizer.push(
        raw(NativeWatchAction::Create, "new.txt", state(12)), kStart + 1ms);
    const auto events = normalizer.takeReady(kStart + 12ms);
    ASSERT_EQ(events.size(), std::size_t{2});
    ASSERT_EQ(events[0].kind, WatchEventKind::Remove);
    ASSERT_EQ(events[1].kind, WatchEventKind::Create);
}

TEST(samePathReplacementRequiresMatchingIdentityToCoalesce) {
    WatchEventNormalizer replaced(
        config(), {WorkspaceEntry{"file.txt", state(40)}}, unchangedScan);
    replaced.push(raw(NativeWatchAction::Remove, "file.txt"), kStart);
    replaced.push(
        raw(NativeWatchAction::Create, "file.txt", state(41)), kStart + 1ms);
    auto events = replaced.takeReady(kStart + 12ms);
    ASSERT_EQ(events.size(), std::size_t{2});
    ASSERT_EQ(events[0].kind, WatchEventKind::Remove);
    ASSERT_EQ(events[0].identity,
              std::optional<FileIdentity>{state(40).identity});
    ASSERT_EQ(events[1].kind, WatchEventKind::Create);
    ASSERT_EQ(events[1].identity,
              std::optional<FileIdentity>{state(41).identity});

    WatchEventNormalizer recreated(
        config(), {WorkspaceEntry{"file.txt", state(42)}}, unchangedScan);
    recreated.push(raw(NativeWatchAction::Remove, "file.txt"), kStart);
    recreated.push(
        raw(NativeWatchAction::Create, "file.txt", state(42, 2)), kStart + 1ms);
    events = recreated.takeReady(kStart + 12ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Modify);
}

TEST(samePathReplacementCoalescesFollowupsWithNewIdentity) {
    WatchEventNormalizer modified(
        config(), {WorkspaceEntry{"file.txt", state(43)}}, unchangedScan);
    modified.push(raw(NativeWatchAction::Remove, "file.txt"), kStart);
    modified.push(
        raw(NativeWatchAction::Create, "file.txt", state(44, 1)), kStart + 1ms);
    modified.push(
        raw(NativeWatchAction::Modify, "file.txt", state(44, 3)), kStart + 2ms);
    auto events = modified.takeReady(kStart + 13ms);
    ASSERT_EQ(events.size(), std::size_t{2});
    ASSERT_EQ(events[0].kind, WatchEventKind::Remove);
    ASSERT_EQ(events[0].identity,
              std::optional<FileIdentity>{state(43).identity});
    ASSERT_EQ(events[1].kind, WatchEventKind::Create);
    ASSERT_EQ(events[1].identity,
              std::optional<FileIdentity>{state(44).identity});
    ASSERT_EQ(events[1].size, std::optional<std::uint64_t>{3});

    WatchEventNormalizer vanished(
        config(), {WorkspaceEntry{"file.txt", state(45)}}, unchangedScan);
    vanished.push(raw(NativeWatchAction::Remove, "file.txt"), kStart);
    vanished.push(
        raw(NativeWatchAction::Create, "file.txt", state(46)), kStart + 1ms);
    vanished.push(
        raw(NativeWatchAction::Remove, "file.txt"), kStart + 2ms);
    events = vanished.takeReady(kStart + 13ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Remove);
    ASSERT_EQ(events[0].identity,
              std::optional<FileIdentity>{state(45).identity});
}

TEST(unmatchedRenameHalvesBecomeBoundaryEvents) {
    WatchEventNormalizer movedOut(
        config(), {WorkspaceEntry{"out.txt", state(9)}}, unchangedScan);
    movedOut.push(raw(NativeWatchAction::RenameFrom, "out.txt",
                       std::nullopt, 10), kStart);
    auto events = finish(movedOut);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Remove);

    WatchEventNormalizer movedIn(config(), {}, unchangedScan);
    movedIn.push(raw(NativeWatchAction::RenameTo, "in.txt", state(10),
                      11), kStart);
    events = finish(movedIn);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Create);
}

TEST(atomicRenameReplaceSaveIsNeverDroppedAndSurfacesAsModify) {
    // An editor that saves by renaming the original OUT of the watched tree and
    // writing a fresh file at the same path (vim writebackup, emacs, VS Code, mv)
    // produces an UNPAIRED RenameFrom(path) plus a Create(path)+Modify. The
    // rename-away must not expire to a Remove that coalesce-cancels the recreate:
    // the workspace effect is a single Modify. A regression here silently hides
    // every rename-based external save from an open document.
    WatchEventNormalizer replaced(
        config(), {WorkspaceEntry{"file.txt", state(9)}}, unchangedScan);
    replaced.push(raw(NativeWatchAction::RenameFrom, "file.txt",
                      std::nullopt, /*renameToken=*/10),
                  kStart);
    replaced.push(raw(NativeWatchAction::Create, "file.txt", state(11)),
                  kStart + 1ms);
    replaced.push(raw(NativeWatchAction::Modify, "file.txt", state(11, 2)),
                  kStart + 2ms);
    auto replacedEvents = replaced.takeReady(kStart + 13ms);
    ASSERT_EQ(replacedEvents.size(), std::size_t{1});
    ASSERT_EQ(replacedEvents[0].kind, WatchEventKind::Modify);
    ASSERT_EQ(replacedEvents[0].path, std::filesystem::path{"file.txt"});
}

TEST(modifyThenGenuineMoveAwayStillReportsRemove) {
    // The atomic-replace guard must not swallow a genuine move-away: a file
    // modified and THEN renamed out of the tree (its pending Modify PRE-dates the
    // rename) must surface as a Remove, not be misread as a recreate/Modify.
    WatchEventNormalizer movedAway(
        config(), {WorkspaceEntry{"file.txt", state(9)}}, unchangedScan);
    movedAway.push(raw(NativeWatchAction::Modify, "file.txt", state(9, 2)),
                   kStart);
    movedAway.push(raw(NativeWatchAction::RenameFrom, "file.txt",
                       std::nullopt, /*renameToken=*/10),
                   kStart + 1ms);
    auto events = movedAway.takeReady(kStart + 12ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Remove);
    ASSERT_EQ(events[0].path, std::filesystem::path{"file.txt"});
}

TEST(atomicRenameReplaceInOneClockTickStillSurfacesAsModify) {
    // The recreate-after-rename test must not rely on the observation clock: a
    // coarse clock stamps an entire poll batch with one time, so RenameFrom and
    // the recreating Create can share a timestamp. Ingestion ORDER (arrival), not
    // the timestamp, must classify this as an atomic replace.
    WatchEventNormalizer replaced(
        config(), {WorkspaceEntry{"file.txt", state(9)}}, unchangedScan);
    replaced.push(raw(NativeWatchAction::RenameFrom, "file.txt",
                      std::nullopt, /*renameToken=*/10),
                  kStart);
    replaced.push(raw(NativeWatchAction::Create, "file.txt", state(11)),
                  kStart);
    replaced.push(raw(NativeWatchAction::Modify, "file.txt", state(11, 2)),
                  kStart);
    auto events = replaced.takeReady(kStart + 11ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Modify);
    ASSERT_EQ(events[0].path, std::filesystem::path{"file.txt"});
}

TEST(debounceReleasesOnlyAfterTheWindow) {
    WatchEventNormalizer normalizer(config(), {}, unchangedScan);
    normalizer.push(raw(NativeWatchAction::Create, "later.txt", state(1)), kStart);
    ASSERT_TRUE(normalizer.takeReady(kStart + 9ms).empty());
    ASSERT_EQ(normalizer.takeReady(kStart + 10ms).size(), std::size_t{1});
}

TEST(exactSaveResultIsCorrelatedOnce) {
    WatchEventNormalizer normalizer(config(), {}, unchangedScan);
    const ssg::SaveExpectation saved{"saved.txt", state(20, 12, 99)};
    normalizer.registerSave(saved);
    normalizer.push(raw(NativeWatchAction::Modify, "saved.txt",
                        state(20, 12, 99)), kStart);
    auto first = normalizer.takeReady(kStart + 11ms);
    ASSERT_EQ(first.size(), std::size_t{1});
    ASSERT_EQ(first[0].origin, WatchEventOrigin::SsgSave);

    normalizer.push(raw(NativeWatchAction::Modify, "saved.txt",
                        state(20, 13, 100)), kStart + 20ms);
    auto second = normalizer.takeReady(kStart + 31ms);
    ASSERT_EQ(second.size(), std::size_t{1});
    ASSERT_EQ(second[0].origin, WatchEventOrigin::External);
}

TEST(overflowRescansAndEmitsSyntheticChanges) {
    const std::vector<WorkspaceEntry> initial{
        {"changed.txt", state(1, 1)}, {"deleted.txt", state(2, 2)}};
    const auto scan = [](std::size_t limit) {
        ASSERT_EQ(limit, std::size_t{16});
        return WorkspaceScan{{
            {"changed.txt", state(1, 3)},
            {"created.txt", state(3, 4)},
        }, true};
    };
    WatchEventNormalizer normalizer(config(), initial, scan);
    normalizer.push(raw(NativeWatchAction::Overflow, ""), kStart);

    const auto events = normalizer.takeReady(kStart);
    ASSERT_EQ(events.size(), std::size_t{4});
    ASSERT_EQ(events[0].kind, WatchEventKind::Overflow);
    ASSERT_EQ(events[1].kind, WatchEventKind::Modify);
    ASSERT_EQ(events[2].kind, WatchEventKind::Create);
    ASSERT_EQ(events[3].kind, WatchEventKind::Remove);
}

TEST(partialRescanNeverPublishesPartialTruth) {
    int scans = 0;
    const auto partial = [&scans](std::size_t) {
        ++scans;
        if (scans == 1) {
            return WorkspaceScan{{{"partial.txt", state(4)}}, false};
        }
        return WorkspaceScan{{{"complete.txt", state(5)}}, true};
    };
    WatchEventNormalizer normalizer(config(), {}, partial);
    normalizer.push(raw(NativeWatchAction::Overflow, ""), kStart);
    auto events = normalizer.takeReady(kStart);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Overflow);
    normalizer.push(raw(NativeWatchAction::Overflow, ""), kStart + 1ms);
    ASSERT_TRUE(normalizer.takeReady(kStart + 99ms).empty());
    ASSERT_EQ(scans, 1);
    events = normalizer.takeReady(kStart + 100ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Create);
    ASSERT_EQ(scans, 2);
}

TEST(rescanUsesStableIdentityToRecoverRename) {
    const auto scan = [](std::size_t) {
        return WorkspaceScan{{{"after.txt", state(15, 2)}}, true};
    };
    WatchEventNormalizer normalizer(
        config(), {WorkspaceEntry{"before.txt", state(15, 2)}}, scan);
    normalizer.push(raw(NativeWatchAction::Overflow, ""), kStart);
    const auto events = normalizer.takeReady(kStart);
    ASSERT_EQ(events.size(), std::size_t{2});
    ASSERT_EQ(events[1].kind, WatchEventKind::Rename);
    ASSERT_EQ(events[1].path, std::filesystem::path{"after.txt"});
    ASSERT_EQ(events[1].previousPath,
              std::optional<std::filesystem::path>{"before.txt"});
}

TEST(queueExhaustionCollapsesToOverflowAndRescan) {
    int scans = 0;
    const auto scan = [&scans](std::size_t) {
        ++scans;
        return WorkspaceScan{{
            {"a", state(1)},
            {"b", state(2)},
            {"c", state(3)},
        }, true};
    };
    WatchEventNormalizer normalizer(config(2), {}, scan);
    normalizer.push(raw(NativeWatchAction::Create, "a", state(1)), kStart);
    normalizer.push(raw(NativeWatchAction::Create, "b", state(2)), kStart);
    normalizer.push(raw(NativeWatchAction::Create, "c", state(3)), kStart);

    const auto events = normalizer.takeReady(kStart);
    ASSERT_EQ(scans, 1);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Overflow);
}

TEST(renameQueueOverflowDoesNotInvalidateActivePair) {
    auto small = config(1);
    const auto scan = [](std::size_t) {
        return WorkspaceScan{{
            {"occupied.txt", state(50)},
            {"after.txt", state(51)},
        }, true};
    };
    WatchEventNormalizer normalizer(
        small, {WorkspaceEntry{"before.txt", state(51)}}, scan);
    normalizer.push(
        raw(NativeWatchAction::Create, "occupied.txt", state(50)), kStart);
    normalizer.push(raw(NativeWatchAction::RenameFrom, "before.txt",
                        std::nullopt, 70), kStart + 1ms);
    normalizer.push(raw(NativeWatchAction::RenameTo, "after.txt",
                        state(51), 70), kStart + 2ms);

    const auto events = normalizer.takeReady(kStart + 2ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Overflow);
}

TEST(expiringRenameQueueOverflowDoesNotInvalidateIteration) {
    auto small = config(1);
    const auto scan = [](std::size_t) {
        return WorkspaceScan{{
            {"occupied.txt", state(52)},
        }, true};
    };
    WatchEventNormalizer normalizer(
        small, {WorkspaceEntry{"before.txt", state(53)}}, scan);
    normalizer.push(
        raw(NativeWatchAction::Create, "occupied.txt", state(52)), kStart);
    normalizer.push(raw(NativeWatchAction::RenameFrom, "before.txt",
                        std::nullopt, 71), kStart);

    const auto events = normalizer.takeReady(kStart + 10ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Overflow);
}

TEST(invalidBoundsAreRejectedAtConstruction) {
    auto invalid = config();
    invalid.maxQueuedEvents = 0;
    ASSERT_THROWS(WatchEventNormalizer(invalid, {}, unchangedScan),
                  std::invalid_argument);
}

std::filesystem::path uniqueTempDirectory() {
    const auto path = std::filesystem::temp_directory_path() /
        ("ssg-watcher-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch().count()));
    std::filesystem::create_directory(path);
    return path;
}

TEST(watchFileStateObservesFilesystemMetadata) {
    const auto root = uniqueTempDirectory();
    const auto file = root / "file.txt";
    const std::string contents{"watch-state"};
    std::ofstream{file} << contents;

    const auto observedFile = WatchFileState::observe(file);
    ASSERT_TRUE(observedFile.has_value());
    if (observedFile) {
        std::error_code error;
        const auto modified = std::filesystem::last_write_time(file, error);
        ASSERT_FALSE(error);
        ASSERT_EQ(observedFile->identity, ssg::statFile(file)->identity);
        ASSERT_EQ(observedFile->size,
                  static_cast<std::uint64_t>(contents.size()));
        ASSERT_EQ(
            observedFile->modificationTime,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                modified.time_since_epoch()).count());
    }

    const auto emptyFile = root / "empty.txt";
    std::ofstream{emptyFile};
    const auto observedEmptyFile = WatchFileState::observe(emptyFile);
    ASSERT_TRUE(observedEmptyFile.has_value());
    if (observedEmptyFile) {
        ASSERT_EQ(observedEmptyFile->size, std::uint64_t{0});
    }

    const auto observedDirectory = WatchFileState::observe(root);
    ASSERT_TRUE(observedDirectory.has_value());
    if (observedDirectory) {
        ASSERT_EQ(observedDirectory->identity, ssg::statFile(root)->identity);
        ASSERT_EQ(observedDirectory->size, std::uint64_t{0});
    }
    ASSERT_FALSE(WatchFileState::observe(root / "missing").has_value());

    std::filesystem::remove_all(root);
}

std::vector<WatchEvent> pollUntil(
    ssg::FilesystemWatcher& watcher, WatchEventKind kind,
    std::chrono::milliseconds budget = 2s) {
    std::vector<WatchEvent> all;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        auto batch = watcher.poll(20ms);
        all.insert(all.end(), batch.begin(), batch.end());
        for (const auto& event : all) {
            if (event.kind == kind) {
                return all;
            }
        }
    }
    return all;
}

TEST(platformAdapterReportsRecursiveNormalizedEvents) {
    const auto root = uniqueTempDirectory();
    {
        auto watcher = ssg::makePlatformFilesystemWatcher(root, config());
        std::filesystem::create_directory(root / "sub");
        std::ofstream(root / "sub" / "file.txt") << "one";

        auto events = pollUntil(*watcher, WatchEventKind::Create);
        bool sawFile = false;
        for (const auto& event : events) {
            sawFile = sawFile ||
                (event.kind == WatchEventKind::Create &&
                 event.path == std::filesystem::path{"sub/file.txt"});
        }
        ASSERT_TRUE(sawFile);

        std::filesystem::rename(root / "sub" / "file.txt",
                                root / "sub" / "renamed.txt");
        events = pollUntil(*watcher, WatchEventKind::Rename);
        bool sawRename = false;
        for (const auto& event : events) {
            sawRename = sawRename ||
                (event.kind == WatchEventKind::Rename &&
                 event.path == std::filesystem::path{"sub/renamed.txt"} &&
                 event.previousPath ==
                     std::optional<std::filesystem::path>{"sub/file.txt"});
        }
        ASSERT_TRUE(sawRename);
    }
    std::filesystem::remove_all(root);
}

TEST(platformPollTimeoutIsFinite) {
    const auto root = uniqueTempDirectory();
    {
        auto watcher = ssg::makePlatformFilesystemWatcher(root, config());
        const auto before = std::chrono::steady_clock::now();
        (void)watcher->poll(20ms);
        const auto elapsed = std::chrono::steady_clock::now() - before;
        ASSERT_TRUE(elapsed < 500ms);
    }

    std::filesystem::remove_all(root);
}

bool pollMetadataUntilDirty(
    ssg::GitMetadataWatcher& watcher,
    std::chrono::milliseconds budget = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (watcher.poll(20ms)) return true;
    }
    return false;
}

TEST(platformGitMetadataWatcherIgnoresObjectsAndReportsGitStateChanges) {
    const auto root = uniqueTempDirectory();
    const auto metadata = root / "metadata";
    std::filesystem::create_directories(metadata / "refs" / "heads");
    std::ofstream{metadata / "HEAD"} << "ref: refs/heads/main\n";
    std::ofstream{metadata / "index"} << "index";
    {
        auto watcher = ssg::makePlatformGitMetadataWatcher({metadata});
        std::filesystem::create_directories(metadata / "objects");
        std::ofstream{metadata / "objects" / "pack"} << "object";
        ASSERT_FALSE(watcher->poll(50ms));
        std::ofstream{metadata / "refs" / "heads" / "main"} << "ref";
        ASSERT_TRUE(pollMetadataUntilDirty(*watcher));
    }
    std::filesystem::remove_all(root);
}

} // namespace

SSG_TEST_SUITE(ssg_watcher_tests) {
    RUN(createModifyDeleteScriptsAreDeterministic);
    RUN(renamePairingPreservesIdentityAndFinalPath);
    RUN(renameThenDeleteReportsTheOriginalPath);
    RUN(newSourceRenamedToExistingPathIsOneModify);
    RUN(newSourceRenamedToNewPathIsOneCreate);
    RUN(identityReuseAtAnotherPathKeepsBothEvents);
    RUN(samePathReplacementRequiresMatchingIdentityToCoalesce);
    RUN(samePathReplacementCoalescesFollowupsWithNewIdentity);
    RUN(unmatchedRenameHalvesBecomeBoundaryEvents);
    RUN(atomicRenameReplaceSaveIsNeverDroppedAndSurfacesAsModify);
    RUN(modifyThenGenuineMoveAwayStillReportsRemove);
    RUN(atomicRenameReplaceInOneClockTickStillSurfacesAsModify);
    RUN(debounceReleasesOnlyAfterTheWindow);
    RUN(exactSaveResultIsCorrelatedOnce);
    RUN(overflowRescansAndEmitsSyntheticChanges);
    RUN(partialRescanNeverPublishesPartialTruth);
    RUN(rescanUsesStableIdentityToRecoverRename);
    RUN(queueExhaustionCollapsesToOverflowAndRescan);
    RUN(renameQueueOverflowDoesNotInvalidateActivePair);
    RUN(expiringRenameQueueOverflowDoesNotInvalidateIteration);
    RUN(invalidBoundsAreRejectedAtConstruction);
    RUN(watchFileStateObservesFilesystemMetadata);
    RUN(platformAdapterReportsRecursiveNormalizedEvents);
    RUN(platformPollTimeoutIsFinite);
    RUN(platformGitMetadataWatcherIgnoresObjectsAndReportsGitStateChanges);
    std::cout << "Passed: " << passed << " Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
