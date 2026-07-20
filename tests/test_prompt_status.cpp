#include "ssg/prompt.h"
#include "ssg/status.h"
#include "test_helpers.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace ssg;

std::string read_fixture(const std::string& relative) {
    std::ifstream input(std::filesystem::path(SSG_SOURCE_DIR) / relative);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

std::string rect_text(const Rect& rect) {
    return std::to_string(rect.x) + "," + std::to_string(rect.y) + "," +
           std::to_string(rect.width) + "," + std::to_string(rect.height);
}

std::string kind_text(PromptKind kind) {
    switch (kind) {
    case PromptKind::Path: return "path";
    case PromptKind::Find: return "find";
    case PromptKind::Replace: return "replace";
    case PromptKind::Settings: return "settings";
    case PromptKind::CommandArgument: return "command_argument";
    }
    return "";
}

std::string control_kind_text(PromptControlKind kind) {
    switch (kind) {
    case PromptControlKind::Input: return "input";
    case PromptControlKind::Toggle: return "toggle";
    case PromptControlKind::Count: return "count";
    }
    return "";
}

PromptRequest request(PromptKind kind) {
    PromptRequest value;
    value.kind = kind;
    value.accessible_label =
        kind == PromptKind::Path ? "Path" :
        kind == PromptKind::Replace ? "Replace" : "Find";
    value.inputs.push_back(
        {kind == PromptKind::Path ? "path" : "find",
         kind == PromptKind::Path ? "Path" : "Find text", "needle"});
    if (kind == PromptKind::Replace) {
        value.inputs.push_back({"replace", "Replacement text", "value"});
    }
    if (kind == PromptKind::Find || kind == PromptKind::Replace) {
        value.toggles.push_back({"case", "Case sensitive", false, 6});
        value.toggles.push_back({"word", "Whole word", true, 7});
        value.match_count = PromptMatchCount{"matches", "Match count", "2/7"};
    }
    return value;
}

std::string geometry_line(PromptKind kind) {
    PromptSurface surface;
    const auto opened = surface.open(request(kind));
    ASSERT_TRUE(opened.accepted());
    const auto rows = prompt_row_count(kind);
    const auto layout =
        compute_prompt_layout(surface, Rect{0, 4, 20, static_cast<int>(rows)});
    ASSERT_TRUE(layout.accepted());

    std::string line = kind_text(kind) + "|" + rect_text(layout.view->rect) + "|";
    for (std::size_t i = 0; i < layout.view->controls.size(); ++i) {
        const auto& control = layout.view->controls[i];
        if (i != 0) {
            line += ";";
        }
        line += control_kind_text(control.kind) + ":" + control.id + ":" +
                rect_text(control.rect);
    }
    return line;
}

TEST(prompt_geometry_matches_golden) {
    const std::string actual =
        geometry_line(PromptKind::Path) + "\n" +
        geometry_line(PromptKind::Find) + "\n" +
        geometry_line(PromptKind::Replace) + "\n";
    ASSERT_EQ(actual,
              read_fixture("tests/fixtures/prompt_status/geometry.txt"));
}

TEST(prompt_rows_and_invalid_reservation_are_typed) {
    ASSERT_EQ(prompt_row_count(PromptKind::Path), std::uint8_t{1});
    ASSERT_EQ(prompt_row_count(PromptKind::Find), std::uint8_t{2});
    ASSERT_EQ(prompt_row_count(PromptKind::Replace), std::uint8_t{3});
    ASSERT_EQ(prompt_row_count(PromptKind::Settings), std::uint8_t{1});
    ASSERT_EQ(prompt_row_count(PromptKind::CommandArgument), std::uint8_t{1});
    ASSERT_EQ(prompt_row_count(PromptKind::Palette), std::uint8_t{0});

    PromptSurface surface;
    ASSERT_TRUE(surface.open(request(PromptKind::Find)).accepted());
    const auto bad = compute_prompt_layout(surface, Rect{0, 0, 20, 1});
    ASSERT_FALSE(bad.accepted());
    ASSERT_EQ(bad.error->code, PromptErrorCode::InvalidReservation);
}

TEST(prompt_submit_and_cancel_are_non_modal) {
    PromptSurface surface;
    ASSERT_TRUE(surface.open(request(PromptKind::Replace)).accepted());
    const auto submitted = surface.submit();
    ASSERT_TRUE(submitted.accepted());
    ASSERT_TRUE(submitted.submission.has_value());
    ASSERT_EQ(submitted.submission->values,
              (std::vector<std::string>{"needle", "value"}));
    ASSERT_FALSE(surface.active());

    ASSERT_TRUE(surface.open(request(PromptKind::Path)).accepted());
    const auto cancelled = surface.cancel();
    ASSERT_TRUE(cancelled.accepted());
    ASSERT_FALSE(cancelled.submission.has_value());
    ASSERT_FALSE(surface.active());
}

StatusItem status(std::uint64_t id, StatusPriority priority,
                  std::string text, std::vector<StatusAction> actions = {}) {
    return StatusItem{StatusId{id}, priority, std::move(text),
                      std::move(actions)};
}

std::vector<std::uint64_t> ids(const StatusViewState& view) {
    std::vector<std::uint64_t> values;
    for (const auto& item : view.items) {
        values.push_back(item.id.value());
    }
    return values;
}

TEST(status_priority_and_navigation_transition_table) {
    StatusQueue queue;
    ASSERT_TRUE(queue.enqueue(status(1, StatusPriority::Information, "one")).accepted);
    ASSERT_TRUE(queue.enqueue(status(2, StatusPriority::Warning, "two")).accepted);
    ASSERT_TRUE(queue.enqueue(status(3, StatusPriority::Error, "three")).accepted);
    ASSERT_TRUE(queue.enqueue(status(4, StatusPriority::Warning, "four")).accepted);

    ASSERT_EQ(ids(queue.view_state()),
              (std::vector<std::uint64_t>{3, 2, 4, 1}));
    auto view = queue.view_state();
    ASSERT_EQ(view.selected, std::size_t{0});
    queue.next();
    view = queue.view_state();
    ASSERT_EQ(view.selected, std::size_t{1});
    queue.previous();
    view = queue.view_state();
    ASSERT_EQ(view.selected, std::size_t{0});
    queue.previous();
    view = queue.view_state();
    ASSERT_EQ(view.selected, std::size_t{3});
    queue.dismiss();
    ASSERT_EQ(ids(queue.view_state()),
              (std::vector<std::uint64_t>{3, 2, 4}));
    view = queue.view_state();
    ASSERT_EQ(view.selected, std::size_t{2});
}

TEST(status_capacity_admission_and_eviction_table) {
    StatusQueue queue;
    for (std::uint64_t id = 1; id <= StatusQueue::capacity; ++id) {
        ASSERT_TRUE(queue.enqueue(
            status(id, StatusPriority::Information, std::to_string(id))).accepted);
    }
    const auto rejected =
        queue.enqueue(status(17, StatusPriority::Progress, "rejected"));
    ASSERT_FALSE(rejected.accepted);
    auto view = queue.view_state();
    ASSERT_EQ(view.items.size(), StatusQueue::capacity);

    const auto admitted =
        queue.enqueue(status(18, StatusPriority::Error, "admitted"));
    ASSERT_TRUE(admitted.accepted);
    ASSERT_EQ(admitted.evicted, std::optional<StatusId>{StatusId{1}});
    view = queue.view_state();
    ASSERT_EQ(view.items.front().id, StatusId{18});
}

TEST(status_stale_actions_are_rejected_without_mutation) {
    StatusQueue queue;
    StatusAction retry{"retry", "Retry build", "build.retry"};
    auto first = queue.enqueue(
        status(7, StatusPriority::Error, "Build failed", {retry}));
    const StatusActionInvocation stale{StatusId{7}, "retry", first.generation};
    queue.dismiss();
    auto replacement = queue.enqueue(
        status(7, StatusPriority::Error, "Build failed again", {retry}));
    ASSERT_NE(first.generation, replacement.generation);
    const auto before = queue.view_state();
    const auto rejected = queue.invoke_action(stale);
    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error, StatusActionError::Stale);
    ASSERT_EQ(queue.view_state(), before);

    const StatusActionInvocation current{
        StatusId{7}, "retry", replacement.generation};
    const auto invoked = queue.invoke_action(current);
    ASSERT_TRUE(invoked.accepted());
    ASSERT_EQ(invoked.command_id, std::optional<std::string>{"build.retry"});
}

TEST(footer_projection_and_accessibility_match_golden) {
    PromptSurface prompt;
    ASSERT_TRUE(prompt.open(request(PromptKind::Find)).accepted());
    const auto layout = compute_prompt_layout(prompt, Rect{0, 4, 20, 2});
    StatusQueue queue;
    std::vector<StatusAction> actions{
        {"retry", "Retry build", "build.retry"},
        {"log", "Open log", "log.open"}};
    ASSERT_TRUE(queue.enqueue(
        status(9, StatusPriority::Error, "Build failed", actions)).accepted);
    const auto footer = queue.footer_projection();
    ASSERT_EQ(footer.value, std::string{"Build failed 1/1"});
    ASSERT_EQ(footer.actions[0].id, std::string{"retry"});
    ASSERT_EQ(footer.actions[1].accessible_label, std::string{"Open log"});

    std::string actual = "prompt|" + layout.view->accessible_label + "\n";
    for (const auto& control : layout.view->controls) {
        actual += control_kind_text(control.kind) + "|" +
                  control.accessible_label + "\n";
    }
    const auto status_view = queue.view_state();
    actual += "status|" + status_view.items[0].accessible_label + "\n";
    for (const auto& action : footer.actions) {
        actual += "action|" + action.accessible_label + "\n";
    }
    ASSERT_EQ(actual,
              read_fixture("tests/fixtures/prompt_status/accessibility.txt"));
}

TEST(command_catalog_and_delta_are_exact) {
    const PromptStatusCommandSet commands;
    const std::vector<std::string_view> expected{
        "prompt.submit", "prompt.cancel", "status.next", "status.previous",
        "status.dismiss", "status.invoke_action"};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQ(commands.descriptors[i].id, expected[i]);
    }

    const PromptStatusViewState before{};
    const auto unchanged = derive_prompt_status_delta(before, before);
    ASSERT_FALSE(unchanged.changed);
    ASSERT_FALSE(unchanged.replacement.has_value());
    PromptStatusViewState after{};
    after.status.items.push_back(
        StatusItemView{StatusId{1}, StatusPriority::Information, 1, "ready"});
    const auto changed = derive_prompt_status_delta(before, after);
    ASSERT_TRUE(changed.changed);
    ASSERT_EQ(changed.replacement, std::optional<PromptStatusViewState>{after});
}

} // namespace

int main() {
    RUN(prompt_geometry_matches_golden);
    RUN(prompt_rows_and_invalid_reservation_are_typed);
    RUN(prompt_submit_and_cancel_are_non_modal);
    RUN(status_priority_and_navigation_transition_table);
    RUN(status_capacity_admission_and_eviction_table);
    RUN(status_stale_actions_are_rejected_without_mutation);
    RUN(footer_projection_and_accessibility_match_golden);
    RUN(command_catalog_and_delta_are_exact);
    return failed == 0 ? 0 : 1;
}
