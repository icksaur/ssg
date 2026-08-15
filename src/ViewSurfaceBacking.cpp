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
    switch (surface) {
    case ViewSurface::TabView:
        return kTabView;
    case ViewSurface::FileTree:
        return kFileTree;
    case ViewSurface::GitStatus:
        return kGitStatus;
    case ViewSurface::FindResults:
        return kFindResults;
    }
    // A corrupt enumerator has no backing; the contract is never-empty, so this is
    // a hard error rather than an empty span, matching viewSurfaceName.
    throw std::invalid_argument(
        "viewSurfaceBackingSections: unrecognized ViewSurface");
}

}  // namespace ssg
