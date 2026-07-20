#include "ssg/follow_edits.h"
#include "test_helpers.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ssg;

DiffFileView changed_file(std::string id, std::filesystem::path path,
                          std::size_t newest_line, bool deleted = false) {
    DiffFileView view{DiffFileId{std::move(id)}};
    view.path = std::move(path);
    view.deleted = deleted;
    view.hunks.push_back(
        {.baseline_start = newest_line,
         .target_start = newest_line,
         .baseline_lines = {"old\n"},
         .target_lines = deleted ? std::vector<std::string>{}
                                 : std::vector<std::string>{"new\n"}});
    return view;
}

DiffViewState current_diff(std::initializer_list<DiffFileView> files,
                           std::uint64_t revision) {
    return {Revision{revision}, files};
}

std::vector<std::string> split(std::string_view line) {
    std::vector<std::string> fields;
    std::istringstream input{std::string{line}};
    for (std::string field; std::getline(input, field, '\t');) {
        fields.push_back(std::move(field));
    }
    return fields;
}

std::string target_id(const std::optional<FollowTarget>& target) {
    return target ? target->id.value() : "-";
}

std::string queue_ids(const FollowEditsViewState& state) {
    if (state.queued_targets.empty()) {
        return "-";
    }
    std::string result;
    for (const auto& target : state.queued_targets) {
        if (!result.empty()) {
            result += ",";
        }
        result += target.id.value();
    }
    return result;
}

std::string row_for(const FollowEditsViewState& state, std::uint64_t client) {
    for (const auto& view : state.clients) {
        if (view.client == ClientId{client}) {
            return std::to_string(view.offset.first_row);
        }
    }
    return "-";
}

TEST(independent_transition_table_covers_shared_follow_policy) {
    std::ifstream input{
        std::filesystem::path{SSG_FOLLOW_EDITS_FIXTURE_DIR} / "transitions.tsv"};
    ASSERT_TRUE(input.good());

    FollowEditsModel model{{.queue_capacity = 4,
                            .resume_binding = "Ctrl+Shift+F"}};
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto fields = split(line);
        ASSERT_EQ(fields.size(), std::size_t{8});
        const auto& operation = fields[0];
        if (operation == "attach") {
            ASSERT_TRUE(model.attach_client(
                                 ClientId{std::stoull(fields[1])},
                                 ViewportDimensions{80,
                                     static_cast<std::uint32_t>(
                                         std::stoul(fields[2]))})
                            .accepted());
        } else if (operation == "change") {
            const auto separator = fields[1].find(':');
            const auto id = fields[1].substr(0, separator);
            const auto line_number =
                std::stoull(fields[1].substr(separator + 1));
            ASSERT_TRUE(model
                            .accept_external_change(
                                changed_file(id, id + ".txt", line_number),
                                Revision{std::stoull(fields[2])})
                            .accepted());
        } else if (operation == "navigate_user" ||
                   operation == "navigate_programmatic" ||
                   operation == "navigate_non_navigation") {
            auto classification = NavigationClass::User;
            if (operation == "navigate_programmatic") {
                classification = NavigationClass::Programmatic;
            } else if (operation == "navigate_non_navigation") {
                classification = NavigationClass::NonNavigation;
            }
            ASSERT_TRUE(model
                            .apply_navigation(
                                {.client = ClientId{std::stoull(fields[1])},
                                 .classification = classification,
                                 .offset = FollowScrollOffset{
                                     std::stoull(fields[2]), 0}})
                            .accepted());
        } else if (operation == "resume") {
            ASSERT_TRUE(model
                            .resume(current_diff(
                                {changed_file("a", "a.txt", 20),
                                 changed_file("b", "b.txt", 30)},
                                3))
                            .accepted());
        } else if (operation == "pause") {
            ASSERT_TRUE(model.pause().accepted());
        } else {
            throw std::runtime_error{"unknown transition operation"};
        }

        const auto state = model.view_state();
        ASSERT_EQ(state.mode == FollowMode::Following ? "following" : "paused",
                  fields[3]);
        ASSERT_EQ(target_id(state.active_target), fields[4]);
        ASSERT_EQ(queue_ids(state), fields[5]);
        ASSERT_EQ(row_for(state, 1), fields[6]);
        ASSERT_EQ(row_for(state, 2), fields[7]);
    }
}

TEST(dirty_conflict_uses_disk_diff_target_without_buffer_policy) {
    FollowEditsModel model;
    ASSERT_TRUE(model
                    .accept_external_change(
                        changed_file("dirty", "dirty.txt", 6), Revision{1})
                    .accepted());
    ASSERT_EQ(target_id(model.view_state().active_target), "dirty");
}

TEST(queue_is_bounded_and_same_file_replaces_in_place) {
    FollowEditsModel model{{.queue_capacity = 2,
                            .resume_binding = "Ctrl+Shift+F"}};
    ASSERT_TRUE(model.pause().accepted());
    ASSERT_TRUE(model
                    .accept_external_change(changed_file("a", "a", 1),
                                            Revision{1})
                    .accepted());
    ASSERT_TRUE(model
                    .accept_external_change(changed_file("b", "b", 2),
                                            Revision{2})
                    .accepted());
    ASSERT_TRUE(model
                    .accept_external_change(changed_file("a", "renamed-a", 9),
                                            Revision{3})
                    .accepted());

    const auto state = model.view_state();
    ASSERT_EQ(queue_ids(state), "b,a");
    ASSERT_EQ(state.queued_targets.back().path,
              std::filesystem::path{"renamed-a"});

    ASSERT_TRUE(model
                    .accept_external_change(changed_file("c", "c", 4),
                                            Revision{4})
                    .accepted());
    ASSERT_EQ(queue_ids(model.view_state()), "a,c");
}

TEST(resume_resolves_rename_delete_and_skips_reverted_or_missing_targets) {
    FollowEditsModel model;
    ASSERT_TRUE(model.pause().accepted());
    ASSERT_TRUE(model
                    .accept_external_change(changed_file("rename", "old", 1),
                                            Revision{1})
                    .accepted());
    ASSERT_TRUE(model
                    .accept_external_change(changed_file("revert", "revert", 2),
                                            Revision{2})
                    .accepted());
    ASSERT_TRUE(model
                    .accept_external_change(changed_file("delete", "gone", 3),
                                            Revision{3})
                    .accepted());

    auto renamed = changed_file("rename", "new", 7);
    renamed.previous_path = "old";
    auto deleted = changed_file("delete", "gone", 8, true);
    deleted.previous_path = "gone";
    ASSERT_TRUE(
        model.resume(current_diff({renamed, deleted}, 4)).accepted());
    ASSERT_EQ(target_id(model.view_state().active_target), "delete");
    ASSERT_TRUE(model.view_state().active_target->deleted);

    ASSERT_TRUE(model.pause().accepted());
    ASSERT_TRUE(model
                    .accept_external_change(changed_file("missing", "x", 1),
                                            Revision{4})
                    .accepted());
    const auto before = model.view_state();
    ASSERT_TRUE(model.resume(current_diff({}, 5)).accepted());
    const auto after = model.view_state();
    ASSERT_EQ(after.mode, FollowMode::Following);
    ASSERT_EQ(after.active_target, before.active_target);
    ASSERT_EQ(after.clients, before.clients);
    ASSERT_TRUE(after.queued_targets.empty());
}

TEST(stale_changes_and_invalid_clients_are_failure_atomic) {
    FollowEditsModel model;
    ASSERT_TRUE(model
                    .accept_external_change(changed_file("a", "a", 1),
                                            Revision{2})
                    .accepted());
    const auto before = model.view_state();
    ASSERT_EQ(model
                  .accept_external_change(changed_file("b", "b", 2),
                                          Revision{2})
                  .error,
              FollowEditsError::StaleRevision);
    ASSERT_EQ(model.view_state(), before);

    ASSERT_EQ(model
                  .apply_navigation(
                      {.client = ClientId{99},
                       .classification = NavigationClass::User,
                       .offset = FollowScrollOffset{3, 0}})
                  .error,
              FollowEditsError::UnknownClient);
    ASSERT_EQ(model.view_state(), before);

    ASSERT_TRUE(model.pause().accepted());
    const auto paused = model.view_state();
    ASSERT_EQ(model.resume(current_diff({}, 1)).error,
              FollowEditsError::StaleRevision);
    ASSERT_EQ(model.view_state(), paused);
}

TEST(command_view_delta_and_footer_are_complete) {
    const auto commands = follow_edits_command_set().descriptors();
    ASSERT_EQ(commands.size(), std::size_t{2});
    ASSERT_EQ(commands[0].id, "follow_edits.resume");
    ASSERT_EQ(commands[1].id, "follow_edits.pause");

    FollowEditsModel model{{.queue_capacity = 2,
                            .resume_binding = "Ctrl+Shift+F"}};
    const auto before = model.view_state();
    ASSERT_TRUE(model.pause().accepted());
    const auto after = model.view_state();
    const auto delta = derive_follow_edits_delta(before, after);
    ASSERT_EQ(delta.base_generation, before.generation);
    ASSERT_EQ(delta.generation, after.generation);
    ASSERT_EQ(delta.replacement, after);

    const auto footer = model.footer_projection();
    ASSERT_EQ(footer.mode, "paused");
    ASSERT_EQ(footer.resume_binding, std::optional<std::string>{"Ctrl+Shift+F"});
    ASSERT_EQ(footer.resume_command,
              std::optional<std::string>{"follow_edits.resume"});
}

TEST(configuration_rejects_invalid_queue_capacity) {
    ASSERT_THROWS(FollowEditsModel(
                      {.queue_capacity = 0,
                       .resume_binding = "Ctrl+Shift+F"}),
                  std::invalid_argument);
}

}  // namespace

int main() {
    RUN(independent_transition_table_covers_shared_follow_policy);
    RUN(dirty_conflict_uses_disk_diff_target_without_buffer_policy);
    RUN(queue_is_bounded_and_same_file_replaces_in_place);
    RUN(resume_resolves_rename_delete_and_skips_reverted_or_missing_targets);
    RUN(stale_changes_and_invalid_clients_are_failure_atomic);
    RUN(command_view_delta_and_footer_are_complete);
    RUN(configuration_rejects_invalid_queue_capacity);
    return failed == 0 ? 0 : 1;
}
