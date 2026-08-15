#include <ssg/WholeScreenAssembly.h>

#include <ssg/ChromeRegionShape.h>
#include <ssg/Widget.h>  // ViewSurface, Overflow

#include <string>
#include <utility>
#include <variant>

namespace ssg {

namespace {

// A built-in status field is a provider-backed Field keyed by the field id. It carries
// only STABLE structure -- the id and the collapse rank; its value, accessible label,
// and click command all ride uiState (resolved per frame by the ChromeProviderResolver
// keyed by id), so a field whose value, command, or provider presence changes never
// alters the schema structure. A field the resolver has no value for resolves to no
// leaf state (the semantic drop), exactly as the grid drops an empty status field.
WidgetDescriptor fieldFor(const StatusField& field) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::Field;
    widget.id = field.id;
    widget.value = ValueSource{/*isProvider=*/true, /*literal=*/"", field.id};
    widget.rank = field.collapseRank;
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

// A built-in header/footer region: status fields as the left group, `right` as the
// right group, no center, in the shared canonical region shape.
UiNode builtinRegion(std::string_view base, const std::vector<StatusField>& fields,
                     std::vector<WidgetDescriptor> right) {
    std::vector<WidgetDescriptor> left;
    left.reserve(fields.size());
    for (const StatusField& field : fields) left.push_back(fieldFor(field));
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
                 std::vector<UiNode> children) {
    UiContainer body;
    body.axis = axis;
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

}  // namespace

UiComposition assembleWholeScreen(
    const std::vector<StatusField>& headerFields,
    const std::vector<StatusField>& footerFields,
    std::string_view hintCommandId,
    const StyleDimensions& dimensions,
    const std::optional<ValidatedComposition>& composedOverride) {
    const UiNode* composedHeader = composedArea(composedOverride, kHeaderNodeId);
    const UiNode* composedFooter = composedArea(composedOverride, kFooterNodeId);

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
                        : builtinRegion(kHeaderNodeId, headerFields, {});
    UiNode footer = composedFooter
                        ? *composedFooter
                        : builtinRegion(kFooterNodeId, footerFields,
                                        std::move(footerRight));
    header = withSize(std::move(header), Size::exact(dimensions.headerHeight));
    footer = withSize(std::move(footer), Size::exact(dimensions.footerHeight));

    UiNode panel = container(
        kPanelNodeId, Axis::Column, Size::exact(dimensions.panelTargetWidth),
        {viewLeaf(kFileTreeNodeId, ViewSurface::FileTree, Size::flex()),
         viewLeaf(kGitStatusNodeId, ViewSurface::GitStatus, Size::flex())});
    UiNode content = container(
        kContentNodeId, Axis::Column, Size::flex(),
        {viewLeaf(kTabViewNodeId, ViewSurface::TabView, Size::flex()),
         viewLeaf(kFindResultsNodeId, ViewSurface::FindResults, Size::flex())});
    UiNode body = container(kBodyNodeId, Axis::Row, Size::flex(),
                            {std::move(panel), std::move(content)});

    UiComposition out;
    out.root = container(kRootNodeId, Axis::Column, Size::flex(),
                         {std::move(header), std::move(body), std::move(footer)});
    return out;
}

}  // namespace ssg
