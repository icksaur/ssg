#include "ssg/external_modification.h"
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
                ("ssg-external-" + std::to_string(++next_))} {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

    const std::filesystem::path& path() const { return path_; }

private:
    inline static std::uint64_t next_ = 0;
    std::filesystem::path path_;
};

ssg::JournalDocument document(std::string text, bool dirty) {
    return {ssg::JournalDocumentKey::saved("note.txt"),
            ssg::DocumentMode::edit, dirty, std::move(text)};
}

ssg::WatchEvent event(std::uint64_t sequence,
                      ssg::WatchEventOrigin origin =
                          ssg::WatchEventOrigin::external) {
    ssg::WatchEvent result;
    result.kind = ssg::WatchEventKind::modify;
    result.path = "note.txt";
    result.sequence = sequence;
    result.origin = origin;
    return result;
}

ssg::ExternalEventInput input(std::uint64_t sequence, std::string content,
                              ssg::WatchEventOrigin origin =
                                  ssg::WatchEventOrigin::external) {
    return {event(sequence, origin), ssg::DiffFileId{"note"}, std::move(content)};
}

struct Fixture {
    TemporaryDirectory temporary;
    ssg::RecoveryActions recovery =
        ssg::RecoveryActions::create(temporary.path() / "recovery");
    ssg::DiffModel diff;
    ssg::ExternalModificationFlow flow{recovery, diff};

    Fixture() {
        ASSERT_TRUE(diff.seed_non_git(
                            {{ssg::DiffFileId{"note"}, "note.txt", "base\n"}},
                            ssg::Revision{1})
                        .accepted());
    }
};

TEST(command_set_is_complete_and_ordered) {
    const auto descriptors = ssg::external_modification_command_set().descriptors();
    ASSERT_EQ(descriptors.size(), 3U);
    ASSERT_EQ(descriptors[0].id, "external.reload");
    ASSERT_EQ(descriptors[1].id, "external.keep_buffer");
    ASSERT_EQ(descriptors[2].id, "external.open_diff");
}

TEST(clean_external_edit_auto_reloads_without_recovery_status) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("base\n", false)};

    const auto result =
        fixture.flow.process_event(input(2, "disk\n"), open);

    ASSERT_TRUE(result.accepted());
    ASSERT_FALSE(result.status_published);
    ASSERT_EQ(open->utf8_content, "disk\n");
    ASSERT_FALSE(open->dirty);
    ASSERT_TRUE(fixture.recovery.records().empty());
    ASSERT_TRUE(fixture.flow.view_state().files.empty());
}

TEST(dirty_external_edit_preserves_buffer_and_publishes_actions) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};

    const auto result =
        fixture.flow.process_event(input(2, "disk\n"), open);
    const auto state = fixture.flow.view_state();

    ASSERT_TRUE(result.accepted());
    ASSERT_TRUE(result.status_published);
    ASSERT_EQ(open->utf8_content, "buffer\n");
    ASSERT_TRUE(open->dirty);
    ASSERT_EQ(state.files.size(), 1U);
    ASSERT_EQ(state.files[0].status,
              ssg::ExternalDocumentStatus::externally_modified);
    ASSERT_EQ(state.files[0].actions.size(), 3U);
    ASSERT_EQ(state.files[0].actions[0], ssg::ExternalAction::reload);
    ASSERT_EQ(state.files[0].actions[1], ssg::ExternalAction::keep_buffer);
    ASSERT_EQ(state.files[0].actions[2], ssg::ExternalAction::open_diff);
}

TEST(open_diff_is_observational_and_keep_buffer_acknowledges_disk) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    ASSERT_TRUE(fixture.flow.process_event(input(2, "disk\n"), open).accepted());
    const auto before = fixture.flow.view_state();

    const auto target = fixture.flow.open_diff(ssg::DiffFileId{"note"});
    ASSERT_TRUE(target.accepted());
    ASSERT_EQ(target.target->path, std::filesystem::path{"note.txt"});
    ASSERT_EQ(fixture.flow.view_state(), before);

    const auto kept = fixture.flow.keep_buffer(ssg::DiffFileId{"note"});
    ASSERT_TRUE(kept.accepted());
    ASSERT_EQ(open->utf8_content, "buffer\n");
    ASSERT_TRUE(open->dirty);
    ASSERT_TRUE(fixture.flow.view_state().files.empty());
    ASSERT_TRUE(fixture.recovery.records().empty());
}

TEST(reload_is_reversible_and_record_precedes_buffer_replacement) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    ASSERT_TRUE(fixture.flow.process_event(input(2, "disk\n"), open).accepted());

    const auto reloaded =
        fixture.flow.reload(ssg::DiffFileId{"note"}, open);

    ASSERT_TRUE(reloaded.accepted());
    ASSERT_TRUE(reloaded.compensation.has_value());
    ASSERT_EQ(open->utf8_content, "disk\n");
    ASSERT_FALSE(open->dirty);
    ASSERT_TRUE(fixture.flow.view_state().files.empty());
    ASSERT_EQ(fixture.recovery.records().size(), 1U);

    const auto restored =
        fixture.recovery.restore_document(*reloaded.compensation, open);
    ASSERT_TRUE(restored.accepted());
    ASSERT_EQ(open->utf8_content, "buffer\n");
    ASSERT_TRUE(open->dirty);
}

TEST(ssg_save_advances_baseline_without_duplicate_status) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("saved\n", false)};

    const auto saved = fixture.flow.process_event(
        input(2, "saved\n", ssg::WatchEventOrigin::ssg_save), open);

    ASSERT_TRUE(saved.accepted());
    ASSERT_FALSE(saved.status_published);
    ASSERT_TRUE(fixture.flow.view_state().files.empty());
    ASSERT_EQ(open->utf8_content, "saved\n");
}

TEST(genuine_external_edit_is_not_consumed_by_save_correlation) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    ASSERT_TRUE(fixture.flow
                    .process_event(
                        input(2, "saved\n", ssg::WatchEventOrigin::ssg_save),
                        open)
                    .accepted());

    const auto external =
        fixture.flow.process_event(input(3, "other\n"), open);

    ASSERT_TRUE(external.accepted());
    ASSERT_TRUE(external.status_published);
    ASSERT_EQ(open->utf8_content, "buffer\n");
    ASSERT_EQ(fixture.flow.view_state().files.size(), 1U);
}

TEST(stale_event_is_failure_atomic) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    ASSERT_TRUE(fixture.flow.process_event(input(2, "disk\n"), open).accepted());
    const auto state = fixture.flow.view_state();
    const auto diff = fixture.diff.view_state();
    const auto before = open;

    const auto stale =
        fixture.flow.process_event(input(2, "stale\n"), open);

    ASSERT_FALSE(stale.accepted());
    ASSERT_EQ(stale.error, ssg::ExternalModificationError::stale_event);
    ASSERT_EQ(open, before);
    ASSERT_EQ(fixture.flow.view_state(), state);
    ASSERT_EQ(fixture.diff.view_state(), diff);
}

TEST(diff_rejection_does_not_suppress_dirty_buffer_safety_status) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / "recovery");
    ssg::DiffModel diff;
    ssg::ExternalModificationFlow flow{recovery, diff};
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};

    const auto result = flow.process_event(input(2, "disk\n"), open);

    ASSERT_TRUE(result.accepted());
    ASSERT_FALSE(result.diff_routed);
    ASSERT_TRUE(result.status_published);
    ASSERT_EQ(open->utf8_content, "buffer\n");
    ASSERT_TRUE(open->dirty);
    ASSERT_EQ(flow.view_state().files.size(), 1U);
}

TEST(view_delta_replays_as_target_state) {
    Fixture fixture;
    std::optional<ssg::JournalDocument> open{document("buffer\n", true)};
    const auto base = fixture.flow.view_state();
    ASSERT_TRUE(fixture.flow.process_event(input(2, "disk\n"), open).accepted());
    const auto target = fixture.flow.view_state();

    const auto delta = ssg::derive_external_modification_delta(base, target);
    const auto replayed = ssg::replay_external_modification_delta(base, delta);

    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(*replayed.state, target);
    const auto stale = ssg::replay_external_modification_delta(target, delta);
    ASSERT_FALSE(stale.accepted());
    ASSERT_EQ(stale.error, ssg::ExternalDeltaError::stale_revision);
}

}  // namespace

int main() {
    RUN(command_set_is_complete_and_ordered);
    RUN(clean_external_edit_auto_reloads_without_recovery_status);
    RUN(dirty_external_edit_preserves_buffer_and_publishes_actions);
    RUN(open_diff_is_observational_and_keep_buffer_acknowledges_disk);
    RUN(reload_is_reversible_and_record_precedes_buffer_replacement);
    RUN(ssg_save_advances_baseline_without_duplicate_status);
    RUN(genuine_external_edit_is_not_consumed_by_save_correlation);
    RUN(stale_event_is_failure_atomic);
    RUN(diff_rejection_does_not_suppress_dirty_buffer_safety_status);
    RUN(view_delta_replays_as_target_state);
    return failed == 0 ? 0 : 1;
}
