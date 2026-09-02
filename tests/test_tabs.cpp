#include "test_helpers.h"

#include <ssg/TabManager.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

class FakeLifecycle final : public ssg::TabLifecycle {
public:
    std::vector<ssg::TabId> failClose;
    std::vector<ssg::TabId> ephemeralClose;
    std::vector<ssg::TabId> noCompensation;
    bool failReopen = false;
    std::optional<ssg::FileDocumentId> reopenedDocument;
    std::optional<ssg::JournalDocumentKey> reopenedDocumentKey;
    int closeCalls = 0;
    int reopenCalls = 0;

    ssg::TabLifecycleResult close(
        const ssg::TabState& tab,
        std::chrono::milliseconds durabilityTimeout) override {
        ++closeCalls;
        if (durabilityTimeout <= 0ms) {
            return {ssg::TabError::DurabilityFailed, "invalid timeout",
                    std::nullopt, std::nullopt, std::nullopt, false};
        }
        if (std::find(failClose.begin(), failClose.end(), tab.id) !=
            failClose.end()) {
            return {ssg::TabError::DurabilityFailed, "durability failed",
                    std::nullopt, std::nullopt, std::nullopt, false};
        }
        if (std::find(ephemeralClose.begin(), ephemeralClose.end(), tab.id) !=
            ephemeralClose.end()) {
            return {ssg::TabError::None, {}, std::nullopt, std::nullopt,
                    std::nullopt, false, true};
        }
        if (std::find(noCompensation.begin(), noCompensation.end(), tab.id) !=
            noCompensation.end()) {
            return {ssg::TabError::None, {}, std::nullopt, std::nullopt,
                    std::nullopt, false, false};
        }
        return {ssg::TabError::None, {},
                ssg::RecoveryRecordId{"closed-" +
                                      std::to_string(tab.id.value())},
                std::nullopt, std::nullopt,
                tab.dirty};
    }

    ssg::TabLifecycleResult reopen(
        const ssg::TabState&,
        const ssg::RecoveryRecordId&) override {
        ++reopenCalls;
        if (failReopen) {
            return {ssg::TabError::LifecycleFailed, "restore failed",
                    std::nullopt, std::nullopt, std::nullopt, false};
        }
        return {ssg::TabError::None, {}, std::nullopt, reopenedDocument,
                reopenedDocumentKey, true};
    }
};

ssg::JournalDocumentKey saved(std::string_view path) {
    return ssg::JournalDocumentKey::saved(path);
}

ssg::TabId openSaved(ssg::TabManager& tabs, std::uint64_t document,
                      std::string_view path, bool dirty = false) {
    return *tabs
                .openDocument(ssg::FileDocumentId{document}, saved(path), path,
                               ssg::DocumentMode::Edit, dirty)
                .tab;
}

TEST(commandSetExactlyOwnsNineTabCommands) {
    const auto descriptors = ssg::tabManagementCommandSet().descriptors();
    const std::array<std::string_view, 9> expected{
        "tab.close",       "tab.close_others", "tab.close_all",
        "tab.reopen_closed", "tab.next",         "tab.previous",
        "tab.activate",    "tab.move_left",     "tab.move_right",
    };
    for (std::size_t index = 0; index < expected.size(); ++index) {
        ASSERT_EQ(descriptors[index].id, expected[index]);
    }
}

TEST(duplicateDocumentIdentityActivatesExistingTab) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    const auto first = openSaved(tabs, 1, "src/a.cpp");
    (void)openSaved(tabs, 2, "src/b.cpp");
    const auto duplicate = tabs.openDocument(
        ssg::FileDocumentId{99}, saved("src/a.cpp"), "other",
        ssg::DocumentMode::ReadOnly, true, ssg::TabRecoveryBadge::Failed);

    ASSERT_TRUE(duplicate.accepted());
    ASSERT_EQ(duplicate.tab, std::optional{first});
    ASSERT_EQ(tabs.viewState().tabs.size(), std::size_t{2});
    ASSERT_EQ(tabs.viewState().active, std::optional{first});
}

TEST(allTabKindsNavigateCyclicallyAndReorder) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    const auto document = openSaved(tabs, 1, "a");
    const auto diff =
        *tabs.openContent(ssg::TabKind::LiveDiff, "diff:a", "Diff",
                           ssg::DocumentMode::Diff)
             .tab;
    const auto output =
        *tabs.openContent(ssg::TabKind::ReadOnlyOutput, "output:1", "Output",
                           ssg::DocumentMode::ReadOnly)
             .tab;
    (void)tabs.openContent(ssg::TabKind::SearchResults, "search:x", "Search",
                            ssg::DocumentMode::ReadOnly);
    (void)tabs.openContent(ssg::TabKind::TreeView, "tree:files", "Files",
                            ssg::DocumentMode::ReadOnly);

    ASSERT_TRUE(tabs.activate(document).accepted());
    ASSERT_EQ(tabs.previous().tab,
              std::optional{tabs.viewState().tabs.back().id});
    ASSERT_EQ(tabs.next().tab, std::optional{document});
    ASSERT_TRUE(tabs.moveLeft(output).accepted());
    ASSERT_EQ(tabs.viewState().tabs[1].id, output);
    ASSERT_TRUE(tabs.moveRight(output).accepted());
    ASSERT_EQ(tabs.viewState().tabs[2].id, output);
    ASSERT_EQ(tabs.viewState().tabs[1].id, diff);
}

TEST(activeClosePrefersRightThenLeftAndDirtyFailureIsAtomic) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    const auto a = openSaved(tabs, 1, "a");
    const auto b = openSaved(tabs, 2, "b", true);
    const auto c = openSaved(tabs, 3, "c");
    (void)tabs.activate(b);
    lifecycle.failClose.push_back(b);
    const auto before = tabs.viewState();

    const auto closeFailure = tabs.close(b, 100ms);
    ASSERT_EQ(closeFailure.error, ssg::TabError::DurabilityFailed);
    ASSERT_EQ(tabs.viewState(), before);
    ASSERT_EQ(tabs.recentlyClosedCount(), std::size_t{0});

    lifecycle.failClose.clear();
    ASSERT_TRUE(tabs.close(b, 100ms).accepted());
    ASSERT_EQ(tabs.viewState().active, std::optional{c});
    ASSERT_TRUE(tabs.close(c, 100ms).accepted());
    ASSERT_EQ(tabs.viewState().active, std::optional{a});
}

TEST(batchCloseIsLeftToRightBestEffort) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    const auto a = openSaved(tabs, 1, "a");
    const auto b = openSaved(tabs, 2, "b", true);
    const auto c = openSaved(tabs, 3, "c");
    const auto d = openSaved(tabs, 4, "d", true);
    (void)tabs.activate(c);
    lifecycle.failClose = {b, d};

    const auto result = tabs.closeOthers(a, 100ms);
    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(result.failures.size(), std::size_t{2});
    ASSERT_EQ(tabs.viewState().tabs.size(), std::size_t{3});
    ASSERT_EQ(tabs.viewState().tabs[0].id, a);
    ASSERT_EQ(tabs.viewState().tabs[1].id, b);
    ASSERT_EQ(tabs.viewState().tabs[2].id, d);
    ASSERT_EQ(tabs.viewState().active, std::optional{d});
}

TEST(reopenIsLifoRetryableAndRestoresPositionAndActivation) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    const auto a = openSaved(tabs, 1, "a");
    const auto b = openSaved(tabs, 2, "b");
    const auto c = openSaved(tabs, 3, "c");
    (void)tabs.close(b, 100ms);
    (void)tabs.close(c, 100ms);

    lifecycle.failReopen = true;
    ASSERT_EQ(tabs.reopenClosed().error, ssg::TabError::LifecycleFailed);
    ASSERT_EQ(tabs.recentlyClosedCount(), std::size_t{2});

    lifecycle.failReopen = false;
    ASSERT_EQ(tabs.reopenClosed().tab, std::optional{c});
    ASSERT_EQ(tabs.viewState().active, std::optional{c});
    ASSERT_EQ(tabs.reopenClosed().tab, std::optional{b});
    ASSERT_EQ(tabs.viewState().tabs[0].id, a);
    ASSERT_EQ(tabs.viewState().tabs[1].id, b);
    ASSERT_EQ(tabs.viewState().tabs[2].id, c);
    ASSERT_EQ(tabs.viewState().active, std::optional{b});
}

TEST(recentlyClosedEvictsOldestAtConfiguredBound) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle, {.maximumRecentlyClosed = 2}};
    const auto a = openSaved(tabs, 1, "a");
    const auto b = openSaved(tabs, 2, "b");
    const auto c = openSaved(tabs, 3, "c");
    (void)tabs.close(a, 100ms);
    (void)tabs.close(b, 100ms);
    (void)tabs.close(c, 100ms);

    ASSERT_EQ(tabs.recentlyClosedCount(), std::size_t{2});
    ASSERT_EQ(tabs.reopenClosed().tab, std::optional{c});
    ASSERT_EQ(tabs.reopenClosed().tab, std::optional{b});
    ASSERT_EQ(tabs.reopenClosed().error, ssg::TabError::NoRecentlyClosed);
}

TEST(reopenActivatesAnIdentityAlreadyOpenedByAnotherPath) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    const auto original = openSaved(tabs, 1, "a");
    (void)tabs.close(original, 100ms);
    const auto replacement = openSaved(tabs, 2, "a");

    ASSERT_EQ(tabs.reopenClosed().tab, std::optional{replacement});
    ASSERT_EQ(tabs.viewState().tabs.size(), std::size_t{1});
    ASSERT_EQ(tabs.viewState().active, std::optional{replacement});
    ASSERT_EQ(tabs.recentlyClosedCount(), std::size_t{0});
    ASSERT_EQ(lifecycle.reopenCalls, 0);
}

TEST(untitledLabelsAreSmallestAvailableAndReopenIsStable) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    const auto first = tabs.openDocument(
        ssg::FileDocumentId{1},
        ssg::JournalDocumentKey::untitled(ssg::UntitledDocumentId::generate()),
        "", ssg::DocumentMode::Edit, true);
    const auto second = tabs.openDocument(
        ssg::FileDocumentId{2},
        ssg::JournalDocumentKey::untitled(ssg::UntitledDocumentId::generate()),
        "", ssg::DocumentMode::Edit, true);
    ASSERT_EQ(tabs.viewState().tabs[0].label, std::string{"Untitled 1"});
    ASSERT_EQ(tabs.viewState().tabs[1].label, std::string{"Untitled 2"});

    (void)tabs.close(*first.tab, 100ms);
    const auto third = tabs.openDocument(
        ssg::FileDocumentId{3},
        ssg::JournalDocumentKey::untitled(ssg::UntitledDocumentId::generate()),
        "", ssg::DocumentMode::Edit, true);
    ASSERT_EQ(tabs.viewState().tabs.back().label, std::string{"Untitled 1"});
    (void)tabs.reopenClosed();
    const auto reopened = std::find_if(
        tabs.viewState().tabs.begin(), tabs.viewState().tabs.end(),
        [id = *first.tab](const auto& tab) { return tab.id == id; });
    ASSERT_TRUE(reopened != tabs.viewState().tabs.end());
    ASSERT_EQ(reopened->label, std::string{"Untitled 1"});
    ASSERT_NE(third.tab, second.tab);
}

TEST(reopenUntitledRebindsDocumentKeyForDedup) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    auto originalKey =
        ssg::JournalDocumentKey::untitled(ssg::UntitledDocumentId::generate());
    auto reopenedKey =
        ssg::JournalDocumentKey::untitled(ssg::UntitledDocumentId::generate());
    while (reopenedKey == originalKey) {
        reopenedKey =
            ssg::JournalDocumentKey::untitled(ssg::UntitledDocumentId::generate());
    }
    auto opened = tabs.openDocument(ssg::FileDocumentId{1}, originalKey, "",
                                    ssg::DocumentMode::Edit, true);
    ASSERT_TRUE(opened.accepted());
    ASSERT_TRUE(opened.tab.has_value());
    if (!opened.tab) return;
    ASSERT_TRUE(tabs.close(*opened.tab, 100ms).accepted());

    lifecycle.reopenedDocument = ssg::FileDocumentId{2};
    lifecycle.reopenedDocumentKey = reopenedKey;
    ASSERT_TRUE(tabs.reopenClosed().accepted());

    ASSERT_EQ(tabs.viewState().tabs.size(), std::size_t{1});
    ASSERT_EQ(tabs.viewState().tabs.front().document,
              lifecycle.reopenedDocument);
    ASSERT_EQ(tabs.viewState().tabs.front().documentKey,
              lifecycle.reopenedDocumentKey);

    auto duplicate = tabs.openDocument(*lifecycle.reopenedDocument, reopenedKey,
                                       "reopened", ssg::DocumentMode::Edit, false);
    ASSERT_TRUE(duplicate.accepted());
    ASSERT_EQ(tabs.viewState().tabs.size(), std::size_t{1});
}

TEST(badgesUpdateExactly) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    (void)openSaved(tabs, 7, "a");
    ASSERT_TRUE(tabs.updateDocument(
                        ssg::FileDocumentId{7}, saved("a"), "a",
                        ssg::DocumentMode::ReadOnly, true,
                        ssg::TabRecoveryBadge::Pending)
                    .accepted());
    const auto target = tabs.viewState();
    ASSERT_EQ(target.tabs[0].mode, ssg::DocumentMode::ReadOnly);
    ASSERT_TRUE(target.tabs[0].dirty);
    ASSERT_EQ(target.tabs[0].recovery, ssg::TabRecoveryBadge::Pending);
}

TEST(closeAcceptsAMissingCompensationOnlyForAnEphemeralTab) {
    FakeLifecycle lifecycle;
    ssg::TabManager tabs{lifecycle};
    const auto document = openSaved(tabs, 1, "a");
    const auto output =
        *tabs.openContent(ssg::TabKind::ReadOnlyOutput, "output:1", "Output",
                          ssg::DocumentMode::ReadOnly)
             .tab;

    // An ephemeral (regenerable) tab returns no reopen record and closes anyway;
    // it is not added to the reopen-closed history.
    lifecycle.ephemeralClose.push_back(output);
    ASSERT_TRUE(tabs.close(output, 100ms).accepted());
    ASSERT_EQ(tabs.recentlyClosedCount(), std::size_t{0});
    ASSERT_EQ(tabs.viewState().tabs.size(), std::size_t{1});

    // A non-ephemeral tab that returns no compensation is a lifecycle bug: the
    // close is refused and the tab is left in place.
    lifecycle.noCompensation.push_back(document);
    const auto refused = tabs.close(document, 100ms);
    ASSERT_EQ(refused.error, ssg::TabError::LifecycleFailed);
    ASSERT_EQ(tabs.viewState().tabs.size(), std::size_t{1});
    ASSERT_EQ(tabs.viewState().tabs[0].id, document);
}

}  // namespace

SSG_TEST_SUITE(test_tabs) {
    RUN(commandSetExactlyOwnsNineTabCommands);
    RUN(duplicateDocumentIdentityActivatesExistingTab);
    RUN(allTabKindsNavigateCyclicallyAndReorder);
    RUN(activeClosePrefersRightThenLeftAndDirtyFailureIsAtomic);
    RUN(batchCloseIsLeftToRightBestEffort);
    RUN(reopenIsLifoRetryableAndRestoresPositionAndActivation);
    RUN(recentlyClosedEvictsOldestAtConfiguredBound);
    RUN(reopenActivatesAnIdentityAlreadyOpenedByAnotherPath);
    RUN(untitledLabelsAreSmallestAvailableAndReopenIsStable);
    RUN(reopenUntitledRebindsDocumentKeyForDedup);
    RUN(badgesUpdateExactly);
    RUN(closeAcceptsAMissingCompensationOnlyForAnEphemeralTab);
    std::cout << "\nPassed: " << passed << " Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
