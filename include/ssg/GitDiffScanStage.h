#pragma once

#include <ssg/DiffIngressResult.h>
#include <ssg/FollowEditsModel.h>
#include <ssg/GitDiffSource.h>

#include <vector>

namespace ssg {

// Stages an entire git scan before the editor adopts any diff or follow state.
// On rejection, the editor must not adopt either model or any status-only ids.
struct GitDiffScanStage {
    DiffIngressResult result;
    DiffModel diff;
    FollowEditsModel follow;
    std::vector<DiffFileId> statusOnlyIds;
    bool mutated = false;

    [[nodiscard]] bool accepted() const noexcept { return result.accepted(); }
};

[[nodiscard]] GitDiffScanStage stageGitDiffScan(
    GitDiffScan scan, const DiffModel& diff, const FollowEditsModel& follow);

} // namespace ssg
