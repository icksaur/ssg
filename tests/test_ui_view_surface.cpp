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

ssg::UiChromeLowerResult lowerLegacyChromeForTest(
    const ssg::UiNode& region, ssg::Rect rect,
    ssg::ShellNodeKind kind, ssg::SemanticRole role,
    const ssg::Style& style,
    const ssg::ChromeProviderResolver& resolver,
    std::vector<ssg::AccessibilityNode>& out) {
    ssg::SolvedChromeSurface solved;
    auto result = ssg::lowerUiChromeRegion(
        region, rect, role, style, resolver, solved);
    for (const auto& item : solved.items) {
        out.push_back({kind, item.id, item.label, item.rect, item.role,
                       item.content, item.command, item.statusInvocation});
    }
    return result;
}

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

// The StatusActions widget's data channel is encoded as a value (not prose), so a
// client renders its actions from the same section the library owns them on.
TEST(statusActionsIsBackedByPromptStatus) {
    ASSERT_TRUE(ssg::statusActionsBackingSection() ==
                SnapshotSection::PromptStatus);
}

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
    ASSERT_TRUE(has(ViewSurface::TabBar, SnapshotSection::Tabs));
    ASSERT_TRUE(has(ViewSurface::Document, SnapshotSection::Document));
    ASSERT_TRUE(has(ViewSurface::Document, SnapshotSection::Selection));
    ASSERT_TRUE(has(ViewSurface::Document, SnapshotSection::Syntax));
    ASSERT_TRUE(has(ViewSurface::FileTree, SnapshotSection::Tree));
    ASSERT_TRUE(has(ViewSurface::GitStatus, SnapshotSection::Tree));
    ASSERT_TRUE(has(ViewSurface::FindResults, SnapshotSection::Palette));
    ASSERT_TRUE(has(ViewSurface::Symbols, SnapshotSection::Tree));
    ASSERT_TRUE(has(ViewSurface::FooterPrompt, SnapshotSection::PromptView));
    ASSERT_TRUE(has(ViewSurface::Notice, SnapshotSection::NoticeView));
    ASSERT_TRUE(has(ViewSurface::ExternalModification,
                    SnapshotSection::ExternalModification));
}

// The external-modification View surface renders only against its own schema
// section: its backing is exactly the ExternalModification section and it
// borrows no other surface's data channel.
TEST(externalModSurfaceIsBackedOnlyByTheExternalModSection) {
    const auto sections =
        viewSurfaceBackingSections(ViewSurface::ExternalModification);
    ASSERT_EQ(sections.size(), std::size_t{1});
    ASSERT_TRUE(sections.front() == SnapshotSection::ExternalModification);
}

// The grid chrome lowering renders only chrome widget kinds; a View reaching it is
// a loud conformance failure, never silent empty content. A left/right leaf must be
// Auto-sized while a View must be Exact/Flex, so the only View shape reachable
// through a validated chrome region is the Flex/Exact center -- exercise that.
TEST(gridChromeLoweringRefusesAViewCenter) {
    WidgetDescriptor view;
    view.kind = WidgetKind::View;
    view.id = "footer.middle.0";
    view.surface = ViewSurface::FileTree;

    // Row[ left(Auto), middle(Flex) with a Flex View center, right(Auto) ].
    UiContainer left{Axis::Row, {}, {}, {}};
    UiContainer middle{Axis::Row, {}, {},
                       {UiNode{UiNodeId{"footer.middle.0"}, Size::flex(),
                               UiLeaf{view}}}};
    UiContainer right{Axis::Row, {}, {}, {}};
    UiContainer root{
        Axis::Row, {}, {},
        {UiNode{UiNodeId{"footer.left"}, Size::autoSize(), std::move(left)},
         UiNode{UiNodeId{"footer.middle"}, Size::flex(), std::move(middle)},
         UiNode{UiNodeId{"footer.right"}, Size::autoSize(), std::move(right)}}};
    UiNode regionRoot{UiNodeId{"footer"}, Size::flex(), std::move(root)};

    std::vector<ssg::AccessibilityNode> out;
    const auto empty =
        [](std::string_view) -> std::optional<ssg::ResolvedProvider> {
        return std::nullopt;
    };
    const auto result = lowerLegacyChromeForTest(
        regionRoot, {0, 0, 100, 1}, ssg::ShellNodeKind::FooterField,
        ssg::SemanticRole::Footer, ssg::Style{}, empty, out);
    ASSERT_TRUE(!result.ok());
}

}  // namespace

int main() {
    RUN(everyViewSurfaceHasANonEmptyBacking);
    RUN(theSurfaceBackingMappingIsTheSpecifiedContract);
    RUN(externalModSurfaceIsBackedOnlyByTheExternalModSection);
    RUN(statusActionsIsBackedByPromptStatus);
    RUN(gridChromeLoweringRefusesAViewCenter);
    return failed == 0 ? 0 : 1;
}
