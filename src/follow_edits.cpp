#include "ssg/follow_edits.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ssg {

namespace {

auto findClient(std::vector<FollowClientView>& clients, ClientId client) {
    return std::find_if(clients.begin(), clients.end(),
                        [client](const FollowClientView& view) {
                            return view.client == client;
                        });
}

auto findClient(const std::vector<FollowClientView>& clients, ClientId client) {
    return std::find_if(clients.begin(), clients.end(),
                        [client](const FollowClientView& view) {
                            return view.client == client;
                        });
}

FollowScrollOffset offsetFor(std::size_t targetLine,
                              const ViewportDimensions& dimensions) {
    const auto rows = static_cast<std::uint64_t>(dimensions.rows);
    const auto line = static_cast<std::uint64_t>(targetLine);
    return {line >= rows ? line - rows + 1 : 0, 0};
}

}  // namespace

FollowEditsDelta deriveFollowEditsDelta(const FollowEditsViewState& base,
                                           const FollowEditsViewState& target) {
    FollowEditsDelta delta{base.generation, target.generation, std::nullopt};
    if (base != target) {
        delta.replacement = target;
    }
    return delta;
}

FollowEditsCommandSet followEditsCommandSet() {
    return {};
}

FollowEditsModel::FollowEditsModel(FollowEditsConfig config)
    : config_{std::move(config)} {
    if (config_.queueCapacity == 0) {
        throw std::invalid_argument{"follow queue capacity must be positive"};
    }
}

FollowEditsResult FollowEditsModel::attachClient(
    ClientId client, ViewportDimensions dimensions) {
    if (dimensions.columns == 0 || dimensions.rows == 0) {
        return {FollowEditsError::InvalidViewport};
    }
    if (findClient(state_.clients, client) != state_.clients.end()) {
        return {FollowEditsError::DuplicateClient};
    }

    FollowScrollOffset offset;
    if (state_.activeTarget) {
        offset = offsetFor(state_.activeTarget->newestHunkLine, dimensions);
    }
    state_.clients.push_back({client, dimensions, offset});
    advanceGeneration();
    return {};
}

FollowEditsResult FollowEditsModel::detachClient(ClientId client) {
    const auto found = findClient(state_.clients, client);
    if (found == state_.clients.end()) {
        return {FollowEditsError::UnknownClient};
    }
    state_.clients.erase(found);
    advanceGeneration();
    return {};
}

FollowEditsResult FollowEditsModel::acceptExternalChange(
    const DiffFileView& file, Revision sourceRevision) {
    if (sourceRevision <= latestSourceRevision_) {
        return {FollowEditsError::StaleRevision};
    }

    latestSourceRevision_ = sourceRevision;
    std::erase_if(state_.queuedTargets, [&file](const FollowTarget& target) {
        return target.id == file.id;
    });

    if (!file.hunks.empty()) {
        auto target = targetFor(file, sourceRevision);
        state_.queuedTargets.push_back(target);
        if (state_.queuedTargets.size() > config_.queueCapacity) {
            state_.queuedTargets.erase(state_.queuedTargets.begin());
        }
        if (state_.mode == FollowMode::Following) {
            activate(target);
        }
    }

    advanceGeneration();
    return {};
}

FollowEditsResult FollowEditsModel::applyNavigation(
    const FollowNavigation& navigation) {
    const auto client = findClient(state_.clients, navigation.client);
    if (client == state_.clients.end()) {
        return {FollowEditsError::UnknownClient};
    }

    if (navigation.classification == NavigationClass::User) {
        state_.mode = FollowMode::Paused;
    }
    if (navigation.pane) {
        state_.activePane = *navigation.pane;
    }
    if (navigation.offset) {
        client->offset = *navigation.offset;
    }
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
    for (auto queued = state_.queuedTargets.rbegin();
         queued != state_.queuedTargets.rend(); ++queued) {
        const auto current =
            std::find_if(currentDiff.files.begin(), currentDiff.files.end(),
                         [&queued](const DiffFileView& file) {
                             return file.id == queued->id && !file.hunks.empty();
                         });
        if (current != currentDiff.files.end()) {
            resolved = targetFor(*current, queued->sourceRevision);
            break;
        }
    }

    state_.mode = FollowMode::Following;
    state_.queuedTargets.clear();
    if (resolved) {
        activate(*resolved);
    }
    advanceGeneration();
    return {};
}

FollowEditsViewState FollowEditsModel::viewState() const {
    return state_;
}

FollowEditsFooterProjection FollowEditsModel::footerProjection() const {
    if (state_.mode == FollowMode::Paused) {
        return {"paused", config_.resumeBinding, "follow_edits.resume"};
    }
    return {"following", std::nullopt, std::nullopt};
}

FollowTarget FollowEditsModel::targetFor(const DiffFileView& file,
                                          Revision sourceRevision) const {
    const auto opened = diffOpenFile(file);
    return {file.id, opened.path, file.deleted, file.hunks.back().targetStart,
            sourceRevision};
}

void FollowEditsModel::activate(const FollowTarget& target) {
    state_.activeTarget = target;
    for (auto& client : state_.clients) {
        client.offset = offsetFor(target.newestHunkLine, client.dimensions);
    }
}

void FollowEditsModel::advanceGeneration() noexcept {
    if (state_.generation != std::numeric_limits<std::uint64_t>::max()) {
        ++state_.generation;
    }
}

}  // namespace ssg
