#include "ssg/FollowEditsModel.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ssg {

namespace {

bool sameRange(std::size_t leftStart, std::size_t leftSize,
               std::size_t rightStart, std::size_t rightSize) {
    return leftStart == rightStart && leftSize == rightSize;
}

bool correspondsTo(const DiffHunk& incoming, const DiffHunk& prior) {
    if (sameRange(incoming.baselineStart, incoming.baselineLines.size(),
                  prior.baselineStart, prior.baselineLines.size())) {
        return true;
    }
    return sameRange(incoming.targetStart, incoming.targetLines.size(),
                     prior.targetStart, prior.targetLines.size());
}

const DiffHunk* newestIntroducedHunk(
    const DiffFileView& file, const std::vector<DiffHunk>& priorHunks) {
    const DiffHunk* newest = nullptr;
    for (const auto& incoming : file.hunks) {
        const auto prior = std::find_if(
            priorHunks.begin(), priorHunks.end(), [&](const DiffHunk& candidate) {
                return correspondsTo(incoming, candidate);
            });
        if (prior == priorHunks.end()) {
            newest = &incoming;
        }
    }
    return newest;
}

}  // namespace

FollowEditsModel::FollowEditsModel(FollowEditsConfig config)
    : config_{std::move(config)} {
    if (config_.queueCapacity == 0) {
        throw std::invalid_argument{"follow queue capacity must be positive"};
    }
}

FollowEditsResult FollowEditsModel::acceptExternalChange(
    const DiffFileView& file, std::uint64_t sourceRevision) {
    return acceptExternalChanges({FollowDiffChange{file, {}, sourceRevision}});
}

FollowEditsResult FollowEditsModel::acceptExternalChanges(
    std::vector<FollowDiffChange> changes) {
    auto expectedRevision = latestSourceRevision_;
    for (const auto& change : changes) {
        if (change.sourceRevision <= expectedRevision) {
            return {FollowEditsError::StaleRevision};
        }
        expectedRevision = change.sourceRevision;
    }

    std::optional<std::pair<FollowTarget, DiffFileView>> activation;
    for (const auto& change : changes) {
        activation.reset();
        latestSourceRevision_ = change.sourceRevision;
        std::erase_if(state_.queuedTargets,
                      [&change](const FollowTarget& target) {
                          return target.id == change.file.id;
                      });
        const auto* hunk =
            newestIntroducedHunk(change.file, change.priorHunks);
        if (hunk == nullptr) {
            continue;
        }
        auto target = targetFor(change.file, *hunk, change.sourceRevision);
        state_.queuedTargets.push_back(target);
        if (state_.queuedTargets.size() > config_.queueCapacity) {
            state_.queuedTargets.erase(state_.queuedTargets.begin());
        }
        activation = std::pair{std::move(target), change.file};
    }

    if (activation && state_.mode == FollowMode::Following) {
        activate(activation->first);
    }
    if (!changes.empty()) {
        advanceGeneration();
    }
    return {};
}

FollowEditsResult FollowEditsModel::applyNavigation(
    const FollowNavigation& navigation) {
    if (navigation.classification == NavigationClass::User) {
        state_.mode = FollowMode::Paused;
    }
    advanceGeneration();
    return {};
}

FollowEditsResult FollowEditsModel::notifyLocalEdit() {
    if (state_.mode != FollowMode::Following) {
        return {};
    }
    state_.mode = FollowMode::Paused;
    advanceGeneration();
    return {};
}

FollowEditsResult FollowEditsModel::pause() {
    state_.mode = FollowMode::Paused;
    advanceGeneration();
    return {};
}

FollowEditsResult FollowEditsModel::resume(const DiffViewState& currentDiff) {
    if (currentDiff.revision < latestSourceRevision_) {
        return {FollowEditsError::StaleRevision};
    }

    std::optional<FollowTarget> resolved;
    std::optional<DiffFileView> resolvedFile;
    for (auto queued = state_.queuedTargets.rbegin();
         queued != state_.queuedTargets.rend(); ++queued) {
        const auto current =
            std::find_if(currentDiff.files.begin(), currentDiff.files.end(),
                         [&queued](const DiffFileView& file) {
                             return file.id == queued->id && !file.hunks.empty();
                         });
        if (current != currentDiff.files.end()) {
            const auto opened = diffOpenFile(*current);
            resolved = *queued;
            resolved->path = opened.path;
            resolved->deleted = opened.deleted;
            resolvedFile = *current;
            break;
        }
    }

    state_.mode = FollowMode::Following;
    state_.queuedTargets.clear();
    if (resolved && resolvedFile) {
        activate(*resolved);
    }
    advanceGeneration();
    return {};
}

FollowEditsResult FollowEditsModel::toggle(const DiffViewState& currentDiff) {
    if (state_.mode == FollowMode::Following) {
        return pause();
    }
    return resume(currentDiff);
}

FollowEditsViewState FollowEditsModel::viewState() const {
    return state_;
}

FollowEditsFooterProjection FollowEditsModel::footerProjection() const {
    if (state_.mode == FollowMode::Paused) {
        return {"paused", config_.resumeBinding, "follow_edits.toggle"};
    }
    return {"following", std::nullopt, "follow_edits.toggle"};
}

FollowTarget FollowEditsModel::targetFor(const DiffFileView& file,
                                          const DiffHunk& hunk,
                                          std::uint64_t sourceRevision) const {
    const auto opened = diffOpenFile(file);
    return {file.id, opened.path, file.deleted, hunk.targetStart, sourceRevision};
}

void FollowEditsModel::activate(const FollowTarget& target) {
    state_.activeTarget = target;
}

void FollowEditsModel::advanceGeneration() noexcept {
    if (state_.generation != std::numeric_limits<std::uint64_t>::max()) {
        ++state_.generation;
    }
}

}  // namespace ssg
