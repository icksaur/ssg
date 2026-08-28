#include "ssg/ExternalModificationFlow.h"
#include "test_helpers.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_{std::filesystem::temp_directory_path() /
                ("ssg-external-" + std::to_string(++next))} {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

    const std::filesystem::path& path() const { return path_; }

private:
    inline static std::uint64_t next = 0;
    std::filesystem::path path_;
};

ssg::JournalDocument document(std::string text, bool dirty) {
    return {ssg::JournalDocumentKey::saved("note.txt"),
            ssg::DocumentMode::Edit, dirty, std::move(text)};
}

ssg::WatchEvent event(std::uint64_t sequence,
                      ssg::WatchEventOrigin origin =
                          ssg::WatchEventOrigin::External) {
    ssg::WatchEvent result;
    result.kind = ssg::WatchEventKind::Modify;
    result.path = "note.txt";
    result.sequence = sequence;
    result.origin = origin;
    return result;
}

ssg::ExternalEventInput input(std::uint64_t sequence, std::string content,
                              ssg::WatchEventOrigin origin =
                                  ssg::WatchEventOrigin::External) {
    return {event(sequence, origin), ssg::DiffFileId{"note"}, "base\n",
            std::move(content)};
}

struct Fixture {
    TemporaryDirectory temporary;
    ssg::RecoveryManager recovery =
        ssg::RecoveryManager::create(temporary.path() / "recovery");
    ssg::DiffModel diff;
    ssg::ExternalModificationFlow flow{recovery, diff};

    Fixture() {
        ASSERT_TRUE(diff.seedNonGit(
                            {{ssg::DiffFileId{"note"}, "note.txt", "base\n"}},
                            ssg::Revision{1})
                        .accepted());
    }
};

TEST(commandSetIsCompleteAndOrdered) {
    const auto descriptors = ssg::externalModificationCommandSet().descriptors();
    ASSERT_EQ(descriptors.size(), 3U);
    ASSERT_EQ(descriptors[0].id, "external.reload");
    ASSERT_EQ(descriptors[1].id, "external.keep_buffer");
    ASSERT_EQ(descriptors[2].id, "external.open_diff");
}

TEST(cleanExternalEditAutoReloadsWithoutRecoveryStatus) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("base\n", false)};

    const auto result =
        fixture.flow.processEvent(input(2, "disk\n"), ssg::Revision{2}, open);

    ASSERT_TRUE(result.accepted());
    ASSERT_FALSE(result.statusPublished);
    ASSERT_EQ(open->utf8Content, "disk\n");
    ASSERT_FALSE(open->dirty);
    ASSERT_TRUE(fixture.recovery.records().empty());
    ASSERT_TRUE(fixture.flow.viewState().files.empty());
}

TEST(aFailedCleanCommitRaisesTheConflictInsteadOfClearing) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("base\n", false)};

    // Clean document, but the workspace commit the reconcile supplies fails. The
    // flow must NOT clear to a stale buffer: it stages, sees the commit fail, and
    // raises the conflict (Decision 11's stage->commit->publish for the clean path).
    const auto result = fixture.flow.processEvent(
        input(2, "disk\n"), ssg::Revision{2}, open, []() { return false; });

    ASSERT_TRUE(result.accepted());
    ASSERT_TRUE(result.statusPublished);
    ASSERT_EQ(open->utf8Content, "base\n");
    ASSERT_FALSE(open->dirty);
    const auto state = fixture.flow.viewState();
    ASSERT_EQ(state.files.size(), 1U);
    ASSERT_EQ(state.files[0].status,
              ssg::ExternalDocumentStatus::ExternallyModified);
}

TEST(aSucceedingCleanCommitAdoptsTheDiskContent) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("base\n", false)};

    bool committed = false;
    const auto result = fixture.flow.processEvent(
        input(2, "disk\n"), ssg::Revision{2}, open,
        [&]() { committed = true; return true; });

    ASSERT_TRUE(committed);
    ASSERT_TRUE(result.accepted());
    ASSERT_FALSE(result.statusPublished);
    ASSERT_EQ(open->utf8Content, "disk\n");
    ASSERT_TRUE(fixture.flow.viewState().files.empty());
}

TEST(dirtyExternalEditPreservesBufferAndPublishesActions) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};

    const auto result =
        fixture.flow.processEvent(input(2, "disk\n"), ssg::Revision{2}, open);
    const auto state = fixture.flow.viewState();

    ASSERT_TRUE(result.accepted());
    ASSERT_TRUE(result.statusPublished);
    ASSERT_EQ(open->utf8Content, "buffer\n");
    ASSERT_TRUE(open->dirty);
    ASSERT_EQ(state.files.size(), 1U);
    ASSERT_EQ(state.files[0].status,
              ssg::ExternalDocumentStatus::ExternallyModified);
    ASSERT_EQ(state.files[0].statusLabel, std::string{"M"});
    ASSERT_EQ(state.files[0].actions.size(), 3U);
    ASSERT_EQ(state.files[0].actions[0],
              ssg::externalActionAffordance(ssg::ExternalAction::Reload));
    ASSERT_EQ(state.files[0].actions[1],
              ssg::externalActionAffordance(
                  ssg::ExternalAction::KeepBuffer));
    ASSERT_EQ(state.files[0].actions[2],
              ssg::externalActionAffordance(ssg::ExternalAction::OpenDiff));
    const auto diff = fixture.diff.file(ssg::DiffFileId{"note"});
    ASSERT_TRUE(diff.has_value());
    if (diff) {
        ASSERT_EQ(diff->get().hunks.front().baselineLines,
                  (std::vector<std::string>{"base\n"}));
        ASSERT_EQ(diff->get().hunks.front().targetLines,
                  (std::vector<std::string>{"disk\n"}));
    }
}

TEST(openDiffIsObservationalAndKeepBufferAcknowledgesDisk) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    ASSERT_TRUE(fixture.flow.processEvent(input(2, "disk\n"), ssg::Revision{2}, open).accepted());
    const auto before = fixture.flow.viewState();

    const auto target = fixture.flow.openDiff(ssg::DiffFileId{"note"});
    ASSERT_TRUE(target.accepted());
    ASSERT_EQ(target.target->path, std::filesystem::path{"note.txt"});
    ASSERT_EQ(fixture.flow.viewState(), before);

    const auto kept = fixture.flow.keepBuffer(
        ssg::DiffFileId{"note"},
        [](bool, const std::optional<std::string>&) { return true; });
    ASSERT_TRUE(kept.accepted());
    ASSERT_EQ(open->utf8Content, "buffer\n");
    ASSERT_TRUE(open->dirty);
    ASSERT_TRUE(fixture.flow.viewState().files.empty());
    ASSERT_TRUE(fixture.recovery.records().empty());
}

TEST(reloadIsReversibleAndRecordPrecedesBufferReplacement) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    ASSERT_TRUE(fixture.flow.processEvent(input(2, "disk\n"), ssg::Revision{2}, open).accepted());

    const auto reloaded =
        fixture.flow.reload(ssg::DiffFileId{"note"}, open);

    ASSERT_TRUE(reloaded.accepted());
    ASSERT_TRUE(reloaded.compensation.has_value());
    ASSERT_EQ(open->utf8Content, "disk\n");
    ASSERT_FALSE(open->dirty);
    ASSERT_TRUE(fixture.flow.viewState().files.empty());
    ASSERT_EQ(fixture.recovery.records().size(), 1U);

    const auto restored =
        fixture.recovery.restoreDocument(*reloaded.compensation, open);
    ASSERT_TRUE(restored.accepted());
    ASSERT_EQ(open->utf8Content, "buffer\n");
    ASSERT_TRUE(open->dirty);
}

TEST(ssgSaveAdvancesBaselineWithoutDuplicateStatus) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("saved\n", false)};

    const auto saved = fixture.flow.processEvent(
        input(2, "saved\n", ssg::WatchEventOrigin::SsgSave), ssg::Revision{2}, open);

    ASSERT_TRUE(saved.accepted());
    ASSERT_FALSE(saved.statusPublished);
    ASSERT_TRUE(fixture.flow.viewState().files.empty());
    ASSERT_EQ(open->utf8Content, "saved\n");
}

TEST(genuineExternalEditIsNotConsumedBySaveCorrelation) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    ASSERT_TRUE(fixture.flow
                    .processEvent(
                        input(2, "saved\n", ssg::WatchEventOrigin::SsgSave),
                        ssg::Revision{2}, open)
                    .accepted());

    const auto external =
        fixture.flow.processEvent(input(3, "other\n"), ssg::Revision{3}, open);

    ASSERT_TRUE(external.accepted());
    ASSERT_TRUE(external.statusPublished);
    ASSERT_EQ(open->utf8Content, "buffer\n");
    ASSERT_EQ(fixture.flow.viewState().files.size(), 1U);
}

TEST(staleEventIsFailureAtomic) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    ASSERT_TRUE(fixture.flow.processEvent(input(2, "disk\n"), ssg::Revision{2}, open).accepted());
    const auto state = fixture.flow.viewState();
    const auto diff = fixture.diff.viewState();
    const auto before = open;

    const auto stale =
        fixture.flow.processEvent(input(2, "stale\n"), ssg::Revision{3}, open);

    ASSERT_FALSE(stale.accepted());
    ASSERT_EQ(stale.error, ssg::ExternalModificationError::StaleEvent);
    ASSERT_EQ(open, before);
    ASSERT_EQ(fixture.flow.viewState(), state);
    ASSERT_EQ(fixture.diff.viewState(), diff);
}

TEST(diffRejectionDoesNotSuppressDirtyBufferSafetyStatus) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryManager::create(temporary.path() / "recovery");
    ssg::DiffModel diff;
    ssg::ExternalModificationFlow flow{recovery, diff};
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};

    const auto result = flow.processEvent(input(2, "disk\n"), ssg::Revision{2}, open);

    ASSERT_TRUE(result.accepted());
    ASSERT_FALSE(result.diffRouted);
    ASSERT_TRUE(result.statusPublished);
    ASSERT_EQ(open->utf8Content, "buffer\n");
    ASSERT_TRUE(open->dirty);
    ASSERT_EQ(flow.viewState().files.size(), 1U);
}

TEST(viewDeltaReplaysAsTargetState) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    const auto base = fixture.flow.viewState();
    ASSERT_TRUE(fixture.flow.processEvent(input(2, "disk\n"), ssg::Revision{2}, open).accepted());
    const auto target = fixture.flow.viewState();

    const auto delta = ssg::ExternalModificationDeltaCodec{}.derive(base, target);
    const auto replayed = ssg::ExternalModificationDeltaCodec{}.replay(base, delta);

    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(*replayed.state, target);
    const auto stale = ssg::ExternalModificationDeltaCodec{}.replay(target, delta);
    ASSERT_FALSE(stale.accepted());
    ASSERT_EQ(stale.error, ssg::ExternalDeltaError::StaleRevision);
}

TEST(aFailedWorkspaceCommitLeavesThePendingActionRaised) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    ASSERT_TRUE(fixture.flow
                    .processEvent(input(2, "disk\n"), ssg::Revision{2}, open)
                    .accepted());
    ASSERT_EQ(fixture.flow.viewState().files.size(), 1U);

    // A commit that reports it did not land must leave the action raised, so the
    // section never clears over a buffer the reload did not actually replace.
    const auto failedReload = fixture.flow.resolveReload(
        ssg::DiffFileId{"note"},
        [](const std::string&) -> ssg::ExternalReloadCommitResult {
            return {false, std::nullopt};
        });
    ASSERT_FALSE(failedReload.accepted());
    ASSERT_EQ(failedReload.error, ssg::ExternalModificationError::RecoveryFailed);
    ASSERT_EQ(fixture.flow.viewState().files.size(), 1U);

    // The primitive commits the CAPTURED bytes, not a fresh disk read; a landing
    // commit receives exactly them and then clears the action.
    std::string committed;
    const auto ok = fixture.flow.resolveReload(
        ssg::DiffFileId{"note"},
        [&](const std::string& content) -> ssg::ExternalReloadCommitResult {
            committed = content;
            return {true, std::nullopt};
        });
    ASSERT_TRUE(ok.accepted());
    ASSERT_EQ(committed, "disk\n");
    ASSERT_TRUE(fixture.flow.viewState().files.empty());
}

TEST(aFailedKeepBufferCommitLeavesTheConflictRaised) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    ASSERT_TRUE(fixture.flow
                    .processEvent(input(2, "disk\n"), ssg::Revision{2}, open)
                    .accepted());
    ASSERT_EQ(fixture.flow.viewState().files.size(), 1U);

    // The dismissal drives its cross-store commit with the EXACT captured state; a
    // commit that reports it did not land must leave the conflict raised (both
    // stores stay at their prior state), never clear over an un-advanced baseline.
    bool sawContent = false;
    const auto rejected = fixture.flow.keepBuffer(
        ssg::DiffFileId{"note"},
        [&](bool removed, const std::optional<std::string>& content) {
            sawContent = !removed && content.has_value() && *content == "disk\n";
            return false;
        });
    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error, ssg::ExternalModificationError::RecoveryFailed);
    ASSERT_TRUE(sawContent);
    ASSERT_EQ(fixture.flow.viewState().files.size(), 1U);

    // A landing commit receives the same captured state and then clears the action.
    const auto ok = fixture.flow.keepBuffer(
        ssg::DiffFileId{"note"},
        [](bool, const std::optional<std::string>&) { return true; });
    ASSERT_TRUE(ok.accepted());
    ASSERT_TRUE(fixture.flow.viewState().files.empty());
    ASSERT_EQ(open->utf8Content, "buffer\n");
    ASSERT_TRUE(open->dirty);
}

TEST(keepBufferWithNoBaselineAdvancingCommitNeverClears) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    ASSERT_TRUE(fixture.flow
                    .processEvent(input(2, "disk\n"), ssg::Revision{2}, open)
                    .accepted());
    ASSERT_EQ(fixture.flow.viewState().files.size(), 1U);

    // An explicitly-empty commit advances no baseline; clearing over it would
    // resurrect the dismissed conflict, so keepBuffer must refuse and leave the
    // conflict raised.
    const auto rejected =
        fixture.flow.keepBuffer(ssg::DiffFileId{"note"}, ssg::ExternalKeepBufferCommit{});
    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error, ssg::ExternalModificationError::RecoveryFailed);
    ASSERT_EQ(fixture.flow.viewState().files.size(), 1U);
}

}  // namespace

TEST(aRenameRetiresPendingKeyedByThePreviousId) {
    Fixture fixture;
    ASSERT_TRUE(fixture.diff
                    .seedNonGit({{ssg::DiffFileId{"note2"}, "renamed.txt",
                                  "base\n"}},
                                ssg::Revision{2})
                    .accepted());
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    ASSERT_TRUE(fixture.flow
                    .processEvent(input(3, "disk\n"), ssg::Revision{3}, open)
                    .accepted());
    ASSERT_EQ(fixture.flow.viewState().files.size(), 1U);

    ssg::WatchEvent rename;
    rename.kind = ssg::WatchEventKind::Rename;
    rename.path = "renamed.txt";
    rename.previousPath = std::filesystem::path{"note.txt"};
    rename.sequence = 4;
    rename.origin = ssg::WatchEventOrigin::External;
    ssg::ExternalEventInput renameInput{rename, ssg::DiffFileId{"note2"}, "base\n",
                                        std::string{"disk2\n"}};
    renameInput.previousId = ssg::DiffFileId{"note"};
    const auto result = fixture.flow.processEvent(std::move(renameInput),
                                                  ssg::Revision{4}, open);

    ASSERT_TRUE(result.accepted());
    // The old-id pending entry was retired; only the new-id entry remains.
    const auto state = fixture.flow.viewState();
    ASSERT_EQ(state.files.size(), 1U);
    ASSERT_EQ(state.files[0].id, ssg::DiffFileId{"note2"});
}

TEST(theExternalModSelectionFollowsTheListAndSurvivesResolves) {
    TemporaryDirectory temporary;
    ssg::RecoveryManager recovery =
        ssg::RecoveryManager::create(temporary.path() / "recovery");
    ssg::DiffModel diff;
    ASSERT_TRUE(diff.seedNonGit({{ssg::DiffFileId{"a"}, "a.txt", "base\n"},
                                 {ssg::DiffFileId{"b"}, "b.txt", "base\n"}},
                                ssg::Revision{1})
                    .accepted());
    ssg::ExternalModificationFlow flow{recovery, diff};

    auto raise = [&](const char* id, const char* path, std::uint64_t sequence,
                     ssg::Revision revision) {
        std::optional<ssg::JournalDocument> open{
            {ssg::JournalDocumentKey::saved(path), ssg::DocumentMode::Edit, true,
             "buffer\n"}};
        ssg::WatchEvent e;
        e.kind = ssg::WatchEventKind::Modify;
        e.path = path;
        e.sequence = sequence;
        e.origin = ssg::WatchEventOrigin::External;
        ssg::ExternalEventInput in{e, ssg::DiffFileId{id}, "base\n",
                                   std::string{"disk\n"}};
        ASSERT_TRUE(flow.processEvent(std::move(in), revision, open).accepted());
    };

    // The section becomes non-empty: the selection homes to the first file.
    raise("a", "a.txt", 2, ssg::Revision{2});
    ASSERT_TRUE(flow.viewState().selected == ssg::DiffFileId{"a"});
    // Adding a file keeps the selection where it was.
    raise("b", "b.txt", 3, ssg::Revision{3});
    ASSERT_EQ(flow.viewState().files.size(), 2U);
    ASSERT_TRUE(flow.viewState().selected == ssg::DiffFileId{"a"});

    // The movers wrap around like the tree.
    ASSERT_TRUE(flow.selectNext());
    ASSERT_TRUE(flow.viewState().selected == ssg::DiffFileId{"b"});
    ASSERT_TRUE(flow.selectNext());
    ASSERT_TRUE(flow.viewState().selected == ssg::DiffFileId{"a"});

    // Resolving the selected file re-homes to the file that now occupies its slot
    // (the next file), not to nullopt while others remain.
    ASSERT_TRUE(flow.viewState().selected == ssg::DiffFileId{"a"});
    ASSERT_TRUE(flow.keepBuffer(ssg::DiffFileId{"a"},
                               [](bool, const std::optional<std::string>&) {
                                   return true;
                               })
                    .accepted());
    ASSERT_EQ(flow.viewState().files.size(), 1U);
    ASSERT_TRUE(flow.viewState().selected == ssg::DiffFileId{"b"});

    // Resolving the last file clears the selection.
    ASSERT_TRUE(flow.keepBuffer(ssg::DiffFileId{"b"},
                               [](bool, const std::optional<std::string>&) {
                                   return true;
                               })
                    .accepted());
    ASSERT_TRUE(flow.viewState().files.empty());
    ASSERT_FALSE(flow.viewState().selected.has_value());
}

TEST(aSelectionOnlyExternalDeltaReplaysToTheMovedSelection) {
    ssg::ExternalDocumentView fa{ssg::DiffFileId{"a"}, "a.txt",
                                 ssg::ExternalDocumentStatus::ExternallyModified,
                                 "x", "M",
                                 {ssg::externalActionAffordance(
                                     ssg::ExternalAction::Reload)}};
    ssg::ExternalDocumentView fb{ssg::DiffFileId{"b"}, "b.txt",
                                 ssg::ExternalDocumentStatus::ExternallyModified,
                                 "y", "M",
                                 {ssg::externalActionAffordance(
                                     ssg::ExternalAction::Reload)}};
    ssg::ExternalModificationViewState base{ssg::Revision{5}, "two files",
                                            {fa, fb},
                                            ssg::DiffFileId{"a"}};
    ssg::ExternalModificationViewState target{ssg::Revision{6}, "two files",
                                              {fa, fb},
                                              ssg::DiffFileId{"b"}};
    ssg::ExternalModificationDeltaCodec codec;
    const auto delta = codec.derive(base, target);
    // A selection-only move: files unchanged, the selected id moved.
    ASSERT_TRUE(delta.upserted.empty());
    ASSERT_TRUE(delta.removed.empty());
    ASSERT_TRUE(delta.selected == ssg::DiffFileId{"b"});
    const auto replayed = codec.replay(base, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_TRUE(replayed.state->selected == ssg::DiffFileId{"b"});
    ASSERT_TRUE(replayed.state->files == base.files);

    // A delta whose selection names no surviving file fails loud, never replaying a
    // dangling selection.
    auto dangling = delta;
    dangling.selected = ssg::DiffFileId{"ghost"};
    ASSERT_FALSE(codec.replay(base, dangling).accepted());
}

int main() {
    RUN(commandSetIsCompleteAndOrdered);
    RUN(cleanExternalEditAutoReloadsWithoutRecoveryStatus);
    RUN(aFailedCleanCommitRaisesTheConflictInsteadOfClearing);
    RUN(aSucceedingCleanCommitAdoptsTheDiskContent);
    RUN(aRenameRetiresPendingKeyedByThePreviousId);
    RUN(dirtyExternalEditPreservesBufferAndPublishesActions);
    RUN(openDiffIsObservationalAndKeepBufferAcknowledgesDisk);
    RUN(reloadIsReversibleAndRecordPrecedesBufferReplacement);
    RUN(ssgSaveAdvancesBaselineWithoutDuplicateStatus);
    RUN(genuineExternalEditIsNotConsumedBySaveCorrelation);
    RUN(staleEventIsFailureAtomic);
    RUN(diffRejectionDoesNotSuppressDirtyBufferSafetyStatus);
    RUN(viewDeltaReplaysAsTargetState);
    RUN(aFailedWorkspaceCommitLeavesThePendingActionRaised);
    RUN(aFailedKeepBufferCommitLeavesTheConflictRaised);
    RUN(keepBufferWithNoBaselineAdvancingCommitNeverClears);
    RUN(theExternalModSelectionFollowsTheListAndSurvivesResolves);
    RUN(aSelectionOnlyExternalDeltaReplaysToTheMovedSelection);
    return failed == 0 ? 0 : 1;
}
