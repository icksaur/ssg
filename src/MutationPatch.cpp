#include <ssg/MutationPatch.h>

#include <set>
#include <stdexcept>
#include <utility>
#include <string>
#include <variant>

namespace ssg {

PresenceConfig PresenceConfig::initial(const ValidatedSchema& validated,
                                       const std::vector<UiNodeId>& hidden) {
    const UiSchema& schema = validated.schema();
    PresenceConfig config;
    config.generation_ = schema.generation;
    config.basis_ = PresenceBasis{0};
    for (const auto& region : schema.regions) {
        // Walk each region's tree, marking every node present.
        std::vector<const UiNode*> stack{&region.root};
        while (!stack.empty()) {
            const UiNode* node = stack.back();
            stack.pop_back();
            config.set(node->id, true);
            if (const auto* container =
                    std::get_if<UiContainer>(&node->content)) {
                for (const auto& child : container->children) {
                    stack.push_back(&child);
                }
            }
        }
    }
    for (const auto& id : hidden) {
        if (!validated.contains(id)) {
            throw std::invalid_argument(
                "PresenceConfig::initial: hidden id outside the schema");
        }
    }

    // Hiding a node hides its whole subtree -- the same semantics a patch hide
    // applies -- so an initially-hidden container never leaves a descendant marked
    // present. Find each hidden node, then mark it and its descendants absent.
    const std::set<UiNodeId> hiddenSet{hidden.begin(), hidden.end()};
    if (!hiddenSet.empty()) {
        std::vector<std::pair<const UiNode*, bool>> stack;  // node, ancestorHidden
        for (const auto& region : schema.regions) {
            stack.emplace_back(&region.root, false);
        }
        while (!stack.empty()) {
            const auto [node, ancestorHidden] = stack.back();
            stack.pop_back();
            const bool hide = ancestorHidden || hiddenSet.contains(node->id);
            if (hide) config.set(node->id, false);
            if (const auto* container =
                    std::get_if<UiContainer>(&node->content)) {
                for (const auto& child : container->children) {
                    stack.emplace_back(&child, hide);
                }
            }
        }
    }
    return config;
}

bool PresenceConfig::isPresent(const UiNodeId& id) const {
    const auto it = present_.find(id);
    return it != present_.end() && it->second;
}

void PresenceConfig::set(const UiNodeId& id, bool present) {
    present_[id] = present;
}

namespace {

// Parent/child structure of a schema, keyed by node id. Built once per apply so
// ancestor and subtree sets are cheap and the id-based rules can be checked
// without re-walking the tree per op.
struct Index {
    std::set<UiNodeId> all;
    std::map<UiNodeId, UiNodeId> parent;              // node -> its parent
    std::map<UiNodeId, std::vector<UiNodeId>> kids;    // node -> its children

    void addNode(const UiNode& node, const UiNodeId* parentId) {
        all.insert(node.id);
        if (parentId) {
            parent[node.id] = *parentId;
            kids[*parentId].push_back(node.id);
        }
        if (const auto* container = std::get_if<UiContainer>(&node.content)) {
            for (const auto& child : container->children) {
                addNode(child, &node.id);
            }
        }
    }

    [[nodiscard]] static Index build(const UiSchema& schema) {
        Index index;
        for (const auto& region : schema.regions) {
            index.addNode(region.root, nullptr);
        }
        return index;
    }

    // Self and all descendants (a hidden container hides its subtree).
    void collectSubtree(const UiNodeId& id, std::set<UiNodeId>& out) const {
        out.insert(id);
        const auto it = kids.find(id);
        if (it == kids.end()) return;
        for (const auto& child : it->second) collectSubtree(child, out);
    }

    // The proper ancestors of a node (a shown node implies showing these).
    void collectAncestors(const UiNodeId& id, std::set<UiNodeId>& out) const {
        auto it = parent.find(id);
        while (it != parent.end()) {
            out.insert(it->second);
            it = parent.find(it->second);
        }
    }
};

}  // namespace

PatchResult applyMutationPatch(const ValidatedSchema& validated,
                               const PresenceConfig& pre,
                               const MutationPatch& patch) {
    const UiSchema& schema = validated.schema();
    if (patch.generation != schema.generation) {
        return {"mutation patch targets a different generation"};
    }
    if (pre.generation() != schema.generation) {
        return {"presence pre-state is from a different generation"};
    }
    if (patch.basis != pre.basis()) {
        return {"mutation patch was predicted against a stale presence basis"};
    }

    const Index index = Index::build(schema);

    // Every op must name a real node, carry a valid kind, and no two ops may name
    // the same node (a conflict is rejected, never order-resolved).
    std::set<UiNodeId> targeted;
    for (const auto& op : patch.ops) {
        if (op.kind != MutationOpKind::Show && op.kind != MutationOpKind::Hide &&
            op.kind != MutationOpKind::Toggle) {
            return {"mutation patch has a corrupt operation kind"};
        }
        if (!index.all.contains(op.target)) {
            return {"mutation patch names unknown node \"" + op.target.value() +
                    "\""};
        }
        if (!targeted.insert(op.target).second) {
            return {"mutation patch has conflicting ops on node \"" +
                    op.target.value() + "\""};
        }
    }

    // Resolve toggles against the pre-state into explicit show/hide sets. Targets
    // are unique, so a node cannot land in both here.
    std::set<UiNodeId> explicitShow;
    std::set<UiNodeId> explicitHide;
    for (const auto& op : patch.ops) {
        MutationOpKind kind = op.kind;
        if (kind == MutationOpKind::Toggle) {
            kind = pre.isPresent(op.target) ? MutationOpKind::Hide
                                            : MutationOpKind::Show;
        }
        if (kind == MutationOpKind::Show) {
            explicitShow.insert(op.target);
        } else {
            explicitHide.insert(op.target);
        }
    }

    // Parent/child expansion: showing a node implies showing its ancestors;
    // hiding a node hides its whole subtree.
    std::set<UiNodeId> requiredShow = explicitShow;
    for (const auto& id : explicitShow) index.collectAncestors(id, requiredShow);
    std::set<UiNodeId> requiredHide;
    for (const auto& id : explicitHide) index.collectSubtree(id, requiredHide);

    // A node required both shown and hidden is a contradiction (e.g. hiding an
    // ancestor while showing a descendant). Reject rather than pick a winner.
    for (const auto& id : requiredShow) {
        if (requiredHide.contains(id)) {
            return {"mutation patch contradicts itself at node \"" + id.value() +
                    "\""};
        }
    }

    // Disjoint sets, so application is order-independent (determinism/replay).
    PresenceConfig post = pre;
    for (const auto& id : requiredHide) post.set(id, false);
    for (const auto& id : requiredShow) post.set(id, true);
    post.basis_ = pre.basis().next();
    return {std::nullopt, post};
}

}  // namespace ssg
