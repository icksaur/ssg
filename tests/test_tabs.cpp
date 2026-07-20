#include "test_helpers.h"

#include <ssg/tabs.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

class FakeLifecycle final : public ssg::TabLifecycle {
public:
    std::vector<ssg::TabId> fail_close;
    bool fail_reopen = false;
    int close_calls = 0;
    int reopen_calls = 0;

    ssg::TabLifecycleResult close(
        const ssg::TabState& tab,
        std::chrono::milliseconds durability_timeout) override {
        ++close_calls;
        if (durability_timeout <= 0ms) {
            return {ssg::TabError::DurabilityFailed, "invalid timeout"};
        }
        if (std::find(fail_close.begin(), fail_close.end(), tab.id) !=
            fail_close.end()) {
            return {ssg::TabError::DurabilityFailed, "durability failed"};
        }
        return {ssg::TabError::None, {},
                ssg::RecoveryRecordId{"closed-" +
                                      std::to_string(tab.id.value())},
                tab.dirty};
    }

    ssg::TabLifecycleResult reopen(
        const ssg::TabState&,
        const ssg::RecoveryRecordId&) override {
        ++reopen_calls;
        if (fail_reopen) {
            return {ssg::TabError::LifecycleFailed, "restore failed"};
        }
        return {};
    }
};

ssg::JournalDocumentKey saved(std::string_view path) {
    return ssg::JournalDocumentKey::saved(path);
}

ssg::TabId open_saved(ssg::TabManager& tabs, std::uint64_t document,
                      std::string_view path, bool dirty = false) {
    return *tabs
                .open_document(ssg::FileDocumentId{document}, saved(path), path,
                               ssg::DocumentMode::Edit, dirty)
                .tab;
}

TEST(command_set_exactly_owns_nine_tab_commands) {
    const auto descriptors = ssg::tab_management_command_set().descriptors();
    const std::array<std::string_view, 9> expected{
        "tab.close",       "tab.close_others", "tab.close_all",
        "tab.reopen_closed", "tab.next",         "tab.previous",
        "tab.activate",    "tab.move_left",     "tab.move_right",
    };
    for (std::size_t index = 0; index < expected.size(); ++index) {
        ASSERT_EQ(descriptors[index].id, expected[index]);
    }
}

TEST(duplicate_document_identity_activates_existing_tab) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    const auto first = open_saved(tabs, 1, "src/a.cpp");
    (void)open_saved(tabs, 2, "src/b.cpp");
    const auto duplicate = tabs.open_document(
        ssg::FileDocumentId{99}, saved("src/a.cpp"), "other",
        ssg::DocumentMode::ReadOnly, true, ssg::TabRecoveryBadge::Failed);

    ASSERT_TRUE(duplicate.accepted());
    ASSERT_EQ(duplicate.tab, std::optional{first});
    ASSERT_EQ(tabs.view_state().tabs.size(), std::size_t{2});
    ASSERT_EQ(tabs.view_state().active, std::optional{first});
}

TEST(all_tab_kinds_navigate_cyclically_and_reorder) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    const auto document = open_saved(tabs, 1, "a");
    const auto diff =
        *tabs.open_content(ssg::TabKind::LiveDiff, "diff:a", "Diff",
                           ssg::DocumentMode::Diff)
             .tab;
    const auto output =
        *tabs.open_content(ssg::TabKind::ReadOnlyOutput, "output:1", "Output",
                           ssg::DocumentMode::ReadOnly)
             .tab;
    (void)tabs.open_content(ssg::TabKind::SearchResults, "search:x", "Search",
                            ssg::DocumentMode::ReadOnly);
    (void)tabs.open_content(ssg::TabKind::TreeView, "tree:files", "Files",
                            ssg::DocumentMode::ReadOnly);

    ASSERT_TRUE(tabs.activate(document).accepted());
    ASSERT_EQ(tabs.previous().tab,
              std::optional{tabs.view_state().tabs.back().id});
    ASSERT_EQ(tabs.next().tab, std::optional{document});
    ASSERT_TRUE(tabs.move_left(output).accepted());
    ASSERT_EQ(tabs.view_state().tabs[1].id, output);
    ASSERT_TRUE(tabs.move_right(output).accepted());
    ASSERT_EQ(tabs.view_state().tabs[2].id, output);
    ASSERT_EQ(tabs.view_state().tabs[1].id, diff);
}

TEST(active_close_prefers_right_then_left_and_dirty_failure_is_atomic) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    const auto a = open_saved(tabs, 1, "a");
    const auto b = open_saved(tabs, 2, "b", true);
    const auto c = open_saved(tabs, 3, "c");
    (void)tabs.activate(b);
    lifecycle.fail_close.push_back(b);
    const auto before = tabs.view_state();

    const auto close_failure = tabs.close(b, 100ms);
    ASSERT_EQ(close_failure.error, ssg::TabError::DurabilityFailed);
    ASSERT_EQ(tabs.view_state(), before);
    ASSERT_EQ(tabs.recently_closed_count(), std::size_t{0});

    lifecycle.fail_close.clear();
    ASSERT_TRUE(tabs.close(b, 100ms).accepted());
    ASSERT_EQ(tabs.view_state().active, std::optional{c});
    ASSERT_TRUE(tabs.close(c, 100ms).accepted());
    ASSERT_EQ(tabs.view_state().active, std::optional{a});
}

TEST(batch_close_is_left_to_right_best_effort) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    const auto a = open_saved(tabs, 1, "a");
    const auto b = open_saved(tabs, 2, "b", true);
    const auto c = open_saved(tabs, 3, "c");
    const auto d = open_saved(tabs, 4, "d", true);
    (void)tabs.activate(c);
    lifecycle.fail_close = {b, d};

    const auto result = tabs.close_others(a, 100ms);
    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(result.failures.size(), std::size_t{2});
    ASSERT_EQ(tabs.view_state().tabs.size(), std::size_t{3});
    ASSERT_EQ(tabs.view_state().tabs[0].id, a);
    ASSERT_EQ(tabs.view_state().tabs[1].id, b);
    ASSERT_EQ(tabs.view_state().tabs[2].id, d);
    ASSERT_EQ(tabs.view_state().active, std::optional{d});
}

TEST(reopen_is_lifo_retryable_and_restores_position_and_activation) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    const auto a = open_saved(tabs, 1, "a");
    const auto b = open_saved(tabs, 2, "b");
    const auto c = open_saved(tabs, 3, "c");
    (void)tabs.close(b, 100ms);
    (void)tabs.close(c, 100ms);

    lifecycle.fail_reopen = true;
    ASSERT_EQ(tabs.reopen_closed().error, ssg::TabError::LifecycleFailed);
    ASSERT_EQ(tabs.recently_closed_count(), std::size_t{2});

    lifecycle.fail_reopen = false;
    ASSERT_EQ(tabs.reopen_closed().tab, std::optional{c});
    ASSERT_EQ(tabs.view_state().active, std::optional{c});
    ASSERT_EQ(tabs.reopen_closed().tab, std::optional{b});
    ASSERT_EQ(tabs.view_state().tabs[0].id, a);
    ASSERT_EQ(tabs.view_state().tabs[1].id, b);
    ASSERT_EQ(tabs.view_state().tabs[2].id, c);
    ASSERT_EQ(tabs.view_state().active, std::optional{b});
}

TEST(recently_closed_evicts_oldest_at_configured_bound) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle, {.maximum_recently_closed = 2}};
    const auto a = open_saved(tabs, 1, "a");
    const auto b = open_saved(tabs, 2, "b");
    const auto c = open_saved(tabs, 3, "c");
    (void)tabs.close(a, 100ms);
    (void)tabs.close(b, 100ms);
    (void)tabs.close(c, 100ms);

    ASSERT_EQ(tabs.recently_closed_count(), std::size_t{2});
    ASSERT_EQ(tabs.reopen_closed().tab, std::optional{c});
    ASSERT_EQ(tabs.reopen_closed().tab, std::optional{b});
    ASSERT_EQ(tabs.reopen_closed().error, ssg::TabError::NoRecentlyClosed);
}

TEST(reopen_activates_an_identity_already_opened_by_another_path) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    const auto original = open_saved(tabs, 1, "a");
    (void)tabs.close(original, 100ms);
    const auto replacement = open_saved(tabs, 2, "a");

    ASSERT_EQ(tabs.reopen_closed().tab, std::optional{replacement});
    ASSERT_EQ(tabs.view_state().tabs.size(), std::size_t{1});
    ASSERT_EQ(tabs.view_state().active, std::optional{replacement});
    ASSERT_EQ(tabs.recently_closed_count(), std::size_t{0});
    ASSERT_EQ(lifecycle.reopen_calls, 0);
    const auto delta = ssg::derive_tab_delta({}, tabs.view_state());
    ASSERT_TRUE(ssg::replay_tab_delta({}, delta).accepted());
}

TEST(untitled_labels_are_smallest_available_and_reopen_is_stable) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    const auto first = tabs.open_document(
        ssg::FileDocumentId{1},
        ssg::JournalDocumentKey::untitled(ssg::UntitledDocumentId::generate()),
        "", ssg::DocumentMode::Edit, true);
    const auto second = tabs.open_document(
        ssg::FileDocumentId{2},
        ssg::JournalDocumentKey::untitled(ssg::UntitledDocumentId::generate()),
        "", ssg::DocumentMode::Edit, true);
    ASSERT_EQ(tabs.view_state().tabs[0].label, std::string{"Untitled 1"});
    ASSERT_EQ(tabs.view_state().tabs[1].label, std::string{"Untitled 2"});

    (void)tabs.close(*first.tab, 100ms);
    const auto third = tabs.open_document(
        ssg::FileDocumentId{3},
        ssg::JournalDocumentKey::untitled(ssg::UntitledDocumentId::generate()),
        "", ssg::DocumentMode::Edit, true);
    ASSERT_EQ(tabs.view_state().tabs.back().label, std::string{"Untitled 1"});
    (void)tabs.reopen_closed();
    const auto reopened = std::find_if(
        tabs.view_state().tabs.begin(), tabs.view_state().tabs.end(),
        [id = *first.tab](const auto& tab) { return tab.id == id; });
    ASSERT_TRUE(reopened != tabs.view_state().tabs.end());
    ASSERT_EQ(reopened->label, std::string{"Untitled 1"});
    ASSERT_NE(third.tab, second.tab);
}

TEST(badges_update_and_delta_replay_is_exact) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    const auto base = tabs.view_state();
    (void)open_saved(tabs, 7, "a");
    ASSERT_TRUE(tabs.update_document(
                        ssg::FileDocumentId{7},
                        ssg::DocumentMode::ReadOnly, true,
                        ssg::TabRecoveryBadge::Pending)
                    .accepted());
    const auto target = tabs.view_state();
    const auto delta = ssg::derive_tab_delta(base, target);
    const auto replay = ssg::replay_tab_delta(base, delta);
    ASSERT_TRUE(replay.accepted());
    ASSERT_EQ(replay.state, std::optional{target});
    ASSERT_EQ(target.tabs[0].mode, ssg::DocumentMode::ReadOnly);
    ASSERT_TRUE(target.tabs[0].dirty);
    ASSERT_EQ(target.tabs[0].recovery, ssg::TabRecoveryBadge::Pending);
    ASSERT_FALSE(ssg::derive_tab_delta(target, target).state.has_value());
}

}  // namespace

int main() {
    RUN(command_set_exactly_owns_nine_tab_commands);
    RUN(duplicate_document_identity_activates_existing_tab);
    RUN(all_tab_kinds_navigate_cyclically_and_reorder);
    RUN(active_close_prefers_right_then_left_and_dirty_failure_is_atomic);
    RUN(batch_close_is_left_to_right_best_effort);
    RUN(reopen_is_lifo_retryable_and_restores_position_and_activation);
    RUN(recently_closed_evicts_oldest_at_configured_bound);
    RUN(reopen_activates_an_identity_already_opened_by_another_path);
    RUN(untitled_labels_are_smallest_available_and_reopen_is_stable);
    RUN(badges_update_and_delta_replay_is_exact);
    std::cout << "\nPassed: " << passed << " Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
