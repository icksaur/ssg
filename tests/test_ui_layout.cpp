#include "ssg/ui_layout.h"
#include "test_helpers.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace ssg;

bool overlaps(const Rect& a, const Rect& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width &&
           a.y < b.y + b.height && b.y < a.y + a.height;
}

void assert_rect(Rect actual, Rect expected) {
    ASSERT_EQ(actual, expected);
}

ShellLayoutRequest request(int columns, int rows) {
    ShellLayoutRequest value;
    value.viewport = {columns, rows};
    value.header_fields = {
        {"active_command", "Active command", "INSERT", 0},
        {"current_path", "Current path", "src/main.cpp", 1},
        {"mode", "Editor mode", "edit", 2},
    };
    value.footer_fields = {
        {"actionable_status", "Status message", "Saved", 0},
        {"follow_state", "Follow edits state and resume binding", "following", 1},
        {"background_activity", "Background activity", "idle", 2},
        {"encoding", "Text encoding", "UTF-8", 3},
        {"line_ending", "Line ending", "LF", 4},
        {"git_branch", "Git branch", "main", 5},
        {"git_repository", "Git repository", "ssg", 6},
        {"file_type", "File type", "C++", 7},
        {"file_size", "File size", "1 KiB", 8},
    };
    value.tabs = {{"main.cpp", "main.cpp tab", true}};
    value.footer_actions = {{"status.retry", "Retry status action"}};
    value.panel_provider_label = "Files";
    return value;
}

TEST(hand_authored_geometry_goldens) {
    ShellState state;

    auto minimum = request(20, 4);
    state.toggle_panel();
    auto minimum_result = compute_shell_layout(minimum, state);
    ASSERT_TRUE(minimum_result.accepted());
    ASSERT_FALSE(minimum_result.view->panel.has_value());
    assert_rect(*minimum_result.view->header, {0, 0, 20, 1});
    assert_rect(*minimum_result.view->tab_bar, {0, 1, 20, 1});
    assert_rect(minimum_result.view->panes[0].content, {0, 2, 19, 1});
    assert_rect(minimum_result.view->panes[0].scrollbar, {19, 2, 1, 1});
    assert_rect(*minimum_result.view->footer, {0, 3, 20, 1});
    const auto has_header_field = [&](std::string_view id) {
        return std::ranges::any_of(
            minimum_result.view->accessibility_nodes, [&](const auto& node) {
                return node.kind == ShellNodeKind::header_field &&
                       node.id == id;
            });
    };
    ASSERT_TRUE(has_header_field("active_command"));
    ASSERT_FALSE(has_header_field("current_path"));
    ASSERT_FALSE(has_header_field("mode"));

    auto wide = request(80, 12);
    wide.reserved_prompt_rows = 2;
    auto wide_result = compute_shell_layout(wide, state);
    ASSERT_TRUE(wide_result.accepted());
    assert_rect(*wide_result.view->header, {0, 0, 80, 1});
    assert_rect(*wide_result.view->panel, {0, 1, 24, 10});
    assert_rect(*wide_result.view->tab_bar, {24, 1, 56, 1});
    assert_rect(*wide_result.view->prompt, {24, 2, 56, 2});
    assert_rect(wide_result.view->panes[0].content, {24, 4, 55, 7});
    assert_rect(wide_result.view->panes[0].scrollbar, {79, 4, 1, 7});
    assert_rect(*wide_result.view->footer, {0, 11, 80, 1});

    auto focused = request(20, 4);
    state.toggle_distraction_free();
    auto focused_result = compute_shell_layout(focused, state);
    ASSERT_TRUE(focused_result.accepted());
    ASSERT_FALSE(focused_result.view->header.has_value());
    ASSERT_FALSE(focused_result.view->footer.has_value());
    ASSERT_FALSE(focused_result.view->tab_bar.has_value());
    assert_rect(focused_result.view->panes[0].content, {0, 0, 19, 4});
    assert_rect(focused_result.view->panes[0].scrollbar, {19, 0, 1, 4});
}

TEST(viewport_and_prompt_errors_are_typed) {
    ShellState state;
    auto narrow = compute_shell_layout(request(19, 4), state);
    ASSERT_FALSE(narrow.accepted());
    ASSERT_EQ(narrow.error->code, ShellLayoutErrorCode::viewport_too_small);
    ASSERT_FALSE(narrow.view.has_value());

    auto short_view = compute_shell_layout(request(20, 3), state);
    ASSERT_FALSE(short_view.accepted());
    ASSERT_EQ(short_view.error->code, ShellLayoutErrorCode::viewport_too_small);

    auto invalid_prompt = request(80, 12);
    invalid_prompt.reserved_prompt_rows = 4;
    auto invalid = compute_shell_layout(invalid_prompt, state);
    ASSERT_FALSE(invalid.accepted());
    ASSERT_EQ(invalid.error->code, ShellLayoutErrorCode::invalid_prompt_rows);

    auto no_room = request(20, 4);
    no_room.reserved_prompt_rows = 2;
    auto no_room_result = compute_shell_layout(no_room, state);
    ASSERT_FALSE(no_room_result.accepted());
    ASSERT_EQ(no_room_result.error->code, ShellLayoutErrorCode::viewport_too_small);
}

TEST(pane_commands_preserve_topology_and_order) {
    ShellState state;
    const auto first = state.active_pane();
    const auto second = state.split_active(SplitAxis::vertical);
    const auto third = state.split_active(SplitAxis::horizontal);
    ASSERT_EQ(state.pane_count(), std::size_t{3});
    ASSERT_EQ(state.active_pane(), third);

    state.previous_pane();
    ASSERT_EQ(state.active_pane(), second);
    state.previous_pane();
    ASSERT_EQ(state.active_pane(), first);
    state.next_pane();
    ASSERT_EQ(state.active_pane(), second);

    auto view = compute_shell_layout(request(80, 20), state);
    ASSERT_TRUE(view.accepted());
    ASSERT_EQ(view.view->panes.size(), std::size_t{3});
    ASSERT_TRUE(state.focus_pane(PaneDirection::down, *view.view));
    ASSERT_NE(state.active_pane(), first);
    ASSERT_TRUE(state.close_active_pane());
    ASSERT_EQ(state.pane_count(), std::size_t{2});
    ASSERT_TRUE(state.close_active_pane());
    ASSERT_FALSE(state.close_active_pane());
}

TEST(panel_commands_preserve_provider_state_when_hidden) {
    ShellState state({"Files", "Git", "Symbols"});
    ASSERT_FALSE(state.panel_requested());
    state.toggle_panel();
    ASSERT_TRUE(state.panel_requested());
    ASSERT_TRUE(state.focus_panel());
    ASSERT_EQ(state.active_panel_provider(), std::string_view{"Files"});
    state.next_panel_provider();
    ASSERT_EQ(state.active_panel_provider(), std::string_view{"Git"});
    state.previous_panel_provider();
    ASSERT_EQ(state.active_panel_provider(), std::string_view{"Files"});
    state.toggle_panel();
    ASSERT_FALSE(state.panel_requested());
    ASSERT_FALSE(state.panel_focused());
    ASSERT_EQ(state.active_panel_provider(), std::string_view{"Files"});
    state.toggle_panel();
    ASSERT_TRUE(state.focus_panel());
    state.toggle_distraction_free();
    state.toggle_distraction_free();
    ASSERT_TRUE(state.panel_focused());
}

TEST(exact_owned_command_set) {
    constexpr std::array expected{
        std::string_view{"pane.split_horizontal"},
        std::string_view{"pane.split_vertical"},
        std::string_view{"pane.close"},
        std::string_view{"pane.next"},
        std::string_view{"pane.previous"},
        std::string_view{"pane.focus_left"},
        std::string_view{"pane.focus_right"},
        std::string_view{"pane.focus_up"},
        std::string_view{"pane.focus_down"},
        std::string_view{"panel.toggle"},
        std::string_view{"panel.focus"},
        std::string_view{"panel.next_provider"},
        std::string_view{"panel.previous_provider"},
        std::string_view{"view.toggle_distraction_free"},
    };
    constexpr ShellCommandSet commands;
    ASSERT_EQ(commands.descriptors.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQ(commands.descriptors[i].id, expected[i]);
    }
}

TEST(accessibility_nodes_have_labels_and_roles) {
    ShellState state({"Files"});
    auto input = request(80, 12);
    state.toggle_panel();
    input.reserved_prompt_rows = 1;
    input.empty_state = true;
    auto result = compute_shell_layout(input, state);
    ASSERT_TRUE(result.accepted());

    const auto& nodes = result.view->accessibility_nodes;
    ASSERT_FALSE(nodes.empty());
    for (const auto& node : nodes) {
        ASSERT_FALSE(node.label.empty());
        ASSERT_TRUE(node.rect.width > 0);
        ASSERT_TRUE(node.rect.height > 0);
    }
    const auto has_kind = [&](ShellNodeKind kind) {
        return std::ranges::any_of(nodes, [=](const auto& node) {
            return node.kind == kind;
        });
    };
    ASSERT_TRUE(has_kind(ShellNodeKind::header));
    ASSERT_TRUE(has_kind(ShellNodeKind::footer));
    ASSERT_TRUE(has_kind(ShellNodeKind::footer_action));
    ASSERT_TRUE(has_kind(ShellNodeKind::tab));
    ASSERT_TRUE(has_kind(ShellNodeKind::panel_provider));
    ASSERT_TRUE(has_kind(ShellNodeKind::pane));
    ASSERT_TRUE(has_kind(ShellNodeKind::scrollbar));
    ASSERT_TRUE(has_kind(ShellNodeKind::prompt_reservation));
    ASSERT_TRUE(has_kind(ShellNodeKind::empty_state));
    for (const auto& field : nodes) {
        if (field.kind != ShellNodeKind::footer_field) continue;
        for (const auto& action : nodes) {
            if (action.kind == ShellNodeKind::footer_action) {
                ASSERT_FALSE(overlaps(field.rect, action.rect));
            }
        }
    }

    std::ostringstream actual;
    for (const auto& node : nodes) {
        actual << static_cast<int>(node.kind) << '|' << node.id << '|'
               << node.label << '|' << node.rect.x << ',' << node.rect.y << ','
               << node.rect.width << ',' << node.rect.height << '|'
               << static_cast<int>(node.role) << '\n';
    }
    std::ifstream golden(std::string{SSG_SOURCE_DIR} +
                         "/tests/fixtures/ui_layout/accessibility.txt");
    const std::string expected((std::istreambuf_iterator<char>(golden)),
                               std::istreambuf_iterator<char>());
    if (actual.str() != expected) {
        std::cerr << "  accessibility actual:\n" << actual.str()
                  << "  accessibility expected:\n" << expected;
    }
    ASSERT_EQ(actual.str(), expected);
}

TEST(non_overlap_and_cardinality_properties) {
    for (int columns = 20; columns <= 100; ++columns) {
        for (int rows = 4; rows <= 20; ++rows) {
            ShellState state;
            state.split_active(SplitAxis::vertical);
            state.split_active(SplitAxis::horizontal);
            state.toggle_panel();
            auto input = request(columns, rows);
            auto result = compute_shell_layout(input, state);
            ASSERT_TRUE(result.accepted());
            const auto& view = *result.view;
            ASSERT_TRUE(view.panes.size() == 1 || view.panes.size() == 3);
            ASSERT_EQ(view.scrollbar_count(), view.panes.size());

            std::vector<Rect> leaves;
            leaves.push_back(*view.header);
            leaves.push_back(*view.footer);
            leaves.push_back(*view.tab_bar);
            if (view.panel) leaves.push_back(*view.panel);
            for (const auto& pane : view.panes) {
                leaves.push_back(pane.content);
                leaves.push_back(pane.scrollbar);
            }
            for (std::size_t i = 0; i < leaves.size(); ++i) {
                ASSERT_TRUE(leaves[i].x >= 0 && leaves[i].y >= 0);
                ASSERT_TRUE(leaves[i].right() <= columns);
                ASSERT_TRUE(leaves[i].bottom() <= rows);
                for (std::size_t j = i + 1; j < leaves.size(); ++j) {
                    ASSERT_FALSE(overlaps(leaves[i], leaves[j]));
                }
            }
        }
    }
}

TEST(status_field_manifest_has_exact_order_and_labels) {
    std::ifstream input(std::string{SSG_SOURCE_DIR} + "/data/ui/status_fields.json");
    const std::string json((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    const std::array ids{
        "active_command", "current_path", "mode", "actionable_status",
        "follow_state", "background_activity", "encoding", "line_ending",
        "git_branch", "git_repository", "file_type", "file_size",
    };
    std::size_t position = 0;
    for (const auto* id : ids) {
        const auto found = json.find(std::string{"\"id\":\""} + id + '"', position);
        ASSERT_NE(found, std::string::npos);
        position = found + 1;
    }
    ASSERT_EQ(std::count(json.begin(), json.end(), '{'), ids.size());
    ASSERT_EQ(json.find("\"accessible_label\":\"\""), std::string::npos);
}

} // namespace

TEST(accessibility_leaf_nodes_carry_display_content) {
    ShellState state;
    state.toggle_panel();
    auto result = compute_shell_layout(request(80, 12), state);
    ASSERT_TRUE(result.accepted());
    const auto find = [&](ShellNodeKind kind,
                          std::string_view id) -> const AccessibilityNode* {
        for (const auto& node : result.view->accessibility_nodes) {
            if (node.kind == kind && node.id == id) return &node;
        }
        return nullptr;
    };
    const auto* command = find(ShellNodeKind::header_field, "active_command");
    ASSERT_TRUE(command != nullptr);
    if (command) ASSERT_EQ(command->content, std::string{"INSERT"});
    const auto* tab = find(ShellNodeKind::tab, "tab.0");
    ASSERT_TRUE(tab != nullptr);
    if (tab) ASSERT_EQ(tab->content, std::string{"main.cpp"});
    const auto* provider = find(ShellNodeKind::panel_provider, "panel.provider");
    ASSERT_TRUE(provider != nullptr);
    if (provider) ASSERT_EQ(provider->content, std::string{"Files"});
    // Container nodes carry no display text.
    const auto* header = find(ShellNodeKind::header, "header");
    ASSERT_TRUE(header != nullptr);
    if (header) ASSERT_TRUE(header->content.empty());
}

TEST(dirty_tab_content_shows_marker) {
    auto value = request(80, 12);
    value.tabs = {{"a.cpp", "a.cpp tab", true, true}};
    ShellState state;
    auto result = compute_shell_layout(value, state);
    ASSERT_TRUE(result.accepted());
    const AccessibilityNode* tab = nullptr;
    for (const auto& node : result.view->accessibility_nodes) {
        if (node.kind == ShellNodeKind::tab) tab = &node;
    }
    ASSERT_TRUE(tab != nullptr);
    if (tab) ASSERT_EQ(tab->content, std::string{"a.cpp *"});
}

int main() {
    RUN(hand_authored_geometry_goldens);
    RUN(viewport_and_prompt_errors_are_typed);
    RUN(pane_commands_preserve_topology_and_order);
    RUN(panel_commands_preserve_provider_state_when_hidden);
    RUN(exact_owned_command_set);
    RUN(accessibility_nodes_have_labels_and_roles);
    RUN(accessibility_leaf_nodes_carry_display_content);
    RUN(dirty_tab_content_shows_marker);
    RUN(non_overlap_and_cardinality_properties);
    RUN(status_field_manifest_has_exact_order_and_labels);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
