#include <ssg/WholeScreenAssembly.h>

#include <ssg/ChromeRegionShape.h>
#include <ssg/Widget.h>  // ViewSurface, Overflow

#include <string>
#include <utility>
#include <variant>

namespace ssg {

namespace {

// A built-in status field becomes a provider-backed Field keyed by the field id: the
// same ChromeProviderResolver the composed path uses resolves its value, label, and
// inherited click command. Its collapse rank carries onto the widget so a consumer that
// collapses a crowded row honors the same priority the grid does.
WidgetDescriptor fieldFor(const StatusField& field) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::Field;
    widget.id = field.id;
    widget.value = ValueSource{/*isProvider=*/true, /*literal=*/"", field.id};
    if (field.commandId) widget.command = field.commandId;
    widget.rank = field.collapseRank;
    return widget;
}

// The footer help hint becomes a right-group Field: a literal label with the hint's
// click command, truncating when the row is crowded (as the grid footer truncates it).
WidgetDescriptor hintField(const ShellFooterHint& hint) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::Field;
    widget.id = "footer.hint";
    widget.value = ValueSource{/*isProvider=*/false, hint.label, ""};
    if (!hint.commandId.empty()) widget.command = hint.commandId;
    widget.overflow = Overflow::Truncate;
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
    const std::optional<ShellFooterHint>& footerHint,
    const StyleDimensions& dimensions,
    const std::optional<ValidatedComposition>& composedOverride) {
    const UiNode* composedHeader = composedArea(composedOverride, kHeaderNodeId);
    const UiNode* composedFooter = composedArea(composedOverride, kFooterNodeId);

    std::vector<WidgetDescriptor> footerRight;
    if (footerHint && !footerHint->label.empty())
        footerRight.push_back(hintField(*footerHint));

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
