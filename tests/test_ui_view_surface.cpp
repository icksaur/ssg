// Contract oracle for the opaque View surface vocabulary: every ViewSurface names
// a non-empty, stable set of authoritative snapshot sections that back it, and the
// grid chrome path refuses a View it cannot render (rather than emitting empty).
// The surface->section mapping is the enforced data-channel contract for the
// closed surface set.

#include "ssg/ChromeLowering.h"
#include "ssg/UiTree.h"
#include "ssg/ViewSurfaceBacking.h"
#include "ssg/Widget.h"
#include "test_helpers.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

using ssg::Axis;
using ssg::kAllViewSurfaces;
using ssg::Size;
using ssg::SnapshotSection;
using ssg::UiContainer;
using ssg::UiLeaf;
using ssg::UiNode;
using ssg::UiNodeId;
using ssg::viewSurfaceBackingSections;
using ssg::ViewSurface;
using ssg::WidgetDescriptor;
using ssg::WidgetKind;

// CONTRACT witness: every surface in the closed vocabulary maps to at least one
// authoritative snapshot section -- a surface with no data channel is forbidden.
TEST(everyViewSurfaceHasANonEmptyBacking) {
    for (const ViewSurface surface : kAllViewSurfaces) {
        ASSERT_TRUE(!viewSurfaceBackingSections(surface).empty());
    }
}

// The mapping is the specific, stable contract each client renders against.
TEST(theSurfaceBackingMappingIsTheSpecifiedContract) {
    const auto has = [](ViewSurface surface, SnapshotSection section) {
        for (const SnapshotSection s : viewSurfaceBackingSections(surface))
            if (s == section) return true;
        return false;
    };
    ASSERT_TRUE(has(ViewSurface::TabView, SnapshotSection::Tabs));
    ASSERT_TRUE(has(ViewSurface::TabView, SnapshotSection::Document));
    ASSERT_TRUE(has(ViewSurface::FileTree, SnapshotSection::Tree));
    ASSERT_TRUE(has(ViewSurface::GitStatus, SnapshotSection::Tree));
    ASSERT_TRUE(has(ViewSurface::FindResults, SnapshotSection::Palette));
}

// The grid chrome lowering renders only chrome widget kinds; a View reaching it is
// a loud conformance failure, never silent empty content.
TEST(gridChromeLoweringRefusesAViewLeaf) {
    WidgetDescriptor view;
    view.kind = WidgetKind::View;
    view.id = "footer.left.0";
    view.surface = ViewSurface::FileTree;

    // A canonical chrome region shape: Row[ left(Auto), middle(Flex), right(Auto) ]
    // with the View in the left group.
    UiContainer left{Axis::Row, {}, {},
                     {UiNode{UiNodeId{"footer.left.0"}, Size::autoSize(),
                             UiLeaf{view}}}};
    UiContainer middle{Axis::Row, {}, {}, {}};
    UiContainer right{Axis::Row, {}, {}, {}};
    UiContainer root{
        Axis::Row, {}, {},
        {UiNode{UiNodeId{"footer.left"}, Size::autoSize(), std::move(left)},
         UiNode{UiNodeId{"footer.middle"}, Size::flex(), std::move(middle)},
         UiNode{UiNodeId{"footer.right"}, Size::autoSize(), std::move(right)}}};
    ssg::UiRegion region{ssg::RegionRole::Bottom,
                         UiNode{UiNodeId{"footer"}, Size::flex(),
                                std::move(root)}};

    std::vector<ssg::AccessibilityNode> out;
    const auto empty =
        [](std::string_view) -> std::optional<ssg::ResolvedProvider> {
        return std::nullopt;
    };
    const auto result = ssg::lowerUiChromeRegion(
        region, {0, 0, 100, 1}, ssg::ShellNodeKind::FooterField,
        ssg::SemanticRole::Footer, ssg::Style{}, empty, out);
    ASSERT_TRUE(!result.ok());
}

}  // namespace

int main() {
    RUN(everyViewSurfaceHasANonEmptyBacking);
    RUN(theSurfaceBackingMappingIsTheSpecifiedContract);
    RUN(gridChromeLoweringRefusesAViewLeaf);
    return failed == 0 ? 0 : 1;
}
