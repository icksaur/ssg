#include <tui/UiRegionProjection.h>

#include <ssg/GraphemeLayout.h>
#include <tui/StatusFieldGrid.h>
#include <ssg/StatusQueue.h>
#include <ssg/Theme.h>  // semanticRoleFromName
#include <ssg/UiStateResolver.h>
#include <tui/WidgetLayout.h>

#include <optional>
#include <ranges>
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
// Resolve a widget's displayed content + accessible label + inherited command for
// the GRID lowering: content is the glyph a terminal draws (a checkbox composes its
// box), and a literal checkbox labels itself with that glyph. Byte-identical to the
// shipped behavior; the semantic resolution below is a separate, glyph-free path.
Resolved resolveWidget(const WidgetDescriptor& w, const Style& style,
                       const WidgetProviderResolver& resolveProvider) {
    Resolved r;
    const auto semantic =
        resolveUiLeafState(w, resolveProvider, SemanticRole::Text);

    switch (w.kind) {
    case WidgetKind::Label:
    case WidgetKind::Field: {
        // A provider widget's label is the provider's; a literal widget labels
        // itself with its own text. Match the built-in status-field skip: drop
        // when EITHER the value or the accessible label is empty.
        if (!semantic) {
            r.drop = true;
            return r;
        }
        const std::string_view providerId =
            w.value && w.value->isProvider ? w.value->provider : std::string_view{};
        r.content =
            statusFieldGridDisplay(providerId, semantic->value, style);
        r.label = semantic->label;
        break;
    }
    case WidgetKind::Checkbox: {
        if (!semantic) {
            r.drop = true;
            return r;
        }
        r.content = checkboxText(semantic->checked.value_or(false),
                                 semantic->value, style.toggle);
        r.label = semantic->label.empty()
                      ? r.content
                      : semantic->label;
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
    if (w.kind != WidgetKind::Label) {
        r.command = semantic ? semantic->command : std::nullopt;
    }
    return r;
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
// plus their separator/center params into accessibility nodes. projectUiRegion
// reads these groups off the canonical region tree and feeds them here, so the
// grid lowering has one entry point.
static int lowerUiRegionGroups(
    const std::vector<const WidgetDescriptor*>& left,
    const std::vector<const WidgetDescriptor*>& right,
    const WidgetDescriptor* center, int separator, CenterWidth centerWidth,
    int centerFixed, const Rect& rect, SemanticRole defaultRole, const Style& style,
    const WidgetProviderResolver& resolveProvider,
    std::vector<SolvedUiItem>& out, const StatusViewState* statusView) {
    WidgetStack stack{separator};
    std::vector<Packed> packed;

    const auto pack = [&](const WidgetDescriptor& w, const std::string& stackId,
                          bool isLeft, bool isCenter) {
        if (w.kind == WidgetKind::StatusActions) {
            if (!statusView || statusView->items.empty() ||
                statusView->selected >= statusView->items.size()) {
                return;
            }
            const auto& itemView =
                statusView->items[statusView->selected];
            const auto actionNodes = projectStatusActionNodes(*statusView);
            for (std::size_t actionIndex = 0; actionIndex < itemView.actions.size(); ++actionIndex) {
                const auto& action = itemView.actions[actionIndex];
                if (action.accessibleLabel.empty()) continue;
                const auto& actionNode = actionNodes[actionIndex];
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
                packed.push_back(
                    {actionStackId, &w, actionNode.id.value(), resolved.content,
                     resolved.label, actionNode.commandId,
                     SemanticRole::StatusInfo});
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
        const SemanticRole role =
            (w.kind == WidgetKind::Field && w.id == "footer.hint")
                ? SemanticRole::Footer
                : widgetRole(w, defaultRole);
        packed.push_back({stackId, &w, w.id, resolved.content, resolved.label,
                          resolved.command, role});
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
        out.push_back({item->nodeId, item->label,
                       {rect.x + placement->offset, rect.y, placement->size, 1},
                       item->role, item->content, item->command});
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
// (a malformed UI-region group).
std::optional<std::vector<const WidgetDescriptor*>> groupLeaves(
    const UiNode& group) {
    const auto* container = std::get_if<UiContainer>(&group.content);
    if (!container) return std::nullopt;
    std::vector<const WidgetDescriptor*> widgets;
    for (const auto& child : container->children) {
        const auto* leaf = std::get_if<UiLeaf>(&child.content);
        if (leaf) {
            widgets.push_back(&leaf->widget);
            continue;
        }
        if (child.id.value() != "footer.status_actions") {
            return std::nullopt;
        }
        const auto* actions = std::get_if<UiContainer>(&child.content);
        if (!actions || actions->axis != Axis::Row) return std::nullopt;
        for (const auto& action : actions->children) {
            const auto* actionLeaf = std::get_if<UiLeaf>(&action.content);
            if (!actionLeaf || action.size.kind() != SizeKind::Auto) {
                return std::nullopt;
            }
            widgets.push_back(&actionLeaf->widget);
        }
    }
    return widgets;
}

}  // namespace

UiRegionProjectionResult projectUiRegion(
    const UiNode& regionRoot, const Rect& rect, SemanticRole defaultRole,
    const Style& style,
    const WidgetProviderResolver& resolveProvider,
    SolvedUiRegion& out, const StatusViewState* statusView,
    const PromptInputProjection* input) {
    out = SolvedUiRegion{rect};
    // The canonical UI-region shape: a Row root of exactly three groups --
    // left(Auto, Row), middle(Flex, Row), right(Auto, Row) -- so the packing is
    // encoded in the sizing. Every field the shape depends on is CHECKED here (no
    // silently-ignored axis/size/gap), so a tree a generic client would lay out
    // differently is rejected rather than lowered.
    const auto* root = std::get_if<UiContainer>(&regionRoot.content);
    if (!root || root->axis != Axis::Row) {
        return {"UI region root must be a Row container"};
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
                return {"UI region prompt input must be the canonical "
                        "input_line node"};
            }
            if (promptInput) {
                return {"UI region must have at most one prompt input"};
            }
            promptInput = &leaf->widget;
            continue;
        }
        groups.push_back(&child);
    }
    if (groups.size() != 3) {
        return {"UI region root must have exactly three groups "
                "(left, middle, right)"};
    }

    const UiNode& leftGroup = *groups[0];
    const UiNode& middleGroup = *groups[1];
    const UiNode& rightGroup = *groups[2];

    const auto* leftContainer = std::get_if<UiContainer>(&leftGroup.content);
    const auto* middleContainer = std::get_if<UiContainer>(&middleGroup.content);
    const auto* rightContainer = std::get_if<UiContainer>(&rightGroup.content);
    if (!leftContainer || !middleContainer || !rightContainer) {
        return {"UI region groups must be containers"};
    }
    if (leftContainer->axis != Axis::Row || middleContainer->axis != Axis::Row ||
        rightContainer->axis != Axis::Row) {
        return {"UI region groups must be Row containers"};
    }
    // Sizing encodes the packing: Auto ends, Flex middle. A deviation would render
    // differently on a generic client, so reject it.
    if (leftGroup.size.kind() != SizeKind::Auto ||
        rightGroup.size.kind() != SizeKind::Auto ||
        middleGroup.size.kind() != SizeKind::Flex) {
        return {"UI region groups must be Auto/Flex/Auto sized"};
    }
    // A UI-region row reserves no inset at the root or any group, and only the left
    // group carries a gap (the separator); the right group's gap is zero. Checked,
    // not ignored, so a generic client and the grid agree on the geometry.
    if (root->inset != Inset{} || leftContainer->inset != Inset{} ||
        middleContainer->inset != Inset{} || rightContainer->inset != Inset{}) {
        return {"UI region reserves no inset"};
    }
    // The root spaces its three groups by the packing sizing alone, not a gap;
    // a root gap would be honored by a generic client but ignored by the grid.
    if (root->gap != Gap{} || rightContainer->gap != Gap{}) {
        return {"UI region root/right group must have no gap"};
    }

    const auto leftWidgets = groupLeaves(leftGroup);
    const auto rightWidgets = groupLeaves(rightGroup);
    if (!leftWidgets || !rightWidgets) {
        return {"UI region left/right groups must hold only leaves"};
    }
    // Every left/right leaf is content-sized (Auto), matching the group's packing.
    for (const auto& child :
         std::get<UiContainer>(leftGroup.content).children) {
        if (child.size.kind() != SizeKind::Auto) {
            return {"UI region left leaves must be Auto sized"};
        }
    }
    for (const auto& child :
         std::get<UiContainer>(rightGroup.content).children) {
        if (child.size.kind() != SizeKind::Auto) {
            return {"UI region right leaves must be Auto sized"};
        }
    }

    // UI-region projection renders only directly projected widget kinds; an opaque
    // View surface is not lowerable to cells, so a View reaching this path is a
    // loud conformance failure, never silently emitted as empty content.
    for (const auto* group : {leftWidgets ? &*leftWidgets : nullptr,
                              rightWidgets ? &*rightWidgets : nullptr}) {
        if (!group) continue;
        for (const WidgetDescriptor* w : *group) {
            if (w->kind == WidgetKind::View) {
                return {"UI region cannot render a view leaf"};
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
        return {"UI region middle group has more than one widget"};
    }
    if (middleContainer->children.size() == 1) {
        const UiNode& centerNode = middleContainer->children.front();
        const auto* leaf = std::get_if<UiLeaf>(&centerNode.content);
        if (!leaf) return {"UI region center is not a leaf"};
        center = &leaf->widget;
        if (center->kind == WidgetKind::View) {
            return {"UI region cannot render a view leaf"};
        }
        if (centerNode.size.kind() == SizeKind::Auto) {
            return {"UI region center leaf must be Flex or Exact sized"};
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

    const int rightEdge = lowerUiRegionGroups(
        *leftWidgets, *rightWidgets, center, separator, centerWidth, centerFixed,
        groupsRect, defaultRole, style, resolveProvider, out.items,
        statusView);

    // The input line grows across the header's remaining width after the groups'
    // consumed right edge (which includes node-less spacers), then scrolls its own
    // tail and clamps its ghost inside `layoutInputLine`. The grid host derives the
    // caret from the emitted query geometry.
    if (showInput) {
        int inputX = rightEdge;
        if (inputX > rect.x) ++inputX;  // a space between the fields and the input
        const int available = std::max(0, rect.x + rect.width - inputX);
        const auto line =
            layoutInputLine(promptInput->sigil, input->query, input->ghost, available);
        SolvedUiInput solvedInput;
        solvedInput.nodeId =
            UiNodeId{std::string{kHeaderPromptInputNodeId}};
        solvedInput.query = {inputX, rect.y, line.width, 1};
        solvedInput.queryText = line.text;
        solvedInput.caret = {inputX + line.width, rect.y, 1, 1};
        inputX += line.width;
        if (line.ghostWidth > 0) {
            solvedInput.ghost =
                Rect{inputX, rect.y, line.ghostWidth, 1};
            solvedInput.ghostText = line.ghostText;
        }
        out.input = std::move(solvedInput);
    }
    return {std::nullopt, rightEdge};
}

UiRegionProjectionResult solveUiRegion(
    const UiNode& regionRoot, const Rect& rect, SemanticRole defaultRole,
    const Style& style, Generation schemaGeneration,
    const UiStateSection& state,
    SolvedUiRegion& out, const StatusViewState* statusView,
    const PromptInputProjection* input) {
    if (state.generation != schemaGeneration) {
        return {regionRoot.id.value() +
                " UI state generation does not match schema"};
    }
    struct ProviderState {
        std::string id;
        ResolvedProvider value;
    };
    std::vector<ProviderState> providers;
    const auto stateFor = [&](const UiNodeId& id) -> const UiNodeState* {
        const auto found = std::ranges::find(state.nodes, id, &UiNodeState::id);
        return found == state.nodes.end() ? nullptr : &*found;
    };
    const auto collect = [&](const auto& self, const UiNode& node) -> void {
        if (!stateFor(node.id)) return;
        if (const auto* leaf = std::get_if<UiLeaf>(&node.content)) {
            const auto* nodeState = stateFor(node.id);
            if (nodeState && nodeState->leaf) {
                const auto& semantic = *nodeState->leaf;
                if (leaf->widget.value &&
                    leaf->widget.value->isProvider) {
                    providers.push_back(
                        {leaf->widget.value->provider,
                         {semantic.value, semantic.label, semantic.command,
                          semantic.active}});
                }
                if (leaf->widget.checked &&
                    leaf->widget.checked->isProvider &&
                    semantic.checked) {
                    providers.push_back(
                        {leaf->widget.checked->provider,
                         {*semantic.checked ? "true" : "false", {},
                          std::nullopt, std::nullopt}});
                }
            }
        }
        if (const auto* container =
                std::get_if<UiContainer>(&node.content)) {
            for (const auto& child : container->children) self(self, child);
        }
    };
    const auto validate = [&](const auto& self,
                              const UiNode& node) -> std::optional<std::string> {
        if (!stateFor(node.id)) {
            return regionRoot.id.value() +
                   " UI state is missing node " + node.id.value();
        }
        if (const auto* container =
                std::get_if<UiContainer>(&node.content)) {
            for (const auto& child : container->children) {
                if (auto error = self(self, child)) return error;
            }
        }
        return std::nullopt;
    };
    if (auto error = validate(validate, regionRoot)) {
        return {*error};
    }
    collect(collect, regionRoot);
    const WidgetProviderResolver resolver =
        [providers = std::move(providers)](
            std::string_view id) -> std::optional<ResolvedProvider> {
        const auto found = std::ranges::find(providers, id, &ProviderState::id);
        return found == providers.end()
                   ? std::nullopt
                   : std::optional<ResolvedProvider>{found->value};
    };
    return projectUiRegion(regionRoot, rect, defaultRole, style, resolver,
                               out, statusView, input);
}

}  // namespace ssg
