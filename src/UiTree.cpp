#include <ssg/UiTree.h>

#include <array>
#include <set>
#include <span>
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
            if (node.size.kind() == SizeKind::Auto &&
                *w.surface != ViewSurface::FooterPrompt) {
                // A View names its client-rendered surface and has no content to
                // hug, so it is Exact- or Flex-sized -- EXCEPT the footer prompt,
                // whose intrinsic (reservation-sized) footprint the runtime sizes
                // by prompt kind, so it alone may be Auto. validateWellKnownAreas
                // pins that allowance to the canonical footer.prompt node.
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

// The whole-screen well-known-area contract: the complete canonical topology the
// assembler publishes, including required containers, View leaves, surfaces,
// parentage, and sibling order.
std::optional<std::string> rejectStrayFooterPrompt(const UiNode& node) {
    if (const auto* leaf = std::get_if<UiLeaf>(&node.content)) {
        if (leaf->widget.kind == WidgetKind::View && leaf->widget.surface &&
            *leaf->widget.surface == ViewSurface::FooterPrompt &&
            node.id.value() != kFooterPromptNodeId) {
            return std::string{node.id.value()} +
                   ": the FooterPrompt surface belongs only to the \"" +
                   std::string{kFooterPromptNodeId} + "\" node";
        }
        return std::nullopt;
    }
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children) {
            if (auto err = rejectStrayFooterPrompt(child)) return err;
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
    constexpr std::array rootIds{kHeaderNodeId, kBodyNodeId, kFooterPromptNodeId,
                                 kFooterNodeId};
    if (auto err = requireChildren(schema.root, "root", rootIds, root)) return err;
    const UiNode& header = root->children[0];
    if (header.id.value() != wellKnownAreaId(WellKnownArea::Header) ||
        !header.isContainer()) {
        return std::string{"header: must be a direct child container of root"};
    }
    const UiNode& body = root->children[1];
    // The footer prompt sits between the body and the footer: a View leaf naming
    // ViewSurface::FooterPrompt, always assembled and hidden by presence. It is
    // the sole node permitted to carry an Auto-sized FooterPrompt View; a stray
    // FooterPrompt View anywhere else is rejected below.
    if (auto err = requireViewLeaf(root->children[2], kFooterPromptNodeId,
                                   ViewSurface::FooterPrompt))
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
    constexpr std::array panelIds{kFileTreeNodeId, kGitStatusNodeId, kSymbolsNodeId};
    if (auto err = requireChildren(bodyContainer->children[0], "panel", panelIds, panel))
        return err;
    if (auto err = requireViewLeaf(panel->children[0], kFileTreeNodeId,
                                   ViewSurface::FileTree))
        return err;
    if (auto err = requireViewLeaf(panel->children[1], kGitStatusNodeId,
                                   ViewSurface::GitStatus))
        return err;
    if (auto err = requireViewLeaf(panel->children[2], kSymbolsNodeId,
                                   ViewSurface::Symbols))
        return err;

    const UiContainer* content = nullptr;
    constexpr std::array contentIds{kTabViewNodeId, kFindResultsNodeId};
    if (auto err =
            requireChildren(bodyContainer->children[1], "content", contentIds, content))
        return err;
    if (auto err = requireViewLeaf(content->children[0], kTabViewNodeId,
                                   ViewSurface::TabView))
        return err;
    if (auto err = requireViewLeaf(content->children[1], kFindResultsNodeId,
                                   ViewSurface::FindResults))
        return err;
    // The FooterPrompt surface is bound to the canonical footer.prompt node; a
    // View naming it anywhere else (the only other place an Auto-sized View can
    // pass validateUiSchema) is a misplacement, so reject it.
    if (auto err = rejectStrayFooterPrompt(schema.root)) return err;
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
