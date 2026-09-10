#include "test_helpers.h"

#include <ssg/TabManager.h>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

ssg::JournalDocumentKey saved(std::string_view path) {
    return ssg::JournalDocumentKey::saved(path);
}

ssg::TabState tabState(const ssg::TabManager& tabs, ssg::TabId id) {
    const auto& view = tabs.viewState().tabs;
    const auto found = std::find_if(
        view.begin(), view.end(),
        [id](const ssg::TabState& tab) { return tab.id == id; });
    if (found == view.end()) {
        throw std::logic_error{"tab not found"};
    }
    return *found;
}

ssg::TabLifecycleResult acceptedCloseResult(const ssg::TabState& tab) {
    return {ssg::TabError::None, {},
            ssg::RecoveryRecordId{"closed-" + std::to_string(tab.id.value())},
            std::nullopt, std::nullopt, tab.dirty};
}

ssg::TabLifecycleResult failedCloseResult() {
    return {ssg::TabError::DurabilityFailed, "durability failed", std::nullopt,
            std::nullopt, std::nullopt, false};
}

ssg::TabLifecycleResult ephemeralCloseResult() {
    return {ssg::TabError::None, {}, std::nullopt, std::nullopt, std::nullopt,
            false, true};
}

ssg::TabLifecycleResult missingCompensationResult() {
    return {ssg::TabError::None, {}, std::nullopt, std::nullopt, std::nullopt,
            false, false};
}

ssg::TabCloseOutcome acceptedCloseOutcome(const ssg::TabState& tab) {
    return {tab.id, acceptedCloseResult(tab)};
}

ssg::TabCloseOutcome failedCloseOutcome(ssg::TabId id) {
    return {id, failedCloseResult()};
}

ssg::TabId openSaved(ssg::TabManager& tabs, std::uint64_t document,
                     std::string_view path, bool dirty = false) {
    return *tabs
                .openDocument(ssg::FileDocumentId{document}, saved(path), path,
                              ssg::DocumentMode::Edit, dirty)
                .tab;
}

TEST(duplicateDocumentIdentityActivatesExistingTab) {
    ssg::TabManager tabs;
    const auto first = openSaved(tabs, 1, "src/a.cpp");
    (void)openSaved(tabs, 2, "src/b.cpp");
    const auto duplicate = tabs.openDocument(
        ssg::FileDocumentId{99}, saved("src/a.cpp"), "other",
        ssg::DocumentMode::ReadOnly, true, ssg::ScratchDurability::Failed);

    ASSERT_TRUE(duplicate.accepted());
    ASSERT_EQ(duplicate.tab, std::optional{first});
    ASSERT_EQ(tabs.viewState().tabs.size(), std::size_t{2});
    ASSERT_EQ(tabs.viewState().active, std::optional{first});
}

TEST(allTabKindsNavigateCyclicallyAndReorder) {
    ssg::TabManager tabs;
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
    ssg::TabManager tabs;
    const auto a = openSaved(tabs, 1, "a");
    const auto b = openSaved(tabs, 2, "b", true);
    const auto c = openSaved(tabs, 3, "c");
    (void)tabs.activate(b);
    const auto before = tabs.viewState();

    const auto closeFailure = tabs.close(b, failedCloseResult());
    ASSERT_EQ(closeFailure.error, ssg::TabError::DurabilityFailed);
    ASSERT_EQ(tabs.viewState(), before);

    ASSERT_TRUE(tabs.close(b, acceptedCloseResult(tabState(tabs, b))).accepted());
    ASSERT_EQ(tabs.viewState().active, std::optional{c});
    ASSERT_TRUE(tabs.close(c, acceptedCloseResult(tabState(tabs, c))).accepted());
    ASSERT_EQ(tabs.viewState().active, std::optional{a});
}

TEST(batchCloseIsLeftToRightBestEffort) {
    ssg::TabManager tabs;
    const auto a = openSaved(tabs, 1, "a");
    const auto b = openSaved(tabs, 2, "b", true);
    const auto c = openSaved(tabs, 3, "c");
    const auto d = openSaved(tabs, 4, "d", true);
    (void)tabs.activate(c);

    const auto result = tabs.closeOthers(
        a, {failedCloseOutcome(b), acceptedCloseOutcome(tabState(tabs, c)),
            failedCloseOutcome(d)});
    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(result.failures.size(), std::size_t{2});
    ASSERT_EQ(tabs.viewState().tabs.size(), std::size_t{3});
    ASSERT_EQ(tabs.viewState().tabs[0].id, a);
    ASSERT_EQ(tabs.viewState().tabs[1].id, b);
    ASSERT_EQ(tabs.viewState().tabs[2].id, d);
    ASSERT_EQ(tabs.viewState().active, std::optional{d});
}

TEST(reopenIsLifoRetryableAndRestoresPositionAndActivation) {
    ssg::TabManager tabs;
    const auto a = openSaved(tabs, 1, "a");
    const auto b = openSaved(tabs, 2, "b");
    const auto c = openSaved(tabs, 3, "c");
    (void)tabs.close(b, acceptedCloseResult(tabState(tabs, b)));
    (void)tabs.close(c, acceptedCloseResult(tabState(tabs, c)));

    auto begin = tabs.beginReopenClosed();
    ASSERT_TRUE(std::holds_alternative<ssg::TabReopenRequest>(begin));
    auto request = std::get<ssg::TabReopenRequest>(begin);
    auto failedResult = tabs.finishReopenClosed(
        request, {ssg::TabError::LifecycleFailed, "restore failed", std::nullopt,
                  std::nullopt, std::nullopt, false});
    ASSERT_EQ(failedResult.error, ssg::TabError::LifecycleFailed);

    begin = tabs.beginReopenClosed();
    ASSERT_TRUE(std::holds_alternative<ssg::TabReopenRequest>(begin));
    request = std::get<ssg::TabReopenRequest>(begin);
    ASSERT_EQ(tabs.finishReopenClosed(
                  request, {ssg::TabError::None, {}, std::nullopt, std::nullopt,
                            std::nullopt, true})
                  .tab,
              std::optional{c});
    begin = tabs.beginReopenClosed();
    ASSERT_TRUE(std::holds_alternative<ssg::TabReopenRequest>(begin));
    request = std::get<ssg::TabReopenRequest>(begin);
    ASSERT_EQ(tabs.finishReopenClosed(
                  request, {ssg::TabError::None, {}, std::nullopt, std::nullopt,
                            std::nullopt, true})
                  .tab,
              std::optional{b});
    ASSERT_EQ(tabs.viewState().tabs[0].id, a);
    ASSERT_EQ(tabs.viewState().tabs[1].id, b);
    ASSERT_EQ(tabs.viewState().tabs[2].id, c);
    ASSERT_EQ(tabs.viewState().active, std::optional{b});
}

TEST(recentlyClosedEvictsOldestAtConfiguredBound) {
    ssg::TabManager tabs{{.maximumRecentlyClosed = 2}};
    const auto a = openSaved(tabs, 1, "a");
    const auto b = openSaved(tabs, 2, "b");
    const auto c = openSaved(tabs, 3, "c");
    (void)tabs.close(a, acceptedCloseResult(tabState(tabs, a)));
    (void)tabs.close(b, acceptedCloseResult(tabState(tabs, b)));
    (void)tabs.close(c, acceptedCloseResult(tabState(tabs, c)));

    auto begin = tabs.beginReopenClosed();
    ASSERT_TRUE(std::holds_alternative<ssg::TabReopenRequest>(begin));
    auto request = std::get<ssg::TabReopenRequest>(begin);
    ASSERT_EQ(tabs.finishReopenClosed(
                  request, {ssg::TabError::None, {}, std::nullopt, std::nullopt,
                            std::nullopt, true})
                  .tab,
              std::optional{c});
    begin = tabs.beginReopenClosed();
    ASSERT_TRUE(std::holds_alternative<ssg::TabReopenRequest>(begin));
    request = std::get<ssg::TabReopenRequest>(begin);
    ASSERT_EQ(tabs.finishReopenClosed(
                  request, {ssg::TabError::None, {}, std::nullopt, std::nullopt,
                            std::nullopt, true})
                  .tab,
              std::optional{b});
    const auto noRecent = std::get<ssg::TabResult>(tabs.beginReopenClosed());
    ASSERT_EQ(noRecent.error, ssg::TabError::NoRecentlyClosed);
}

TEST(reopenActivatesAnIdentityAlreadyOpenedByAnotherPath) {
    ssg::TabManager tabs;
    const auto original = openSaved(tabs, 1, "a");
    (void)tabs.close(original, acceptedCloseResult(tabState(tabs, original)));
    const auto replacement = openSaved(tabs, 2, "a");

    const auto reopened = std::get<ssg::TabResult>(tabs.beginReopenClosed());
    ASSERT_EQ(reopened.tab, std::optional{replacement});
    ASSERT_EQ(tabs.viewState().tabs.size(), std::size_t{1});
    ASSERT_EQ(tabs.viewState().active, std::optional{replacement});
}

TEST(untitledLabelsAreSmallestAvailableAndReopenIsStable) {
    ssg::TabManager tabs;
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

    (void)tabs.close(*first.tab, acceptedCloseResult(tabState(tabs, *first.tab)));
    const auto third = tabs.openDocument(
        ssg::FileDocumentId{3},
        ssg::JournalDocumentKey::untitled(ssg::UntitledDocumentId::generate()),
        "", ssg::DocumentMode::Edit, true);
    ASSERT_EQ(tabs.viewState().tabs.back().label, std::string{"Untitled 1"});
    auto begin = tabs.beginReopenClosed();
    ASSERT_TRUE(std::holds_alternative<ssg::TabReopenRequest>(begin));
    auto request = std::get<ssg::TabReopenRequest>(begin);
    ASSERT_TRUE(tabs.finishReopenClosed(
                    request, {ssg::TabError::None, {}, std::nullopt, std::nullopt,
                              std::nullopt, true})
                    .accepted());
    const auto reopened = std::find_if(
        tabs.viewState().tabs.begin(), tabs.viewState().tabs.end(),
        [id = *first.tab](const auto& tab) { return tab.id == id; });
    ASSERT_TRUE(reopened != tabs.viewState().tabs.end());
    ASSERT_EQ(reopened->label, std::string{"Untitled 1"});
    ASSERT_NE(third.tab, second.tab);
}

TEST(reopenUntitledRebindsDocumentKeyForDedup) {
    ssg::TabManager tabs;
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
    ASSERT_TRUE(
        tabs.close(*opened.tab, acceptedCloseResult(tabState(tabs, *opened.tab)))
            .accepted());

    auto begin = tabs.beginReopenClosed();
    ASSERT_TRUE(std::holds_alternative<ssg::TabReopenRequest>(begin));
    auto request = std::get<ssg::TabReopenRequest>(begin);
    ASSERT_TRUE(tabs.finishReopenClosed(
                    request, {ssg::TabError::None, {}, std::nullopt,
                              ssg::FileDocumentId{2}, reopenedKey, true})
                    .accepted());

    ASSERT_EQ(tabs.viewState().tabs.size(), std::size_t{1});
    ASSERT_EQ(tabs.viewState().tabs.front().document,
              std::optional{ssg::FileDocumentId{2}});
    ASSERT_EQ(tabs.viewState().tabs.front().documentKey,
              std::optional{reopenedKey});

    auto duplicate = tabs.openDocument(ssg::FileDocumentId{2}, reopenedKey,
                                       "reopened", ssg::DocumentMode::Edit, false);
    ASSERT_TRUE(duplicate.accepted());
    ASSERT_EQ(tabs.viewState().tabs.size(), std::size_t{1});
}

TEST(badgesUpdateExactly) {
    ssg::TabManager tabs;
    (void)openSaved(tabs, 7, "a");
    ASSERT_TRUE(tabs.updateDocument(
                    ssg::FileDocumentId{7}, saved("a"), "a",
                    ssg::DocumentMode::ReadOnly, true,
                    ssg::ScratchDurability::Pending)
                    .accepted());
    const auto target = tabs.viewState();
    ASSERT_EQ(target.tabs[0].mode, ssg::DocumentMode::ReadOnly);
    ASSERT_TRUE(target.tabs[0].dirty);
    ASSERT_EQ(target.tabs[0].recovery,
              std::optional{ssg::ScratchDurability::Pending});
}

TEST(closeAcceptsAMissingCompensationOnlyForAnEphemeralTab) {
    ssg::TabManager tabs;
    const auto document = openSaved(tabs, 1, "a");
    const auto output =
        *tabs.openContent(ssg::TabKind::ReadOnlyOutput, "output:1", "Output",
                          ssg::DocumentMode::ReadOnly)
             .tab;

    ASSERT_TRUE(tabs.close(output, ephemeralCloseResult()).accepted());
    ASSERT_EQ(tabs.viewState().tabs.size(), std::size_t{1});

    const auto refused = tabs.close(document, missingCompensationResult());
    ASSERT_EQ(refused.error, ssg::TabError::LifecycleFailed);
    ASSERT_EQ(tabs.viewState().tabs.size(), std::size_t{1});
    ASSERT_EQ(tabs.viewState().tabs[0].id, document);
}

}  // namespace

SSG_TEST_SUITE(test_tabs) {
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
