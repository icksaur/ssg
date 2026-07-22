#include "ssg/GitDiffSource.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace ssg {
namespace {

std::optional<DiffFileId> pathIdentity(const std::filesystem::path& path) {
    auto text = path.generic_string();
    if (text.empty()) {
        return std::nullopt;
    }
    return DiffFileId{std::move(text)};
}

std::set<DiffFileId> currentIds(const DiffModel& model) {
    std::set<DiffFileId> ids;
    for (const auto& file : model.viewState().files) {
        ids.insert(file.id);
    }
    return ids;
}

}  // namespace

GitDiffSource::GitDiffSource(DiffModel& diffModel, GitDiffConfig config)
    : diffModel_{&diffModel},
      config_{std::move(config)},
      nextRevision_{Revision{diffModel.viewState().revision.value() + 1}} {}

GitDiffRefreshResult GitDiffSource::refresh(GitRepository& repository) {
    auto scan = repository.scanDiff(config_);
    return applyFullScan(scan);
}

GitDiffRefreshResult GitDiffSource::refreshPaths(
    GitRepository& repository, const std::vector<std::filesystem::path>& paths) {
    auto scan = repository.scanPaths(paths, config_);
    return applyPathScan(repository, scan);
}

GitDiffRefreshResult GitDiffSource::applyFullScan(const GitDiffScan& scan) {
    if (!scan.complete) {
        return {.applied = false, .requestedRescan = true, .accepted = true};
    }

    std::set<DiffFileId> seen;
    for (auto file : scan.files) {
        auto result = diffModel_->updateGitFile(
            GitDiffFile{
                .id = file.id,
                .path = std::move(file.path),
                .previousPath = std::move(file.previousPath),
                .baselineContent = std::move(file.baselineContent),
                .workingContent = std::move(file.workingContent),
                .baselineIdentity = scan.baselineIdentity,
            },
            nextDiffRevision());
        if (!result.accepted()) {
            return {.applied = false, .requestedRescan = true, .accepted = false};
        }
        seen.insert(file.id);
        if (file.previousPath) {
            if (auto previous = pathIdentity(*file.previousPath);
                previous && *previous != file.id) {
                (void)diffModel_->removeFile(*previous, nextDiffRevision());
            }
        }
    }

    for (const auto& id : currentIds(*diffModel_)) {
        if (seen.contains(id)) {
            continue;
        }
        auto removed = diffModel_->removeFile(id, nextDiffRevision());
        if (!removed.accepted() && removed.error != DiffError::UnknownFile) {
            return {.applied = false, .requestedRescan = true, .accepted = false};
        }
    }

    baselineIdentity_ = scan.baselineIdentity;
    return {.applied = true, .requestedRescan = false, .accepted = true};
}

GitDiffRefreshResult GitDiffSource::applyPathScan(
    GitRepository& repository, const GitWorkingTreeScan& scan) {
    if (!scan.complete) {
        return {.applied = false, .requestedRescan = true, .accepted = true};
    }
    if (!baselineIdentity_.empty() &&
        baselineIdentity_ != scan.baselineIdentity) {
        return refresh(repository);
    }

    std::set<std::filesystem::path> requested;
    for (const auto& path : scan.requestedPaths) {
        requested.insert(path.lexically_normal());
    }

    for (const auto& file : scan.files) {
        if (!file.previousPath) {
            continue;
        }
        const auto current = file.path.lexically_normal();
        const auto previous = file.previousPath->lexically_normal();
        if (!requested.contains(current) || !requested.contains(previous)) {
            return refresh(repository);
        }
    }

    std::set<DiffFileId> present;
    for (auto file : scan.files) {
        auto result = diffModel_->updateGitFile(
            GitDiffFile{
                .id = file.id,
                .path = std::move(file.path),
                .previousPath = std::move(file.previousPath),
                .baselineContent = std::move(file.baselineContent),
                .workingContent = std::move(file.workingContent),
                .baselineIdentity = scan.baselineIdentity,
            },
            nextDiffRevision());
        if (!result.accepted()) {
            return {.applied = false, .requestedRescan = true, .accepted = false};
        }
        present.insert(file.id);
    }

    for (const auto& path : scan.requestedPaths) {
        auto id = pathIdentity(path);
        if (!id || present.contains(*id)) {
            continue;
        }
        auto removed = diffModel_->removeFile(*id, nextDiffRevision());
        if (!removed.accepted() && removed.error != DiffError::UnknownFile) {
            return {.applied = false, .requestedRescan = true, .accepted = false};
        }
    }

    baselineIdentity_ = scan.baselineIdentity;
    return {.applied = true, .requestedRescan = false, .accepted = true};
}

Revision GitDiffSource::nextDiffRevision() {
    auto current = nextRevision_;
    nextRevision_ = Revision{nextRevision_.value() + 1};
    return current;
}

}  // namespace ssg
