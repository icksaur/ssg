#include <ssg/ChromeRegionShape.h>

#include <string>
#include <utility>
#include <variant>

namespace ssg {

namespace {

UiNode leafFor(const WidgetDescriptor& widget, std::string id, Size size) {
    return UiNode{UiNodeId{std::move(id)}, size, UiLeaf{widget}};
}

}  // namespace

UiNode chromeGroup(std::string id, const std::vector<WidgetDescriptor>& widgets,
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

UiNode chromeRegion(std::string_view base,
                    const std::vector<WidgetDescriptor>& left,
                    const std::vector<WidgetDescriptor>& right,
                    const std::optional<WidgetDescriptor>& center,
                    CenterWidth centerWidth, int centerFixed, int separator) {
    const std::string baseId{base};
    UiNode leftGroup = chromeGroup(baseId + ".left", left, separator);
    UiNode rightGroup = chromeGroup(baseId + ".right", right, 0);

    UiContainer middleContainer;
    middleContainer.axis = Axis::Row;
    if (center) {
        const Size centerSize = centerWidth == CenterWidth::Fixed
                                    ? Size::exact(centerFixed)
                                    : Size::flex();
        middleContainer.children.push_back(
            leafFor(*center, baseId + ".middle.0", centerSize));
    }
    UiNode middle{UiNodeId{baseId + ".middle"}, Size::flex(),
                  std::move(middleContainer)};

    UiContainer rootContainer;
    rootContainer.axis = Axis::Row;
    rootContainer.children.push_back(std::move(leftGroup));
    rootContainer.children.push_back(std::move(middle));
    rootContainer.children.push_back(std::move(rightGroup));

    return UiNode{UiNodeId{baseId}, Size::flex(), std::move(rootContainer)};
}

}  // namespace ssg
