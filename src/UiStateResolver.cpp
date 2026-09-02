#include <ssg/UiStateResolver.h>

#include <ssg/Theme.h>

#include <string>
#include <utility>
#include <variant>

namespace ssg {
namespace {

bool truthy(std::string_view value) { return value == "true"; }

struct Sources {
    std::string value;
    std::string providerLabel;
    std::optional<std::string> inheritedCommand;
    std::optional<bool> active;
    bool fromProvider = false;
};

Sources resolveSources(const WidgetDescriptor& widget,
                       const WidgetProviderResolver& resolveProvider) {
    Sources sources;
    if (!widget.value) return sources;
    if (!widget.value->isProvider) {
        sources.value = widget.value->literal;
        return sources;
    }

    sources.fromProvider = true;
    if (const auto resolved = resolveProvider(widget.value->provider)) {
        sources.value = resolved->value;
        sources.providerLabel = resolved->accessibleLabel;
        sources.inheritedCommand = resolved->commandId;
        sources.active = resolved->active;
    }
    return sources;
}

bool resolveChecked(const WidgetDescriptor& widget,
                    const WidgetProviderResolver& resolveProvider) {
    if (!widget.checked) return false;
    if (!widget.checked->isProvider) {
        return truthy(widget.checked->literal);
    }
    const auto resolved = resolveProvider(widget.checked->provider);
    return resolved && truthy(resolved->value);
}

SemanticRole effectiveRole(const WidgetDescriptor& widget,
                           SemanticRole defaultRole) {
    if (widget.role) {
        if (const auto parsed = semanticRoleFromName(*widget.role)) {
            return *parsed;
        }
    }
    return defaultRole;
}

SemanticRole defaultRoleForArea(const UiNodeId& id) {
    if (id.value() == kHeaderNodeId) return SemanticRole::Header;
    if (id.value() == kFooterNodeId) return SemanticRole::Footer;
    return SemanticRole::Text;
}

void collectNodeStates(const UiNode& node,
                       const WidgetProviderResolver& resolveProvider,
                       SemanticRole defaultRole,
                       std::vector<UiNodeState>& out) {
    UiNodeState state;
    state.id = node.id;
    if (const auto* leaf = std::get_if<UiLeaf>(&node.content)) {
        state.leaf =
            resolveUiLeafState(leaf->widget, resolveProvider, defaultRole);
    }
    out.push_back(std::move(state));
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children) {
            collectNodeStates(child, resolveProvider, defaultRole, out);
        }
    }
}

}  // namespace

std::optional<UiLeafState> resolveUiLeafState(
    const WidgetDescriptor& widget,
    const WidgetProviderResolver& resolveProvider,
    SemanticRole defaultRole) {
    const Sources sources = resolveSources(widget, resolveProvider);
    const std::string label =
        sources.fromProvider ? sources.providerLabel : sources.value;
    const std::optional<std::string> command =
        widget.command ? widget.command : sources.inheritedCommand;
    const SemanticRole role = effectiveRole(widget, defaultRole);

    switch (widget.kind) {
    case WidgetKind::Label:
    case WidgetKind::Field:
        if (sources.value.empty() || label.empty()) return std::nullopt;
        return UiLeafState{sources.value, label, command, std::nullopt, role};
    case WidgetKind::Checkbox:
        return UiLeafState{sources.value, label, command,
                           resolveChecked(widget, resolveProvider), role};
    case WidgetKind::TextInput:
        if (!sources.fromProvider || label.empty()) return std::nullopt;
        return UiLeafState{sources.value, label, command, std::nullopt, role,
                           sources.active};
    default:
        return std::nullopt;
    }
}

UiStateSection resolveUiState(
    const ValidatedSchema& schema,
    const WidgetProviderResolver& resolveProvider) {
    UiStateSection section;
    section.generation = schema.generation();
    const UiNode& root = schema.schema().root;
    section.nodes.push_back(UiNodeState{root.id, std::nullopt});
    if (const auto* container = std::get_if<UiContainer>(&root.content)) {
        for (const auto& area : container->children) {
            collectNodeStates(area, resolveProvider,
                              defaultRoleForArea(area.id), section.nodes);
        }
    }
    return section;
}

}  // namespace ssg
