#include <ssg/ChromeLowering.h>

#include <ssg/GraphemeLayout.h>
#include <ssg/StatusQueue.h>
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
    std::string nodeId;
    std::string content;
    std::string label;
    std::optional<std::string> command;
    SemanticRole role;
    ShellNodeKind kind = ShellNodeKind::FooterField;
    std::optional<StatusActionInvocation> statusInvocation;
};

struct Resolved {
    std::string content;
    std::string label;
    std::optional<std::string> command;
    bool drop = false;
};

bool truthy(std::string_view value) { return value == "true"; }

// The resolved sources shared by the TUI's glyph lowering and the semantic
// dynamic-state resolution, so the two cannot diverge on how a source resolves.
struct Sources {
    std::string value;
    std::string providerLabel;
    std::optional<std::string> inheritedCommand;
    std::optional<bool> active;
    bool fromProvider = false;
};

Sources resolveSources(const WidgetDescriptor& w,
                       const ChromeProviderResolver& resolveProvider) {
    Sources s;
    if (w.value) {
        if (w.value->isProvider) {
            s.fromProvider = true;
            if (const auto resolved = resolveProvider(w.value->provider)) {
                s.value = resolved->value;
                s.providerLabel = resolved->accessibleLabel;
                s.inheritedCommand = resolved->commandId;
                s.active = resolved->active;
            }
        } else {
            s.value = w.value->literal;
        }
    }
    return s;
}

bool resolveChecked(const WidgetDescriptor& w,
                    const ChromeProviderResolver& resolveProvider) {
    if (!w.checked) return false;
    if (w.checked->isProvider) {
        const auto resolved = resolveProvider(w.checked->provider);
        return resolved && truthy(resolved->value);
    }
    return truthy(w.checked->literal);
}

// Resolve a widget's displayed content + accessible label + inherited command for
// the GRID lowering: content is the glyph a terminal draws (a checkbox composes its
// box), and a literal checkbox labels itself with that glyph. Byte-identical to the
// shipped behavior; the semantic resolution below is a separate, glyph-free path.
Resolved resolveWidget(const WidgetDescriptor& w, const Style& style,
                       const ChromeProviderResolver& resolveProvider) {
    Resolved r;
    const Sources s = resolveSources(w, resolveProvider);

    switch (w.kind) {
    case WidgetKind::Label:
    case WidgetKind::Field: {
        // A provider widget's label is the provider's; a literal widget labels
        // itself with its own text. Match the built-in status-field skip: drop
        // when EITHER the value or the accessible label is empty.
        const std::string label = s.fromProvider ? s.providerLabel : s.value;
        if (s.value.empty() || label.empty()) {
            r.drop = true;
            return r;
        }
        r.content = s.value;
        r.label = label;
        break;
    }
    case WidgetKind::Checkbox: {
        const bool checked = resolveChecked(w, resolveProvider);
        r.content = checkboxText(checked, s.value, style.toggle);
        r.label = s.providerLabel.empty() ? r.content : s.providerLabel;
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
    r.command = w.command ? w.command : s.inheritedCommand;
    return r;
}

// The SEMANTIC leaf state a non-grid client renders, glyph-free and
// geometry-independent. A Label/Field with an empty value or label resolves to no
// leaf state (the semantic drop); a checkbox always resolves (its caption is the
// bare value, its label the semantic caption/provider label, plus the checked
// bool); a spacer/container resolve to none. The command precedence matches the
// grid path (descriptor overrides inherited).
std::optional<UiLeafState> semanticLeafState(
    const WidgetDescriptor& w, const ChromeProviderResolver& resolveProvider,
    SemanticRole defaultRole) {
    const Sources s = resolveSources(w, resolveProvider);
    const std::string label = s.fromProvider ? s.providerLabel : s.value;
    const std::optional<std::string> command =
        w.command ? w.command : s.inheritedCommand;
    // The effective role the library owns: the widget's own valid role name, else
    // the region default. Resolved here so a client colors by ordinal, never by
    // re-deriving role names.
    SemanticRole role = defaultRole;
    if (w.role) {
        if (const auto parsed = semanticRoleFromName(*w.role)) role = *parsed;
    }
    switch (w.kind) {
    case WidgetKind::Label:
    case WidgetKind::Field:
        if (s.value.empty() || label.empty()) return std::nullopt;
        return UiLeafState{s.value, label, command, std::nullopt, role};
    case WidgetKind::Checkbox:
        return UiLeafState{s.value, label, command,
                           resolveChecked(w, resolveProvider), role};
    case WidgetKind::TextInput:
        if (!s.fromProvider || label.empty()) return std::nullopt;
        return UiLeafState{s.value, label, command, std::nullopt, role,
                           s.active};
    default:
        return std::nullopt;  // Spacer/Container carry no leaf state
    }
}

int displayCells(std::string_view text) {
    return static_cast<int>(GraphemeLayout{}.computeRun(text).totalCells);
}

int widgetDesired(const WidgetDescriptor& w, const Resolved& resolved,
                  const Style& style) {
    if (w.kind == WidgetKind::Spacer) return w.width.value_or(0);
    if (w.kind == WidgetKind::StatusActions ||
        (w.kind == WidgetKind::Field && w.id == "footer.hint")) {
        return std::max(1, displayCells(resolved.content) +
                               style.dimensions.labelPadding);
    }
    return measureFieldCells(resolved.content);
}

SemanticRole widgetRole(const WidgetDescriptor& w, SemanticRole defaultRole) {
    if (w.role) {
        if (const auto parsed = semanticRoleFromName(*w.role)) return *parsed;
    }
    return defaultRole;
}

StackItem stackItemFor(const WidgetDescriptor& w, std::string stackId,
                       const Resolved& resolved, const Style& style) {
    StackItem item;
    item.id = std::move(stackId);
    item.content = resolved.content;
    item.desired = widgetDesired(w, resolved, style);
    item.rank = w.rank;
    item.keep = w.keep;
    item.overflow = w.overflow;
    item.sigil = w.sigil;
    return item;
}

}  // namespace

// Shared core: lower three ordered widget groups (left, optional center, right)
// plus their separator/center params into accessibility nodes. lowerUiChromeRegion
// reads these groups off the canonical region tree and feeds them here, so the
// grid lowering has one entry point.
static int lowerChromeGroups(
    const std::vector<const WidgetDescriptor*>& left,
    const std::vector<const WidgetDescriptor*>& right,
    const WidgetDescriptor* center, int separator, CenterWidth centerWidth,
    int centerFixed, const Rect& rect, ShellNodeKind regionNodeKind,
    SemanticRole defaultRole, const Style& style,
    const ChromeProviderResolver& resolveProvider,
    std::vector<AccessibilityNode>& out, const StatusViewState* statusView) {
    WidgetStack stack{separator};
    std::vector<Packed> packed;

    const auto pack = [&](const WidgetDescriptor& w, const std::string& stackId,
                          bool isLeft, bool isCenter) {
        if (w.kind == WidgetKind::StatusActions) {
            if (!statusView || statusView->items.empty()) return;
            const std::size_t selected = std::min(statusView->selected, statusView->items.size() - 1);
            const auto& itemView = statusView->items[selected];
            for (std::size_t actionIndex = 0; actionIndex < itemView.actions.size(); ++actionIndex) {
                const auto& action = itemView.actions[actionIndex];
                if (action.accessibleLabel.empty()) continue;
                Resolved resolved;
                resolved.content = action.accessibleLabel;
                resolved.label = action.accessibleLabel;
                const std::string actionStackId = stackId + ".A" + std::to_string(actionIndex);
                StackItem stackItem = stackItemFor(w, actionStackId, resolved, style);
                stackItem.rank = w.rank;
                stackItem.overflow = Overflow::Truncate;
                if (isCenter) {
                    stack.center(std::move(stackItem), centerWidth, centerFixed);
                } else if (isLeft) {
                    stack.packLeft(std::move(stackItem));
                } else {
                    stack.packRight(std::move(stackItem));
                }
                packed.push_back({actionStackId, &w, action.id, resolved.content, resolved.label,
                                  std::nullopt, SemanticRole::StatusInfo,
                                  ShellNodeKind::FooterAction,
                                  StatusActionInvocation{itemView.id, action.id, itemView.generation}});
            }
            return;
        }
        const Resolved resolved = resolveWidget(w, style, resolveProvider);
        if (resolved.drop) return;
        StackItem item = stackItemFor(w, stackId, resolved, style);
        if (isCenter) {
            stack.center(std::move(item), centerWidth, centerFixed);
        } else if (isLeft) {
            stack.packLeft(std::move(item));
        } else {
            stack.packRight(std::move(item));
        }
        const ShellNodeKind kind =
            (w.kind == WidgetKind::Field && w.id == "footer.hint")
                ? ShellNodeKind::FooterHint
                : regionNodeKind;
        const SemanticRole role =
            (kind == ShellNodeKind::FooterHint) ? SemanticRole::Footer
            : (kind == ShellNodeKind::FooterAction) ? SemanticRole::StatusInfo
                                                : widgetRole(w, defaultRole);
        packed.push_back({stackId, &w, w.id, resolved.content, resolved.label,
                          resolved.command, role, kind, std::nullopt});
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

    const auto emitOne = [&](std::string_view stackId) {
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
        out.push_back({item->kind, item->nodeId, item->label,
                       {rect.x + placement->offset, rect.y, placement->size, 1},
                       item->role, item->content, item->command,
                       item->statusInvocation});
    };

    const auto emit = [&](std::string_view stackId) {
        emitOne(stackId);
        const std::string prefix = std::string{stackId} + ".A";
        for (const auto& p : packed) {
            if (p.stackId.rfind(prefix, 0) == 0) emitOne(p.stackId);
        }
    };

    // Emit left → center → right, so hit-test order is deterministic.
    for (std::size_t i = 0; i < left.size(); ++i)
        emit("L" + std::to_string(i));
    if (center) emit("C");
    for (std::size_t i = 0; i < right.size(); ++i)
        emit("R" + std::to_string(i));
    return consumedRight;
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
    const UiNode& regionRoot, const Rect& rect, ShellNodeKind regionNodeKind,
    SemanticRole defaultRole, const Style& style,
    const ChromeProviderResolver& resolveProvider,
    std::vector<AccessibilityNode>& out, const StatusViewState* statusView,
    const PromptInputProjection* input) {
    // The canonical chrome shape: a Row root of exactly three groups --
    // left(Auto, Row), middle(Flex, Row), right(Auto, Row) -- so the packing is
    // encoded in the sizing. Every field the shape depends on is CHECKED here (no
    // silently-ignored axis/size/gap), so a tree a generic client would lay out
    // differently is rejected rather than lowered.
    const auto* root = std::get_if<UiContainer>(&regionRoot.content);
    if (!root || root->axis != Axis::Row) {
        return {"chrome region root must be a Row container"};
    }
    // The header's canonical prompt-input TextInput is not one of the three
    // collapse groups: it is extracted by its well-known id (wherever it sits in
    // tree order) and lowered separately by the reserve/expand rule below. A region
    // without it (the footer, or a broken caller projection) leaves the three
    // groups unchanged.
    const WidgetDescriptor* promptInput = nullptr;
    std::vector<const UiNode*> groups;
    groups.reserve(root->children.size());
    for (const UiNode& child : root->children) {
        if (const auto* leaf = std::get_if<UiLeaf>(&child.content);
            leaf && leaf->widget.kind == WidgetKind::TextInput) {
            if (child.id.value() != kHeaderPromptInputNodeId ||
                leaf->widget.id != kHeaderPromptInputNodeId) {
                return {"chrome region prompt input must be the canonical "
                        "input_line node"};
            }
            if (promptInput) {
                return {"chrome region must have at most one prompt input"};
            }
            promptInput = &leaf->widget;
            continue;
        }
        groups.push_back(&child);
    }
    if (groups.size() != 3) {
        return {"chrome region root must have exactly three groups "
                "(left, middle, right)"};
    }

    const UiNode& leftGroup = *groups[0];
    const UiNode& middleGroup = *groups[1];
    const UiNode& rightGroup = *groups[2];

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

    // The grid chrome lowering renders only chrome widget kinds; an opaque View
    // surface is not lowerable to chrome cells, so a View reaching this path is a
    // loud conformance failure, never silently emitted as empty content.
    for (const auto* group : {leftWidgets ? &*leftWidgets : nullptr,
                              rightWidgets ? &*rightWidgets : nullptr}) {
        if (!group) continue;
        for (const WidgetDescriptor* w : *group) {
            if (w->kind == WidgetKind::View) {
                return {"chrome region cannot render a view leaf"};
            }
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
        if (center->kind == WidgetKind::View) {
            return {"chrome region cannot render a view leaf"};
        }
        if (centerNode.size.kind() == SizeKind::Auto) {
            return {"chrome region center leaf must be Flex or Exact sized"};
        }
        if (centerNode.size.kind() == SizeKind::Exact) {
            centerWidth = CenterWidth::Fixed;
            centerFixed = centerNode.size.extent();
        }
    }

    // The prompt input's fixed reservation is subtracted from the groups' width
    // BEFORE they collapse, never after -- so the status fields never reflow as the
    // user types (the reservation is the floor protecting the input's room) and the
    // input never overlaps their cells (hit-testing takes the first node containing a
    // cell). When no input is visible, the groups fill the whole rect exactly as
    // before.
    const bool showInput = promptInput && input && input->visible;
    Rect groupsRect = rect;
    if (showInput) {
        const int reserved = std::min(style.inputLineReservation(), rect.width);
        groupsRect.width = std::max(0, rect.width - reserved);
    }

    const int rightEdge = lowerChromeGroups(
        *leftWidgets, *rightWidgets, center, separator, centerWidth, centerFixed,
        groupsRect, regionNodeKind, defaultRole, style, resolveProvider, out,
        statusView);

    // The input line grows across the header's remaining width after the groups'
    // consumed right edge (which includes node-less spacers), then scrolls its own
    // tail and clamps its ghost inside `layoutInputLine`. The grid host derives the
    // caret from the emitted query geometry; ShellState only stamps the text/widths.
    if (showInput) {
        int inputX = rightEdge;
        if (inputX > rect.x) ++inputX;  // a space between the fields and the input
        const int available = std::max(0, rect.x + rect.width - inputX);
        const auto line =
            layoutInputLine(promptInput->sigil, input->query, input->ghost, available);
        out.push_back({regionNodeKind, "input_line.query", "Input line",
                       {inputX, rect.y, line.width, 1}, SemanticRole::Prompt,
                       line.text, std::nullopt, std::nullopt});
        inputX += line.width;
        if (line.ghostWidth > 0) {
            out.push_back({regionNodeKind, "input_line.ghost",
                           "Input line completion",
                           {inputX, rect.y, line.ghostWidth, 1},
                           SemanticRole::LineNumber, line.ghostText, std::nullopt,
                           std::nullopt});
        }
    }
    return {std::nullopt, rightEdge};
}

namespace {

// The default SemanticRole a well-known area's widgets take when a widget declares
// none: Header for the header subtree, Footer for the footer subtree, else Text.
// Keyed on the well-known node id, since placement is id-based, not a region enum.
SemanticRole defaultRoleForArea(const UiNodeId& id) {
    if (id.value() == kHeaderNodeId) return SemanticRole::Header;
    if (id.value() == kFooterNodeId) return SemanticRole::Footer;
    return SemanticRole::Text;
}

// Pre-order walk: record each node's presence (always present in phase 6) and, for
// a leaf, its semantic state (including its effective role, resolved against the
// region default).
void collectNodeStates(const UiNode& node,
                       const ChromeProviderResolver& resolveProvider,
                       SemanticRole defaultRole,
                       std::vector<UiNodeState>& out) {
    UiNodeState state;
    state.id = node.id;
    if (const auto* leaf = std::get_if<UiLeaf>(&node.content)) {
        state.leaf =
            semanticLeafState(leaf->widget, resolveProvider, defaultRole);
    }
    out.push_back(std::move(state));
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children) {
            collectNodeStates(child, resolveProvider, defaultRole, out);
        }
    }
}

}  // namespace

UiStateSection resolveUiState(const ValidatedSchema& schema,
                              const ChromeProviderResolver& resolveProvider) {
    UiStateSection section;
    section.generation = schema.generation();
    const UiNode& root = schema.schema().root;
    // The root carries the well-known areas as children; each area's widgets take
    // that area's default role. The root node itself has no leaf.
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
