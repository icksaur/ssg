#include <ssg/GitDiffScanStage.h>

#include <algorithm>
#include <utility>

namespace ssg {

GitDiffScanStage stageGitDiffScan(
    GitDiffScan scan, const DiffModel& diff, const FollowEditsModel& follow) {
    GitDiffScanStage staged{{}, diff, follow};
    std::vector<FollowDiffChange> followChanges;
    followChanges.reserve(scan.files.size() +
                          staged.diff.viewState().files.size());

    std::uint64_t nextRevision = staged.diff.viewState().revision + 1;
    const auto nextMutationRevision = [&nextRevision]() {
        const auto current = nextRevision;
        ++nextRevision;
        return current;
    };
    const auto removeDetailedFile =
        [&](const DiffFileId& id) -> DiffIngressResult {
        const auto prior = staged.diff.file(id);
        if (!prior || !staged.diff.isGitFile(id)) {
            return {};
        }
        auto removedFile = prior->get();
        auto priorHunks = removedFile.hunks;
        const auto revision = nextMutationRevision();
        const auto removed = staged.diff.removeFile(id, revision);
        if (!removed.accepted()) {
            return {DiffIngressError::DiffRejected};
        }
        staged.mutated = true;
        removedFile.deleted = true;
        removedFile.currentContent.clear();
        removedFile.hunks.clear();
        removedFile.changedLines.clear();
        followChanges.push_back(
            {std::move(removedFile), std::move(priorHunks), revision});
        return {};
    };

    std::vector<DiffFileId> scannedIds;
    scannedIds.reserve(scan.files.size());
    for (auto& file : scan.files) {
        scannedIds.push_back(file.id);
        std::vector<DiffHunk> priorHunks;
        if (const auto prior = staged.diff.file(file.id)) {
            priorHunks = prior->get().hunks;
        }
        const auto revision = nextMutationRevision();
        const auto applied = staged.diff.updateGitFile(
            {.id = file.id,
             .path = file.path,
             .previousPath = file.previousPath,
             .baselineContent = std::move(file.baselineContent),
             .workingContent = std::move(file.workingContent)},
            scan.baselineIdentity, revision);
        if (!applied.accepted()) {
            if (applied.error != DiffError::WorkLimitExceeded) {
                staged.result = {DiffIngressError::DiffRejected};
                return staged;
            }
            staged.statusOnlyIds.push_back(file.id);
            if (auto removed = removeDetailedFile(file.id);
                !removed.accepted()) {
                staged.result = removed;
                return staged;
            }
            continue;
        }
        const auto changedFile = staged.diff.file(file.id);
        if (!changedFile) {
            staged.result = {DiffIngressError::DiffRejected};
            return staged;
        }
        staged.mutated = true;
        followChanges.push_back(
            {changedFile->get(), std::move(priorHunks), revision});
    }

    const auto stagedView = staged.diff.viewState();
    for (const auto& file : stagedView.files) {
        if (std::find(scannedIds.begin(), scannedIds.end(), file.id) !=
            scannedIds.end()) {
            continue;
        }
        if (!staged.diff.isGitFile(file.id)) {
            continue;
        }
        if (auto removed = removeDetailedFile(file.id); !removed.accepted()) {
            staged.result = removed;
            return staged;
        }
    }

    if (staged.mutated) {
        const auto followed =
            staged.follow.acceptExternalChanges(std::move(followChanges));
        if (!followed.accepted()) {
            staged.result = {DiffIngressError::FollowRejected};
        }
    }
    return staged;
}

} // namespace ssg
