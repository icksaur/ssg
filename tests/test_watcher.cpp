#include "ssg/watcher.h"
#include "test_helpers.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
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

const auto start = ssg::WatchTimePoint{};

WatchFileState state(std::uint64_t id, std::uint64_t size = 1,
                     std::int64_t modified = 1) {
    return {{1, {id, 0}}, size, modified};
}

NativeWatchEvent raw(NativeWatchAction action, std::string path,
                     std::optional<WatchFileState> observed = std::nullopt,
                     std::uint64_t rename_token = 0) {
    return {action, std::move(path), rename_token, observed};
}

WatcherConfig config(std::size_t max_events = 16) {
    WatcherConfig result;
    result.debounce = 10ms;
    result.rescan_retry = 100ms;
    result.max_queued_events = max_events;
    result.max_pending_renames = 4;
    result.max_save_expectations = 4;
    result.max_rescan_entries = 16;
    return result;
}

WorkspaceScan unchangedScan(std::size_t) {
    return {{}, true};
}

std::vector<WatchEvent> finish(WatchEventNormalizer& normalizer) {
    return normalizer.takeReady(start + 11ms);
}

TEST(createModifyDeleteScriptsAreDeterministic) {
    WatchEventNormalizer normalizer(config(), {}, unchangedScan);
    normalizer.push(raw(NativeWatchAction::Create, "new.txt", state(1)), start);
    normalizer.push(raw(NativeWatchAction::Modify, "new.txt", state(1, 2)), start + 1ms);

    auto events = finish(normalizer);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Create);
    ASSERT_EQ(events[0].path, std::filesystem::path{"new.txt"});
    ASSERT_EQ(events[0].size, std::optional<std::uint64_t>{2});

    WatchEventNormalizer vanished(config(), {}, unchangedScan);
    vanished.push(raw(NativeWatchAction::Create, "gone.txt", state(2)), start);
    vanished.push(raw(NativeWatchAction::Remove, "gone.txt"), start + 1ms);
    ASSERT_TRUE(finish(vanished).empty());

    WatchEventNormalizer deleted(
        config(), {WorkspaceEntry{"old.txt", state(3, 4)}}, unchangedScan);
    deleted.push(raw(NativeWatchAction::Modify, "old.txt", state(3, 5)), start);
    deleted.push(raw(NativeWatchAction::Remove, "old.txt"), start + 1ms);
    events = finish(deleted);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Remove);
    ASSERT_EQ(events[0].identity, std::optional<FileIdentity>{state(3).identity});
}

TEST(renamePairingPreservesIdentityAndFinalPath) {
    WatchEventNormalizer normalizer(
        config(), {WorkspaceEntry{"before.txt", state(7, 3)}}, unchangedScan);
    normalizer.push(raw(NativeWatchAction::RenameFrom, "before.txt",
                        std::nullopt, 42), start);
    normalizer.push(raw(NativeWatchAction::RenameTo, "after.txt",
                        state(7, 3), 42), start + 1ms);
    normalizer.push(raw(NativeWatchAction::Modify, "after.txt",
                        state(7, 8)), start + 2ms);

    const auto events = normalizer.takeReady(start + 13ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Rename);
    ASSERT_EQ(events[0].previous_path,
              std::optional<std::filesystem::path>{"before.txt"});
    ASSERT_EQ(events[0].path, std::filesystem::path{"after.txt"});
    ASSERT_EQ(events[0].identity, std::optional<FileIdentity>{state(7).identity});
    ASSERT_EQ(events[0].size, std::optional<std::uint64_t>{8});
}

TEST(renameThenDeleteReportsTheOriginalPath) {
    WatchEventNormalizer normalizer(
        config(), {WorkspaceEntry{"before.txt", state(8)}}, unchangedScan);
    normalizer.push(raw(NativeWatchAction::RenameFrom, "before.txt",
                        std::nullopt, 50), start);
    normalizer.push(raw(NativeWatchAction::RenameTo, "after.txt",
                        state(8), 50), start + 1ms);
    normalizer.push(
        raw(NativeWatchAction::Remove, "after.txt"), start + 2ms);
    const auto events = normalizer.takeReady(start + 13ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Remove);
    ASSERT_EQ(events[0].path, std::filesystem::path{"before.txt"});
    ASSERT_FALSE(events[0].previous_path.has_value());
}

TEST(newSourceRenamedToExistingPathIsOneModify) {
    WatchEventNormalizer normalizer(
        config(), {WorkspaceEntry{"target.txt", state(30, 1)}}, unchangedScan);
    normalizer.registerSave({"target.txt", state(31, 8, 4)});
    normalizer.push(
        raw(NativeWatchAction::Create, "temp.txt", state(31, 8, 4)), start);
    normalizer.push(raw(NativeWatchAction::RenameFrom, "temp.txt",
                        std::nullopt, 60), start + 1ms);
    normalizer.push(raw(NativeWatchAction::RenameTo, "target.txt",
                        state(31, 8, 4), 60), start + 2ms);

    const auto events = normalizer.takeReady(start + 13ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Modify);
    ASSERT_EQ(events[0].path, std::filesystem::path{"target.txt"});
    ASSERT_EQ(events[0].origin, WatchEventOrigin::SsgSave);
}

TEST(newSourceRenamedToNewPathIsOneCreate) {
    WatchEventNormalizer normalizer(config(), {}, unchangedScan);
    normalizer.push(
        raw(NativeWatchAction::Create, "temp.txt", state(32)), start);
    normalizer.push(raw(NativeWatchAction::RenameFrom, "temp.txt",
                        std::nullopt, 61), start + 1ms);
    normalizer.push(raw(NativeWatchAction::RenameTo, "target.txt",
                        state(32), 61), start + 2ms);

    const auto events = normalizer.takeReady(start + 13ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Create);
    ASSERT_EQ(events[0].path, std::filesystem::path{"target.txt"});
}

TEST(identityReuseAtAnotherPathKeepsBothEvents) {
    WatchEventNormalizer normalizer(
        config(), {WorkspaceEntry{"old.txt", state(12)}}, unchangedScan);
    normalizer.push(raw(NativeWatchAction::Remove, "old.txt"), start);
    normalizer.push(
        raw(NativeWatchAction::Create, "new.txt", state(12)), start + 1ms);
    const auto events = normalizer.takeReady(start + 12ms);
    ASSERT_EQ(events.size(), std::size_t{2});
    ASSERT_EQ(events[0].kind, WatchEventKind::Remove);
    ASSERT_EQ(events[1].kind, WatchEventKind::Create);
}

TEST(samePathReplacementRequiresMatchingIdentityToCoalesce) {
    WatchEventNormalizer replaced(
        config(), {WorkspaceEntry{"file.txt", state(40)}}, unchangedScan);
    replaced.push(raw(NativeWatchAction::Remove, "file.txt"), start);
    replaced.push(
        raw(NativeWatchAction::Create, "file.txt", state(41)), start + 1ms);
    auto events = replaced.takeReady(start + 12ms);
    ASSERT_EQ(events.size(), std::size_t{2});
    ASSERT_EQ(events[0].kind, WatchEventKind::Remove);
    ASSERT_EQ(events[0].identity,
              std::optional<FileIdentity>{state(40).identity});
    ASSERT_EQ(events[1].kind, WatchEventKind::Create);
    ASSERT_EQ(events[1].identity,
              std::optional<FileIdentity>{state(41).identity});

    WatchEventNormalizer recreated(
        config(), {WorkspaceEntry{"file.txt", state(42)}}, unchangedScan);
    recreated.push(raw(NativeWatchAction::Remove, "file.txt"), start);
    recreated.push(
        raw(NativeWatchAction::Create, "file.txt", state(42, 2)), start + 1ms);
    events = recreated.takeReady(start + 12ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Modify);
}

TEST(samePathReplacementCoalescesFollowupsWithNewIdentity) {
    WatchEventNormalizer modified(
        config(), {WorkspaceEntry{"file.txt", state(43)}}, unchangedScan);
    modified.push(raw(NativeWatchAction::Remove, "file.txt"), start);
    modified.push(
        raw(NativeWatchAction::Create, "file.txt", state(44, 1)), start + 1ms);
    modified.push(
        raw(NativeWatchAction::Modify, "file.txt", state(44, 3)), start + 2ms);
    auto events = modified.takeReady(start + 13ms);
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
    vanished.push(raw(NativeWatchAction::Remove, "file.txt"), start);
    vanished.push(
        raw(NativeWatchAction::Create, "file.txt", state(46)), start + 1ms);
    vanished.push(
        raw(NativeWatchAction::Remove, "file.txt"), start + 2ms);
    events = vanished.takeReady(start + 13ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Remove);
    ASSERT_EQ(events[0].identity,
              std::optional<FileIdentity>{state(45).identity});
}

TEST(unmatchedRenameHalvesBecomeBoundaryEvents) {
    WatchEventNormalizer moved_out(
        config(), {WorkspaceEntry{"out.txt", state(9)}}, unchangedScan);
    moved_out.push(raw(NativeWatchAction::RenameFrom, "out.txt",
                       std::nullopt, 10), start);
    auto events = finish(moved_out);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Remove);

    WatchEventNormalizer moved_in(config(), {}, unchangedScan);
    moved_in.push(raw(NativeWatchAction::RenameTo, "in.txt", state(10),
                      11), start);
    events = finish(moved_in);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Create);
}

TEST(debounceReleasesOnlyAfterTheWindow) {
    WatchEventNormalizer normalizer(config(), {}, unchangedScan);
    normalizer.push(raw(NativeWatchAction::Create, "later.txt", state(1)), start);
    ASSERT_TRUE(normalizer.takeReady(start + 9ms).empty());
    ASSERT_EQ(normalizer.takeReady(start + 10ms).size(), std::size_t{1});
}

TEST(exactSaveResultIsCorrelatedOnce) {
    WatchEventNormalizer normalizer(config(), {}, unchangedScan);
    const ssg::SaveExpectation saved{"saved.txt", state(20, 12, 99)};
    normalizer.registerSave(saved);
    normalizer.push(raw(NativeWatchAction::Modify, "saved.txt",
                        state(20, 12, 99)), start);
    auto first = normalizer.takeReady(start + 11ms);
    ASSERT_EQ(first.size(), std::size_t{1});
    ASSERT_EQ(first[0].origin, WatchEventOrigin::SsgSave);

    normalizer.push(raw(NativeWatchAction::Modify, "saved.txt",
                        state(20, 13, 100)), start + 20ms);
    auto second = normalizer.takeReady(start + 31ms);
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
    normalizer.push(raw(NativeWatchAction::Overflow, ""), start);

    const auto events = normalizer.takeReady(start);
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
    normalizer.push(raw(NativeWatchAction::Overflow, ""), start);
    auto events = normalizer.takeReady(start);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Overflow);
    normalizer.push(raw(NativeWatchAction::Overflow, ""), start + 1ms);
    ASSERT_TRUE(normalizer.takeReady(start + 99ms).empty());
    ASSERT_EQ(scans, 1);
    events = normalizer.takeReady(start + 100ms);
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
    normalizer.push(raw(NativeWatchAction::Overflow, ""), start);
    const auto events = normalizer.takeReady(start);
    ASSERT_EQ(events.size(), std::size_t{2});
    ASSERT_EQ(events[1].kind, WatchEventKind::Rename);
    ASSERT_EQ(events[1].path, std::filesystem::path{"after.txt"});
    ASSERT_EQ(events[1].previous_path,
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
    normalizer.push(raw(NativeWatchAction::Create, "a", state(1)), start);
    normalizer.push(raw(NativeWatchAction::Create, "b", state(2)), start);
    normalizer.push(raw(NativeWatchAction::Create, "c", state(3)), start);

    const auto events = normalizer.takeReady(start);
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
        raw(NativeWatchAction::Create, "occupied.txt", state(50)), start);
    normalizer.push(raw(NativeWatchAction::RenameFrom, "before.txt",
                        std::nullopt, 70), start + 1ms);
    normalizer.push(raw(NativeWatchAction::RenameTo, "after.txt",
                        state(51), 70), start + 2ms);

    const auto events = normalizer.takeReady(start + 2ms);
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
        raw(NativeWatchAction::Create, "occupied.txt", state(52)), start);
    normalizer.push(raw(NativeWatchAction::RenameFrom, "before.txt",
                        std::nullopt, 71), start);

    const auto events = normalizer.takeReady(start + 10ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::Overflow);
}

TEST(invalidBoundsAreRejectedAtConstruction) {
    auto invalid = config();
    invalid.max_queued_events = 0;
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
        bool saw_file = false;
        for (const auto& event : events) {
            saw_file = saw_file ||
                (event.kind == WatchEventKind::Create &&
                 event.path == std::filesystem::path{"sub/file.txt"});
        }
        ASSERT_TRUE(saw_file);

        std::filesystem::rename(root / "sub" / "file.txt",
                                root / "sub" / "renamed.txt");
        events = pollUntil(*watcher, WatchEventKind::Rename);
        bool saw_rename = false;
        for (const auto& event : events) {
            saw_rename = saw_rename ||
                (event.kind == WatchEventKind::Rename &&
                 event.path == std::filesystem::path{"sub/renamed.txt"} &&
                 event.previous_path ==
                     std::optional<std::filesystem::path>{"sub/file.txt"});
        }
        ASSERT_TRUE(saw_rename);
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

} // namespace

int main() {
    RUN(createModifyDeleteScriptsAreDeterministic);
    RUN(renamePairingPreservesIdentityAndFinalPath);
    RUN(renameThenDeleteReportsTheOriginalPath);
    RUN(newSourceRenamedToExistingPathIsOneModify);
    RUN(newSourceRenamedToNewPathIsOneCreate);
    RUN(identityReuseAtAnotherPathKeepsBothEvents);
    RUN(samePathReplacementRequiresMatchingIdentityToCoalesce);
    RUN(samePathReplacementCoalescesFollowupsWithNewIdentity);
    RUN(unmatchedRenameHalvesBecomeBoundaryEvents);
    RUN(debounceReleasesOnlyAfterTheWindow);
    RUN(exactSaveResultIsCorrelatedOnce);
    RUN(overflowRescansAndEmitsSyntheticChanges);
    RUN(partialRescanNeverPublishesPartialTruth);
    RUN(rescanUsesStableIdentityToRecoverRename);
    RUN(queueExhaustionCollapsesToOverflowAndRescan);
    RUN(renameQueueOverflowDoesNotInvalidateActivePair);
    RUN(expiringRenameQueueOverflowDoesNotInvalidateIteration);
    RUN(invalidBoundsAreRejectedAtConstruction);
    RUN(platformAdapterReportsRecursiveNormalizedEvents);
    RUN(platformPollTimeoutIsFinite);
    std::cout << "Passed: " << passed << " Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
