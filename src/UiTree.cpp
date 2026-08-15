#include <ssg/UiTree.h>

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

UiSchemaValidation validateUiSchema(const UiSchema& schema) {
    std::set<std::string> seenIds;
    std::optional<std::string> error;
    walk(schema.root, std::string{}, seenIds, error);
    if (error) return {error};
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
