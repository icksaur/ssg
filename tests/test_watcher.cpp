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

WorkspaceScan unchanged_scan(std::size_t) {
    return {{}, true};
}

std::vector<WatchEvent> finish(WatchEventNormalizer& normalizer) {
    return normalizer.take_ready(start + 11ms);
}

TEST(create_modify_delete_scripts_are_deterministic) {
    WatchEventNormalizer normalizer(config(), {}, unchanged_scan);
    normalizer.push(raw(NativeWatchAction::create, "new.txt", state(1)), start);
    normalizer.push(raw(NativeWatchAction::modify, "new.txt", state(1, 2)), start + 1ms);

    auto events = finish(normalizer);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::create);
    ASSERT_EQ(events[0].path, std::filesystem::path{"new.txt"});
    ASSERT_EQ(events[0].size, std::optional<std::uint64_t>{2});

    WatchEventNormalizer vanished(config(), {}, unchanged_scan);
    vanished.push(raw(NativeWatchAction::create, "gone.txt", state(2)), start);
    vanished.push(raw(NativeWatchAction::remove, "gone.txt"), start + 1ms);
    ASSERT_TRUE(finish(vanished).empty());

    WatchEventNormalizer deleted(
        config(), {WorkspaceEntry{"old.txt", state(3, 4)}}, unchanged_scan);
    deleted.push(raw(NativeWatchAction::modify, "old.txt", state(3, 5)), start);
    deleted.push(raw(NativeWatchAction::remove, "old.txt"), start + 1ms);
    events = finish(deleted);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::remove);
    ASSERT_EQ(events[0].identity, std::optional<FileIdentity>{state(3).identity});
}

TEST(rename_pairing_preserves_identity_and_final_path) {
    WatchEventNormalizer normalizer(
        config(), {WorkspaceEntry{"before.txt", state(7, 3)}}, unchanged_scan);
    normalizer.push(raw(NativeWatchAction::rename_from, "before.txt",
                        std::nullopt, 42), start);
    normalizer.push(raw(NativeWatchAction::rename_to, "after.txt",
                        state(7, 3), 42), start + 1ms);
    normalizer.push(raw(NativeWatchAction::modify, "after.txt",
                        state(7, 8)), start + 2ms);

    const auto events = normalizer.take_ready(start + 13ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::rename);
    ASSERT_EQ(events[0].previous_path,
              std::optional<std::filesystem::path>{"before.txt"});
    ASSERT_EQ(events[0].path, std::filesystem::path{"after.txt"});
    ASSERT_EQ(events[0].identity, std::optional<FileIdentity>{state(7).identity});
    ASSERT_EQ(events[0].size, std::optional<std::uint64_t>{8});
}

TEST(rename_then_delete_reports_the_original_path) {
    WatchEventNormalizer normalizer(
        config(), {WorkspaceEntry{"before.txt", state(8)}}, unchanged_scan);
    normalizer.push(raw(NativeWatchAction::rename_from, "before.txt",
                        std::nullopt, 50), start);
    normalizer.push(raw(NativeWatchAction::rename_to, "after.txt",
                        state(8), 50), start + 1ms);
    normalizer.push(
        raw(NativeWatchAction::remove, "after.txt"), start + 2ms);
    const auto events = normalizer.take_ready(start + 13ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::remove);
    ASSERT_EQ(events[0].path, std::filesystem::path{"before.txt"});
    ASSERT_FALSE(events[0].previous_path.has_value());
}

TEST(identity_reuse_at_another_path_keeps_both_events) {
    WatchEventNormalizer normalizer(
        config(), {WorkspaceEntry{"old.txt", state(12)}}, unchanged_scan);
    normalizer.push(raw(NativeWatchAction::remove, "old.txt"), start);
    normalizer.push(
        raw(NativeWatchAction::create, "new.txt", state(12)), start + 1ms);
    const auto events = normalizer.take_ready(start + 12ms);
    ASSERT_EQ(events.size(), std::size_t{2});
    ASSERT_EQ(events[0].kind, WatchEventKind::remove);
    ASSERT_EQ(events[1].kind, WatchEventKind::create);
}

TEST(unmatched_rename_halves_become_boundary_events) {
    WatchEventNormalizer moved_out(
        config(), {WorkspaceEntry{"out.txt", state(9)}}, unchanged_scan);
    moved_out.push(raw(NativeWatchAction::rename_from, "out.txt",
                       std::nullopt, 10), start);
    auto events = finish(moved_out);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::remove);

    WatchEventNormalizer moved_in(config(), {}, unchanged_scan);
    moved_in.push(raw(NativeWatchAction::rename_to, "in.txt", state(10),
                      11), start);
    events = finish(moved_in);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::create);
}

TEST(debounce_releases_only_after_the_window) {
    WatchEventNormalizer normalizer(config(), {}, unchanged_scan);
    normalizer.push(raw(NativeWatchAction::create, "later.txt", state(1)), start);
    ASSERT_TRUE(normalizer.take_ready(start + 9ms).empty());
    ASSERT_EQ(normalizer.take_ready(start + 10ms).size(), std::size_t{1});
}

TEST(exact_save_result_is_correlated_once) {
    WatchEventNormalizer normalizer(config(), {}, unchanged_scan);
    const ssg::SaveExpectation saved{"saved.txt", state(20, 12, 99)};
    normalizer.register_save(saved);
    normalizer.push(raw(NativeWatchAction::modify, "saved.txt",
                        state(20, 12, 99)), start);
    auto first = normalizer.take_ready(start + 11ms);
    ASSERT_EQ(first.size(), std::size_t{1});
    ASSERT_EQ(first[0].origin, WatchEventOrigin::ssg_save);

    normalizer.push(raw(NativeWatchAction::modify, "saved.txt",
                        state(20, 13, 100)), start + 20ms);
    auto second = normalizer.take_ready(start + 31ms);
    ASSERT_EQ(second.size(), std::size_t{1});
    ASSERT_EQ(second[0].origin, WatchEventOrigin::external);
}

TEST(overflow_rescans_and_emits_synthetic_changes) {
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
    normalizer.push(raw(NativeWatchAction::overflow, ""), start);

    const auto events = normalizer.take_ready(start);
    ASSERT_EQ(events.size(), std::size_t{4});
    ASSERT_EQ(events[0].kind, WatchEventKind::overflow);
    ASSERT_EQ(events[1].kind, WatchEventKind::modify);
    ASSERT_EQ(events[2].kind, WatchEventKind::create);
    ASSERT_EQ(events[3].kind, WatchEventKind::remove);
}

TEST(partial_rescan_never_publishes_partial_truth) {
    int scans = 0;
    const auto partial = [&scans](std::size_t) {
        ++scans;
        if (scans == 1) {
            return WorkspaceScan{{{"partial.txt", state(4)}}, false};
        }
        return WorkspaceScan{{{"complete.txt", state(5)}}, true};
    };
    WatchEventNormalizer normalizer(config(), {}, partial);
    normalizer.push(raw(NativeWatchAction::overflow, ""), start);
    auto events = normalizer.take_ready(start);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::overflow);
    ASSERT_TRUE(normalizer.take_ready(start + 99ms).empty());
    ASSERT_EQ(scans, 1);
    events = normalizer.take_ready(start + 100ms);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::create);
    ASSERT_EQ(scans, 2);
}

TEST(rescan_uses_stable_identity_to_recover_rename) {
    const auto scan = [](std::size_t) {
        return WorkspaceScan{{{"after.txt", state(15, 2)}}, true};
    };
    WatchEventNormalizer normalizer(
        config(), {WorkspaceEntry{"before.txt", state(15, 2)}}, scan);
    normalizer.push(raw(NativeWatchAction::overflow, ""), start);
    const auto events = normalizer.take_ready(start);
    ASSERT_EQ(events.size(), std::size_t{2});
    ASSERT_EQ(events[1].kind, WatchEventKind::rename);
    ASSERT_EQ(events[1].path, std::filesystem::path{"after.txt"});
    ASSERT_EQ(events[1].previous_path,
              std::optional<std::filesystem::path>{"before.txt"});
}

TEST(queue_exhaustion_collapses_to_overflow_and_rescan) {
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
    normalizer.push(raw(NativeWatchAction::create, "a", state(1)), start);
    normalizer.push(raw(NativeWatchAction::create, "b", state(2)), start);
    normalizer.push(raw(NativeWatchAction::create, "c", state(3)), start);

    const auto events = normalizer.take_ready(start);
    ASSERT_EQ(scans, 1);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].kind, WatchEventKind::overflow);
}

TEST(invalid_bounds_are_rejected_at_construction) {
    auto invalid = config();
    invalid.max_queued_events = 0;
    ASSERT_THROWS(WatchEventNormalizer(invalid, {}, unchanged_scan),
                  std::invalid_argument);
}

std::filesystem::path unique_temp_directory() {
    const auto path = std::filesystem::temp_directory_path() /
        ("ssg-watcher-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch().count()));
    std::filesystem::create_directory(path);
    return path;
}

std::vector<WatchEvent> poll_until(
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

TEST(platform_adapter_reports_recursive_normalized_events) {
    const auto root = unique_temp_directory();
    {
        auto watcher = ssg::make_platform_filesystem_watcher(root, config());
        std::filesystem::create_directory(root / "sub");
        std::ofstream(root / "sub" / "file.txt") << "one";

        auto events = poll_until(*watcher, WatchEventKind::create);
        bool saw_file = false;
        for (const auto& event : events) {
            saw_file = saw_file ||
                (event.kind == WatchEventKind::create &&
                 event.path == std::filesystem::path{"sub/file.txt"});
        }
        ASSERT_TRUE(saw_file);

        std::filesystem::rename(root / "sub" / "file.txt",
                                root / "sub" / "renamed.txt");
        events = poll_until(*watcher, WatchEventKind::rename);
        bool saw_rename = false;
        for (const auto& event : events) {
            saw_rename = saw_rename ||
                (event.kind == WatchEventKind::rename &&
                 event.path == std::filesystem::path{"sub/renamed.txt"} &&
                 event.previous_path ==
                     std::optional<std::filesystem::path>{"sub/file.txt"});
        }
        ASSERT_TRUE(saw_rename);
    }
    std::filesystem::remove_all(root);
}

TEST(platform_poll_timeout_is_finite) {
    const auto root = unique_temp_directory();
    {
        auto watcher = ssg::make_platform_filesystem_watcher(root, config());
        const auto before = std::chrono::steady_clock::now();
        (void)watcher->poll(20ms);
        const auto elapsed = std::chrono::steady_clock::now() - before;
        ASSERT_TRUE(elapsed < 500ms);
    }
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    RUN(create_modify_delete_scripts_are_deterministic);
    RUN(rename_pairing_preserves_identity_and_final_path);
    RUN(rename_then_delete_reports_the_original_path);
    RUN(identity_reuse_at_another_path_keeps_both_events);
    RUN(unmatched_rename_halves_become_boundary_events);
    RUN(debounce_releases_only_after_the_window);
    RUN(exact_save_result_is_correlated_once);
    RUN(overflow_rescans_and_emits_synthetic_changes);
    RUN(partial_rescan_never_publishes_partial_truth);
    RUN(rescan_uses_stable_identity_to_recover_rename);
    RUN(queue_exhaustion_collapses_to_overflow_and_rescan);
    RUN(invalid_bounds_are_rejected_at_construction);
    RUN(platform_adapter_reports_recursive_normalized_events);
    RUN(platform_poll_timeout_is_finite);
    std::cout << "Passed: " << passed << " Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
