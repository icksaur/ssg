#include <ssg/UiTree.h>

#include <algorithm>
#include <array>
#include <set>
#include <span>
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

std::optional<ResolvedUiNodeStyle> resolveStyle(
    const UiNode& node, std::string_view target,
    ResolvedUiNodeStyle inherited) {
    if (node.style.foreground) inherited.foreground = node.style.foreground;
    if (node.style.background) inherited.background = node.style.background;
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
                // validateWellKnownAreas pins each allowance to its canonical node.
                error = here + ": a \"view\" leaf must not be Auto-sized";
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
        } else if (w.kind == WidgetKind::StatusActions) {
            // A StatusActions widget is opaque and client-rendered from the
            // promptStatus section (the selected status item's actions); like a
            // View it carries only its id, but it has intrinsic content (a variable
            // action list), so unlike a View it may be Auto-sized. Any widget-only
            // field is semantic state no consumer reads, so it is rejected.
            if (w.value || w.checked || w.width || w.role || w.command ||
                w.surface || !w.sigil.empty() || w.rank != 0 || w.keep ||
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

std::optional<ResolvedUiNodeStyle> resolveUiNodeStyle(
    const UiSchema& schema, std::string_view nodeId) {
    return resolveStyle(schema.root, nodeId, {});
}

const UiNode* findUiNode(const UiSchema& schema,
                         const UiNodeId& id) noexcept {
    return findNode(schema.root, id);
}

namespace {

std::optional<std::string> requireChildren(const UiNode& node,
                                           std::string_view label,
                                           std::span<const std::string_view> ids,
                                           const UiContainer*& container) {
    container = std::get_if<UiContainer>(&node.content);
    if (!container) return std::string{label} + ": must be a container";
    if (container->children.size() != ids.size()) {
        return std::string{label} + ": must have the canonical children";
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (container->children[i].id.value() != ids[i]) {
            return std::string{label} + ": child order must be canonical";
        }
    }
    return std::nullopt;
}

std::optional<std::string> requireViewLeaf(const UiNode& node, std::string_view id,
                                           ViewSurface surface) {
    if (node.id.value() != id) return std::string{id} + ": child order must be canonical";
    const auto* leaf = std::get_if<UiLeaf>(&node.content);
    if (!leaf) return std::string{id} + ": must be a View leaf";
    if (leaf->widget.kind != WidgetKind::View) {
        return std::string{id} + ": must be a View leaf";
    }
    if (!leaf->widget.surface || *leaf->widget.surface != surface) {
        return std::string{id} + ": must name its canonical View surface";
    }
    return std::nullopt;
}

std::optional<std::string> requireFooterPrompt(const UiNode& node) {
    if (node.id.value() != kFooterPromptNodeId) {
        return std::string{kFooterPromptNodeId} +
               ": child order must be canonical";
    }
    const auto* column = std::get_if<UiContainer>(&node.content);
    if (!column || column->axis != Axis::Column ||
        node.size.kind() != SizeKind::Auto) {
        return std::string{kFooterPromptNodeId} +
               ": must be an Auto-sized Column container";
    }
    for (std::size_t index = 0; index < column->children.size(); ++index) {
        const UiNode& child = column->children[index];
        if (child.id.value() == kFooterPromptOptionsNodeId) {
            if (index + 1 != column->children.size()) {
                return std::string{kFooterPromptOptionsNodeId} +
                       ": must be the final prompt row";
            }
            const auto* options = std::get_if<UiContainer>(&child.content);
            if (!options || options->axis != Axis::Row ||
                child.size.kind() != SizeKind::Exact ||
                options->children.empty()) {
                return std::string{kFooterPromptOptionsNodeId} +
                       ": must be an Exact-sized non-empty Row";
            }
            for (std::size_t option = 0; option < options->children.size();
                 ++option) {
                const UiNode& item = options->children[option];
                const auto* leaf = std::get_if<UiLeaf>(&item.content);
                const bool count = option + 1 == options->children.size();
                if (!leaf ||
                    leaf->widget.kind !=
                        (count ? WidgetKind::Label : WidgetKind::Checkbox) ||
                    item.size.kind() !=
                        (count ? SizeKind::Flex : SizeKind::Exact)) {
                    return std::string{kFooterPromptOptionsNodeId} +
                           ": must contain exact Checkboxes followed by a "
                           "flexible Label";
                }
            }
            continue;
        }
        const auto* leaf = std::get_if<UiLeaf>(&child.content);
        if (!leaf || leaf->widget.kind != WidgetKind::TextInput ||
            child.size.kind() != SizeKind::Exact) {
            return std::string{kFooterPromptNodeId} +
                   ": input rows must be exact TextInput leaves";
        }
    }
    return std::nullopt;
}

// The whole-screen well-known-area contract: the complete canonical topology the
// assembler publishes, including required containers, View leaves, surfaces,
// parentage, and sibling order.
std::optional<std::string> rejectStraySurface(const UiNode& node,
                                              ViewSurface surface,
                                              std::string_view canonicalId) {
    if (const auto* leaf = std::get_if<UiLeaf>(&node.content)) {
        if (leaf->widget.kind == WidgetKind::View && leaf->widget.surface &&
            *leaf->widget.surface == surface &&
            node.id.value() != canonicalId) {
            return std::string{node.id.value()} +
                   ": the " + std::string{viewSurfaceName(surface)} +
                   " surface belongs only to the \"" +
                   std::string{canonicalId} + "\" node";
        }
        return std::nullopt;
    }
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children) {
            if (auto err = rejectStraySurface(child, surface, canonicalId))
                return err;
        }
    }
    return std::nullopt;
}

std::optional<std::string> checkWellKnownAreas(const UiSchema& schema) {
    if (schema.root.id.value() != wellKnownAreaId(WellKnownArea::Root)) {
        return std::string{"root: the required \"root\" area must be the schema "
                           "root"};
    }
    const UiContainer* root = nullptr;
    constexpr std::array rootIds{kHeaderNodeId, kBodyNodeId,
                                 kFooterPromptNodeId, kFooterNodeId};
    if (auto err = requireChildren(schema.root, "root", rootIds, root)) return err;
    const UiNode& header = root->children[0];
    if (header.id.value() != wellKnownAreaId(WellKnownArea::Header) ||
        !header.isContainer()) {
        return std::string{"header: must be a direct child container of root"};
    }
    const UiNode& body = root->children[1];
    // The footer prompt sits between the body and footer as the authoritative
    // control-layout subtree and is hidden by presence while inactive.
    if (auto err = requireFooterPrompt(root->children[2]))
        return err;
    const UiNode& footer = root->children[3];
    if (footer.id.value() != wellKnownAreaId(WellKnownArea::Footer) ||
        !footer.isContainer()) {
        return std::string{"footer: must be a direct child container of root"};
    }

    const UiContainer* bodyContainer = nullptr;
    constexpr std::array bodyIds{kPanelNodeId, kContentNodeId};
    if (auto err = requireChildren(body, "body", bodyIds, bodyContainer)) return err;

    const UiContainer* panel = nullptr;
    constexpr std::array panelIds{kTreeNodeId};
    if (auto err = requireChildren(bodyContainer->children[0], "panel", panelIds, panel))
        return err;
    if (auto err = requireViewLeaf(panel->children[0], kTreeNodeId,
                                   ViewSurface::Tree))
        return err;

    const UiContainer* content = nullptr;
    constexpr std::array contentIds{
        kTabBarNodeId, kNoticeNodeId, kExternalModNodeId, kEditorNodeId,
        kFindResultsViewportNodeId};
    if (auto err =
            requireChildren(bodyContainer->children[1], "content", contentIds, content))
        return err;
    if (content->scroll != ScrollAxis::None) {
        return std::string{"content: must not be a scroll viewport"};
    }
    if (auto err = requireViewLeaf(content->children[0], kTabBarNodeId,
                                   ViewSurface::TabBar))
        return err;
    if (auto err = requireViewLeaf(content->children[1], kNoticeNodeId,
                                   ViewSurface::Notice))
        return err;
    if (auto err = requireViewLeaf(content->children[2], kExternalModNodeId,
                                   ViewSurface::ExternalModification))
        return err;
    const UiContainer* editor = nullptr;
    constexpr std::array editorIds{kDocumentViewportNodeId};
    if (auto err =
            requireChildren(content->children[3], "editor", editorIds, editor))
        return err;
    const UiContainer* documentViewport = nullptr;
    constexpr std::array documentIds{kDocumentNodeId};
    if (auto err = requireChildren(editor->children[0], "document viewport",
                                   documentIds, documentViewport))
        return err;
    if (documentViewport->scroll != ScrollAxis::Vertical) {
        return std::string{"document viewport: must scroll vertically"};
    }
    if (auto err = requireViewLeaf(documentViewport->children[0], kDocumentNodeId,
                                   ViewSurface::Document))
        return err;
    const UiContainer* findResultsViewport = nullptr;
    constexpr std::array findResultsIds{kFindResultsNodeId};
    if (auto err = requireChildren(content->children[4], "find-results viewport",
                                   findResultsIds, findResultsViewport))
        return err;
    if (findResultsViewport->scroll != ScrollAxis::Vertical) {
        return std::string{"find-results viewport: must scroll vertically"};
    }
    if (auto err = requireViewLeaf(findResultsViewport->children[0],
                                   kFindResultsNodeId,
                                   ViewSurface::FindResults))
        return err;
    if (auto err = rejectStraySurface(schema.root, ViewSurface::Notice,
                                      kNoticeNodeId))
        return err;
    if (auto err = rejectStraySurface(schema.root,
                                      ViewSurface::ExternalModification,
                                      kExternalModNodeId))
        return err;
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
