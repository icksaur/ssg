#include <ssg/UiTree.h>

#include <algorithm>
#include <set>
#include <string>
#include <variant>

namespace ssg {

namespace {

const UiNode* findNode(const UiNode& node, const UiNodeId& id) noexcept {
    if (node.id == id) return &node;
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children) {
            if (const UiNode* found = findNode(child, id)) return found;
        }
    }
    return nullptr;
}

UiNode* findMutableNode(UiNode& node, const UiNodeId& id) noexcept {
    if (node.id == id) return &node;
    if (auto* container = std::get_if<UiContainer>(&node.content)) {
        for (auto& child : container->children) {
            if (UiNode* found = findMutableNode(child, id)) return found;
        }
    }
    return nullptr;
}

std::optional<ResolvedUiNodeStyle> resolveStyle(
    const UiNode& node, std::string_view target,
    ResolvedUiNodeStyle inherited) {
    inherited = node.style.resolve(inherited);
    if (node.id.value() == target) return inherited;
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children) {
            if (auto resolved = resolveStyle(child, target, inherited)) {
                return resolved;
            }
        }
    }
    return std::nullopt;
}

// Returns the effective visibility of `target` if found under `node`, computed as
// `node.visible` propagated down through `ancestorsVisible`; nullopt if `target`
// is not in this subtree, so a caller can distinguish "not found" from "found but
// hidden".
std::optional<bool> effectiveVisibility(const UiNode& node, bool ancestorsVisible,
                                        const UiNodeId& target) {
    const bool visible = ancestorsVisible && node.visible;
    if (node.id == target) return visible;
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children) {
            if (auto found = effectiveVisibility(child, visible, target)) {
                return found;
            }
        }
    }
    return std::nullopt;
}

// Walks the tree under `node`, appending its path segment, checking each id is
// non-empty and not already seen. Sets `error` and stops on the first violation.
void walk(const UiNode& node, std::string path, std::set<std::string>& seen,
          std::optional<std::string>& error) {
    if (error) return;

    const std::string here =
        path.empty() ? node.id.value() : path + "/" + node.id.value();

    if (node.id.empty()) {
        error = here + ": empty node id";
        return;
    }
    if (!seen.insert(node.id.value()).second) {
        error = here + ": duplicate node id \"" + node.id.value() + "\"";
        return;
    }

    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        const bool hasResponsive = std::ranges::any_of(
            container->children, [](const UiNode& child) {
                return child.size.kind() == SizeKind::Responsive;
            });
        const bool hasAuto = std::ranges::any_of(
            container->children, [](const UiNode& child) {
                return child.size.kind() == SizeKind::Auto;
            });
        if (hasResponsive && hasAuto) {
            error = here +
                    ": Responsive and Auto direct children cannot be mixed";
            return;
        }
        for (const auto& child : container->children) {
            walk(child, here, seen, error);
            if (error) return;
        }
    } else if (const auto* leaf = std::get_if<UiLeaf>(&node.content)) {
        const WidgetDescriptor& w = leaf->widget;
        if (w.kind == WidgetKind::View) {
            // A View names its client-rendered surface, and has no content to hug,
            // so its size must be explicit rather than Auto.
            if (!w.surface) {
                error = here + ": a \"view\" leaf requires a surface";
                return;
            }
            if (std::ranges::find(kAllViewSurfaces, *w.surface) ==
                kAllViewSurfaces.end()) {
                error = here + ": a \"view\" leaf names an unknown surface";
                return;
            }
            if (node.size.kind() == SizeKind::Auto &&
                *w.surface != ViewSurface::Notice &&
                *w.surface != ViewSurface::ExternalModification) {
                // A View names its client-rendered surface and has no content to
                // hug, so it is Exact- or Flex-sized -- EXCEPT the
                // draft-conflict notice and the external-modification bar,
                // whose intrinsic (reservation-sized) footprints the runtime
                // sizes, so they alone may be Auto.
                error = here + ": a \"view\" leaf must not be Auto-sized";
                return;
            }
            // A View is opaque: it carries only its id + surface. Any widget-only
            // field is semantic state no View consumer reads, so it is rejected
            // rather than silently ignored.
            if (w.width || w.role || w.command || !w.sigil.empty() ||
                w.rank != 0 || w.keep || w.overflow != Overflow::None) {
                error = here + ": a \"view\" leaf carries only an id and a surface";
                return;
            }
        } else if (w.kind == WidgetKind::StatusActions) {
            // A StatusActions widget is opaque and client-rendered from the
            // promptStatus section (the selected status item's actions); like a
            // View it carries only its id, but it has intrinsic content (a variable
            // action list), so unlike a View it may be Auto-sized. Any widget-only
            // field is semantic state no consumer reads, so it is rejected.
            if (w.width || w.role || w.command || w.surface ||
                !w.sigil.empty() || w.rank != 0 || w.keep ||
                w.overflow != Overflow::None) {
                error = here + ": a \"status_actions\" leaf carries only an id";
                return;
            }
        } else if (w.surface) {
            // A surface belongs to a View leaf alone.
            error = here + ": \"surface\" is only allowed on a \"view\" leaf";
            return;
        }
    }
}

}  // namespace

UiSchemaValidation validateUiSchema(const UiSchema& schema) {
    std::set<std::string> seenIds;
    std::optional<std::string> error;
    walk(schema.root, std::string{}, seenIds, error);
    if (error) return {error};
    return {};
}

std::optional<ResolvedUiNodeStyle> resolveUiNodeStyle(
    const UiSchema& schema, std::string_view nodeId) {
    return resolveStyle(schema.root, nodeId, {});
}

const UiNode* findUiNode(const UiSchema& schema,
                         const UiNodeId& id) noexcept {
    return findNode(schema.root, id);
}

bool isUiNodeVisible(const UiSchema& schema, const UiNodeId& id) noexcept {
    return effectiveVisibility(schema.root, true, id).value_or(false);
}

FocusTarget effectiveUiFocus(const UiSchema& schema) noexcept {
    return *findUiNode(schema, schema.focusPath.back())->focusContext;
}

namespace {

bool validFocusPath(const UiSchema& schema) {
    if (schema.focusPath.empty()) return false;
    const bool everyNodeExists = std::all_of(
        schema.focusPath.begin(), schema.focusPath.end(),
        [&](const UiNodeId& id) {
            const UiNode* node = findUiNode(schema, id);
            return node && node->focusContext;
        });
    if (!everyNodeExists) return false;
    const UiNode* base = findUiNode(schema, schema.focusPath.front());
    const bool validBase =
        base && (base->focusContext == FocusTarget::Editor ||
                 base->focusContext == FocusTarget::Panel);
    return validBase && isUiNodeVisible(schema, schema.focusPath.back());
}

}  // namespace

UiSchemaValidation validatePublishedUiTree(const UiSchema& schema) {
    if (auto error = validateUiSchema(schema); !error.ok()) return error;
    if (!validFocusPath(schema)) {
        return {std::string{"focusPath: must name declared focus hosts, start "
                            "at an editor or sidebar host, and end at an "
                            "effectively visible node"}};
    }
    return {};
}

UiSchema requirePublishedUiTree(UiSchema schema) {
    const UiSchemaValidation validation = validatePublishedUiTree(schema);
    if (!validation.ok()) {
        throw std::invalid_argument("invalid UI tree: " + *validation.error);
    }
    return schema;
}

namespace {

void collectIds(const UiNode& node, std::set<UiNodeId>& out) {
    out.insert(node.id);
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children) collectIds(child, out);
    }
}

}  // namespace

std::set<UiNodeId> uiSchemaNodeIds(const UiSchema& schema) {
    std::set<UiNodeId> ids;
    collectIds(schema.root, ids);
    return ids;
}

UiNode emptyUiRoot() {
    return UiNode{UiNodeId{std::string{kRootNodeId}}, Size::flex(),
                  UiContainer{Axis::Column, {}, {}, {}}};
}

void setUiNodeVisible(UiSchema& schema, const UiNodeId& id, bool visible) {
    UiNode* node = findMutableNode(schema.root, id);
    if (!node) {
        throw std::invalid_argument("setUiNodeVisible: unknown node id");
    }
    node->visible = visible;
}

}  // namespace ssg
