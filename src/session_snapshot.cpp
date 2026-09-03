#include <ssg/session_snapshot.h>

#include <utility>

namespace ssg {

bool operator==(SessionSnapshotSections const& left,
                SessionSnapshotSections const& right) {
    return left.document == right.document &&
           left.selection == right.selection && left.history == right.history &&
           left.clipboard == right.clipboard &&
           left.promptStatus == right.promptStatus && left.search == right.search &&
           left.findReplace == right.findReplace && left.settings == right.settings &&
           left.keymap == right.keymap && left.textEncoding == right.textEncoding &&
           left.tabs == right.tabs && left.diff == right.diff &&
           left.externalModification == right.externalModification &&
           left.followEdits == right.followEdits && left.tree == right.tree &&
           left.syntax == right.syntax && left.lspSync == right.lspSync &&
           left.lspFeatures == right.lspFeatures && left.theme == right.theme &&
           left.palette == right.palette && left.uiTree == right.uiTree &&
           left.noticeView == right.noticeView &&
           left.watcherAvailable == right.watcherAvailable;
}

SessionSnapshot::SessionSnapshot(Revision revision, SessionTopology topology,
                                 SessionSnapshotSections sections)
    : revision_{revision},
      topology_{std::move(topology)},
      sections_{std::move(sections)} {}

bool SessionSnapshot::operator==(SessionSnapshot const& other) const {
    return revision_ == other.revision_ && topology_ == other.topology_ &&
           sections_ == other.sections_;
}

}  // namespace ssg
