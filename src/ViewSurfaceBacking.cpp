#include <ssg/ViewSurfaceBacking.h>

#include <array>

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
    // Unreachable for a valid enumerator; a corrupt value has no backing.
    return {};
}

}  // namespace ssg
