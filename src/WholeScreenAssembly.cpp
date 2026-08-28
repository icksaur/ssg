#include <ssg/WholeScreenAssembly.h>

#include <ssg/ChromeRegionShape.h>
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
// ChromeProviderResolver keyed by id), so a field whose value, command, or provider
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

// The stable status-actions widget: one node carrying only its id, whose data (the
// selected status item's actions, its status id, and generation) rides the promptStatus
// section. It is ALWAYS emitted in the built-in footer so the schema stays generation-
// stable while the actions vary frame to frame on their own section cadence.
WidgetDescriptor statusActionsWidget() {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::StatusActions;
    widget.id = "footer.status_actions";
    return widget;
}

// A built-in header/footer region: catalog fields as the left group, `right` as the
// right group, no center, in the shared canonical region shape.
UiNode builtinRegion(std::string_view base,
                     const std::vector<StatusFieldCatalogEntry>& entries,
                     std::vector<WidgetDescriptor> right) {
    std::vector<WidgetDescriptor> left;
    left.reserve(entries.size());
    for (const StatusFieldCatalogEntry& entry : entries)
        left.push_back(fieldFor(entry));
    return chromeRegion(base, left, right, /*center=*/std::nullopt,
                        CenterWidth::Flex, /*centerFixed=*/0, /*separator=*/1);
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

// The composed area with `id`, if the override carries one as a direct child of its
// root. A composed override is a root Column of the header/footer subtrees it defines;
// an area it omits falls through to the built-in.
const UiNode* composedArea(const std::optional<ValidatedComposition>& override,
                           std::string_view id) {
    if (!override) return nullptr;
    const UiComposition& comp = override->composition();
    if (const auto* root = std::get_if<UiContainer>(&comp.root.content)) {
        for (const UiNode& child : root->children) {
            if (child.id.value() == id) return &child;
        }
    }
    return nullptr;
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
    return UiNode{UiNodeId{std::string{kHeaderPromptInputNodeId}}, Size::autoSize(),
                  UiLeaf{widget}};
}

std::string promptControlNodeId(std::string_view controlId) {
    return std::string{kFooterPromptNodeId} + ".control." +
           std::string{controlId};
}

UiNode footerPromptInput(const PromptControl& control) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::TextInput;
    widget.id = control.id;
    widget.value = ValueSource{true, "", control.id};
    widget.command = control.command;
    widget.role = "prompt";
    return UiNode{UiNodeId{promptControlNodeId(control.id)}, Size::exact(1),
                  UiLeaf{std::move(widget)}};
}

UiNode footerPromptToggle(const PromptControl& control, int width) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::Checkbox;
    widget.id = control.id;
    widget.value = ValueSource{false, control.accessibleLabel, ""};
    widget.checked = ValueSource{true, "", control.id};
    widget.command = control.command;
    widget.role = "prompt";
    return UiNode{UiNodeId{promptControlNodeId(control.id)}, Size::exact(width),
                  UiLeaf{std::move(widget)}};
}

UiNode footerPromptCount(const PromptControl& control) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::Label;
    widget.id = control.id;
    widget.value = ValueSource{true, "", control.id};
    widget.role = "prompt";
    return UiNode{UiNodeId{promptControlNodeId(control.id)}, Size::flex(),
                  UiLeaf{std::move(widget)}};
}

// Insert the prompt input right after the header's left group (the status
// fields), so tree order matches the visual order the query line occupies: a
// client that lays out in tree order renders it immediately after the fields and
// before the flex middle, rather than pushed to the far right past the middle.
// The grid extracts it by id (position-independent) and places it by the
// reserve/expand rule. Applies to both the built-in and a composed header, each
// the canonical [left, middle, right].
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
    std::string_view promptSigil,
    const std::optional<ValidatedComposition>& composedOverride) {
    const UiNode* composedHeader = composedArea(composedOverride, kHeaderNodeId);
    const UiNode* composedFooter = composedArea(composedOverride, kFooterNodeId);

    // Split the stable catalog superset by region -- the entry's own region, so a
    // caller cannot mis-split header/footer or smuggle in a projected subset.
    std::vector<StatusFieldCatalogEntry> headerEntries;
    std::vector<StatusFieldCatalogEntry> footerEntries;
    for (const StatusFieldCatalogEntry& entry : catalog) {
        (entry.region == StatusFieldRegion::Header ? headerEntries : footerEntries)
            .push_back(entry);
    }

    // The built-in footer right group is STRUCTURALLY STABLE: the hint (provider-
    // backed, label rides uiState) and the status-actions affordance (data rides
    // promptStatus) are always present. A composed ssg.chrome footer replaces the
    // whole built-in footer and so omits both -- matching the grid's whole-footer
    // replacement.
    std::vector<WidgetDescriptor> footerRight;
    footerRight.push_back(hintField(hintCommandId));
    footerRight.push_back(statusActionsWidget());

    UiNode header = composedHeader
                        ? *composedHeader
                        : builtinRegion(kHeaderNodeId, headerEntries, {});
    UiNode footer = composedFooter
                        ? *composedFooter
                        : builtinRegion(kFooterNodeId, footerEntries,
                                        std::move(footerRight));
    header = withStyle(
        withSize(std::move(header), Size::exact(dimensions.headerHeight)),
        SemanticRole::Header, SemanticRole::HeaderBackground);
    footer = withStyle(
        withSize(std::move(footer), Size::exact(dimensions.footerHeight)),
        SemanticRole::Footer, SemanticRole::FooterBackground);
    insertPromptInput(header, promptSigil);
    UiNode panel = withStyle(
        container(
            kPanelNodeId, Axis::Column, Size::exact(dimensions.panelTargetWidth),
            {viewLeaf(kFileTreeNodeId, ViewSurface::FileTree, Size::flex()),
             viewLeaf(kGitStatusNodeId, ViewSurface::GitStatus, Size::flex()),
             viewLeaf(kSymbolsNodeId, ViewSurface::Symbols, Size::flex())},
            ScrollAxis::Vertical),
        SemanticRole::PanelInactive, SemanticRole::TreeBackground);
    UiNode documentViewport = container(
        kDocumentViewportNodeId, Axis::Column, Size::flex(),
        {withStyle(viewLeaf(kDocumentNodeId, ViewSurface::Document, Size::flex()),
                   SemanticRole::Text, SemanticRole::Canvas)},
        ScrollAxis::Vertical);
    UiNode editor = container(
        kEditorNodeId, Axis::Column, Size::flex(),
        {withStyle(viewLeaf(kTabBarNodeId, ViewSurface::TabBar,
                            Size::exact(dimensions.tabBarHeight)),
                   SemanticRole::TabInactive,
                   SemanticRole::TabInactiveBackground),
         std::move(documentViewport)});
    UiNode findResultsViewport = container(
        kFindResultsViewportNodeId, Axis::Column, Size::flex(),
        {withStyle(viewLeaf(kFindResultsNodeId, ViewSurface::FindResults,
                            Size::flex()),
                   SemanticRole::Text, SemanticRole::Canvas)},
        ScrollAxis::Vertical);
    UiNode content = container(
        kContentNodeId, Axis::Column, Size::flex(),
        {std::move(editor), std::move(findResultsViewport)});
    UiNode body = container(kBodyNodeId, Axis::Row, Size::flex(),
                            {std::move(panel), std::move(content)});
    // The draft-conflict notice's semantic surface, always assembled and hidden by
    // presence (WholeScreenInteraction). Auto-sized so its footprint is the runtime's
    // reserved chrome row above the document; the grid host ignores it and renders
    // ShellNotice with rects.
    UiNode notice = withStyle(
        viewLeaf(kNoticeNodeId, ViewSurface::Notice, Size::autoSize()),
        SemanticRole::Canvas, SemanticRole::StatusWarning);
    // The external-modification bar's semantic surface, always assembled and
    // hidden by presence (WholeScreenInteraction). Auto-sized so its footprint is
    // the runtime's reserved chrome rows above the document (adjacent to the
    // notice, fixed order); the grid host ignores it and renders the bounded
    // ShellExternalBar with rects. 5b-1 left it a bare container to anchor the
    // external-focus capture; 5b-2 gives it the rendered View leaf.
    UiNode externalMod = withStyle(
        viewLeaf(kExternalModNodeId, ViewSurface::ExternalModification,
                 Size::autoSize()),
        SemanticRole::Canvas, SemanticRole::StatusWarning);
    // The footer prompt starts as an empty, hidden container. An accepted prompt
    // request overlays its controls before the schema owner publishes it.
    UiNode footerPrompt = withStyle(
        container(kFooterPromptNodeId, Axis::Column, Size::autoSize(), {}),
        SemanticRole::Prompt, SemanticRole::Canvas);

    UiComposition out;
    out.root = withStyle(
        container(kRootNodeId, Axis::Column, Size::flex(),
                  {std::move(header), std::move(notice),
                   std::move(externalMod), std::move(body),
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

UiNode assembleFooterPrompt(const PromptSurface& prompt) {
    UiNode promptNode = withStyle(
        container(kFooterPromptNodeId, Axis::Column, Size::autoSize(), {}),
        SemanticRole::Prompt, SemanticRole::Canvas);
    if (!prompt.request() ||
        promptFocusRegion(prompt.request()->kind) != PromptRegion::Footer) {
        return promptNode;
    }

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
