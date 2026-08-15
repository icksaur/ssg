#include <ssg/WholeScreenAssembly.h>

#include <ssg/ChromeRegionShape.h>
#include <ssg/Widget.h>  // ViewSurface

#include <string>
#include <utility>
#include <variant>

namespace ssg {

namespace {

// A built-in status field becomes a provider-backed Field keyed by the field id: the
// same ChromeProviderResolver the composed path uses resolves its value, label, and
// inherited click command, so a built-in and a composed field resolve identically.
WidgetDescriptor fieldFor(const StatusField& field) {
    WidgetDescriptor widget;
    widget.kind = WidgetKind::Field;
    widget.id = field.id;
    widget.value = ValueSource{/*isProvider=*/true, /*literal=*/"", field.id};
    if (field.commandId) widget.command = field.commandId;
    return widget;
}

// A built-in header/footer region: its status fields as the left group, no center and
// no right group, in the shared canonical region shape.
UiNode builtinRegion(std::string_view base,
                     const std::vector<StatusField>& fields) {
    std::vector<WidgetDescriptor> left;
    left.reserve(fields.size());
    for (const StatusField& field : fields) left.push_back(fieldFor(field));
    return chromeRegion(base, left, /*right=*/{}, /*center=*/std::nullopt,
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
const UiNode* composedArea(const std::optional<UiComposition>& override,
                           std::string_view id) {
    if (!override) return nullptr;
    if (const auto* root = std::get_if<UiContainer>(&override->root.content)) {
        for (const UiNode& child : root->children) {
            if (child.id.value() == id) return &child;
        }
    }
    return nullptr;
}

// Take a header/footer subtree from whichever source and fix its extent for the
// whole-screen Column, so a composed override cannot change the reserved row count.
UiNode withSize(UiNode node, Size size) {
    node.size = size;
    return node;
}

}  // namespace

UiComposition assembleWholeScreen(
    const std::vector<StatusField>& headerFields,
    const std::vector<StatusField>& footerFields,
    const std::optional<UiComposition>& composedOverride) {
    const UiNode* composedHeader = composedArea(composedOverride, kHeaderNodeId);
    const UiNode* composedFooter = composedArea(composedOverride, kFooterNodeId);

    UiNode header = composedHeader ? *composedHeader
                                   : builtinRegion(kHeaderNodeId, headerFields);
    UiNode footer = composedFooter ? *composedFooter
                                   : builtinRegion(kFooterNodeId, footerFields);
    header = withSize(std::move(header), Size::exact(kHeaderRows));
    footer = withSize(std::move(footer), Size::exact(kFooterRows));

    // panel and content each hold their two mutually-exclusive view leaves; presence
    // (a later step) decides which is shown. Here both are in the retained tree.
    UiNode panel = container(
        kPanelNodeId, Axis::Column, Size::exact(kPanelWidth),
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
