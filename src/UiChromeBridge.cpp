#include <ssg/UiChromeBridge.h>

#include <string>
#include <utility>
#include <vector>

namespace ssg {

namespace {

UiNode leafFor(const WidgetDescriptor& widget, std::string id, Size size) {
    return UiNode{UiNodeId{std::move(id)}, size, UiLeaf{widget}};
}

// A group container holding the given widgets as leaves. Its Size is Auto
// (content-sized: it never grows to fill), so a generic client renders the left
// group flush at the start and the right group flush at the end, with the flex
// middle between them absorbing the slack. `gap` is the separator between items.
UiNode groupFor(std::string id, const std::vector<WidgetDescriptor>& widgets,
                int gap) {
    UiContainer container;
    container.axis = Axis::Row;
    container.gap = Gap::of(gap);
    for (std::size_t i = 0; i < widgets.size(); ++i) {
        container.children.push_back(
            leafFor(widgets[i], id + "." + std::to_string(i), Size::autoSize()));
    }
    return UiNode{UiNodeId{std::move(id)}, Size::autoSize(), std::move(container)};
}

}  // namespace

UiRegion uiChromeRegionFromRow(const RowDescriptor& row, RegionRole role) {
    const std::string base{regionRoleName(role)};

    // Row = [ left(Auto), middle(Flex), right(Auto) ]. The Auto end groups size to
    // content and the Flex middle absorbs the slack, so the packing (left flush,
    // right flush) is encoded in the SIZING, not in positional convention. The
    // center widget, if any, lives at the start of the flex middle; its own Size
    // carries the width policy (Flex fills the middle; Exact is a fixed center
    // that sits right after the left group, matching the grid's leftEnd rule).
    UiNode left = groupFor(base + ".left", row.left, row.separator);
    UiNode right = groupFor(base + ".right", row.right, 0);

    UiContainer middleContainer;
    middleContainer.axis = Axis::Row;
    if (row.center) {
        const Size centerSize = row.centerWidth == CenterWidth::Fixed
                                    ? Size::exact(row.centerFixed)
                                    : Size::flex();
        middleContainer.children.push_back(
            leafFor(*row.center, base + ".middle.0", centerSize));
    }
    UiNode middle{UiNodeId{base + ".middle"}, Size::flex(),
                  std::move(middleContainer)};

    UiContainer rootContainer;
    rootContainer.axis = Axis::Row;
    rootContainer.children.push_back(std::move(left));
    rootContainer.children.push_back(std::move(middle));
    rootContainer.children.push_back(std::move(right));

    return UiRegion{role,
                    UiNode{UiNodeId{base}, Size::flex(), std::move(rootContainer)}};
}

UiSchema uiSchemaFromChrome(const ChromeComposition& chrome,
                            Generation generation) {
    UiSchema schema;
    schema.generation = generation;
    if (chrome.header) {
        schema.regions.push_back(
            uiChromeRegionFromRow(*chrome.header, RegionRole::Top));
    }
    if (chrome.footer) {
        schema.regions.push_back(
            uiChromeRegionFromRow(*chrome.footer, RegionRole::Bottom));
    }
    return schema;
}

}  // namespace ssg
