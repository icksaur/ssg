#include <ssg/ViewSurfaceBacking.h>

#include <array>
#include <stdexcept>

namespace ssg {

std::span<const SnapshotSection> viewSurfaceBackingSections(ViewSurface surface) {
    static constexpr std::array kTabBar{SnapshotSection::Tabs};
    static constexpr std::array kDocument{
        SnapshotSection::Document, SnapshotSection::Selection,
        SnapshotSection::Syntax};
    static constexpr std::array kFindResults{SnapshotSection::Palette};
    static constexpr std::array kTree{SnapshotSection::Tree};
    static constexpr std::array kFooterPrompt{SnapshotSection::PromptView};
    static constexpr std::array kNotice{SnapshotSection::NoticeView};
    static constexpr std::array kExternalModification{
        SnapshotSection::ExternalModification};
    switch (surface) {
    case ViewSurface::TabBar:
        return kTabBar;
    case ViewSurface::FindResults:
        return kFindResults;
    case ViewSurface::FooterPrompt:
        return kFooterPrompt;
    case ViewSurface::Notice:
        return kNotice;
    case ViewSurface::ExternalModification:
        return kExternalModification;
    case ViewSurface::Document:
        return kDocument;
    case ViewSurface::Tree:
        return kTree;
    }
    // A corrupt enumerator has no backing; the contract is never-empty, so this is
    // a hard error rather than an empty span, matching viewSurfaceName.
    throw std::invalid_argument(
        "viewSurfaceBackingSections: unrecognized ViewSurface");
}

SnapshotSection statusActionsBackingSection() {
    return SnapshotSection::PromptStatus;
}

}  // namespace ssg
