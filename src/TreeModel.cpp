#include <ssg/TreeModel.h>


#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace ssg {

std::string_view treeProviderLabel(TreeProviderKind kind) {
    switch (kind) {
    case TreeProviderKind::Filesystem:
        return "files";
    case TreeProviderKind::Git:
        return "git";
    case TreeProviderKind::Symbols:
        return "symbols";
    }
    throw std::logic_error{"corrupt TreeProviderKind enumerator"};
}

bool treeProviderCanBeCreatedEmpty(TreeProviderKind kind) {
    switch (kind) {
    case TreeProviderKind::Filesystem:
        return false;
    case TreeProviderKind::Git:
    case TreeProviderKind::Symbols:
        return true;
    }
    throw std::logic_error{"corrupt TreeProviderKind enumerator"};
}

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
        if (node.parentId) {
            const auto parent = std::lower_bound(
                nodes.begin(), nodes.end(), *node.parentId,
                [](const TreeNode& candidate, const TreeNodeId& id) {
                    return candidate.id < id;
                });
            if (parent == nodes.end() || parent->id != *node.parentId) {
                throw std::invalid_argument(
                    "tree node parent is absent from its provider snapshot");
            }
            parent->expandable = true;
        }
    }

    for (const auto& node : nodes) {
        std::set<TreeNodeId> ancestors;
        const TreeNode* current = &node;
        while (current->parentId) {
            if (!ancestors.insert(current->id).second) {
                throw std::invalid_argument("tree snapshot contains a parent cycle");
            }
            const auto parent = std::lower_bound(
                nodes.begin(), nodes.end(), *current->parentId,
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

std::vector<TreeNodeView> computeVisibleNodes(
    const TreeProviderSnapshot& snapshot,
    const std::vector<TreeNodeId>& expanded) {
    std::vector<TreeNodeView> result;
    std::map<std::optional<TreeNodeId>, std::vector<const TreeNode*>> children;
    for (const auto& node : snapshot.nodes()) {
        children[node.parentId].push_back(&node);
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

// Per-thread, like Renderer's segmentation counter: tests run single-threaded,
// and a per-thread counter needs no synchronization on the hot path.
thread_local std::uint64_t g_visibleNodesRecomputes = 0;

} // namespace

GitTreeAffordance gitTreeAffordance(DiffFileStatus status) {
    switch (status) {
    case DiffFileStatus::Added:
        return {status, "A", SemanticRole::DiffAdded};
    case DiffFileStatus::Modified:
        return {status, "M", SemanticRole::DiffModified};
    case DiffFileStatus::Deleted:
        return {status, "D", SemanticRole::DiffRemoved};
    case DiffFileStatus::Renamed:
        return {status, "R", SemanticRole::DiffModified};
    }
    throw std::invalid_argument("unknown git tree status");
}

bool detail::filesystemTreeEntryDisappeared(
    const std::error_code& error) noexcept {
    return error == std::errc::no_such_file_or_directory;
}

std::optional<TreeNode> detail::inspectFilesystemTreeEntry(
    const TreeProviderId& providerId, const std::filesystem::path& root,
    const std::filesystem::directory_entry& entry) {
    std::error_code error;
    const auto status = entry.symlink_status(error);
    if (filesystemTreeEntryDisappeared(error)) return std::nullopt;
    if (error) {
        throw std::runtime_error(
            "failed to inspect filesystem tree entry: " + error.message());
    }

    const auto relative =
        entry.path().lexically_relative(root).generic_string();
    const bool symlink = std::filesystem::is_symlink(status);
    const bool directory = std::filesystem::is_directory(status);
    const auto parentPath =
        std::filesystem::path{relative}.parent_path().generic_string();
    return TreeNode{
        nodeId(providerId, relative),
        nodeId(providerId, parentPath.empty() ? "." : parentPath),
        entry.path().filename().string(),
        symlink ? TreeNodeKind::Symlink
                : (directory ? TreeNodeKind::Directory : TreeNodeKind::File),
        std::nullopt,
        {},
        std::nullopt,
        relative,
        std::nullopt};
}

const std::vector<TreeNodeView>& TreeModel::ProviderState::visibleNodes() const {
    if (!visibleCache || visibleCache->revision != snapshot.revision() ||
        visibleCache->expandedVersion != expandedVersion) {
        ++g_visibleNodesRecomputes;
        visibleCache = VisibleCache{snapshot.revision(), expandedVersion,
                                    computeVisibleNodes(snapshot, expanded)};
    }
    return visibleCache->nodes;
}

std::uint64_t TreeModel::visibleNodesRecomputeCount() {
    return g_visibleNodesRecomputes;
}

void TreeModel::resetVisibleNodesRecomputeCount() {
    g_visibleNodesRecomputes = 0;
}

TreeProviderId::TreeProviderId(std::string value) : value_(std::move(value)) {
    validateProviderId(value_);
}

TreeNodeId::TreeNodeId(std::string value) : value_(std::move(value)) {
    validateNodeId(value_);
}

TreeProviderSnapshot::TreeProviderSnapshot(
    TreeProviderId providerId, TreeProviderKind kind, TreeRevision revision,
    std::vector<TreeNode> nodes)
    : providerId_(std::move(providerId)),
      kind_(kind),
      revision_(revision),
      nodes_(std::move(nodes)) {
    validateAndSortNodes(providerId_, nodes_);
}

TreeProviderSnapshot TreeProviderSnapshot::fromFilesystem(
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
    // The root was validated immediately above; ENOENT here means it disappeared
    // in the narrow window before enumeration began.
    if (detail::filesystemTreeEntryDisappeared(error)) error.clear();
    while (!error && iterator != end) {
        const auto entry = *iterator;
        auto node = detail::inspectFilesystemTreeEntry(providerId, root, entry);
        if (!node || node->kind == TreeNodeKind::Symlink) {
            iterator.disable_recursion_pending();
        }
        if (node) nodes.push_back(std::move(*node));
        iterator.increment(error);
        if (detail::filesystemTreeEntryDisappeared(error)) {
            error.clear();
            break;
        }
    }
    if (error) {
        throw std::runtime_error("failed to scan filesystem tree: " +
                                 error.message());
    }
    return TreeProviderSnapshot{std::move(providerId),
                                TreeProviderKind::Filesystem, revision,
                                std::move(nodes)};
}

TreeProviderSnapshot TreeProviderSnapshot::fromGit(
    TreeProviderId providerId, TreeRevision revision,
    std::vector<GitTreeRecord> records) {
    std::vector<TreeNode> nodes;
    nodes.reserve(records.size());
    for (auto& record : records) {
        const auto path = normalizeWorkspacePath(std::move(record.workspacePath));
        if (record.label.empty()) {
            throw std::invalid_argument("Git tree labels must be non-empty");
        }
        nodes.push_back(TreeNode{nodeId(providerId, path),
                                 std::nullopt,
                                 std::move(record.label),
                                 TreeNodeKind::GitEntry,
                                 std::nullopt,
                                 std::move(record.commands),
                                 gitTreeAffordance(record.status),
                                 path,
                                 std::nullopt});
    }
    return TreeProviderSnapshot{std::move(providerId), TreeProviderKind::Git,
                                revision, std::move(nodes)};
}

TreeProviderSnapshot TreeProviderSnapshot::fromSymbols(
    TreeProviderId providerId, TreeRevision revision,
    std::vector<SymbolTreeRecord> records) {
    std::vector<TreeNode> nodes;
    nodes.reserve(records.size());
    for (auto& record : records) {
        if (record.stableKey.empty() || record.label.empty()) {
            throw std::invalid_argument(
                "symbol trees require non-empty stable keys and labels");
        }
        std::optional<TreeNodeId> parent;
        if (record.parentKey) {
            parent = nodeId(providerId, *record.parentKey);
        }
        std::optional<std::string> path;
        if (record.workspacePath) {
            path = normalizeWorkspacePath(std::move(*record.workspacePath));
        }
        nodes.push_back(TreeNode{nodeId(providerId, record.stableKey),
                                 std::move(parent),
                                 std::move(record.label),
                                 TreeNodeKind::Symbol,
                                 std::nullopt,
                                 std::move(record.commands),
                                 std::nullopt,
                                 std::move(path),
                                 record.sourceLine});
    }
    return TreeProviderSnapshot{std::move(providerId),
                                TreeProviderKind::Symbols, revision,
                                std::move(nodes)};
}

void TreeModel::replaceProvider(TreeProviderSnapshot snapshot) {
    const TreeProviderId providerId = snapshot.providerId();
    auto iterator = std::lower_bound(
        providers_.begin(), providers_.end(), providerId,
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
        iterator->bumpExpanded();
        iterator->snapshot = std::move(snapshot);
    } else {
        providers_.insert(
            iterator, ProviderState{std::move(snapshot), {}});
    }
    if (!activeProviderId_) activeProviderId_ = providerId;
    revision_ = TreeRevision{revision_.value() + 1};

    // Keep the selection valid against the active provider; default to its
    // first visible node so the tree always has a focus once populated.
    auto* active = activeProvider();
    if (active == nullptr) {
        selected_.reset();
        return;
    }
    const auto& visible = active->visibleNodes();
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
    if (!activeProviderId_) return nullptr;
    auto found = std::find_if(
        providers_.begin(), providers_.end(),
        [&](const ProviderState& state) {
            return state.snapshot.providerId() == *activeProviderId_;
        });
    return found == providers_.end() ? nullptr : &*found;
}

const TreeModel::ProviderState* TreeModel::activeProvider() const {
    if (!activeProviderId_) return nullptr;
    auto found = std::find_if(
        providers_.begin(), providers_.end(),
        [&](const ProviderState& state) {
            return state.snapshot.providerId() == *activeProviderId_;
        });
    return found == providers_.end() ? nullptr : &*found;
}

bool TreeModel::selectNext() {
    auto* provider = activeProvider();
    if (provider == nullptr) return false;
    const auto& visible = provider->visibleNodes();
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
    const auto& visible = provider->visibleNodes();
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
    const auto& visible = provider->visibleNodes();
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
    provider->bumpExpanded();
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

std::vector<TreeModel::ProviderIdentity> TreeModel::providerIdentities() const {
    std::vector<ProviderIdentity> identities;
    identities.reserve(providers_.size());
    for (const auto& state : providers_) {
        identities.push_back(ProviderIdentity{
            TreeProviderBinding{state.snapshot.providerId(), state.snapshot.kind()},
            state.snapshot.revision()});
    }
    return identities;
}

std::optional<TreeProviderBinding> TreeModel::activeProviderBinding() const {
    const auto* provider = activeProvider();
    if (provider == nullptr) return std::nullopt;
    return TreeProviderBinding{provider->snapshot.providerId(),
                               provider->snapshot.kind()};
}

bool TreeModel::activateProvider(const TreeProviderId& providerId) {
    const auto found = std::find_if(
        providers_.begin(), providers_.end(),
        [&](const ProviderState& state) {
            return state.snapshot.providerId() == providerId;
        });
    if (found == providers_.end()) {
        return false;
    }
    if (!activeProviderId_ || *activeProviderId_ != providerId) {
        activeProviderId_ = providerId;
        if (const auto* active = activeProvider()) {
            const auto& visible = active->visibleNodes();
            const bool stillValid =
                selected_ &&
                std::any_of(visible.begin(), visible.end(),
                            [&](const TreeNodeView& view) {
                                return view.node.id == *selected_;
                            });
            if (!stillValid) {
                selected_ = visible.empty()
                                ? std::nullopt
                                : std::optional<TreeNodeId>{
                                      visible.front().node.id};
            }
        }
        revision_ = TreeRevision{revision_.value() + 1};
    }
    return true;
}

bool TreeModel::activateOrCreate(
    const TreeProviderBinding& binding,
    const std::function<TreeRevision()>& revisionForCreate) {
    if (!revisionForCreate) {
        throw std::invalid_argument{
            "TreeModel::activateOrCreate requires a revision source "
            "(revisionForCreate must be callable)"};
    }
    const auto existing = std::find_if(
        providers_.begin(), providers_.end(),
        [&](const ProviderState& state) {
            return state.snapshot.providerId() == binding.id;
        });
    if (existing != providers_.end()) {
        if (existing->snapshot.kind() != binding.kind) return false;
        return activateProvider(binding.id);
    }
    if (!treeProviderCanBeCreatedEmpty(binding.kind)) return false;
    replaceProvider(
        TreeProviderSnapshot{binding.id, binding.kind, revisionForCreate(), {}});
    return activateProvider(binding.id);
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

std::size_t TreeModel::activeVisibleNodeCount() const {
    const auto* active = activeProvider();
    if (active == nullptr) return 0;
    return active->visibleNodes().size();
}

TreeViewState TreeModel::viewState() const {
    TreeViewState result{revision_, {}, activeProviderBinding()};
    result.providers.reserve(providers_.size());
    std::vector<const ProviderState*> ordered;
    ordered.reserve(providers_.size());
    if (const auto* active = activeProvider()) {
        ordered.push_back(active);
    }
    for (const auto& provider : providers_) {
        if (!ordered.empty() && &provider == ordered.front()) {
            continue;
        }
        ordered.push_back(&provider);
    }
    for (const auto* provider : ordered) {
        std::optional<TreeNodeId> providerSelected;
        if (selected_ &&
            selected_->value().starts_with(
                provider->snapshot.providerId().value() + ":")) {
            providerSelected = selected_;
        }
        result.providers.push_back(TreeProviderView{
            provider->snapshot.providerId(), provider->snapshot.kind(),
            provider->visibleNodes(),
            providerSelected});
    }
    return result;
}

const TreeProviderView* activeTreeProvider(const TreeViewState& state) noexcept {
    if (!state.activeBinding) return nullptr;
    const auto found = std::find_if(
        state.providers.begin(), state.providers.end(),
        [&](const TreeProviderView& provider) {
            return provider.providerId == state.activeBinding->id &&
                   provider.kind == state.activeBinding->kind;
        });
    if (found == state.providers.end()) return nullptr;
    const auto duplicate = std::find_if(
        std::next(found), state.providers.end(),
        [&](const TreeProviderView& provider) {
            return provider.providerId == state.activeBinding->id &&
                   provider.kind == state.activeBinding->kind;
        });
    return duplicate == state.providers.end() ? &*found : nullptr;
}

bool isValidTreeViewState(const TreeViewState& state) noexcept {
    if (state.providers.empty()) return !state.activeBinding;
    if (!state.activeBinding || activeTreeProvider(state) == nullptr) return false;
    std::set<TreeProviderId> ids;
    return std::all_of(state.providers.begin(), state.providers.end(),
                       [&](const TreeProviderView& provider) {
                           return ids.insert(provider.providerId).second;
                       });
}

} // namespace ssg
