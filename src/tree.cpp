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

void validateProviderId(std::string_view value) {
    if (value.empty() || value.find(':') != std::string_view::npos) {
        throw std::invalid_argument(
            "tree provider ID must be non-empty and must not contain ':'");
    }
}

void validateNodeId(std::string_view value) {
    if (value.empty()) {
        throw std::invalid_argument("tree node ID must be non-empty");
    }
}

void validateCommand(const TreeNodeCommand& command) {
    if (command.id.empty() || command.label.empty()) {
        throw std::invalid_argument(
            "tree node commands require non-empty IDs and labels");
    }
}

std::string normalizeWorkspacePath(std::string path) {
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

TreeNodeId nodeId(const TreeProviderId& providerId,
                   std::string_view stableKey) {
    if (stableKey.empty()) {
        throw std::invalid_argument("tree stable key must be non-empty");
    }
    return TreeNodeId{providerId.value() + ":" + std::string{stableKey}};
}

void validateAndSortNodes(const TreeProviderId& providerId,
                             std::vector<TreeNode>& nodes) {
    std::sort(nodes.begin(), nodes.end(),
              [](const TreeNode& left, const TreeNode& right) {
                  return left.id < right.id;
              });
    std::set<TreeNodeId> ids;
    for (auto& node : nodes) {
        node.expandable = false;
        if (!node.id.value().starts_with(providerId.value() + ":")) {
            throw std::invalid_argument(
                "tree node ID does not belong to its provider");
        }
        if (!ids.insert(node.id).second) {
            throw std::invalid_argument("tree snapshot contains a duplicate node ID");
        }
        for (const auto& command : node.commands) {
            validateCommand(command);
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

const TreeNode* findNode(const TreeProviderSnapshot& snapshot,
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

std::vector<TreeNodeView> visibleNodes(
    const TreeProviderSnapshot& snapshot,
    const std::vector<TreeNodeId>& expanded) {
    std::vector<TreeNodeView> result;
    std::map<std::optional<TreeNodeId>, std::vector<const TreeNode*>> children;
    for (const auto& node : snapshot.nodes()) {
        children[node.parent_id].push_back(&node);
    }
    std::function<void(const TreeNode&, std::size_t)> append =
        [&](const TreeNode& node, std::size_t depth) {
            const bool isExpanded =
                std::binary_search(expanded.begin(), expanded.end(), node.id);
            result.push_back(TreeNodeView{node, depth, isExpanded});
            if (!isExpanded) {
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

std::size_t providerDeltaCost(const TreeProviderDelta& delta) {
    return 1 + delta.erase_count + delta.insert.size();
}

} // namespace

TreeProviderId::TreeProviderId(std::string value) : value_(std::move(value)) {
    validateProviderId(value_);
}

TreeNodeId::TreeNodeId(std::string value) : value_(std::move(value)) {
    validateNodeId(value_);
}

TreeProviderSnapshot::TreeProviderSnapshot(
    TreeProviderId providerId, TreeProviderKind kind, TreeRevision revision,
    std::vector<TreeNode> nodes)
    : provider_id_(std::move(providerId)),
      kind_(kind),
      revision_(revision),
      nodes_(std::move(nodes)) {
    validateAndSortNodes(provider_id_, nodes_);
}

TreeProviderSnapshot filesystemTreeSnapshot(
    TreeProviderId providerId, const std::filesystem::path& canonicalCwd,
    TreeRevision revision) {
    std::error_code error;
    const auto root = std::filesystem::canonical(canonicalCwd, error);
    if (error || !std::filesystem::is_directory(root, error) || error) {
        throw std::invalid_argument(
            "filesystem tree CWD must be an existing accessible directory");
    }

    std::vector<TreeNode> nodes;
    nodes.push_back(TreeNode{nodeId(providerId, "."),
                             std::nullopt,
                             root.filename().string(),
                             TreeNodeKind::Root,
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

        const auto parentPath =
            std::filesystem::path{relative}.parent_path().generic_string();
        nodes.push_back(TreeNode{
            nodeId(providerId, relative),
            nodeId(providerId, parentPath.empty() ? "." : parentPath),
            entry.path().filename().string(),
            symlink ? TreeNodeKind::Symlink
                    : (directory ? TreeNodeKind::Directory : TreeNodeKind::File),
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
    return TreeProviderSnapshot{std::move(providerId),
                                TreeProviderKind::Filesystem, revision,
                                std::move(nodes)};
}

TreeProviderSnapshot gitTreeSnapshot(TreeProviderId providerId,
                                       TreeRevision revision,
                                       std::vector<GitTreeRecord> records) {
    std::vector<TreeNode> nodes;
    nodes.reserve(records.size());
    for (auto& record : records) {
        const auto path = normalizeWorkspacePath(std::move(record.workspace_path));
        if (record.label.empty()) {
            throw std::invalid_argument("Git tree labels must be non-empty");
        }
        nodes.push_back(TreeNode{nodeId(providerId, path),
                                 std::nullopt,
                                 std::move(record.label),
                                 TreeNodeKind::GitEntry,
                                 std::nullopt,
                                 std::move(record.commands),
                                 record.status,
                                 path,
                                 std::nullopt});
    }
    return TreeProviderSnapshot{std::move(providerId), TreeProviderKind::Git,
                                revision, std::move(nodes)};
}

TreeProviderSnapshot symbolTreeSnapshot(
    TreeProviderId providerId, TreeRevision revision,
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
            parent = nodeId(providerId, *record.parent_key);
        }
        std::optional<std::string> path;
        if (record.workspace_path) {
            path = normalizeWorkspacePath(std::move(*record.workspace_path));
        }
        nodes.push_back(TreeNode{nodeId(providerId, record.stable_key),
                                 std::move(parent),
                                 std::move(record.label),
                                 TreeNodeKind::Symbol,
                                 std::nullopt,
                                 std::move(record.commands),
                                 std::nullopt,
                                 std::move(path),
                                 record.source_line});
    }
    return TreeProviderSnapshot{std::move(providerId),
                                TreeProviderKind::Symbols, revision,
                                std::move(nodes)};
}

TreeCommandSet::TreeCommandSet()
    : descriptors_{{{"tree.toggle_expanded"},
                    {"tree.invoke_node_command"},
                    {"tree.select"},
                    {"tree.select_next"},
                    {"tree.select_previous"},
                    {"tree.activate"},
                    {"tree.scroll"}}} {}

TreeCommandSet treeCommandSet() { return TreeCommandSet{}; }

void TreeModel::replaceProvider(TreeProviderSnapshot snapshot) {
    auto iterator = std::lower_bound(
        providers_.begin(), providers_.end(), snapshot.providerId(),
        [](const ProviderState& state, const TreeProviderId& id) {
            return state.snapshot.providerId() < id;
        });
    if (iterator != providers_.end() &&
        iterator->snapshot.providerId() == snapshot.providerId()) {
        if (snapshot.revision() <= iterator->snapshot.revision()) {
            throw std::invalid_argument(
                "replacement tree snapshot revision must increase");
        }
        std::erase_if(iterator->expanded, [&](const TreeNodeId& id) {
            return findNode(snapshot, id) == nullptr;
        });
        iterator->snapshot = std::move(snapshot);
    } else {
        providers_.insert(
            iterator, ProviderState{std::move(snapshot), {}});
    }
    revision_ = TreeRevision{revision_.value() + 1};

    // Keep the selection valid against the active provider; default to its
    // first visible node so the tree always has a focus once populated.
    auto* active = activeProvider();
    if (active == nullptr) {
        selected_.reset();
        return;
    }
    auto visible = visibleNodes(active->snapshot, active->expanded);
    const bool stillValid =
        selected_ && std::any_of(visible.begin(), visible.end(),
                                 [&](const TreeNodeView& view) {
                                     return view.node.id == *selected_;
                                 });
    if (!stillValid) {
        selected_ = visible.empty()
                        ? std::nullopt
                        : std::optional<TreeNodeId>{visible.front().node.id};
    }
}

TreeModel::ProviderState* TreeModel::activeProvider() {
    return providers_.empty() ? nullptr : &providers_.front();
}

const TreeModel::ProviderState* TreeModel::activeProvider() const {
    return providers_.empty() ? nullptr : &providers_.front();
}

bool TreeModel::selectNext() {
    auto* provider = activeProvider();
    if (provider == nullptr) return false;
    auto visible = visibleNodes(provider->snapshot, provider->expanded);
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

bool TreeModel::selectPrevious() {
    auto* provider = activeProvider();
    if (provider == nullptr) return false;
    auto visible = visibleNodes(provider->snapshot, provider->expanded);
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

bool TreeModel::select(const TreeNodeId& nodeId) {
    auto* provider = activeProvider();
    if (provider == nullptr) return false;
    auto visible = visibleNodes(provider->snapshot, provider->expanded);
    const bool present =
        std::any_of(visible.begin(), visible.end(), [&](const TreeNodeView& view) {
            return view.node.id == nodeId;
        });
    if (!present) return false;
    if (selected_ && *selected_ == nodeId) return true;
    selected_ = nodeId;
    revision_ = TreeRevision{revision_.value() + 1};
    return true;
}

bool TreeModel::toggleSelected() {
    auto* provider = activeProvider();
    if (provider == nullptr || !selected_) return false;
    return toggleExpanded(provider->snapshot.providerId(), *selected_);
}

std::optional<TreeNode> TreeModel::selectedNode() const {
    const auto* provider = activeProvider();
    if (provider == nullptr || !selected_) return std::nullopt;
    const auto* node = findNode(provider->snapshot, *selected_);
    return node ? std::optional<TreeNode>{*node} : std::nullopt;
}

bool TreeModel::toggleExpanded(const TreeProviderId& providerId,
                                const TreeNodeId& nodeId) {
    const auto provider = std::lower_bound(
        providers_.begin(), providers_.end(), providerId,
        [](const ProviderState& state, const TreeProviderId& id) {
            return state.snapshot.providerId() < id;
        });
    if (provider == providers_.end() ||
        provider->snapshot.providerId() != providerId) {
        return false;
    }
    const auto* node = findNode(provider->snapshot, nodeId);
    if (node == nullptr || !node->expandable) {
        return false;
    }
    const auto expanded = std::lower_bound(provider->expanded.begin(),
                                           provider->expanded.end(), nodeId);
    if (expanded != provider->expanded.end() && *expanded == nodeId) {
        provider->expanded.erase(expanded);
    } else {
        provider->expanded.insert(expanded, nodeId);
    }
    revision_ = TreeRevision{revision_.value() + 1};
    return true;
}

bool TreeModel::isExpanded(const TreeProviderId& providerId,
                            const TreeNodeId& nodeId) const {
    const auto provider = std::lower_bound(
        providers_.begin(), providers_.end(), providerId,
        [](const ProviderState& state, const TreeProviderId& id) {
            return state.snapshot.providerId() < id;
        });
    return provider != providers_.end() &&
           provider->snapshot.providerId() == providerId &&
           std::binary_search(provider->expanded.begin(),
                              provider->expanded.end(), nodeId);
}

std::optional<TreeCommandInvocation> TreeModel::invokeNodeCommand(
    const TreeProviderId& providerId, const TreeNodeId& nodeId,
    std::string_view commandId) const {
    const auto provider = std::lower_bound(
        providers_.begin(), providers_.end(), providerId,
        [](const ProviderState& state, const TreeProviderId& id) {
            return state.snapshot.providerId() < id;
        });
    if (provider == providers_.end() ||
        provider->snapshot.providerId() != providerId) {
        return std::nullopt;
    }
    const auto* node = findNode(provider->snapshot, nodeId);
    if (node == nullptr) {
        return std::nullopt;
    }
    const auto command = std::find_if(
        node->commands.begin(), node->commands.end(),
        [&](const TreeNodeCommand& candidate) {
            return candidate.id == commandId;
        });
    if (command == node->commands.end()) {
        return std::nullopt;
    }
    return TreeCommandInvocation{providerId, nodeId,
                                 std::string{commandId}};
}

TreeViewState TreeModel::viewState() const {
    TreeViewState result{revision_, {}};
    result.providers.reserve(providers_.size());
    for (const auto& provider : providers_) {
        std::optional<TreeNodeId> providerSelected;
        if (selected_ &&
            selected_->value().starts_with(
                provider.snapshot.providerId().value() + ":")) {
            providerSelected = selected_;
        }
        result.providers.push_back(TreeProviderView{
            provider.snapshot.providerId(), provider.snapshot.kind(),
            visibleNodes(provider.snapshot, provider.expanded),
            providerSelected});
    }
    return result;
}

std::size_t TreeDelta::operationCount() const noexcept {
    std::size_t result = 0;
    for (const auto& provider : providers) {
        result += providerDeltaCost(provider);
    }
    return result;
}

TreeDelta deriveTreeDelta(const TreeViewState& base,
                            const TreeViewState& target,
                            std::size_t maximumOperations) {
    TreeDelta result{base.revision, target.revision, false, {}};
    std::size_t baseIndex = 0;
    std::size_t targetIndex = 0;
    while (baseIndex < base.providers.size() ||
           targetIndex < target.providers.size()) {
        if (targetIndex == target.providers.size() ||
            (baseIndex < base.providers.size() &&
             base.providers[baseIndex].provider_id <
                 target.providers[targetIndex].provider_id)) {
            result.providers.push_back(
                TreeProviderDelta{base.providers[baseIndex].provider_id,
                                  base.providers[baseIndex].kind, true});
            ++baseIndex;
            continue;
        }
        if (baseIndex == base.providers.size() ||
            target.providers[targetIndex].provider_id <
                base.providers[baseIndex].provider_id) {
            const auto& added = target.providers[targetIndex];
            result.providers.push_back(TreeProviderDelta{
                added.provider_id, added.kind, false, 0, 0, added.nodes,
                added.selected, added.first_visible, added.scrollbar,
                added.visible_node_ids});
            ++targetIndex;
            continue;
        }

        const auto& before = base.providers[baseIndex];
        const auto& after = target.providers[targetIndex];
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
                    after.nodes.end() - static_cast<std::ptrdiff_t>(suffix)},
                after.selected, after.first_visible, after.scrollbar,
                after.visible_node_ids});
        }
        ++baseIndex;
        ++targetIndex;
    }

    if (result.operationCount() > maximumOperations) {
        result.snapshot_required = true;
        result.providers.clear();
    }
    return result;
}

TreeReplayResult replayTreeDelta(const TreeViewState& base,
                                   const TreeDelta& delta) {
    if (base.revision != delta.base_revision) {
        return {std::nullopt, TreeReplayError::StaleRevision};
    }
    if (delta.snapshot_required) {
        return {std::nullopt, TreeReplayError::SnapshotRequired};
    }

    auto state = base;
    std::set<TreeProviderId> changed;
    for (const auto& change : delta.providers) {
        if (!changed.insert(change.provider_id).second) {
            return {std::nullopt, TreeReplayError::MalformedDelta};
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
                return {std::nullopt, TreeReplayError::MalformedDelta};
            }
            state.providers.erase(provider);
            continue;
        }
        if (provider == state.providers.end() ||
            provider->provider_id != change.provider_id) {
            if (change.start != 0 || change.erase_count != 0) {
                return {std::nullopt, TreeReplayError::MalformedDelta};
            }
            state.providers.insert(
                provider, TreeProviderView{change.provider_id, change.kind,
                                           change.insert, change.selected,
                                           change.first_visible,
                                           change.scrollbar,
                                           change.visible_node_ids});
            continue;
        }
        if (change.start > provider->nodes.size() ||
            change.erase_count > provider->nodes.size() - change.start) {
            return {std::nullopt, TreeReplayError::MalformedDelta};
        }
        provider->kind = change.kind;
        auto first = provider->nodes.begin() +
                     static_cast<std::ptrdiff_t>(change.start);
        auto last = first +
                    static_cast<std::ptrdiff_t>(change.erase_count);
        first = provider->nodes.erase(first, last);
        provider->nodes.insert(first, change.insert.begin(), change.insert.end());
        provider->selected = change.selected;
        provider->first_visible = change.first_visible;
        provider->scrollbar = change.scrollbar;
        provider->visible_node_ids = change.visible_node_ids;
    }
    state.revision = delta.revision;
    return {std::move(state), TreeReplayError::None};
}

} // namespace ssg
