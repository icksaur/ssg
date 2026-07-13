#include "ssg/tree.h"
#include "test_helpers.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace ssg;

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("ssg-tree-" + std::to_string(++sequence_))) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

    const std::filesystem::path& path() const { return path_; }

private:
    inline static unsigned sequence_ = 0;
    std::filesystem::path path_;
};

std::vector<std::string> node_ids(const TreeProviderSnapshot& snapshot) {
    std::vector<std::string> result;
    for (const auto& node : snapshot.nodes()) {
        result.push_back(node.id.value());
    }
    return result;
}

const TreeProviderView& only_provider(const TreeViewState& state) {
    ASSERT_EQ(state.providers.size(), std::size_t{1});
    return state.providers.front();
}

TEST(filesystem_snapshot_is_stable_sorted_and_does_not_follow_symlinks) {
    TemporaryDirectory temporary;
    std::filesystem::create_directories(temporary.path() / "z-dir");
    std::filesystem::create_directories(temporary.path() / "a-dir");
    std::ofstream(temporary.path() / "z-dir" / "child.txt") << "child";
    std::ofstream(temporary.path() / "b.txt") << "b";
    std::ofstream(temporary.path() / "a.txt") << "a";
    std::error_code symlink_error;
    std::filesystem::create_directory_symlink(
        temporary.path() / "z-dir", temporary.path() / "a-link", symlink_error);

    const auto first = filesystem_tree_snapshot(
        TreeProviderId{"files"}, temporary.path(), TreeRevision{1});
    std::filesystem::rename(temporary.path() / "a.txt",
                            temporary.path() / "renamed.txt");
    const auto second = filesystem_tree_snapshot(
        TreeProviderId{"files"}, temporary.path(), TreeRevision{2});

    std::vector<std::string> expected{
        "files:.", "files:a-dir", "files:a-link", "files:a.txt", "files:b.txt",
        "files:z-dir", "files:z-dir/child.txt"};
    if (symlink_error) {
        expected.erase(expected.begin() + 2);
    }
    const auto first_ids = node_ids(first);
    const auto second_ids = node_ids(second);
    ASSERT_EQ(first_ids, expected);
    ASSERT_TRUE(std::find(first_ids.begin(), first_ids.end(),
                          "files:a-link/child.txt") == first_ids.end());
    ASSERT_TRUE(std::find(second_ids.begin(), second_ids.end(),
                          "files:a.txt") == second_ids.end());
    ASSERT_TRUE(std::find(second_ids.begin(), second_ids.end(),
                          "files:renamed.txt") != second_ids.end());
    ASSERT_EQ(first.nodes().front().id, second.nodes().front().id);
}

TEST(git_and_symbol_snapshots_are_deterministic_and_use_stable_keys) {
    const auto git = git_tree_snapshot(
        TreeProviderId{"git"}, TreeRevision{7},
        {{.workspace_path = "z.cpp", .label = "renamed label",
          .status = GitTreeStatus::modified},
         {.workspace_path = "a.cpp", .label = "a.cpp",
          .status = GitTreeStatus::added}});
    ASSERT_EQ(node_ids(git),
              (std::vector<std::string>{"git:a.cpp", "git:z.cpp"}));
    ASSERT_EQ(git.nodes()[1].id, TreeNodeId{"git:z.cpp"});

    const auto symbols = symbol_tree_snapshot(
        TreeProviderId{"symbols"}, TreeRevision{8},
        {{.stable_key = "type/Z", .label = "renamed Z"},
         {.stable_key = "type/A", .label = "A"},
         {.stable_key = "type/A/member", .parent_key = "type/A",
          .label = "member"}});
    ASSERT_EQ(node_ids(symbols),
              (std::vector<std::string>{"symbols:type/A",
                                        "symbols:type/A/member",
                                        "symbols:type/Z"}));
}

TEST(expansion_survives_refresh_by_identity_and_disappearing_nodes_are_pruned) {
    TreeModel model;
    model.replace_provider(symbol_tree_snapshot(
        TreeProviderId{"symbols"}, TreeRevision{1},
        {{.stable_key = "type/A", .label = "A"},
         {.stable_key = "type/A/member", .parent_key = "type/A",
          .label = "member"}}));

    ASSERT_TRUE(model.toggle_expanded(TreeProviderId{"symbols"},
                                      TreeNodeId{"symbols:type/A"}));
    ASSERT_EQ(only_provider(model.view_state()).nodes.size(), std::size_t{2});

    model.replace_provider(symbol_tree_snapshot(
        TreeProviderId{"symbols"}, TreeRevision{2},
        {{.stable_key = "type/A", .label = "renamed A"},
         {.stable_key = "type/A/member", .parent_key = "type/A",
          .label = "renamed member"}}));
    ASSERT_TRUE(only_provider(model.view_state()).nodes.front().expanded);
    ASSERT_EQ(only_provider(model.view_state()).nodes.size(), std::size_t{2});

    model.replace_provider(symbol_tree_snapshot(
        TreeProviderId{"symbols"}, TreeRevision{3},
        {{.stable_key = "type/B", .label = "B"}}));
    ASSERT_FALSE(model.is_expanded(TreeProviderId{"symbols"},
                                   TreeNodeId{"symbols:type/A"}));
}

TEST(command_set_is_exact_and_invocation_is_provider_data_only) {
    const auto commands = tree_command_set();
    ASSERT_EQ(commands.descriptors().size(), std::size_t{5});
    ASSERT_EQ(commands.descriptors()[0].id, std::string_view{"tree.toggle_expanded"});
    ASSERT_EQ(commands.descriptors()[1].id,
              std::string_view{"tree.invoke_node_command"});
    ASSERT_EQ(commands.descriptors()[2].id, std::string_view{"tree.select_next"});
    ASSERT_EQ(commands.descriptors()[3].id,
              std::string_view{"tree.select_previous"});
    ASSERT_EQ(commands.descriptors()[4].id, std::string_view{"tree.activate"});

    TreeModel model;
    model.replace_provider(symbol_tree_snapshot(
        TreeProviderId{"symbols"}, TreeRevision{1},
        {{.stable_key = "type/A",
          .label = "A",
          .commands = {{.id = "symbol.open", .label = "Open symbol"}}}}));
    const auto invocation = model.invoke_node_command(
        TreeProviderId{"symbols"}, TreeNodeId{"symbols:type/A"}, "symbol.open");
    ASSERT_TRUE(invocation.has_value());
    ASSERT_EQ(invocation->command_id, std::string{"symbol.open"});
    ASSERT_FALSE(model.invoke_node_command(
        TreeProviderId{"symbols"}, TreeNodeId{"symbols:type/A"}, "missing")
                     .has_value());
}

TEST(selection_navigates_expands_and_reports_selected_node) {
    TreeModel model;
    model.replace_provider(symbol_tree_snapshot(
        TreeProviderId{"symbols"}, TreeRevision{1},
        {{.stable_key = "A", .label = "A"},
         {.stable_key = "A/one", .parent_key = "A", .label = "one"},
         {.stable_key = "B", .label = "B"}}));

    // Populating the provider auto-selects its first visible node.
    auto selected = model.selected_node();
    ASSERT_TRUE(selected.has_value());
    if (selected) ASSERT_EQ(selected->id, TreeNodeId{"symbols:A"});

    // Only roots A and B are visible while A is collapsed; next selects B.
    ASSERT_TRUE(model.select_next());
    selected = model.selected_node();
    if (selected) ASSERT_EQ(selected->id, TreeNodeId{"symbols:B"});

    // At the last visible node, next clamps.
    ASSERT_TRUE(model.select_next());
    selected = model.selected_node();
    if (selected) ASSERT_EQ(selected->id, TreeNodeId{"symbols:B"});

    // Return to A and expand it, revealing its child.
    ASSERT_TRUE(model.select_previous());
    selected = model.selected_node();
    if (selected) ASSERT_EQ(selected->id, TreeNodeId{"symbols:A"});
    ASSERT_TRUE(model.toggle_selected());
    ASSERT_TRUE(model.select_next());
    selected = model.selected_node();
    if (selected) ASSERT_EQ(selected->id, TreeNodeId{"symbols:A/one"});

    // The selection is exposed on the provider view.
    const auto view = model.view_state();
    ASSERT_FALSE(view.providers.empty());
    if (!view.providers.empty()) {
        ASSERT_TRUE(view.providers.front().selected.has_value());
        if (view.providers.front().selected) {
            ASSERT_EQ(*view.providers.front().selected,
                      TreeNodeId{"symbols:A/one"});
        }
    }
}

TEST(bounded_delta_replays_to_independent_view_and_rejects_stale_base) {
    TreeModel model;
    model.replace_provider(symbol_tree_snapshot(
        TreeProviderId{"symbols"}, TreeRevision{1},
        {{.stable_key = "A", .label = "A"},
         {.stable_key = "A/one", .parent_key = "A", .label = "one"}}));
    model.toggle_expanded(TreeProviderId{"symbols"}, TreeNodeId{"symbols:A"});
    const auto base = model.view_state();

    model.replace_provider(symbol_tree_snapshot(
        TreeProviderId{"symbols"}, TreeRevision{2},
        {{.stable_key = "A", .label = "A"},
         {.stable_key = "A/one", .parent_key = "A", .label = "renamed one"},
         {.stable_key = "A/two", .parent_key = "A", .label = "two"}}));
    const auto target = model.view_state();
    const auto delta = derive_tree_delta(base, target, 8);
    ASSERT_FALSE(delta.snapshot_required);
    ASSERT_TRUE(delta.operation_count() <= std::size_t{8});

    const auto replay = replay_tree_delta(base, delta);
    ASSERT_TRUE(replay.accepted());
    ASSERT_EQ(*replay.state, target);

    auto stale = base;
    stale.revision = TreeRevision{base.revision.value() + 1};
    const auto stale_replay = replay_tree_delta(stale, delta);
    ASSERT_FALSE(stale_replay.accepted());
    ASSERT_EQ(stale_replay.error, TreeReplayError::stale_revision);
}

TEST(over_budget_delta_requires_snapshot_without_partial_operations) {
    TreeModel model;
    model.replace_provider(symbol_tree_snapshot(
        TreeProviderId{"symbols"}, TreeRevision{1},
        {{.stable_key = "A", .label = "A"}}));
    const auto base = model.view_state();
    model.replace_provider(symbol_tree_snapshot(
        TreeProviderId{"symbols"}, TreeRevision{2},
        {{.stable_key = "A", .label = "A"},
         {.stable_key = "B", .label = "B"},
         {.stable_key = "C", .label = "C"}}));
    const auto target = model.view_state();

    const auto delta = derive_tree_delta(base, target, 1);
    ASSERT_TRUE(delta.snapshot_required);
    ASSERT_TRUE(delta.providers.empty());
    ASSERT_EQ(delta.operation_count(), std::size_t{0});
    const auto replay = replay_tree_delta(base, delta);
    ASSERT_EQ(replay.error, TreeReplayError::snapshot_required);
}

} // namespace

int main() {
    RUN(filesystem_snapshot_is_stable_sorted_and_does_not_follow_symlinks);
    RUN(git_and_symbol_snapshots_are_deterministic_and_use_stable_keys);
    RUN(expansion_survives_refresh_by_identity_and_disappearing_nodes_are_pruned);
    RUN(command_set_is_exact_and_invocation_is_provider_data_only);
    RUN(selection_navigates_expands_and_reports_selected_node);
    RUN(bounded_delta_replays_to_independent_view_and_rejects_stale_base);
    RUN(over_budget_delta_requires_snapshot_without_partial_operations);
    return failed == 0 ? 0 : 1;
}
