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
    }
}

}  // namespace

UiSchemaValidation validateUiSchema(const UiSchema& schema) {
    std::set<std::string> seenIds;
    std::set<RegionRole> seenRoles;
    std::optional<std::string> error;

    for (const auto& region : schema.regions) {
        if (!seenRoles.insert(region.role).second) {
            return {std::string{regionRoleName(region.role)} +
                    ": duplicate region role"};
        }
        walk(region.root, std::string{regionRoleName(region.role)}, seenIds,
             error);
        if (error) return {error};
    }

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
    for (const auto& region : schema.regions) collectIds(region.root, ids);
    return ids;
}

ValidatedSchema::ValidatedSchema(UiSchema schema)
    : schema_{std::move(schema)}, nodeIds_{uiSchemaNodeIds(schema_)} {}

ValidatedSchemaResult ValidatedSchema::validate(UiSchema schema) {
    UiSchemaValidation validation = validateUiSchema(schema);
    if (!validation.ok()) return ValidatedSchemaResult{*validation.error};
    return ValidatedSchemaResult{ValidatedSchema{std::move(schema)}};
}

}  // namespace ssg
