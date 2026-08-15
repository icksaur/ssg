#include <ssg/UiTree.h>

#include <functional>
#include <set>
#include <string>
#include <variant>

namespace ssg {

namespace {

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
        for (const auto& child : container->children) {
            walk(child, here, seen, error);
            if (error) return;
        }
    } else if (const auto* leaf = std::get_if<UiLeaf>(&node.content)) {
        const WidgetDescriptor& w = leaf->widget;
        if (w.kind == WidgetKind::View) {
            // A View names its client-rendered surface, and has no content to hug,
            // so its size must be Exact or Flex -- never Auto.
            if (!w.surface) {
                error = here + ": a \"view\" leaf requires a surface";
                return;
            }
            if (static_cast<std::size_t>(*w.surface) >= kViewSurfaceCount) {
                error = here + ": a \"view\" leaf names an unknown surface";
                return;
            }
            if (node.size.kind() == SizeKind::Auto) {
                error = here + ": a \"view\" leaf must be Exact- or Flex-sized";
                return;
            }
            // A View is opaque: it carries only its id + surface. Any widget-only
            // field is semantic state no View consumer reads, so it is rejected
            // rather than silently ignored.
            if (w.value || w.checked || w.width || w.role || w.command ||
                !w.sigil.empty() || w.rank != 0 || w.keep ||
                w.overflow != Overflow::None) {
                error = here + ": a \"view\" leaf carries only an id and a surface";
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

namespace {

// A well-known area, wherever it appears in the tree, must be a container (it holds
// child widgets or view leaves, never a bare leaf). The root additionally must BE
// the schema root and carry the root id. This is the typed-identity contract a
// native client relies on when it keys off an area id: uniqueness alone is not
// enough. Returns a message on violation.
std::optional<std::string> checkWellKnownAreas(const UiSchema& schema) {
    // The root identity: the schema's root node is the "root" area and a container.
    if (schema.root.id.value() != wellKnownAreaId(WellKnownArea::Root)) {
        return std::string{"root: the schema root must have the \"root\" id"};
    }
    if (!schema.root.isContainer()) {
        return std::string{"root: the root area must be a container"};
    }
    // Header/footer, wherever present, must be containers and direct children of the
    // root (their canonical ancestry in the whole-screen tree).
    const auto* rootContainer = std::get_if<UiContainer>(&schema.root.content);
    std::set<std::string> rootChildIds;
    if (rootContainer) {
        for (const auto& child : rootContainer->children)
            rootChildIds.insert(child.id.value());
    }
    for (const WellKnownArea area : {WellKnownArea::Header, WellKnownArea::Footer}) {
        const std::string id{wellKnownAreaId(area)};
        std::optional<std::string> found;
        // Find the node with this id anywhere in the tree.
        const std::function<void(const UiNode&)> find = [&](const UiNode& node) {
            if (node.id.value() == id) {
                if (!node.isContainer())
                    found = id + ": a well-known area must be a container";
                else if (!rootChildIds.contains(id))
                    found = id + ": a well-known area must be a direct child of "
                                 "the root";
            }
            if (const auto* c = std::get_if<UiContainer>(&node.content))
                for (const auto& child : c->children) find(child);
        };
        find(schema.root);
        if (found) return found;
    }
    return std::nullopt;
}

}  // namespace

UiSchemaValidation validateUiSchema(const UiSchema& schema) {
    std::set<std::string> seenIds;
    std::optional<std::string> error;
    walk(schema.root, std::string{}, seenIds, error);
    if (error) return {error};
    return {};
}

UiSchemaValidation validateWellKnownAreas(const UiSchema& schema) {
    if (auto areaError = checkWellKnownAreas(schema)) return {areaError};
    return {};
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

ValidatedSchema::ValidatedSchema(UiSchema schema)
    : schema_{std::move(schema)}, nodeIds_{uiSchemaNodeIds(schema_)} {}

ValidatedSchemaResult ValidatedSchema::validate(UiSchema schema) {
    UiSchemaValidation validation = validateUiSchema(schema);
    if (!validation.ok()) return ValidatedSchemaResult{*validation.error};
    return ValidatedSchemaResult{ValidatedSchema{std::move(schema)}};
}

}  // namespace ssg
