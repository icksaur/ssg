#pragma once

#include <ssg/detail/generated/semantic_wire_manifest.h>

#include <ssg/Theme.h>
#include <ssg/Viewport.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

enum class TreeProviderKind {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_TREE_PROVIDER_KIND_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_TREE_PROVIDER_KIND_ENUMERATORS

[[nodiscard]] std::string_view treeProviderLabel(TreeProviderKind kind);
[[nodiscard]] bool treeProviderCanBeCreatedEmpty(TreeProviderKind kind);

// Which tree provider a caller wants active, as a typed pair rather than a
// string. `activateOrCreate` uses `kind` to decide whether a missing provider
// may be lazily created (Git/Symbols) or is a genuine failure (Filesystem).
struct TreeProviderBinding {
    TreeProviderId id;
    TreeProviderKind kind = TreeProviderKind::Filesystem;
    bool operator==(const TreeProviderBinding&) const = default;
};

enum class TreeNodeKind {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_TREE_NODE_KIND_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_TREE_NODE_KIND_ENUMERATORS
enum class GitTreeStatus {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_GIT_TREE_STATUS_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_GIT_TREE_STATUS_ENUMERATORS

struct GitTreeAffordance {
    GitTreeStatus status = GitTreeStatus::Modified;
    std::string shortLabel;
    SemanticRole role = SemanticRole::DiffModified;
    bool operator==(const GitTreeAffordance&) const = default;
};

[[nodiscard]] GitTreeAffordance gitTreeAffordance(GitTreeStatus status);

struct GitTreeRecord;
struct SymbolTreeRecord;

struct TreeNodeCommand {
    std::string id;
    std::string label;
    bool operator==(const TreeNodeCommand&) const = default;
};

struct TreeNode {
    TreeNodeId id;
    std::optional<TreeNodeId> parentId;
    std::string label;
    TreeNodeKind kind;
    std::optional<std::string> icon;
    std::vector<TreeNodeCommand> commands;
    std::optional<GitTreeAffordance> gitStatus;
    std::optional<std::string> workspacePath;
    std::optional<std::uint32_t> sourceLine;
    bool expandable = false;
    bool operator==(const TreeNode&) const = default;
};

class TreeProviderSnapshot {
public:
    TreeProviderSnapshot(TreeProviderId providerId, TreeProviderKind kind,
                         TreeRevision revision, std::vector<TreeNode> nodes);
    static TreeProviderSnapshot fromFilesystem(
        TreeProviderId providerId, const std::filesystem::path& canonicalCwd,
        TreeRevision revision);
    static TreeProviderSnapshot fromGit(TreeProviderId providerId,
                                        TreeRevision revision,
                                        std::vector<GitTreeRecord> records);
    static TreeProviderSnapshot fromSymbols(
        TreeProviderId providerId, TreeRevision revision,
        std::vector<SymbolTreeRecord> records);

    const TreeProviderId& providerId() const noexcept { return providerId_; }
    TreeProviderKind kind() const noexcept { return kind_; }
    TreeRevision revision() const noexcept { return revision_; }
    const std::vector<TreeNode>& nodes() const noexcept { return nodes_; }

private:
    TreeProviderId providerId_;
    TreeProviderKind kind_;
    TreeRevision revision_;
    std::vector<TreeNode> nodes_;
};

struct GitTreeRecord {
    std::string workspacePath;
    std::string label;
    GitTreeStatus status = GitTreeStatus::Modified;
    std::vector<TreeNodeCommand> commands;
};

struct SymbolTreeRecord {
    std::string stableKey;
    std::optional<std::string> parentKey;
    std::string label;
    std::optional<std::string> workspacePath;
    std::optional<std::uint32_t> sourceLine;
    std::vector<TreeNodeCommand> commands;
};

struct TreeCommandDescriptor {
    std::string_view id;
    bool operator==(const TreeCommandDescriptor&) const = default;
};

class TreeCommandSet {
public:
    TreeCommandSet(const TreeCommandSet&) = default;
    TreeCommandSet& operator=(const TreeCommandSet&) = delete;
    const std::array<TreeCommandDescriptor, 8>& descriptors() const noexcept {
        return descriptors_;
    }

private:
    friend TreeCommandSet treeCommandSet();
    TreeCommandSet();
    const std::array<TreeCommandDescriptor, 8> descriptors_;
};

TreeCommandSet treeCommandSet();

struct TreeNodeView {
    TreeNode node;
    std::size_t depth = 0;
    bool expanded = false;
    bool operator==(const TreeNodeView&) const = default;
};

struct TreeProviderView {
    TreeProviderId providerId;
    TreeProviderKind kind;
    std::vector<TreeNodeView> nodes;
    std::optional<TreeNodeId> selected;
    bool operator==(const TreeProviderView&) const = default;
};

// Grid projection of a tree provider's scroll window. `firstVisible` is the index into the provider's
// `nodes` of the first on-screen node; `scrollbar` is its thumb geometry;
// `visibleNodeIds` is the bounded viewport_row -> node id hit map for the visible
// window only (empty when the panel is hidden). The semantic `nodes` carries the
// full expanded list; a grid client windows it with this.
struct TreeWindow {
    std::uint32_t firstVisible = 0;
    ScrollbarMetrics scrollbar{};
    std::vector<TreeNodeId> visibleNodeIds;
    bool operator==(const TreeWindow&) const = default;
};

struct TreeViewState {
    TreeRevision revision{0};
    std::vector<TreeProviderView> providers;
    std::optional<TreeProviderBinding> activeBinding;
    bool operator==(const TreeViewState&) const = default;
};

// Resolve the explicitly active provider. Invalid state returns null; callers
// never infer active identity from provider ordering.
[[nodiscard]] const TreeProviderView* activeTreeProvider(
    const TreeViewState& state) noexcept;
[[nodiscard]] bool isValidTreeViewState(const TreeViewState& state) noexcept;

struct TreeCommandInvocation {
    TreeProviderId providerId;
    TreeNodeId nodeId;
    std::string commandId;
    bool operator==(const TreeCommandInvocation&) const = default;
};

// Argument for `tree.select`: the node to make the active provider's selection.
// A dedicated payload (rather than the TreeCommandInvocation triple) keeps a
// click's argument minimal -- a pointer click needs only the node id.
struct TreeSelectArguments {
    TreeNodeId nodeId;
    bool operator==(const TreeSelectArguments&) const = default;
};

class TreeModel {
public:
    void replaceProvider(TreeProviderSnapshot snapshot);
    bool toggleExpanded(const TreeProviderId& providerId,
                         const TreeNodeId& nodeId);
    bool isExpanded(const TreeProviderId& providerId,
                     const TreeNodeId& nodeId) const;
    bool activateProvider(const TreeProviderId& providerId);

    // The id, kind, and revision of every present provider, WITHOUT materializing node
    // views -- a cheap enumeration for a caller that needs only provider identity and
    // revision (e.g. preparing a tree-backing transition, or stamping a replacement above
    // the provider it replaces), not the full viewState().
    struct ProviderIdentity {
        TreeProviderBinding binding;
        TreeRevision revision{0};
    };
    [[nodiscard]] std::vector<ProviderIdentity> providerIdentities() const;
    [[nodiscard]] std::optional<TreeProviderBinding> activeProviderBinding() const;

    // Activate the provider named by `binding`. When it does not exist yet and
    // its kind is Git or Symbols, create it empty and activate it -- a panel can
    // be shown before its provider has any content. A Filesystem binding is NEVER
    // created here (the filesystem provider is seeded at construction), so a
    // missing one is a genuine failure. Returns false when activation fails and
    // nothing was created.
    //
    // `revisionForCreate` is invoked ONLY on the create path, so a caller whose
    // revision source has a side effect (e.g. a post-increment counter) does not
    // consume a revision when merely re-activating an existing provider. It must
    // be callable: an empty function throws std::invalid_argument (a clear error
    // rather than an opaque std::bad_function_call on the create path). Takes a
    // typed binding, not a shell panel label: the tree does not know the shell's
    // presentation vocabulary (the label -> binding mapping lives in the runtime
    // seam).
    bool activateOrCreate(const TreeProviderBinding& binding,
                          const std::function<TreeRevision()>& revisionForCreate);
    std::optional<TreeCommandInvocation> invokeNodeCommand(
        const TreeProviderId& providerId, const TreeNodeId& nodeId,
        std::string_view commandId) const;

    // Selection navigation over the active provider's visible nodes.  Selection
    // is library-owned UI state so every client presents the same focus.
    bool selectNext();
    bool selectPrevious();
    // Set the active provider's selection to `node_id`. Returns false (leaving
    // the selection unchanged) when no provider is active or the id is not among
    // the active provider's visible nodes.
    bool select(const TreeNodeId& nodeId);
    bool toggleSelected();
    [[nodiscard]] std::optional<TreeNode> selectedNode() const;

    TreeViewState viewState() const;

    // Test instrumentation: the number of full visibleNodes recomputations
    // (cache misses) performed since the last reset. A navigation that changes
    // neither a provider's tree revision nor its expanded set must recompute
    // nothing; this counter is what lets a test assert that, without timing.
    // Per-thread, like Renderer::renderSegmentationCalls.
    [[nodiscard]] static std::uint64_t visibleNodesRecomputeCount();
    static void resetVisibleNodesRecomputeCount();

    // How many nodes the ACTIVE provider currently shows. Cheaper than
    // viewState(), which builds EVERY provider's view and copies each node
    // list, when the scroll paths need only this one number -- a thumb drag
    // asks once per pointer motion. It still walks the active provider, and
    // deliberately reuses the same traversal viewState() does rather than
    // adding a second definition of "which nodes are visible".
    [[nodiscard]] std::size_t activeVisibleNodeCount() const;

private:
    struct ProviderState {
        TreeProviderSnapshot snapshot;
        std::vector<TreeNodeId> expanded;

        // Memoization of the visible-node list. The list is a pure function of
        // (snapshot, expanded); it is recomputed only when the snapshot revision
        // changes (a provider replacement) or the expanded set is mutated. A
        // cursor move or a selection change touches neither, so it is served
        // from cache. `expandedVersion` is bumped by EVERY mutator of `expanded`
        // -- keying on the snapshot revision alone would serve a stale list
        // after an expand/collapse, since toggleExpanded does not change the
        // snapshot revision.
        std::uint64_t expandedVersion = 0;
        struct VisibleCache {
            TreeRevision revision{0};
            std::uint64_t expandedVersion = 0;
            std::vector<TreeNodeView> nodes;
        };
        mutable std::optional<VisibleCache> visibleCache;

        [[nodiscard]] const std::vector<TreeNodeView>& visibleNodes() const;
        void bumpExpanded() noexcept { ++expandedVersion; }
    };

    [[nodiscard]] ProviderState* activeProvider();
    [[nodiscard]] const ProviderState* activeProvider() const;

    TreeRevision revision_{0};
    std::vector<ProviderState> providers_;
    std::optional<TreeProviderId> activeProviderId_;
    std::optional<TreeNodeId> selected_;
};

struct TreeProviderDelta {
    TreeProviderId providerId;
    TreeProviderKind kind;
    bool removeProvider = false;
    std::size_t start = 0;
    std::size_t eraseCount = 0;
    std::vector<TreeNodeView> insert;
    std::optional<TreeNodeId> selected;
    bool operator==(const TreeProviderDelta&) const = default;
};

struct TreeDelta {
    TreeRevision baseRevision{0};
    TreeRevision revision{0};
    bool snapshotRequired = false;
    std::vector<TreeProviderDelta> providers;
    // The complete target ordering. The active provider is first, followed by
    // inactive providers; node splices alone cannot express that permutation.
    std::vector<TreeProviderId> providerOrder;
    std::optional<TreeProviderBinding> activeBinding;

    std::size_t operationCount() const noexcept;
    bool operator==(const TreeDelta&) const = default;
};

enum class TreeReplayError { None, StaleRevision, SnapshotRequired, MalformedDelta };

struct TreeReplayResult {
    std::optional<TreeViewState> state;
    TreeReplayError error = TreeReplayError::None;
    bool accepted() const noexcept { return state.has_value(); }
};

class TreeDeltaCodec {
public:
    [[nodiscard]] TreeDelta derive(const TreeViewState& base,
                                   const TreeViewState& target,
                                   std::size_t maximumOperations) const;
    [[nodiscard]] TreeReplayResult replay(const TreeViewState& base,
                                          const TreeDelta& delta) const;
};

} // namespace ssg
