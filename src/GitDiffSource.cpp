#include "ssg/GitDiffSource.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string_view>

namespace ssg {

GitDiffMode resolveGitDiffMode(const char* envValue,
                               bool watcherAvailable) noexcept {
    if (envValue != nullptr) {
        if (std::string_view{envValue} == "event") return GitDiffMode::Event;
        if (std::string_view{envValue} == "poll") return GitDiffMode::Poll;
    }
    return watcherAvailable ? GitDiffMode::Event : GitDiffMode::Poll;
}

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

GitDiffScan buildPublishedScan(const std::map<DiffFileId, GitDiffFile>& files,
                               const std::string& baselineIdentity,
                               const std::optional<std::string>& currentBranch,
                               Revision publishedRevision) {
    GitDiffScan published{
        .revision = publishedRevision,
        .baselineIdentity = baselineIdentity,
        .currentBranch = currentBranch,
    };
    published.files.reserve(files.size());
    for (const auto& [id, file] : files) {
        (void)id;
        published.files.push_back(file);
    }
    return published;
}

}  // namespace

GitDiffSource::GitDiffSource(DiffModel& diffModel, GitDiffConfig config)
    : diffModel_{&diffModel},
      config_{std::move(config)},
      nextRevision_{Revision{diffModel.viewState().revision.value() + 1}} {}

GitDiffRefreshResult GitDiffSource::refresh(GitRepository& repository) {
    currentBranch_ = repository.currentBranch();
    auto scan = repository.scanDiff(config_);
    return applyFullScan(scan);
}

GitDiffRefreshResult GitDiffSource::refreshPaths(
    GitRepository& repository, const std::vector<std::filesystem::path>& paths) {
    currentBranch_ = repository.currentBranch();
    auto scan = repository.scanPaths(paths, config_);
    return applyPathScan(repository, scan);
}

GitDiffRefreshResult GitDiffSource::applyFullScan(const GitDiffScan& scan) {
    if (!scan.complete) {
        return {.applied = false, .requestedRescan = true, .accepted = true};
    }

    DiffModel stagedDiff = *diffModel_;
    auto stagedFiles = currentFiles_;
    Revision stagedNextRevision = nextRevision_;
    const auto nextMutationRevision = [&stagedNextRevision]() {
        auto current = stagedNextRevision;
        stagedNextRevision = Revision{stagedNextRevision.value() + 1};
        return current;
    };

    std::set<DiffFileId> seen;
    for (const auto& file : scan.files) {
        seen.insert(file.id);
        stagedFiles.insert_or_assign(file.id, file);
        auto result = stagedDiff.updateGitFile(
            file, scan.baselineIdentity, nextMutationRevision());
        if (!result.accepted()) {
            if (result.error == DiffError::WorkLimitExceeded) {
                auto removed =
                    stagedDiff.removeFile(file.id, nextMutationRevision());
                if (!removed.accepted() &&
                    removed.error != DiffError::UnknownFile) {
                    return {.applied = false,
                            .requestedRescan = true,
                            .accepted = false};
                }
            } else {
                return {.applied = false,
                        .requestedRescan = true,
                        .accepted = false};
            }
        }
        if (file.previousPath) {
            if (auto previous = pathIdentity(*file.previousPath);
                previous && *previous != file.id) {
                auto removed =
                    stagedDiff.removeFile(*previous, nextMutationRevision());
                if (!removed.accepted() &&
                    removed.error != DiffError::UnknownFile) {
                    return {.applied = false,
                            .requestedRescan = true,
                            .accepted = false};
                }
                stagedFiles.erase(*previous);
            }
        }
    }

    for (const auto& id : currentIds(stagedDiff)) {
        if (seen.contains(id)) {
            continue;
        }
        auto removed = stagedDiff.removeFile(id, nextMutationRevision());
        if (!removed.accepted() && removed.error != DiffError::UnknownFile) {
            return {.applied = false, .requestedRescan = true, .accepted = false};
        }
    }
    for (auto it = stagedFiles.begin(); it != stagedFiles.end();) {
        if (seen.contains(it->first)) {
            ++it;
            continue;
        }
        it = stagedFiles.erase(it);
    }

    *diffModel_ = std::move(stagedDiff);
    nextRevision_ = stagedNextRevision;
    currentFiles_ = std::move(stagedFiles);
    baselineIdentity_ = scan.baselineIdentity;
    auto publishedRevision = publishedRevision_;
    publishedRevision_ = Revision{publishedRevision_.value() + 1};
    latestAppliedScan_ = buildPublishedScan(currentFiles_, baselineIdentity_,
                                            currentBranch_, publishedRevision);
    publishedBranch_ = currentBranch_;
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

    DiffModel stagedDiff = *diffModel_;
    auto stagedFiles = currentFiles_;
    Revision stagedNextRevision = nextRevision_;
    const auto nextMutationRevision = [&stagedNextRevision]() {
        auto current = stagedNextRevision;
        stagedNextRevision = Revision{stagedNextRevision.value() + 1};
        return current;
    };

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
    for (const auto& file : scan.files) {
        present.insert(file.id);
        stagedFiles.insert_or_assign(file.id, file);
        auto result = stagedDiff.updateGitFile(
            file, scan.baselineIdentity, nextMutationRevision());
        if (!result.accepted()) {
            if (result.error == DiffError::WorkLimitExceeded) {
                auto removed =
                    stagedDiff.removeFile(file.id, nextMutationRevision());
                if (!removed.accepted() &&
                    removed.error != DiffError::UnknownFile) {
                    return {.applied = false,
                            .requestedRescan = true,
                            .accepted = false};
                }
            } else {
                return {.applied = false,
                        .requestedRescan = true,
                        .accepted = false};
            }
        }
        if (file.previousPath) {
            if (auto previous = pathIdentity(*file.previousPath);
                previous && *previous != file.id) {
                auto removed =
                    stagedDiff.removeFile(*previous, nextMutationRevision());
                if (!removed.accepted() &&
                    removed.error != DiffError::UnknownFile) {
                    return {.applied = false,
                            .requestedRescan = true,
                            .accepted = false};
                }
                stagedFiles.erase(*previous);
            }
        }
    }

    for (const auto& path : scan.requestedPaths) {
        auto id = pathIdentity(path);
        if (!id || present.contains(*id)) {
            continue;
        }
        auto removed = stagedDiff.removeFile(*id, nextMutationRevision());
        if (!removed.accepted() && removed.error != DiffError::UnknownFile) {
            return {.applied = false, .requestedRescan = true, .accepted = false};
        }
        stagedFiles.erase(*id);
    }

    *diffModel_ = std::move(stagedDiff);
    nextRevision_ = stagedNextRevision;
    currentFiles_ = std::move(stagedFiles);
    baselineIdentity_ = scan.baselineIdentity;
    auto publishedRevision = publishedRevision_;
    publishedRevision_ = Revision{publishedRevision_.value() + 1};
    latestAppliedScan_ = buildPublishedScan(currentFiles_, baselineIdentity_,
                                            currentBranch_, publishedRevision);
    publishedBranch_ = currentBranch_;
    return {.applied = true, .requestedRescan = false, .accepted = true};
}

std::optional<GitDiffScan> GitDiffSource::takeBranchOnlyScanIfChanged() {
    if (currentBranch_ == publishedBranch_) {
        return std::nullopt;
    }
    publishedBranch_ = currentBranch_;
    return GitDiffScan{.revision = Revision{0}, .currentBranch = currentBranch_};
}

std::optional<GitDiffScan> GitDiffSource::latestAppliedScan() const {
    return latestAppliedScan_;
}

}  // namespace ssg
