#include "test_helpers.h"
#include <ssg/ExternalModificationFlow.h>

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

ssg::WatchEvent
event(std::uint64_t sequence,
      ssg::WatchEventOrigin origin = ssg::WatchEventOrigin::External) {
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
    ssg::Workspace workspace =
        ssg::Workspace::create(temporary.path(), recovery);
    ssg::DiffModel diff;
    ssg::ExternalModificationFlow flow{workspace, diff};
    ssg::FileDocumentId documentId;

    explicit Fixture(bool seedDiff = true) {
        std::ofstream{temporary.path() / "note.txt", std::ios::binary}
            << "base\n";
        const auto opened = workspace.openFile("note.txt");
        ASSERT_TRUE(opened.accepted());
        ASSERT_TRUE(opened.document.has_value());
        documentId = *opened.document;
        if (seedDiff) {
            ASSERT_TRUE(diff.seedNonGit({{ssg::DiffFileId{"note"}, "note.txt",
                                          "base\n"}},
                            std::uint64_t{1})
                        .accepted());
    }
    }

    void setContent(std::string content) {
        const auto snapshot = workspace.document(documentId).snapshot();
        ASSERT_TRUE(
            workspace
                .apply(documentId,
                       {snapshot.revision,
                        {{ssg::ByteOffset{0},
                          static_cast<std::uint64_t>(snapshot.text.size()),
                          std::move(content)}}})
                .accepted());
    }

    [[nodiscard]] ssg::DocumentSnapshot snapshot() const {
        return workspace.document(documentId).snapshot();
    }

    [[nodiscard]] std::optional<ssg::JournalDocument> journal() const {
        const auto state = workspace.state(documentId);
        const auto current = snapshot();
        return ssg::JournalDocument{state->key, current.mode, state->dirty,
                                    current.text};
    }
};

TEST(externalActionAffordanceReturnsLabelAndCommandForEachAction) {
    const auto reload =
        ssg::externalActionAffordance(ssg::ExternalAction::Reload);
    ASSERT_EQ(reload.label, std::string{"Reload"});
    ASSERT_EQ(reload.command, std::string{"external.reload"});

    const auto keepBuffer =
        ssg::externalActionAffordance(ssg::ExternalAction::KeepBuffer);
    ASSERT_EQ(keepBuffer.label, std::string{"Keep"});
    ASSERT_EQ(keepBuffer.command, std::string{"external.keep_buffer"});

    const auto openDiff =
        ssg::externalActionAffordance(ssg::ExternalAction::OpenDiff);
    ASSERT_EQ(openDiff.label, std::string{"Diff"});
    ASSERT_EQ(openDiff.command, std::string{"external.open_diff"});
}

TEST(cleanExternalEditAutoReloadsWithoutRecoveryStatus) {
    Fixture fixture;

    const auto result =
        fixture.flow.processEvent(input(2, "disk\n"), std::uint64_t{2});

    ASSERT_TRUE(result.accepted());
    ASSERT_FALSE(result.statusPublished);
    ASSERT_EQ(fixture.snapshot().text, "disk\n");
    ASSERT_FALSE(fixture.snapshot().dirty);
    ASSERT_TRUE(fixture.recovery.records().empty());
    ASSERT_TRUE(fixture.flow.viewState().files.empty());
}

TEST(dirtyExternalEditPreservesBufferAndPublishesActions) {
    Fixture fixture;
    fixture.setContent("buffer\n");

    const auto result =
        fixture.flow.processEvent(input(2, "disk\n"), std::uint64_t{2});
    const auto state = fixture.flow.viewState();

    ASSERT_TRUE(result.accepted());
    ASSERT_TRUE(result.statusPublished);
    ASSERT_EQ(fixture.snapshot().text, "buffer\n");
    ASSERT_TRUE(fixture.snapshot().dirty);
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
    fixture.setContent("buffer\n");
    ASSERT_TRUE(fixture.flow.processEvent(input(2, "disk\n"), std::uint64_t{2})
                    .accepted());
    const auto before = fixture.flow.viewState();

    const auto target = fixture.flow.openDiff(ssg::DiffFileId{"note"});
    ASSERT_TRUE(target.accepted());
    ASSERT_EQ(target.target->path, std::filesystem::path{"note.txt"});
    ASSERT_EQ(fixture.flow.viewState(), before);

    const auto kept = fixture.flow.keepBuffer(ssg::DiffFileId{"note"});
    ASSERT_TRUE(kept.accepted());
    ASSERT_EQ(fixture.snapshot().text, "buffer\n");
    ASSERT_TRUE(fixture.snapshot().dirty);
    ASSERT_TRUE(fixture.flow.viewState().files.empty());
    ASSERT_TRUE(fixture.recovery.records().empty());
}

TEST(reloadReplacesBufferWithoutRecoveryRecord) {
    Fixture fixture;
    fixture.setContent("buffer\n");
    ASSERT_TRUE(fixture.flow.processEvent(input(2, "disk\n"), std::uint64_t{2})
                    .accepted());
    auto open = fixture.journal();

    const auto reloaded =
        fixture.flow.reload(ssg::DiffFileId{"note"}, open);

    ASSERT_TRUE(reloaded.accepted());
    ASSERT_EQ(open->utf8Content, "disk\n");
    ASSERT_FALSE(open->dirty);
    ASSERT_TRUE(fixture.flow.viewState().files.empty());
    ASSERT_TRUE(fixture.recovery.records().empty());
}

TEST(ssgSaveAdvancesBaselineWithoutDuplicateStatus) {
    Fixture fixture;
    ASSERT_TRUE(
        fixture.workspace.reloadWithContent(fixture.documentId, "saved\n")
            .accepted());

    const auto saved = fixture.flow.processEvent(
        input(2, "saved\n", ssg::WatchEventOrigin::SsgSave), std::uint64_t{2});

    ASSERT_TRUE(saved.accepted());
    ASSERT_FALSE(saved.statusPublished);
    ASSERT_TRUE(fixture.flow.viewState().files.empty());
    ASSERT_EQ(fixture.snapshot().text, "saved\n");
}

TEST(genuineExternalEditIsNotConsumedBySaveCorrelation) {
    Fixture fixture;
    fixture.setContent("buffer\n");
    ASSERT_TRUE(
        fixture.flow
            .processEvent(input(2, "saved\n", ssg::WatchEventOrigin::SsgSave),
                          std::uint64_t{2})
                    .accepted());

    const auto external =
        fixture.flow.processEvent(input(3, "other\n"), std::uint64_t{3});

    ASSERT_TRUE(external.accepted());
    ASSERT_TRUE(external.statusPublished);
    ASSERT_EQ(fixture.snapshot().text, "buffer\n");
    ASSERT_EQ(fixture.flow.viewState().files.size(), 1U);
}

TEST(staleEventIsFailureAtomic) {
    Fixture fixture;
    fixture.setContent("buffer\n");
    ASSERT_TRUE(fixture.flow.processEvent(input(2, "disk\n"), std::uint64_t{2})
                    .accepted());
    const auto state = fixture.flow.viewState();
    const auto diff = fixture.diff.viewState();
    const auto before = fixture.snapshot();

    const auto stale =
        fixture.flow.processEvent(input(2, "stale\n"), std::uint64_t{3});

    ASSERT_FALSE(stale.accepted());
    ASSERT_EQ(stale.error, ssg::ExternalModificationError::StaleEvent);
    ASSERT_EQ(fixture.snapshot(), before);
    ASSERT_EQ(fixture.flow.viewState(), state);
    ASSERT_EQ(fixture.diff.viewState(), diff);
}

TEST(diffRejectionDoesNotSuppressDirtyBufferSafetyStatus) {
    Fixture fixture{false};
    fixture.setContent("buffer\n");

    const auto result =
        fixture.flow.processEvent(input(2, "disk\n"), std::uint64_t{2});

    ASSERT_TRUE(result.accepted());
    ASSERT_FALSE(result.diffRouted);
    ASSERT_TRUE(result.statusPublished);
    ASSERT_EQ(fixture.snapshot().text, "buffer\n");
    ASSERT_TRUE(fixture.snapshot().dirty);
    ASSERT_EQ(fixture.flow.viewState().files.size(), 1U);
}

TEST(aMissingWorkspaceDocumentLeavesThePendingActionRaised) {
    Fixture fixture;
    fixture.setContent("buffer\n");
    ASSERT_TRUE(fixture.flow.processEvent(input(2, "disk\n"), std::uint64_t{2})
                    .accepted());
    ASSERT_EQ(fixture.flow.viewState().files.size(), 1U);

    ASSERT_TRUE(fixture.workspace
                    .adoptExternalRename(fixture.documentId, "moved.txt",
                                         "disk\n", false)
                    .accepted());
    const auto failedReload =
        fixture.flow.resolveReload(ssg::DiffFileId{"note"});
    ASSERT_FALSE(failedReload.accepted());
    ASSERT_EQ(failedReload.error,
              ssg::ExternalModificationError::DocumentMissing);
    ASSERT_EQ(fixture.flow.viewState().files.size(), 1U);
}

TEST(resolveReloadCommitsCapturedContent) {
    Fixture fixture;
    fixture.setContent("buffer\n");
    ASSERT_TRUE(fixture.flow.processEvent(input(2, "disk\n"), std::uint64_t{2})
                    .accepted());

    std::ofstream{fixture.temporary.path() / "note.txt", std::ios::binary}
        << "newer\n";
    const auto ok = fixture.flow.resolveReload(ssg::DiffFileId{"note"});
    ASSERT_TRUE(ok.accepted());
    ASSERT_TRUE(fixture.flow.viewState().files.empty());
    ASSERT_EQ(fixture.snapshot().text, "disk\n");
    ASSERT_FALSE(fixture.snapshot().dirty);
}

}  // namespace

TEST(aRenameRetiresPendingKeyedByThePreviousId) {
    Fixture fixture;
    ASSERT_TRUE(fixture.diff
                    .seedNonGit({{ssg::DiffFileId{"note2"}, "renamed.txt",
                                  "base\n"}},
                                std::uint64_t{2})
                    .accepted());
    fixture.setContent("buffer\n");
    ASSERT_TRUE(fixture.flow.processEvent(input(3, "disk\n"), std::uint64_t{3})
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
    const auto result =
        fixture.flow.processEvent(std::move(renameInput), std::uint64_t{4});

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
    std::ofstream{temporary.path() / "a.txt", std::ios::binary} << "base\n";
    std::ofstream{temporary.path() / "b.txt", std::ios::binary} << "base\n";
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    const auto a = workspace.openFile("a.txt");
    const auto b = workspace.openFile("b.txt");
    ASSERT_TRUE(a.accepted() && a.document.has_value());
    ASSERT_TRUE(b.accepted() && b.document.has_value());
    ssg::DiffModel diff;
    ASSERT_TRUE(diff.seedNonGit({{ssg::DiffFileId{"a"}, "a.txt", "base\n"},
                                 {ssg::DiffFileId{"b"}, "b.txt", "base\n"}},
                                std::uint64_t{1})
                    .accepted());
    ssg::ExternalModificationFlow flow{workspace, diff};

    auto raise = [&](const char* id, const char* path, std::uint64_t sequence,
                     std::uint64_t revision) {
        const auto documentId =
            std::string_view{path} == "a.txt" ? *a.document : *b.document;
        const auto snapshot = workspace.document(documentId).snapshot();
        ASSERT_TRUE(
            workspace
                .apply(documentId,
                       {snapshot.revision,
                        {{ssg::ByteOffset{0},
                          static_cast<std::uint64_t>(snapshot.text.size()),
                          "buffer\n"}}})
                .accepted());
        ssg::WatchEvent e;
        e.kind = ssg::WatchEventKind::Modify;
        e.path = path;
        e.sequence = sequence;
        e.origin = ssg::WatchEventOrigin::External;
        ssg::ExternalEventInput in{e, ssg::DiffFileId{id}, "base\n",
                                   std::string{"disk\n"}};
        ASSERT_TRUE(flow.processEvent(std::move(in), revision).accepted());
    };

    // The section becomes non-empty: the selection homes to the first file.
    raise("a", "a.txt", 2, std::uint64_t{2});
    ASSERT_TRUE(flow.viewState().selected == ssg::DiffFileId{"a"});
    // Adding a file keeps the selection where it was.
    raise("b", "b.txt", 3, std::uint64_t{3});
    ASSERT_EQ(flow.viewState().files.size(), 2U);
    ASSERT_TRUE(flow.viewState().selected == ssg::DiffFileId{"a"});

    // The movers wrap around like the tree.
    ASSERT_TRUE(flow.selectNext());
    ASSERT_TRUE(flow.viewState().selected == ssg::DiffFileId{"b"});
    ASSERT_TRUE(flow.selectNext());
    ASSERT_TRUE(flow.viewState().selected == ssg::DiffFileId{"a"});

    // Resolving the selected file re-homes to the file that now occupies its
    // slot (the next file), not to nullopt while others remain.
    ASSERT_TRUE(flow.viewState().selected == ssg::DiffFileId{"a"});
    ASSERT_TRUE(flow.keepBuffer(ssg::DiffFileId{"a"}).accepted());
    ASSERT_EQ(flow.viewState().files.size(), 1U);
    ASSERT_TRUE(flow.viewState().selected == ssg::DiffFileId{"b"});

    // Resolving the last file clears the selection.
    ASSERT_TRUE(flow.keepBuffer(ssg::DiffFileId{"b"}).accepted());
    ASSERT_TRUE(flow.viewState().files.empty());
    ASSERT_FALSE(flow.viewState().selected.has_value());
}

SSG_TEST_SUITE(test_external_modification) {
    RUN(externalActionAffordanceReturnsLabelAndCommandForEachAction);
    RUN(cleanExternalEditAutoReloadsWithoutRecoveryStatus);
    RUN(aRenameRetiresPendingKeyedByThePreviousId);
    RUN(dirtyExternalEditPreservesBufferAndPublishesActions);
    RUN(openDiffIsObservationalAndKeepBufferAcknowledgesDisk);
    RUN(reloadReplacesBufferWithoutRecoveryRecord);
    RUN(ssgSaveAdvancesBaselineWithoutDuplicateStatus);
    RUN(genuineExternalEditIsNotConsumedBySaveCorrelation);
    RUN(staleEventIsFailureAtomic);
    RUN(diffRejectionDoesNotSuppressDirtyBufferSafetyStatus);
    RUN(aMissingWorkspaceDocumentLeavesThePendingActionRaised);
    RUN(resolveReloadCommitsCapturedContent);
    RUN(theExternalModSelectionFollowsTheListAndSurvivesResolves);
    return failed == 0 ? 0 : 1;
}
