#pragma once

#include <ssg/DiffModel.h>
#include <ssg/Theme.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <system_error>

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
    Filesystem = 0,
    Git = 1,
    Symbols = 2,
};

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
    Root = 0,
    Directory = 1,
    File = 2,
    Symlink = 3,
    GitEntry = 4,
    Symbol = 5,
};
struct GitTreeAffordance {
    DiffFileStatus status = DiffFileStatus::Modified;
    std::string shortLabel;
    SemanticRole role = SemanticRole::DiffModified;
    bool operator==(const GitTreeAffordance&) const = default;
};

[[nodiscard]] GitTreeAffordance gitTreeAffordance(DiffFileStatus status);

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

namespace detail {

[[nodiscard]] bool filesystemTreeEntryDisappeared(
    const std::error_code& error) noexcept;

[[nodiscard]] std::optional<TreeNode> inspectFilesystemTreeEntry(
    const TreeProviderId& providerId, const std::filesystem::path& root,
    const std::filesystem::directory_entry& entry);

}  // namespace detail

class TreeProviderSnapshot {
public:
    TreeProviderSnapshot(TreeProviderId providerId, TreeProviderKind kind,
                         std::vector<TreeNode> nodes);
    static TreeProviderSnapshot fromFilesystem(
        TreeProviderId providerId, const std::filesystem::path& canonicalCwd);
    static TreeProviderSnapshot fromGit(TreeProviderId providerId,
                                        std::vector<GitTreeRecord> records);
    static TreeProviderSnapshot fromSymbols(
        TreeProviderId providerId, std::vector<SymbolTreeRecord> records);

    const TreeProviderId& providerId() const noexcept { return providerId_; }
    TreeProviderKind kind() const noexcept { return kind_; }
    TreeRevision revision() const noexcept { return revision_; }
    const std::vector<TreeNode>& nodes() const noexcept { return nodes_; }

private:
    friend class TreeModel;

    TreeProviderId providerId_;
    TreeProviderKind kind_;
    TreeRevision revision_;
    std::vector<TreeNode> nodes_;
};

struct GitTreeRecord {
    std::string workspacePath;
    std::string label;
    DiffFileStatus status = DiffFileStatus::Modified;
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
    bool activateOrCreate(const TreeProviderBinding& binding);
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

} // namespace ssg
