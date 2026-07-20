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

FollowScrollOffset offsetFor(std::size_t target_line,
                              const ViewportDimensions& dimensions) {
    const auto rows = static_cast<std::uint64_t>(dimensions.rows);
    const auto line = static_cast<std::uint64_t>(target_line);
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
    if (config_.queue_capacity == 0) {
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
    if (state_.active_target) {
        offset = offsetFor(state_.active_target->newest_hunk_line, dimensions);
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
    const DiffFileView& file, Revision source_revision) {
    if (source_revision <= latest_source_revision_) {
        return {FollowEditsError::StaleRevision};
    }

    latest_source_revision_ = source_revision;
    std::erase_if(state_.queued_targets, [&file](const FollowTarget& target) {
        return target.id == file.id;
    });

    if (!file.hunks.empty()) {
        auto target = targetFor(file, source_revision);
        state_.queued_targets.push_back(target);
        if (state_.queued_targets.size() > config_.queue_capacity) {
            state_.queued_targets.erase(state_.queued_targets.begin());
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
        state_.active_pane = *navigation.pane;
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

FollowEditsResult FollowEditsModel::resume(const DiffViewState& current_diff) {
    if (current_diff.revision < latest_source_revision_) {
        return {FollowEditsError::StaleRevision};
    }

    std::optional<FollowTarget> resolved;
    for (auto queued = state_.queued_targets.rbegin();
         queued != state_.queued_targets.rend(); ++queued) {
        const auto current =
            std::find_if(current_diff.files.begin(), current_diff.files.end(),
                         [&queued](const DiffFileView& file) {
                             return file.id == queued->id && !file.hunks.empty();
                         });
        if (current != current_diff.files.end()) {
            resolved = targetFor(*current, queued->source_revision);
            break;
        }
    }

    state_.mode = FollowMode::Following;
    state_.queued_targets.clear();
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
        return {"paused", config_.resume_binding, "follow_edits.resume"};
    }
    return {"following", std::nullopt, std::nullopt};
}

FollowTarget FollowEditsModel::targetFor(const DiffFileView& file,
                                          Revision source_revision) const {
    const auto opened = diffOpenFile(file);
    return {file.id, opened.path, file.deleted, file.hunks.back().target_start,
            source_revision};
}

void FollowEditsModel::activate(const FollowTarget& target) {
    state_.active_target = target;
    for (auto& client : state_.clients) {
        client.offset = offsetFor(target.newest_hunk_line, client.dimensions);
    }
}

void FollowEditsModel::advanceGeneration() noexcept {
    if (state_.generation != std::numeric_limits<std::uint64_t>::max()) {
        ++state_.generation;
    }
}

}  // namespace ssg
