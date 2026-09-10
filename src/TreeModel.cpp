#include <ssg/TreeModel.h>
#include <ssg/platform_files.h>


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
    case TreeProviderKind::Search:
        return "search";
    }
    throw std::logic_error{"corrupt TreeProviderKind enumerator"};
}

bool treeProviderCanBeCreatedEmpty(TreeProviderKind kind) {
    switch (kind) {
    case TreeProviderKind::Filesystem:
        return false;
    case TreeProviderKind::Git:
    case TreeProviderKind::Symbols:
    case TreeProviderKind::Search:
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

std::optional<TreeNode> detail::inspectFilesystemTreeEntry(
    const TreeProviderId& providerId, const std::filesystem::path& root,
    const std::filesystem::directory_entry& entry) {
    const auto status = statFile(entry.path());
    if (!status) return std::nullopt;

    const auto relative =
        entry.path().lexically_relative(root).generic_string();
    const bool symlink = status->kind == FileKind::Symlink;
    const bool directory = status->kind == FileKind::Directory;
    const auto parentPath =
        std::filesystem::path{relative}.parent_path().generic_string();
    return TreeNode{
        nodeId(providerId, relative),
        parentPath.empty() ? std::optional<TreeNodeId>{}
                           : nodeId(providerId, parentPath),
        entry.path().filename().string(),
        symlink ? TreeNodeKind::Symlink
                : (directory ? TreeNodeKind::Directory : TreeNodeKind::File),
        std::nullopt,
        {},
        std::nullopt,
        relative,
        std::nullopt,
        std::nullopt};
}

TreeModel::ProviderState::ProviderState(TreeProviderSnapshot value)
    : snapshot{std::move(value)} {
    if (snapshot.kind() == TreeProviderKind::Search) search.emplace();
}

const std::vector<TreeNodeView>& TreeModel::ProviderState::visibleNodes() const {
    if (!visibleCache || visibleCache->revision != snapshot.revision() ||
        visibleCache->expandedVersion != expandedVersion) {
        visibleCache = VisibleCache{snapshot.revision(), expandedVersion,
                                    computeVisibleNodes(snapshot, expanded)};
    }
    return visibleCache->nodes;
}

TreeProviderId::TreeProviderId(std::string value) : value_(std::move(value)) {
    validateProviderId(value_);
}

TreeNodeId::TreeNodeId(std::string value) : value_(std::move(value)) {
    validateNodeId(value_);
}

TreeProviderSnapshot::TreeProviderSnapshot(
    TreeProviderId providerId, TreeProviderKind kind, std::vector<TreeNode> nodes)
    : providerId_(std::move(providerId)),
      kind_(kind),
      revision_(0),
      nodes_(std::move(nodes)) {
    validateAndSortNodes(providerId_, nodes_);
}

TreeProviderSnapshot TreeProviderSnapshot::fromFilesystemDirectories(
    TreeProviderId providerId, const std::filesystem::path& canonicalCwd,
    std::span<const std::string> loadedDirectories) {
    std::error_code error;
    const auto root = canonicalPath(canonicalCwd, error);
    const auto rootStat = error ? std::optional<FileStat>{} : statFile(root);
    if (error || !rootStat || rootStat->kind != FileKind::Directory) {
        throw std::invalid_argument(
            "filesystem tree CWD must be an existing accessible directory");
    }

    std::vector<TreeNode> nodes;
    std::set<std::string> directories;
    const auto append = [&](const std::filesystem::path& directory,
                            bool rootDirectory) {
        const auto listed =
            listDirectory(directory, DirectoryTraversal::Children);
        if (listed.status == FileIoStatus::NotFound && !rootDirectory) {
            return;
        }
        if (!listed.ok() || !listed.complete) {
            throw std::runtime_error(
                "failed to scan filesystem tree directory " +
                directory.generic_string() + ": " + listed.message);
        }
        for (const auto& entry : listed.entries) {
            auto node =
                detail::inspectFilesystemTreeEntry(providerId, root, entry);
            if (!node) continue;
            if (rootDirectory && node->kind == TreeNodeKind::Directory &&
                node->workspacePath) {
                directories.insert(*node->workspacePath);
            }
            nodes.push_back(std::move(*node));
        }
    };

    append(root, true);
    for (const auto& requested : loadedDirectories) {
        const auto normalized = normalizeWorkspacePath(requested);
        std::filesystem::path ancestor;
        for (const auto& component : std::filesystem::path{normalized}) {
            ancestor /= component;
            directories.insert(ancestor.generic_string());
        }
    }
    for (const auto& relative : directories) {
        const auto path = root / relative;
        const auto status = statFile(path);
        if (!status || status->kind != FileKind::Directory) continue;
        append(path, false);
    }
    return TreeProviderSnapshot{std::move(providerId),
                                TreeProviderKind::Filesystem,
                                std::move(nodes)};
}

TreeProviderSnapshot TreeProviderSnapshot::fromGit(
    TreeProviderId providerId, std::vector<GitTreeRecord> records) {
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
                                 std::nullopt,
                                 std::nullopt});
    }
    return TreeProviderSnapshot{std::move(providerId), TreeProviderKind::Git,
                                std::move(nodes)};
}

TreeProviderSnapshot TreeProviderSnapshot::fromSymbols(
    TreeProviderId providerId, std::vector<SymbolTreeRecord> records) {
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
                                 record.sourceLine,
                                 std::nullopt});
    }
    return TreeProviderSnapshot{std::move(providerId),
                                TreeProviderKind::Symbols, std::move(nodes)};
}

TreeProviderSnapshot TreeProviderSnapshot::fromSearch(
    TreeProviderId providerId, std::vector<SearchTreeRecord> records) {
    std::vector<TreeNode> nodes;
    nodes.reserve(records.size());
    for (auto& record : records) {
        const auto path = normalizeWorkspacePath(std::move(record.workspacePath));
        if (record.label.empty() || record.sourceColumn == 0) {
            throw std::invalid_argument(
                "search tree results require a label and one-based column");
        }
        auto lineKey = std::to_string(record.sourceLine);
        lineKey.insert(0, 20 - lineKey.size(), '0');
        nodes.push_back(TreeNode{
            nodeId(providerId, path + ":" + lineKey),
            std::nullopt,
            std::move(record.label),
            TreeNodeKind::SearchResult,
            std::nullopt,
            {},
            std::nullopt,
            path,
            record.sourceLine,
            record.sourceColumn});
    }
    return TreeProviderSnapshot{std::move(providerId),
                                TreeProviderKind::Search, std::move(nodes)};
}

void TreeModel::replaceProvider(TreeProviderSnapshot snapshot) {
    revision_ = TreeRevision{revision_.value() + 1};
    snapshot.revision_ = revision_;
    const TreeProviderId providerId = snapshot.providerId();
    auto iterator = std::lower_bound(
        providers_.begin(), providers_.end(), providerId,
        [](const ProviderState& state, const TreeProviderId& id) {
            return state.snapshot.providerId() < id;
        });
    if (iterator != providers_.end() &&
        iterator->snapshot.providerId() == snapshot.providerId()) {
        std::erase_if(iterator->expanded, [&](const TreeNodeId& id) {
            return findNode(snapshot, id) == nullptr;
        });
        iterator->bumpExpanded();
        iterator->snapshot = std::move(snapshot);
    } else {
        iterator = providers_.insert(
            iterator, ProviderState{std::move(snapshot)});
    }
    if (!activeProviderId_) activeProviderId_ = providerId;
    const auto& visible = iterator->visibleNodes();
    const bool stillValid =
        iterator->selected &&
        std::any_of(visible.begin(), visible.end(),
                    [&](const TreeNodeView& view) {
                        return view.node.id == *iterator->selected;
                    });
    if (!stillValid) {
        iterator->selected =
            visible.empty()
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
        provider->selected.reset();
        return false;
    }
    std::size_t index = 0;
    if (provider->selected) {
        for (std::size_t i = 0; i < visible.size(); ++i) {
            if (visible[i].node.id == *provider->selected) {
                index = std::min(visible.size() - 1, i + 1);
                break;
            }
        }
    }
    provider->selected = visible[index].node.id;
    revision_ = TreeRevision{revision_.value() + 1};
    return true;
}

bool TreeModel::selectPrevious() {
    auto* provider = activeProvider();
    if (provider == nullptr) return false;
    const auto& visible = provider->visibleNodes();
    if (visible.empty()) {
        provider->selected.reset();
        return false;
    }
    std::size_t index = 0;
    if (provider->selected) {
        for (std::size_t i = 0; i < visible.size(); ++i) {
            if (visible[i].node.id == *provider->selected) {
                index = (i == 0) ? 0 : i - 1;
                break;
            }
        }
    }
    provider->selected = visible[index].node.id;
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
    if (provider->selected && *provider->selected == nodeId) return true;
    provider->selected = nodeId;
    revision_ = TreeRevision{revision_.value() + 1};
    return true;
}

std::optional<TreeNode> TreeModel::selectedNode() const {
    const auto* provider = activeProvider();
    if (provider == nullptr || !provider->selected) return std::nullopt;
    const auto* node = findNode(provider->snapshot, *provider->selected);
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
        revision_ = TreeRevision{revision_.value() + 1};
    }
    return true;
}

bool TreeModel::activateOrCreate(const TreeProviderBinding& binding) {
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
    replaceProvider(TreeProviderSnapshot{binding.id, binding.kind, {}});
    return activateProvider(binding.id);
}

std::optional<SearchTreeState> TreeModel::searchState(
    const TreeProviderId& providerId) const {
    const auto provider = std::lower_bound(
        providers_.begin(), providers_.end(), providerId,
        [](const ProviderState& state, const TreeProviderId& id) {
            return state.snapshot.providerId() < id;
        });
    if (provider == providers_.end() ||
        provider->snapshot.providerId() != providerId) {
        return std::nullopt;
    }
    return provider->search;
}

bool TreeModel::setSearchState(const TreeProviderId& providerId,
                               SearchTreeState state) {
    const auto provider = std::lower_bound(
        providers_.begin(), providers_.end(), providerId,
        [](const ProviderState& candidate, const TreeProviderId& id) {
            return candidate.snapshot.providerId() < id;
        });
    if (provider == providers_.end() ||
        provider->snapshot.providerId() != providerId ||
        !provider->search) {
        return false;
    }
    if (*provider->search == state) return true;
    provider->search = std::move(state);
    revision_ = TreeRevision{revision_.value() + 1};
    return true;
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
        result.providers.push_back(TreeProviderView{
            provider->snapshot.providerId(), provider->snapshot.kind(),
            provider->visibleNodes(),
            provider->selected,
            provider->search});
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
