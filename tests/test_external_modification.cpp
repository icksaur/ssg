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
    return {event(sequence, origin), ssg::DiffFileId{"note"}, std::move(content)};
}

struct Fixture {
    TemporaryDirectory temporary;
    ssg::RecoveryActions recovery =
        ssg::RecoveryActions::create(temporary.path() / "recovery");
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
        fixture.flow.processEvent(input(2, "disk\n"), open);

    ASSERT_TRUE(result.accepted());
    ASSERT_FALSE(result.statusPublished);
    ASSERT_EQ(open->utf8Content, "disk\n");
    ASSERT_FALSE(open->dirty);
    ASSERT_TRUE(fixture.recovery.records().empty());
    ASSERT_TRUE(fixture.flow.viewState().files.empty());
}

TEST(dirtyExternalEditPreservesBufferAndPublishesActions) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};

    const auto result =
        fixture.flow.processEvent(input(2, "disk\n"), open);
    const auto state = fixture.flow.viewState();

    ASSERT_TRUE(result.accepted());
    ASSERT_TRUE(result.statusPublished);
    ASSERT_EQ(open->utf8Content, "buffer\n");
    ASSERT_TRUE(open->dirty);
    ASSERT_EQ(state.files.size(), 1U);
    ASSERT_EQ(state.files[0].status,
              ssg::ExternalDocumentStatus::ExternallyModified);
    ASSERT_EQ(state.files[0].actions.size(), 3U);
    ASSERT_EQ(state.files[0].actions[0], ssg::ExternalAction::Reload);
    ASSERT_EQ(state.files[0].actions[1], ssg::ExternalAction::KeepBuffer);
    ASSERT_EQ(state.files[0].actions[2], ssg::ExternalAction::OpenDiff);
}

TEST(openDiffIsObservationalAndKeepBufferAcknowledgesDisk) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    ASSERT_TRUE(fixture.flow.processEvent(input(2, "disk\n"), open).accepted());
    const auto before = fixture.flow.viewState();

    const auto target = fixture.flow.openDiff(ssg::DiffFileId{"note"});
    ASSERT_TRUE(target.accepted());
    ASSERT_EQ(target.target->path, std::filesystem::path{"note.txt"});
    ASSERT_EQ(fixture.flow.viewState(), before);

    const auto kept = fixture.flow.keepBuffer(ssg::DiffFileId{"note"});
    ASSERT_TRUE(kept.accepted());
    ASSERT_EQ(open->utf8Content, "buffer\n");
    ASSERT_TRUE(open->dirty);
    ASSERT_TRUE(fixture.flow.viewState().files.empty());
    ASSERT_TRUE(fixture.recovery.records().empty());
}

TEST(reloadIsReversibleAndRecordPrecedesBufferReplacement) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    ASSERT_TRUE(fixture.flow.processEvent(input(2, "disk\n"), open).accepted());

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
        input(2, "saved\n", ssg::WatchEventOrigin::SsgSave), open);

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
                        open)
                    .accepted());

    const auto external =
        fixture.flow.processEvent(input(3, "other\n"), open);

    ASSERT_TRUE(external.accepted());
    ASSERT_TRUE(external.statusPublished);
    ASSERT_EQ(open->utf8Content, "buffer\n");
    ASSERT_EQ(fixture.flow.viewState().files.size(), 1U);
}

TEST(staleEventIsFailureAtomic) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    ASSERT_TRUE(fixture.flow.processEvent(input(2, "disk\n"), open).accepted());
    const auto state = fixture.flow.viewState();
    const auto diff = fixture.diff.viewState();
    const auto before = open;

    const auto stale =
        fixture.flow.processEvent(input(2, "stale\n"), open);

    ASSERT_FALSE(stale.accepted());
    ASSERT_EQ(stale.error, ssg::ExternalModificationError::StaleEvent);
    ASSERT_EQ(open, before);
    ASSERT_EQ(fixture.flow.viewState(), state);
    ASSERT_EQ(fixture.diff.viewState(), diff);
}

TEST(diffRejectionDoesNotSuppressDirtyBufferSafetyStatus) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / "recovery");
    ssg::DiffModel diff;
    ssg::ExternalModificationFlow flow{recovery, diff};
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};

    const auto result = flow.processEvent(input(2, "disk\n"), open);

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
    ASSERT_TRUE(fixture.flow.processEvent(input(2, "disk\n"), open).accepted());
    const auto target = fixture.flow.viewState();

    const auto delta = ssg::ExternalModificationDeltaCodec{}.derive(base, target);
    const auto replayed = ssg::ExternalModificationDeltaCodec{}.replay(base, delta);

    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(*replayed.state, target);
    const auto stale = ssg::ExternalModificationDeltaCodec{}.replay(target, delta);
    ASSERT_FALSE(stale.accepted());
    ASSERT_EQ(stale.error, ssg::ExternalDeltaError::StaleRevision);
}

}  // namespace

int main() {
    RUN(commandSetIsCompleteAndOrdered);
    RUN(cleanExternalEditAutoReloadsWithoutRecoveryStatus);
    RUN(dirtyExternalEditPreservesBufferAndPublishesActions);
    RUN(openDiffIsObservationalAndKeepBufferAcknowledgesDisk);
    RUN(reloadIsReversibleAndRecordPrecedesBufferReplacement);
    RUN(ssgSaveAdvancesBaselineWithoutDuplicateStatus);
    RUN(genuineExternalEditIsNotConsumedBySaveCorrelation);
    RUN(staleEventIsFailureAtomic);
    RUN(diffRejectionDoesNotSuppressDirtyBufferSafetyStatus);
    RUN(viewDeltaReplaysAsTargetState);
    return failed == 0 ? 0 : 1;
}
