#include "ssg/tree.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace ssg {
namespace {

void validate_provider_id(std::string_view value) {
    if (value.empty() || value.find(':') != std::string_view::npos) {
        throw std::invalid_argument(
            "tree provider ID must be non-empty and must not contain ':'");
    }
}

void validate_node_id(std::string_view value) {
    if (value.empty()) {
        throw std::invalid_argument("tree node ID must be non-empty");
    }
}

void validate_command(const TreeNodeCommand& command) {
    if (command.id.empty() || command.label.empty()) {
        throw std::invalid_argument(
            "tree node commands require non-empty IDs and labels");
    }
}

std::string normalize_workspace_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    const std::filesystem::path parsed{path};
    if (path.empty() || parsed.is_absolute() || parsed.has_root_name()) {
        throw std::invalid_argument(
            "tree workspace path must be non-empty and relative");
    }
    const auto normalized = parsed.lexically_normal();
    if (normalized.empty() || normalized == ".") {
        throw std::invalid_argument(
            "tree workspace path must identify an entry beneath the CWD");
    }
    for (const auto& component : normalized) {
        if (component == "..") {
            throw std::invalid_argument(
                "tree workspace path must remain beneath the CWD");
        }
    }
    return normalized.generic_string();
}

TreeNodeId node_id(const TreeProviderId& provider_id,
                   std::string_view stable_key) {
    if (stable_key.empty()) {
        throw std::invalid_argument("tree stable key must be non-empty");
    }
    return TreeNodeId{provider_id.value() + ":" + std::string{stable_key}};
}

void validate_and_sort_nodes(const TreeProviderId& provider_id,
                             std::vector<TreeNode>& nodes) {
    std::sort(nodes.begin(), nodes.end(),
              [](const TreeNode& left, const TreeNode& right) {
                  return left.id < right.id;
              });
    std::set<TreeNodeId> ids;
    for (auto& node : nodes) {
        node.expandable = false;
        if (!node.id.value().starts_with(provider_id.value() + ":")) {
            throw std::invalid_argument(
                "tree node ID does not belong to its provider");
        }
        if (!ids.insert(node.id).second) {
            throw std::invalid_argument("tree snapshot contains a duplicate node ID");
        }
        for (const auto& command : node.commands) {
            validate_command(command);
        }
    }
    for (auto& node : nodes) {
        if (node.parent_id) {
            const auto parent = std::lower_bound(
                nodes.begin(), nodes.end(), *node.parent_id,
                [](const TreeNode& candidate, const TreeNodeId& id) {
                    return candidate.id < id;
                });
            if (parent == nodes.end() || parent->id != *node.parent_id) {
                throw std::invalid_argument(
                    "tree node parent is absent from its provider snapshot");
            }
            parent->expandable = true;
        }
    }

    for (const auto& node : nodes) {
        std::set<TreeNodeId> ancestors;
        const TreeNode* current = &node;
        while (current->parent_id) {
            if (!ancestors.insert(current->id).second) {
                throw std::invalid_argument("tree snapshot contains a parent cycle");
            }
            const auto parent = std::lower_bound(
                nodes.begin(), nodes.end(), *current->parent_id,
                [](const TreeNode& candidate, const TreeNodeId& id) {
                    return candidate.id < id;
                });
            current = &*parent;
        }
    }
}

const TreeNode* find_node(const TreeProviderSnapshot& snapshot,
                          const TreeNodeId& id) {
    const auto iterator = std::lower_bound(
        snapshot.nodes().begin(), snapshot.nodes().end(), id,
        [](const TreeNode& node, const TreeNodeId& searched) {
            return node.id < searched;
        });
    return iterator != snapshot.nodes().end() && iterator->id == id
               ? &*iterator
               : nullptr;
}

std::vector<TreeNodeView> visible_nodes(
    const TreeProviderSnapshot& snapshot,
    const std::vector<TreeNodeId>& expanded) {
    std::vector<TreeNodeView> result;
    std::map<std::optional<TreeNodeId>, std::vector<const TreeNode*>> children;
    for (const auto& node : snapshot.nodes()) {
        children[node.parent_id].push_back(&node);
    }
    std::function<void(const TreeNode&, std::size_t)> append =
        [&](const TreeNode& node, std::size_t depth) {
            const bool is_expanded =
                std::binary_search(expanded.begin(), expanded.end(), node.id);
            result.push_back(TreeNodeView{node, depth, is_expanded});
            if (!is_expanded) {
                return;
            }
            for (const auto* child :
                 children[std::optional<TreeNodeId>{node.id}]) {
                append(*child, depth + 1);
            }
        };
    for (const auto* root : children[std::nullopt]) {
        append(*root, 0);
    }
    return result;
}

std::size_t provider_delta_cost(const TreeProviderDelta& delta) {
    return 1 + delta.erase_count + delta.insert.size();
}

} // namespace

TreeProviderId::TreeProviderId(std::string value) : value_(std::move(value)) {
    validate_provider_id(value_);
}

TreeNodeId::TreeNodeId(std::string value) : value_(std::move(value)) {
    validate_node_id(value_);
}

TreeProviderSnapshot::TreeProviderSnapshot(
    TreeProviderId provider_id, TreeProviderKind kind, TreeRevision revision,
    std::vector<TreeNode> nodes)
    : provider_id_(std::move(provider_id)),
      kind_(kind),
      revision_(revision),
      nodes_(std::move(nodes)) {
    validate_and_sort_nodes(provider_id_, nodes_);
}

TreeProviderSnapshot filesystem_tree_snapshot(
    TreeProviderId provider_id, const std::filesystem::path& canonical_cwd,
    TreeRevision revision) {
    std::error_code error;
    const auto root = std::filesystem::canonical(canonical_cwd, error);
    if (error || !std::filesystem::is_directory(root, error) || error) {
        throw std::invalid_argument(
            "filesystem tree CWD must be an existing accessible directory");
    }

    std::vector<TreeNode> nodes;
    nodes.push_back(TreeNode{node_id(provider_id, "."),
                             std::nullopt,
                             root.filename().string(),
                             TreeNodeKind::root,
                             std::nullopt,
                             {},
                             std::nullopt,
                             std::string{"."},
                             std::nullopt});

    std::filesystem::recursive_directory_iterator iterator{
        root, std::filesystem::directory_options::skip_permission_denied, error};
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const auto entry = *iterator;
        const auto relative = entry.path().lexically_relative(root).generic_string();
        const auto status = entry.symlink_status(error);
        if (error) {
            throw std::runtime_error(
                "failed to inspect filesystem tree entry: " + error.message());
        }
        const bool symlink = std::filesystem::is_symlink(status);
        const bool directory = std::filesystem::is_directory(status);
        if (symlink) {
            iterator.disable_recursion_pending();
        }

        const auto parent_path =
            std::filesystem::path{relative}.parent_path().generic_string();
        nodes.push_back(TreeNode{
            node_id(provider_id, relative),
            node_id(provider_id, parent_path.empty() ? "." : parent_path),
            entry.path().filename().string(),
            symlink ? TreeNodeKind::symlink
                    : (directory ? TreeNodeKind::directory : TreeNodeKind::file),
            std::nullopt,
            {},
            std::nullopt,
            relative,
            std::nullopt});
        iterator.increment(error);
    }
    if (error) {
        throw std::runtime_error("failed to scan filesystem tree: " +
                                 error.message());
    }
    return TreeProviderSnapshot{std::move(provider_id),
                                TreeProviderKind::filesystem, revision,
                                std::move(nodes)};
}

TreeProviderSnapshot git_tree_snapshot(TreeProviderId provider_id,
                                       TreeRevision revision,
                                       std::vector<GitTreeRecord> records) {
    std::vector<TreeNode> nodes;
    nodes.reserve(records.size());
    for (auto& record : records) {
        const auto path = normalize_workspace_path(std::move(record.workspace_path));
        if (record.label.empty()) {
            throw std::invalid_argument("Git tree labels must be non-empty");
        }
        nodes.push_back(TreeNode{node_id(provider_id, path),
                                 std::nullopt,
                                 std::move(record.label),
                                 TreeNodeKind::git_entry,
                                 std::nullopt,
                                 std::move(record.commands),
                                 record.status,
                                 path,
                                 std::nullopt});
    }
    return TreeProviderSnapshot{std::move(provider_id), TreeProviderKind::git,
                                revision, std::move(nodes)};
}

TreeProviderSnapshot symbol_tree_snapshot(
    TreeProviderId provider_id, TreeRevision revision,
    std::vector<SymbolTreeRecord> records) {
    std::vector<TreeNode> nodes;
    nodes.reserve(records.size());
    for (auto& record : records) {
        if (record.stable_key.empty() || record.label.empty()) {
            throw std::invalid_argument(
                "symbol trees require non-empty stable keys and labels");
        }
        std::optional<TreeNodeId> parent;
        if (record.parent_key) {
            parent = node_id(provider_id, *record.parent_key);
        }
        std::optional<std::string> path;
        if (record.workspace_path) {
            path = normalize_workspace_path(std::move(*record.workspace_path));
        }
        nodes.push_back(TreeNode{node_id(provider_id, record.stable_key),
                                 std::move(parent),
                                 std::move(record.label),
                                 TreeNodeKind::symbol,
                                 std::nullopt,
                                 std::move(record.commands),
                                 std::nullopt,
                                 std::move(path),
                                 record.source_line});
    }
    return TreeProviderSnapshot{std::move(provider_id),
                                TreeProviderKind::symbols, revision,
                                std::move(nodes)};
}

TreeCommandSet::TreeCommandSet()
    : descriptors_{{{"tree.toggle_expanded"},
                    {"tree.invoke_node_command"},
                    {"tree.select"},
                    {"tree.select_next"},
                    {"tree.select_previous"},
                    {"tree.activate"}}} {}

TreeCommandSet tree_command_set() { return TreeCommandSet{}; }

void TreeModel::replace_provider(TreeProviderSnapshot snapshot) {
    auto iterator = std::lower_bound(
        providers_.begin(), providers_.end(), snapshot.provider_id(),
        [](const ProviderState& state, const TreeProviderId& id) {
            return state.snapshot.provider_id() < id;
        });
    if (iterator != providers_.end() &&
        iterator->snapshot.provider_id() == snapshot.provider_id()) {
        if (snapshot.revision() <= iterator->snapshot.revision()) {
            throw std::invalid_argument(
                "replacement tree snapshot revision must increase");
        }
        std::erase_if(iterator->expanded, [&](const TreeNodeId& id) {
            return find_node(snapshot, id) == nullptr;
        });
        iterator->snapshot = std::move(snapshot);
    } else {
        providers_.insert(
            iterator, ProviderState{std::move(snapshot), {}});
    }
    revision_ = TreeRevision{revision_.value() + 1};

    // Keep the selection valid against the active provider; default to its
    // first visible node so the tree always has a focus once populated.
    auto* active = active_provider();
    if (active == nullptr) {
        selected_.reset();
        return;
    }
    auto visible = visible_nodes(active->snapshot, active->expanded);
    const bool still_valid =
        selected_ && std::any_of(visible.begin(), visible.end(),
                                 [&](const TreeNodeView& view) {
                                     return view.node.id == *selected_;
                                 });
    if (!still_valid) {
        selected_ = visible.empty()
                        ? std::nullopt
                        : std::optional<TreeNodeId>{visible.front().node.id};
    }
}

TreeModel::ProviderState* TreeModel::active_provider() {
    return providers_.empty() ? nullptr : &providers_.front();
}

const TreeModel::ProviderState* TreeModel::active_provider() const {
    return providers_.empty() ? nullptr : &providers_.front();
}

bool TreeModel::select_next() {
    auto* provider = active_provider();
    if (provider == nullptr) return false;
    auto visible = visible_nodes(provider->snapshot, provider->expanded);
    if (visible.empty()) {
        selected_.reset();
        return false;
    }
    std::size_t index = 0;
    if (selected_) {
        for (std::size_t i = 0; i < visible.size(); ++i) {
            if (visible[i].node.id == *selected_) {
                index = std::min(visible.size() - 1, i + 1);
                break;
            }
        }
    }
    selected_ = visible[index].node.id;
    revision_ = TreeRevision{revision_.value() + 1};
    return true;
}

bool TreeModel::select_previous() {
    auto* provider = active_provider();
    if (provider == nullptr) return false;
    auto visible = visible_nodes(provider->snapshot, provider->expanded);
    if (visible.empty()) {
        selected_.reset();
        return false;
    }
    std::size_t index = 0;
    if (selected_) {
        for (std::size_t i = 0; i < visible.size(); ++i) {
            if (visible[i].node.id == *selected_) {
                index = (i == 0) ? 0 : i - 1;
                break;
            }
        }
    }
    selected_ = visible[index].node.id;
    revision_ = TreeRevision{revision_.value() + 1};
    return true;
}

bool TreeModel::select(const TreeNodeId& node_id) {
    auto* provider = active_provider();
    if (provider == nullptr) return false;
    auto visible = visible_nodes(provider->snapshot, provider->expanded);
    const bool present =
        std::any_of(visible.begin(), visible.end(), [&](const TreeNodeView& view) {
            return view.node.id == node_id;
        });
    if (!present) return false;
    if (selected_ && *selected_ == node_id) return true;
    selected_ = node_id;
    revision_ = TreeRevision{revision_.value() + 1};
    return true;
}

bool TreeModel::toggle_selected() {
    auto* provider = active_provider();
    if (provider == nullptr || !selected_) return false;
    return toggle_expanded(provider->snapshot.provider_id(), *selected_);
}

std::optional<TreeNode> TreeModel::selected_node() const {
    const auto* provider = active_provider();
    if (provider == nullptr || !selected_) return std::nullopt;
    const auto* node = find_node(provider->snapshot, *selected_);
    return node ? std::optional<TreeNode>{*node} : std::nullopt;
}

bool TreeModel::toggle_expanded(const TreeProviderId& provider_id,
                                const TreeNodeId& node_id) {
    const auto provider = std::lower_bound(
        providers_.begin(), providers_.end(), provider_id,
        [](const ProviderState& state, const TreeProviderId& id) {
            return state.snapshot.provider_id() < id;
        });
    if (provider == providers_.end() ||
        provider->snapshot.provider_id() != provider_id) {
        return false;
    }
    const auto* node = find_node(provider->snapshot, node_id);
    if (node == nullptr || !node->expandable) {
        return false;
    }
    const auto expanded = std::lower_bound(provider->expanded.begin(),
                                           provider->expanded.end(), node_id);
    if (expanded != provider->expanded.end() && *expanded == node_id) {
        provider->expanded.erase(expanded);
    } else {
        provider->expanded.insert(expanded, node_id);
    }
    revision_ = TreeRevision{revision_.value() + 1};
    return true;
}

bool TreeModel::is_expanded(const TreeProviderId& provider_id,
                            const TreeNodeId& node_id) const {
    const auto provider = std::lower_bound(
        providers_.begin(), providers_.end(), provider_id,
        [](const ProviderState& state, const TreeProviderId& id) {
            return state.snapshot.provider_id() < id;
        });
    return provider != providers_.end() &&
           provider->snapshot.provider_id() == provider_id &&
           std::binary_search(provider->expanded.begin(),
                              provider->expanded.end(), node_id);
}

std::optional<TreeCommandInvocation> TreeModel::invoke_node_command(
    const TreeProviderId& provider_id, const TreeNodeId& node_id,
    std::string_view command_id) const {
    const auto provider = std::lower_bound(
        providers_.begin(), providers_.end(), provider_id,
        [](const ProviderState& state, const TreeProviderId& id) {
            return state.snapshot.provider_id() < id;
        });
    if (provider == providers_.end() ||
        provider->snapshot.provider_id() != provider_id) {
        return std::nullopt;
    }
    const auto* node = find_node(provider->snapshot, node_id);
    if (node == nullptr) {
        return std::nullopt;
    }
    const auto command = std::find_if(
        node->commands.begin(), node->commands.end(),
        [&](const TreeNodeCommand& candidate) {
            return candidate.id == command_id;
        });
    if (command == node->commands.end()) {
        return std::nullopt;
    }
    return TreeCommandInvocation{provider_id, node_id,
                                 std::string{command_id}};
}

TreeViewState TreeModel::view_state() const {
    TreeViewState result{revision_, {}};
    result.providers.reserve(providers_.size());
    for (const auto& provider : providers_) {
        std::optional<TreeNodeId> provider_selected;
        if (selected_ &&
            selected_->value().starts_with(
                provider.snapshot.provider_id().value() + ":")) {
            provider_selected = selected_;
        }
        result.providers.push_back(TreeProviderView{
            provider.snapshot.provider_id(), provider.snapshot.kind(),
            visible_nodes(provider.snapshot, provider.expanded),
            provider_selected});
    }
    return result;
}

std::size_t TreeDelta::operation_count() const noexcept {
    std::size_t result = 0;
    for (const auto& provider : providers) {
        result += provider_delta_cost(provider);
    }
    return result;
}

TreeDelta derive_tree_delta(const TreeViewState& base,
                            const TreeViewState& target,
                            std::size_t maximum_operations) {
    TreeDelta result{base.revision, target.revision, false, {}};
    std::size_t base_index = 0;
    std::size_t target_index = 0;
    while (base_index < base.providers.size() ||
           target_index < target.providers.size()) {
        if (target_index == target.providers.size() ||
            (base_index < base.providers.size() &&
             base.providers[base_index].provider_id <
                 target.providers[target_index].provider_id)) {
            result.providers.push_back(
                TreeProviderDelta{base.providers[base_index].provider_id,
                                  base.providers[base_index].kind, true});
            ++base_index;
            continue;
        }
        if (base_index == base.providers.size() ||
            target.providers[target_index].provider_id <
                base.providers[base_index].provider_id) {
            result.providers.push_back(TreeProviderDelta{
                target.providers[target_index].provider_id,
                target.providers[target_index].kind,
                false,
                0,
                0,
                target.providers[target_index].nodes});
            ++target_index;
            continue;
        }

        const auto& before = base.providers[base_index];
        const auto& after = target.providers[target_index];
        if (before != after) {
            std::size_t prefix = 0;
            while (prefix < before.nodes.size() &&
                   prefix < after.nodes.size() &&
                   before.nodes[prefix] == after.nodes[prefix] &&
                   before.kind == after.kind) {
                ++prefix;
            }
            std::size_t suffix = 0;
            while (suffix < before.nodes.size() - prefix &&
                   suffix < after.nodes.size() - prefix &&
                   before.nodes[before.nodes.size() - 1 - suffix] ==
                       after.nodes[after.nodes.size() - 1 - suffix] &&
                   before.kind == after.kind) {
                ++suffix;
            }
            result.providers.push_back(TreeProviderDelta{
                after.provider_id,
                after.kind,
                false,
                prefix,
                before.nodes.size() - prefix - suffix,
                std::vector<TreeNodeView>{
                    after.nodes.begin() + static_cast<std::ptrdiff_t>(prefix),
                    after.nodes.end() - static_cast<std::ptrdiff_t>(suffix)}});
        }
        ++base_index;
        ++target_index;
    }

    if (result.operation_count() > maximum_operations) {
        result.snapshot_required = true;
        result.providers.clear();
    }
    return result;
}

TreeReplayResult replay_tree_delta(const TreeViewState& base,
                                   const TreeDelta& delta) {
    if (base.revision != delta.base_revision) {
        return {std::nullopt, TreeReplayError::stale_revision};
    }
    if (delta.snapshot_required) {
        return {std::nullopt, TreeReplayError::snapshot_required};
    }

    auto state = base;
    std::set<TreeProviderId> changed;
    for (const auto& change : delta.providers) {
        if (!changed.insert(change.provider_id).second) {
            return {std::nullopt, TreeReplayError::malformed_delta};
        }
        auto provider = std::lower_bound(
            state.providers.begin(), state.providers.end(), change.provider_id,
            [](const TreeProviderView& candidate, const TreeProviderId& id) {
                return candidate.provider_id < id;
            });
        if (change.remove_provider) {
            if (provider == state.providers.end() ||
                provider->provider_id != change.provider_id ||
                change.start != 0 || change.erase_count != 0 ||
                !change.insert.empty()) {
                return {std::nullopt, TreeReplayError::malformed_delta};
            }
            state.providers.erase(provider);
            continue;
        }
        if (provider == state.providers.end() ||
            provider->provider_id != change.provider_id) {
            if (change.start != 0 || change.erase_count != 0) {
                return {std::nullopt, TreeReplayError::malformed_delta};
            }
            state.providers.insert(
                provider, TreeProviderView{change.provider_id, change.kind,
                                           change.insert});
            continue;
        }
        if (change.start > provider->nodes.size() ||
            change.erase_count > provider->nodes.size() - change.start) {
            return {std::nullopt, TreeReplayError::malformed_delta};
        }
        provider->kind = change.kind;
        auto first = provider->nodes.begin() +
                     static_cast<std::ptrdiff_t>(change.start);
        auto last = first +
                    static_cast<std::ptrdiff_t>(change.erase_count);
        first = provider->nodes.erase(first, last);
        provider->nodes.insert(first, change.insert.begin(), change.insert.end());
    }
    state.revision = delta.revision;
    return {std::move(state), TreeReplayError::none};
}

} // namespace ssg
