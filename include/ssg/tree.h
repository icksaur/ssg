#pragma once

#include <ssg/viewport.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

class TreeProviderId {
public:
    explicit TreeProviderId(std::string value);
    const std::string& value() const noexcept { return value_; }
    auto operator<=>(const TreeProviderId&) const = default;

private:
    std::string value_;
};

class TreeNodeId {
public:
    explicit TreeNodeId(std::string value);
    const std::string& value() const noexcept { return value_; }
    auto operator<=>(const TreeNodeId&) const = default;

private:
    std::string value_;
};

class TreeRevision {
public:
    constexpr explicit TreeRevision(std::uint64_t value) noexcept : value_(value) {}
    constexpr std::uint64_t value() const noexcept { return value_; }
    constexpr auto operator<=>(const TreeRevision&) const = default;

private:
    std::uint64_t value_;
};

enum class TreeProviderKind { filesystem, git, symbols };
enum class TreeNodeKind { root, directory, file, symlink, git_entry, symbol };
enum class GitTreeStatus { added, modified, deleted, renamed, untracked };

struct TreeNodeCommand {
    std::string id;
    std::string label;
    bool operator==(const TreeNodeCommand&) const = default;
};

struct TreeNode {
    TreeNodeId id;
    std::optional<TreeNodeId> parent_id;
    std::string label;
    TreeNodeKind kind;
    std::optional<std::string> icon;
    std::vector<TreeNodeCommand> commands;
    std::optional<GitTreeStatus> git_status;
    std::optional<std::string> workspace_path;
    std::optional<std::uint32_t> source_line;
    bool expandable = false;
    bool operator==(const TreeNode&) const = default;
};

class TreeProviderSnapshot {
public:
    TreeProviderSnapshot(TreeProviderId provider_id, TreeProviderKind kind,
                         TreeRevision revision, std::vector<TreeNode> nodes);

    const TreeProviderId& provider_id() const noexcept { return provider_id_; }
    TreeProviderKind kind() const noexcept { return kind_; }
    TreeRevision revision() const noexcept { return revision_; }
    const std::vector<TreeNode>& nodes() const noexcept { return nodes_; }

private:
    TreeProviderId provider_id_;
    TreeProviderKind kind_;
    TreeRevision revision_;
    std::vector<TreeNode> nodes_;
};

struct GitTreeRecord {
    std::string workspace_path;
    std::string label;
    GitTreeStatus status = GitTreeStatus::modified;
    std::vector<TreeNodeCommand> commands;
};

struct SymbolTreeRecord {
    std::string stable_key;
    std::optional<std::string> parent_key;
    std::string label;
    std::optional<std::string> workspace_path;
    std::optional<std::uint32_t> source_line;
    std::vector<TreeNodeCommand> commands;
};

TreeProviderSnapshot filesystem_tree_snapshot(
    TreeProviderId provider_id, const std::filesystem::path& canonical_cwd,
    TreeRevision revision);
TreeProviderSnapshot git_tree_snapshot(TreeProviderId provider_id,
                                       TreeRevision revision,
                                       std::vector<GitTreeRecord> records);
TreeProviderSnapshot symbol_tree_snapshot(
    TreeProviderId provider_id, TreeRevision revision,
    std::vector<SymbolTreeRecord> records);

struct TreeCommandDescriptor {
    std::string_view id;
    bool operator==(const TreeCommandDescriptor&) const = default;
};

class TreeCommandSet {
public:
    TreeCommandSet(const TreeCommandSet&) = default;
    TreeCommandSet& operator=(const TreeCommandSet&) = delete;
    const std::array<TreeCommandDescriptor, 7>& descriptors() const noexcept {
        return descriptors_;
    }

private:
    friend TreeCommandSet tree_command_set();
    TreeCommandSet();
    const std::array<TreeCommandDescriptor, 7> descriptors_;
};

TreeCommandSet tree_command_set();

struct TreeNodeView {
    TreeNode node;
    std::size_t depth = 0;
    bool expanded = false;
    bool operator==(const TreeNodeView&) const = default;
};

struct TreeProviderView {
    TreeProviderId provider_id;
    TreeProviderKind kind;
    std::vector<TreeNodeView> nodes;
    std::optional<TreeNodeId> selected;
    // Scroll state resolved at snapshot time against the panel height (see
    // doc/spec-scroll.md R2). `first_visible` is the index into `nodes` of the
    // first on-screen node; `scrollbar` is its thumb geometry; `visible_node_ids`
    // is the bounded viewport_row -> node id hit map for the visible window only
    // (empty when the panel is hidden). `nodes` still carries the full expanded
    // list; render and hit-testing window it with `first_visible`.
    std::uint32_t first_visible = 0;
    ScrollbarMetrics scrollbar{};
    std::vector<TreeNodeId> visible_node_ids;
    bool operator==(const TreeProviderView&) const = default;
};

struct TreeViewState {
    TreeRevision revision{0};
    std::vector<TreeProviderView> providers;
    bool operator==(const TreeViewState&) const = default;
};

struct TreeCommandInvocation {
    TreeProviderId provider_id;
    TreeNodeId node_id;
    std::string command_id;
    bool operator==(const TreeCommandInvocation&) const = default;
};

// Argument for `tree.select`: the node to make the active provider's selection.
// A dedicated payload (rather than the TreeCommandInvocation triple) keeps a
// click's argument minimal -- a pointer click needs only the node id.
struct TreeSelectArguments {
    TreeNodeId node_id;
    bool operator==(const TreeSelectArguments&) const = default;
};

class TreeModel {
public:
    void replace_provider(TreeProviderSnapshot snapshot);
    bool toggle_expanded(const TreeProviderId& provider_id,
                         const TreeNodeId& node_id);
    bool is_expanded(const TreeProviderId& provider_id,
                     const TreeNodeId& node_id) const;
    std::optional<TreeCommandInvocation> invoke_node_command(
        const TreeProviderId& provider_id, const TreeNodeId& node_id,
        std::string_view command_id) const;

    // Selection navigation over the active provider's visible nodes.  Selection
    // is library-owned UI state so every client presents the same focus.
    bool select_next();
    bool select_previous();
    // Set the active provider's selection to `node_id`. Returns false (leaving
    // the selection unchanged) when no provider is active or the id is not among
    // the active provider's visible nodes.
    bool select(const TreeNodeId& node_id);
    bool toggle_selected();  // Expand/collapse the selected directory.
    [[nodiscard]] std::optional<TreeNode> selected_node() const;

    TreeViewState view_state() const;

private:
    struct ProviderState {
        TreeProviderSnapshot snapshot;
        std::vector<TreeNodeId> expanded;
    };

    [[nodiscard]] ProviderState* active_provider();
    [[nodiscard]] const ProviderState* active_provider() const;

    TreeRevision revision_{0};
    std::vector<ProviderState> providers_;
    std::optional<TreeNodeId> selected_;
};

struct TreeProviderDelta {
    TreeProviderId provider_id;
    TreeProviderKind kind;
    bool remove_provider = false;
    std::size_t start = 0;
    std::size_t erase_count = 0;
    std::vector<TreeNodeView> insert;
    // Resolved scroll state carried so a delta reproduces the provider view even
    // when only the panel-height-resolved scroll state changed (e.g. showing the
    // panel) with no node edit or tree-revision bump.  Ignored for removals.
    std::optional<TreeNodeId> selected;
    std::uint32_t first_visible = 0;
    ScrollbarMetrics scrollbar{};
    std::vector<TreeNodeId> visible_node_ids;
    bool operator==(const TreeProviderDelta&) const = default;
};

struct TreeDelta {
    TreeRevision base_revision{0};
    TreeRevision revision{0};
    bool snapshot_required = false;
    std::vector<TreeProviderDelta> providers;

    std::size_t operation_count() const noexcept;
    bool operator==(const TreeDelta&) const = default;
};

TreeDelta derive_tree_delta(const TreeViewState& base,
                            const TreeViewState& target,
                            std::size_t maximum_operations);

enum class TreeReplayError { none, stale_revision, snapshot_required, malformed_delta };

struct TreeReplayResult {
    std::optional<TreeViewState> state;
    TreeReplayError error = TreeReplayError::none;
    bool accepted() const noexcept { return state.has_value(); }
};

TreeReplayResult replay_tree_delta(const TreeViewState& base,
                                   const TreeDelta& delta);

} // namespace ssg
