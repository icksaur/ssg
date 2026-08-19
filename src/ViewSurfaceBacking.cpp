#include <ssg/ViewSurfaceBacking.h>

#include <array>
#include <stdexcept>

namespace ssg {

std::span<const SnapshotSection> viewSurfaceBackingSections(ViewSurface surface) {
    static constexpr std::array kTabView{SnapshotSection::Tabs,
                                         SnapshotSection::Document};
    static constexpr std::array kFileTree{SnapshotSection::Tree};
    static constexpr std::array kGitStatus{SnapshotSection::Tree};
    static constexpr std::array kFindResults{SnapshotSection::Palette};
    static constexpr std::array kSymbols{SnapshotSection::Tree};
    static constexpr std::array kFooterPrompt{SnapshotSection::PromptView};
    static constexpr std::array kNotice{SnapshotSection::NoticeView};
    static constexpr std::array kExternalModification{
        SnapshotSection::ExternalModification};
    switch (surface) {
    case ViewSurface::TabView:
        return kTabView;
    case ViewSurface::FileTree:
        return kFileTree;
    case ViewSurface::GitStatus:
        return kGitStatus;
    case ViewSurface::FindResults:
        return kFindResults;
    case ViewSurface::Symbols:
        return kSymbols;
    case ViewSurface::FooterPrompt:
        return kFooterPrompt;
    case ViewSurface::Notice:
        return kNotice;
    case ViewSurface::ExternalModification:
        return kExternalModification;
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
