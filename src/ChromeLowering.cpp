#include <ssg/ChromeLowering.h>

#include <ssg/GraphemeLayout.h>
#include <ssg/Theme.h>  // semanticRoleFromName
#include <ssg/Widget.h>

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ssg {

namespace {

// One packed widget: its synthetic stack id (so placement lookup never assumes
// author id uniqueness), its source descriptor, and the resolved projection.
struct Packed {
    std::string stackId;
    const WidgetDescriptor* descriptor;
    std::string content;
    std::string label;
    std::optional<std::string> command;
    SemanticRole role;
};

struct Resolved {
    std::string content;
    std::string label;
    std::optional<std::string> command;
    bool drop = false;
};

bool truthy(std::string_view value) { return value == "true"; }

// Resolve a widget's displayed content + accessible label + inherited command.
Resolved resolveWidget(const WidgetDescriptor& w, const Style& style,
                       const ChromeProviderResolver& resolveProvider) {
    Resolved r;
    std::string value;
    std::string providerLabel;
    std::optional<std::string> inheritedCommand;
    bool fromProvider = false;
    if (w.value) {
        if (w.value->isProvider) {
            fromProvider = true;
            if (const auto resolved = resolveProvider(w.value->provider)) {
                value = resolved->value;
                providerLabel = resolved->accessibleLabel;
                inheritedCommand = resolved->commandId;
            }
        } else {
            value = w.value->literal;
        }
    }

    switch (w.kind) {
    case WidgetKind::Label:
    case WidgetKind::Field: {
        // A provider widget's label is the provider's; a literal widget labels
        // itself with its own text. Match the built-in status-field skip: drop
        // when EITHER the value or the accessible label is empty.
        const std::string label = fromProvider ? providerLabel : value;
        if (value.empty() || label.empty()) {
            r.drop = true;
            return r;
        }
        r.content = value;
        r.label = label;
        break;
    }
    case WidgetKind::Checkbox: {
        bool checked = false;
        if (w.checked) {
            if (w.checked->isProvider) {
                const auto resolved = resolveProvider(w.checked->provider);
                checked = resolved && truthy(resolved->value);
            } else {
                checked = truthy(w.checked->literal);
            }
        }
        r.content = checkboxText(checked, value, style.toggle);
        r.label = providerLabel.empty() ? r.content : providerLabel;
        break;
    }
    case WidgetKind::Spacer:
        r.content = {};  // a blank gap
        r.label = {};
        break;
    default:
        break;  // TextInput/Container are excluded by the decoder
    }

    // The descriptor's own command overrides an inherited one.
    r.command = w.command ? w.command : inheritedCommand;
    return r;
}

int widgetDesired(const WidgetDescriptor& w, const Resolved& resolved) {
    if (w.kind == WidgetKind::Spacer) return w.width.value_or(0);
    return measureFieldCells(resolved.content);
}

SemanticRole widgetRole(const WidgetDescriptor& w, SemanticRole defaultRole) {
    if (w.role) {
        if (const auto parsed = semanticRoleFromName(*w.role)) return *parsed;
    }
    return defaultRole;
}

StackItem stackItemFor(const WidgetDescriptor& w, std::string stackId,
                       const Resolved& resolved) {
    StackItem item;
    item.id = std::move(stackId);
    item.content = resolved.content;
    item.desired = widgetDesired(w, resolved);
    item.rank = w.rank;
    item.keep = w.keep;
    item.overflow = w.overflow;
    item.sigil = w.sigil;
    return item;
}

}  // namespace

// Shared core: lower three ordered widget groups (left, optional center, right)
// plus their separator/center params into accessibility nodes. Both the legacy
// RowDescriptor path and the medium-agnostic tree path feed this, so they build
// the identical WidgetStack and cannot diverge by construction.
static int lowerChromeGroups(
    const std::vector<const WidgetDescriptor*>& left,
    const std::vector<const WidgetDescriptor*>& right,
    const WidgetDescriptor* center, int separator, CenterWidth centerWidth,
    int centerFixed, const Rect& rect, ShellNodeKind nodeKind,
    SemanticRole defaultRole, const Style& style,
    const ChromeProviderResolver& resolveProvider,
    std::vector<AccessibilityNode>& out) {
    WidgetStack stack{separator};
    std::vector<Packed> packed;

    const auto pack = [&](const WidgetDescriptor& w, const std::string& stackId,
                          bool isLeft, bool isCenter) {
        const Resolved resolved = resolveWidget(w, style, resolveProvider);
        if (resolved.drop) return;
        StackItem item = stackItemFor(w, stackId, resolved);
        if (isCenter) {
            stack.center(std::move(item), centerWidth, centerFixed);
        } else if (isLeft) {
            stack.packLeft(std::move(item));
        } else {
            stack.packRight(std::move(item));
        }
        packed.push_back({stackId, &w, resolved.content, resolved.label,
                          resolved.command, widgetRole(w, defaultRole)});
    };

    for (std::size_t i = 0; i < left.size(); ++i)
        pack(*left[i], "L" + std::to_string(i), true, false);
    for (std::size_t i = 0; i < right.size(); ++i)
        pack(*right[i], "R" + std::to_string(i), false, false);
    if (center) pack(*center, "C", false, true);

    const auto solved = stack.resolve(rect.width);
    if (!solved) return rect.x;  // a well-formed row cannot fail; guard defensively

    // The row's consumed right edge INCLUDES node-less Spacers (they are placed
    // stack items), so a caller placing content after the group clears them.
    int consumedRight = rect.x;
    for (const auto& p : solved->placed)
        consumedRight = std::max(consumedRight, rect.x + p.offset + p.size);

    const auto emit = [&](std::string_view stackId) {
        const StackPlacement* placement = nullptr;
        for (const auto& p : solved->placed)
            if (p.id == stackId) placement = &p;
        if (!placement) return;
        const Packed* item = nullptr;
        for (const auto& p : packed)
            if (p.stackId == stackId) item = &p;
        if (!item) return;
        // A Spacer occupies stack space but emits no node -- it is a blank gap,
        // not an interactive element.
        if (item->descriptor->kind == WidgetKind::Spacer) return;
        out.push_back({nodeKind, item->descriptor->id, item->label,
                       {rect.x + placement->offset, rect.y, placement->size, 1},
                       item->role, item->content, item->command});
    };

    // Emit left → center → right, so hit-test order is deterministic.
    for (std::size_t i = 0; i < left.size(); ++i)
        emit("L" + std::to_string(i));
    if (center) emit("C");
    for (std::size_t i = 0; i < right.size(); ++i)
        emit("R" + std::to_string(i));
    return consumedRight;
}

int lowerChromeRow(const RowDescriptor& row, const Rect& rect,
                   ShellNodeKind nodeKind, SemanticRole defaultRole,
                   const Style& style,
                   const ChromeProviderResolver& resolveProvider,
                   std::vector<AccessibilityNode>& out) {
    std::vector<const WidgetDescriptor*> left;
    left.reserve(row.left.size());
    for (const auto& w : row.left) left.push_back(&w);
    std::vector<const WidgetDescriptor*> right;
    right.reserve(row.right.size());
    for (const auto& w : row.right) right.push_back(&w);
    return lowerChromeGroups(left, right, row.center ? &*row.center : nullptr,
                             row.separator, row.centerWidth, row.centerFixed,
                             rect, nodeKind, defaultRole, style, resolveProvider,
                             out);
}

namespace {

// The leaves of a group container, in order; nullopt if any child is not a leaf
// (a malformed chrome group).
std::optional<std::vector<const WidgetDescriptor*>> groupLeaves(
    const UiNode& group) {
    const auto* container = std::get_if<UiContainer>(&group.content);
    if (!container) return std::nullopt;
    std::vector<const WidgetDescriptor*> widgets;
    for (const auto& child : container->children) {
        const auto* leaf = std::get_if<UiLeaf>(&child.content);
        if (!leaf) return std::nullopt;
        widgets.push_back(&leaf->widget);
    }
    return widgets;
}

}  // namespace

UiChromeLowerResult lowerUiChromeRegion(
    const UiRegion& region, const Rect& rect, ShellNodeKind nodeKind,
    SemanticRole defaultRole, const Style& style,
    const ChromeProviderResolver& resolveProvider,
    std::vector<AccessibilityNode>& out) {
    // The canonical chrome shape: a Row root of exactly three groups --
    // left(Auto, Row), middle(Flex, Row), right(Auto, Row) -- so the packing is
    // encoded in the sizing. Every field the shape depends on is CHECKED here (no
    // silently-ignored axis/size/gap), so a tree a generic client would lay out
    // differently is rejected rather than lowered.
    const auto* root = std::get_if<UiContainer>(&region.root.content);
    if (!root || root->axis != Axis::Row) {
        return {"chrome region root must be a Row container"};
    }
    if (root->children.size() != 3) {
        return {"chrome region root must have exactly three groups "
                "(left, middle, right)"};
    }

    const UiNode& leftGroup = root->children[0];
    const UiNode& middleGroup = root->children[1];
    const UiNode& rightGroup = root->children[2];

    const auto* leftContainer = std::get_if<UiContainer>(&leftGroup.content);
    const auto* middleContainer = std::get_if<UiContainer>(&middleGroup.content);
    const auto* rightContainer = std::get_if<UiContainer>(&rightGroup.content);
    if (!leftContainer || !middleContainer || !rightContainer) {
        return {"chrome region groups must be containers"};
    }
    if (leftContainer->axis != Axis::Row || middleContainer->axis != Axis::Row ||
        rightContainer->axis != Axis::Row) {
        return {"chrome region groups must be Row containers"};
    }
    // Sizing encodes the packing: Auto ends, Flex middle. A deviation would render
    // differently on a generic client, so reject it.
    if (leftGroup.size.kind() != SizeKind::Auto ||
        rightGroup.size.kind() != SizeKind::Auto ||
        middleGroup.size.kind() != SizeKind::Flex) {
        return {"chrome region groups must be Auto/Flex/Auto sized"};
    }
    // A chrome row reserves no inset at the root or any group, and only the left
    // group carries a gap (the separator); the right group's gap is zero. Checked,
    // not ignored, so a generic client and the grid agree on the geometry.
    if (root->inset != Inset{} || leftContainer->inset != Inset{} ||
        middleContainer->inset != Inset{} || rightContainer->inset != Inset{}) {
        return {"chrome region reserves no inset"};
    }
    // The root spaces its three groups by the packing sizing alone, not a gap;
    // a root gap would be honored by a generic client but ignored by the grid.
    if (root->gap != Gap{} || rightContainer->gap != Gap{}) {
        return {"chrome region root/right group must have no gap"};
    }

    const auto leftWidgets = groupLeaves(leftGroup);
    const auto rightWidgets = groupLeaves(rightGroup);
    if (!leftWidgets || !rightWidgets) {
        return {"chrome region left/right groups must hold only leaves"};
    }
    // Every left/right leaf is content-sized (Auto), matching the group's packing.
    for (const auto& child :
         std::get<UiContainer>(leftGroup.content).children) {
        if (child.size.kind() != SizeKind::Auto) {
            return {"chrome region left leaves must be Auto sized"};
        }
    }
    for (const auto& child :
         std::get<UiContainer>(rightGroup.content).children) {
        if (child.size.kind() != SizeKind::Auto) {
            return {"chrome region right leaves must be Auto sized"};
        }
    }

    const int separator = leftContainer->gap.extent();

    // The middle group holds zero or one leaf (the center); its Size carries the
    // width policy (Flex fills, Exact is a fixed center -- Auto is not valid here).
    const WidgetDescriptor* center = nullptr;
    CenterWidth centerWidth = CenterWidth::Flex;
    int centerFixed = 0;
    if (middleContainer->children.size() > 1) {
        return {"chrome region middle group has more than one widget"};
    }
    if (middleContainer->children.size() == 1) {
        const UiNode& centerNode = middleContainer->children.front();
        const auto* leaf = std::get_if<UiLeaf>(&centerNode.content);
        if (!leaf) return {"chrome region center is not a leaf"};
        center = &leaf->widget;
        if (centerNode.size.kind() == SizeKind::Auto) {
            return {"chrome region center leaf must be Flex or Exact sized"};
        }
        if (centerNode.size.kind() == SizeKind::Exact) {
            centerWidth = CenterWidth::Fixed;
            centerFixed = centerNode.size.extent();
        }
    }

    const int rightEdge = lowerChromeGroups(
        *leftWidgets, *rightWidgets, center, separator, centerWidth, centerFixed,
        rect, nodeKind, defaultRole, style, resolveProvider, out);
    return {std::nullopt, rightEdge};
}

}  // namespace ssg
