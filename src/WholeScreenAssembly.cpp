#include <ssg/WholeScreenAssembly.h>

#include <ssg/Widget.h>  // ViewSurface, Overflow

#include <string>
#include <stdexcept>
#include <utility>
#include <variant>

namespace ssg {

namespace {

// A built-in status field is a provider-backed Field keyed by the catalog entry id. It
// carries only STABLE structure -- the id and the collapse rank; its value, accessible
// label, and click command all ride uiState (resolved per frame by the
// WidgetProviderResolver keyed by id), so a field whose value, command, or provider
// presence changes never alters the schema structure. A field the resolver has no value
// for resolves to no leaf state (the semantic drop), exactly as the grid drops an empty
// status field.
WidgetDescriptor fieldFor(const StatusFieldCatalogEntry& entry) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::Field;
    widget.id = entry.id;
    widget.value = ValueSource{/*isProvider=*/true, /*literal=*/"", entry.id};
    widget.rank = entry.collapseRank;
    return widget;
}

// The stable footer hint node id, and the provider id its label resolves through.
inline constexpr std::string_view kFooterHintId = "footer.hint";

// The footer help hint is a STABLE right-group Field whose LABEL rides uiState (a
// provider keyed by the hint id), not a literal in the structure -- so the keymap-
// derived hint text changing never alters the schema structure or advances its
// generation. Its click command is the stable hint command. When the label resolves
// empty (the hint unbound), resolveUiState drops the leaf, as the grid drops it.
WidgetDescriptor hintField(std::string_view hintCommandId) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::Field;
    widget.id = std::string{kFooterHintId};
    widget.value =
        ValueSource{/*isProvider=*/true, /*literal=*/"", std::string{kFooterHintId}};
    if (!hintCommandId.empty()) widget.command = std::string{hintCommandId};
    widget.overflow = Overflow::Truncate;
    return widget;
}

UiNode regionGroup(std::string id, std::vector<WidgetDescriptor> widgets,
                   Size size, int gap = 0) {
    UiContainer group;
    group.axis = Axis::Row;
    group.gap = Gap::of(gap);
    for (std::size_t index = 0; index < widgets.size(); ++index) {
        group.children.push_back(
            UiNode{UiNodeId{id + "." + std::to_string(index)}, Size::autoSize(),
                   UiLeaf{std::move(widgets[index])}});
    }
    return UiNode{UiNodeId{std::move(id)}, size, std::move(group)};
}

// A fixed header/footer region expressed only through the published UiNode
// vocabulary: content-sized end groups and a flex spacer group between them.
UiNode builtinRegion(std::string_view base,
                     const std::vector<StatusFieldCatalogEntry>& entries,
                     std::vector<WidgetDescriptor> right) {
    std::vector<WidgetDescriptor> left;
    left.reserve(entries.size());
    for (const StatusFieldCatalogEntry& entry : entries)
        left.push_back(fieldFor(entry));
    const std::string baseId{base};
    UiContainer region;
    region.axis = Axis::Row;
    region.children.push_back(
        regionGroup(baseId + ".left", std::move(left), Size::autoSize(), 1));
    region.children.push_back(
        regionGroup(baseId + ".middle", {}, Size::flex()));
    region.children.push_back(
        regionGroup(baseId + ".right", std::move(right), Size::autoSize()));
    return UiNode{UiNodeId{baseId}, Size::flex(), std::move(region)};
}

UiNode viewLeaf(std::string_view id, ViewSurface surface, Size size) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::View;
    widget.id = std::string{id};
    widget.surface = surface;
    return UiNode{UiNodeId{std::string{id}}, size, UiLeaf{widget}};
}

UiNode container(std::string_view id, Axis axis, Size size,
                 std::vector<UiNode> children,
                 ScrollAxis scroll = ScrollAxis::None) {
    UiContainer body;
    body.axis = axis;
    body.scroll = scroll;
    body.children = std::move(children);
    return UiNode{UiNodeId{std::string{id}}, size, std::move(body)};
}

UiNode withSize(UiNode node, Size size) {
    node.size = size;
    return node;
}

UiNode withStyle(UiNode node, SemanticRole foreground,
                 SemanticRole background) {
    node.style.foreground = foreground;
    node.style.background = background;
    return node;
}

UiNode withFocusContext(UiNode node, FocusTarget context) {
    node.focusContext = context;
    return node;
}

// The always-assembled header prompt input: a TextInput leaf carrying only its
// stable identity and role. Its query/ghost never ride the schema (a client owns
// the prediction locally); the grid lowers it to input_line.query/.ghost nodes
// only when a header prompt is open, gated by presence.
UiNode promptInputLeaf(std::string_view promptSigil) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::TextInput;
    widget.id = std::string{kHeaderPromptInputNodeId};
    widget.role = "prompt";
    widget.sigil = std::string{promptSigil};
    return withFocusContext(
        UiNode{UiNodeId{std::string{kHeaderPromptInputNodeId}},
               Size::autoSize(), UiLeaf{widget}},
        FocusTarget::Prompt);
}

UiNode footerPromptInput(const PromptControl& control) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::TextInput;
    widget.id = control.id;
    widget.value =
        ValueSource{true, "", footerPromptControlNodeId(control.id).value()};
    widget.command = control.command;
    widget.role = "prompt";
    return UiNode{footerPromptControlNodeId(control.id), Size::exact(1),
                  UiLeaf{std::move(widget)}};
}

UiNode footerPromptToggle(const PromptControl& control, int width) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::Checkbox;
    widget.id = control.id;
    widget.value = ValueSource{false, control.accessibleLabel, ""};
    widget.checked =
        ValueSource{true, "", footerPromptControlNodeId(control.id).value()};
    widget.command = control.command;
    widget.role = "prompt";
    return UiNode{footerPromptControlNodeId(control.id), Size::exact(width),
                  UiLeaf{std::move(widget)}};
}

UiNode footerPromptCount(const PromptControl& control) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::Label;
    widget.id = control.id;
    widget.value =
        ValueSource{true, "", footerPromptControlNodeId(control.id).value()};
    widget.role = "prompt";
    return UiNode{footerPromptControlNodeId(control.id), Size::flex(),
                  UiLeaf{std::move(widget)}};
}

// Insert the prompt input after the header's status fields so tree order matches
// the visual order the query line occupies.
void insertPromptInput(UiNode& header, std::string_view promptSigil) {
    if (auto* root = std::get_if<UiContainer>(&header.content)) {
        const auto afterLeftGroup =
            root->children.begin() + (root->children.empty() ? 0 : 1);
        root->children.insert(afterLeftGroup, promptInputLeaf(promptSigil));
    }
}

}  // namespace

UiComposition assembleWholeScreen(
    const std::vector<StatusFieldCatalogEntry>& catalog,
    std::string_view hintCommandId,
    const StyleDimensions& dimensions,
    std::string_view promptSigil) {

    // Split the stable catalog superset by region -- the entry's own region, so a
    // caller cannot mis-split header/footer or smuggle in a projected subset.
    std::vector<StatusFieldCatalogEntry> headerEntries;
    std::vector<StatusFieldCatalogEntry> footerEntries;
    for (const StatusFieldCatalogEntry& entry : catalog) {
        (entry.region == StatusFieldRegion::Header ? headerEntries : footerEntries)
            .push_back(entry);
    }

    // The footer carries the provider-backed hint and an ordinary status-action
    // container as fixed semantic nodes.
    std::vector<WidgetDescriptor> footerRight;
    footerRight.push_back(hintField(hintCommandId));

    UiNode header = builtinRegion(kHeaderNodeId, headerEntries, {});
    UiNode footer =
        builtinRegion(kFooterNodeId, footerEntries, std::move(footerRight));
    auto& region = std::get<UiContainer>(footer.content);
    auto& right = std::get<UiContainer>(region.children[2].content);
    right.children.push_back(
        container("footer.status_actions", Axis::Row, Size::autoSize(), {}));
    header = withStyle(
        withSize(std::move(header), Size::exact(dimensions.headerHeight)),
        SemanticRole::Header, SemanticRole::HeaderBackground);
    footer = withStyle(
        withSize(std::move(footer), Size::exact(dimensions.footerHeight)),
        SemanticRole::Footer, SemanticRole::FooterBackground);
    insertPromptInput(header, promptSigil);
    UiNode panel = withFocusContext(withStyle(
        container(
            kPanelNodeId, Axis::Column,
            Size::optionalPreferred(dimensions.panelTargetWidth,
                                    dimensions.panelMinimumWidth),
            {viewLeaf(kTreeNodeId, ViewSurface::Tree, Size::flex())},
            ScrollAxis::Vertical),
        SemanticRole::PanelInactive, SemanticRole::TreeBackground),
        FocusTarget::Panel);
    // Editor-owned transient UI sits after tabs and before the document. It
    // consumes document rows without spanning or moving the side panel.
    UiNode notice = withStyle(
        viewLeaf(kNoticeNodeId, ViewSurface::Notice, Size::autoSize()),
        SemanticRole::Canvas, SemanticRole::StatusWarning);
    UiNode externalMod = withFocusContext(withStyle(
        viewLeaf(kExternalModNodeId, ViewSurface::ExternalModification,
                 Size::autoSize()),
        SemanticRole::Canvas, SemanticRole::StatusWarning),
        FocusTarget::ExternalModification);
    UiNode documentViewport = container(
        kDocumentViewportNodeId, Axis::Column, Size::flex(),
        {withStyle(viewLeaf(kDocumentNodeId, ViewSurface::Document, Size::flex()),
                   SemanticRole::Text, SemanticRole::Canvas)},
        ScrollAxis::Vertical);
    UiNode editor = withFocusContext(
        container(kEditorNodeId, Axis::Column, Size::flex(),
                  {std::move(documentViewport)}),
        FocusTarget::Editor);
    UiNode findResultsViewport = container(
        kFindResultsViewportNodeId, Axis::Column, Size::flex(),
        {withStyle(viewLeaf(kFindResultsNodeId, ViewSurface::FindResults,
                            Size::flex()),
                   SemanticRole::Text, SemanticRole::Canvas)},
        ScrollAxis::Vertical);
    UiNode content = container(
        kContentNodeId, Axis::Column,
        Size::minimumFlex(dimensions.editorMinimumWidth),
        {withStyle(viewLeaf(kTabBarNodeId, ViewSurface::TabBar,
                            Size::exact(dimensions.tabBarHeight)),
                   SemanticRole::TabInactive,
                   SemanticRole::TabInactiveBackground),
         std::move(notice), std::move(externalMod), std::move(editor),
         std::move(findResultsViewport)});
    UiNode body = container(kBodyNodeId, Axis::Row, Size::flex(),
                            {std::move(panel), std::move(content)});
    // The footer prompt starts as an empty, hidden container. An accepted prompt
    // request overlays its controls before the schema owner publishes it.
    UiNode footerPrompt = withFocusContext(withStyle(
        container(kFooterPromptNodeId, Axis::Column, Size::autoSize(), {}),
        SemanticRole::Prompt, SemanticRole::Canvas),
        FocusTarget::Prompt);

    UiComposition out;
    out.root = withStyle(
        container(kRootNodeId, Axis::Column, Size::flex(),
                  {std::move(header), std::move(body),
                   std::move(footerPrompt), std::move(footer)}),
        SemanticRole::Text, SemanticRole::Canvas);
    return out;
}

UiComposition withFooterPrompt(UiComposition base,
                               const PromptSurface& prompt) {
    if (!prompt.request() ||
        promptFocusRegion(prompt.request()->kind) != PromptRegion::Footer) {
        return base;
    }

    UiNode* promptNode = nullptr;
    if (auto* root = std::get_if<UiContainer>(&base.root.content)) {
        for (auto& child : root->children) {
            if (child.id.value() == kFooterPromptNodeId) {
                promptNode = &child;
                break;
            }
        }
    }
    if (!promptNode) {
        throw std::logic_error(
            "whole-screen composition has no footer.prompt container");
    }

    if (!std::holds_alternative<UiContainer>(promptNode->content)) {
        throw std::logic_error("footer.prompt must be a container");
    }
    *promptNode = assembleFooterPrompt(prompt);
    return base;
}

UiComposition withStatusActions(
    UiComposition base, const std::vector<StatusActionNode>& actions) {
    UiNode* actionContainer = nullptr;
    // The fixed whole-screen footer owns the sole status-action anchor.
    if (auto* root = std::get_if<UiContainer>(&base.root.content)) {
        for (auto& area : root->children) {
            if (area.id.value() != kFooterNodeId) continue;
            auto* region = std::get_if<UiContainer>(&area.content);
            if (!region || region->children.size() != 3 ||
                region->children[2].id.value() != "footer.right") {
                break;
            }
            auto* right =
                std::get_if<UiContainer>(&region->children[2].content);
            if (!right) break;
            for (auto& child : right->children) {
                if (child.id.value() == "footer.status_actions") {
                    actionContainer = &child;
                    break;
                }
            }
            break;
        }
    }
    if (!actionContainer) return base;
    auto* actionChildren =
        std::get_if<UiContainer>(&actionContainer->content);
    if (!actionChildren) {
        throw std::logic_error(
            "footer.status_actions must be a container");
    }
    actionChildren->children.clear();
    actionChildren->children.reserve(actions.size());
    for (const auto& action : actions) {
        WidgetDescriptor widget;
        widget.kind = WidgetKind::Field;
        widget.id = action.id.value();
        widget.value = ValueSource{true, "", action.id.value()};
        widget.role = "status_info";
        actionChildren->children.push_back(
            UiNode{action.id, Size::autoSize(), UiLeaf{std::move(widget)}});
    }
    return base;
}

UiNode assembleFooterPrompt(const PromptSurface& prompt) {
    UiNode promptNode = withFocusContext(withStyle(
        container(kFooterPromptNodeId, Axis::Column, Size::autoSize(), {}),
        SemanticRole::Prompt, SemanticRole::Canvas),
        FocusTarget::Prompt);
    if (!prompt.request() ||
        promptFocusRegion(prompt.request()->kind) != PromptRegion::Footer) {
        return promptNode;
    }
    promptNode.accessibleLabel = prompt.request()->accessibleLabel;

    auto& column = std::get<UiContainer>(promptNode.content);
    const auto controls = resolvePromptControls(*prompt.request());
    std::size_t controlIndex = 0;
    for (; controlIndex < controls.size() &&
           controls[controlIndex].kind == PromptControlKind::Input;
         ++controlIndex) {
        column.children.push_back(footerPromptInput(controls[controlIndex]));
    }
    if (controlIndex < controls.size()) {
        std::vector<UiNode> options;
        for (const auto& toggle : prompt.request()->toggles) {
            options.push_back(
                footerPromptToggle(controls[controlIndex++], toggle.width));
        }
        if (controlIndex < controls.size()) {
            options.push_back(footerPromptCount(controls[controlIndex]));
        }
        column.children.push_back(
            container(kFooterPromptOptionsNodeId, Axis::Row, Size::exact(1),
                      std::move(options)));
    }
    return promptNode;
}

}  // namespace ssg
