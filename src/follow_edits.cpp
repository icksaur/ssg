#include "ssg/follow_edits.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ssg {

namespace {

auto find_client(std::vector<FollowClientView>& clients, ClientId client) {
    return std::find_if(clients.begin(), clients.end(),
                        [client](const FollowClientView& view) {
                            return view.client == client;
                        });
}

auto find_client(const std::vector<FollowClientView>& clients, ClientId client) {
    return std::find_if(clients.begin(), clients.end(),
                        [client](const FollowClientView& view) {
                            return view.client == client;
                        });
}

FollowScrollOffset offset_for(std::size_t target_line,
                              const ViewportDimensions& dimensions) {
    const auto rows = static_cast<std::uint64_t>(dimensions.rows);
    const auto line = static_cast<std::uint64_t>(target_line);
    return {line >= rows ? line - rows + 1 : 0, 0};
}

}  // namespace

FollowEditsDelta derive_follow_edits_delta(const FollowEditsViewState& base,
                                           const FollowEditsViewState& target) {
    FollowEditsDelta delta{base.generation, target.generation, std::nullopt};
    if (base != target) {
        delta.replacement = target;
    }
    return delta;
}

FollowEditsCommandSet follow_edits_command_set() {
    return {};
}

FollowEditsModel::FollowEditsModel(FollowEditsConfig config)
    : config_{std::move(config)} {
    if (config_.queue_capacity == 0) {
        throw std::invalid_argument{"follow queue capacity must be positive"};
    }
}

FollowEditsResult FollowEditsModel::attach_client(
    ClientId client, ViewportDimensions dimensions) {
    if (dimensions.columns == 0 || dimensions.rows == 0) {
        return {FollowEditsError::invalid_viewport};
    }
    if (find_client(state_.clients, client) != state_.clients.end()) {
        return {FollowEditsError::duplicate_client};
    }

    FollowScrollOffset offset;
    if (state_.active_target) {
        offset = offset_for(state_.active_target->newest_hunk_line, dimensions);
    }
    state_.clients.push_back({client, dimensions, offset});
    advance_generation();
    return {};
}

FollowEditsResult FollowEditsModel::detach_client(ClientId client) {
    const auto found = find_client(state_.clients, client);
    if (found == state_.clients.end()) {
        return {FollowEditsError::unknown_client};
    }
    state_.clients.erase(found);
    advance_generation();
    return {};
}

FollowEditsResult FollowEditsModel::accept_external_change(
    const DiffFileView& file, Revision source_revision) {
    if (source_revision <= latest_source_revision_) {
        return {FollowEditsError::stale_revision};
    }

    latest_source_revision_ = source_revision;
    std::erase_if(state_.queued_targets, [&file](const FollowTarget& target) {
        return target.id == file.id;
    });

    if (!file.hunks.empty()) {
        auto target = target_for(file, source_revision);
        state_.queued_targets.push_back(target);
        if (state_.queued_targets.size() > config_.queue_capacity) {
            state_.queued_targets.erase(state_.queued_targets.begin());
        }
        if (state_.mode == FollowMode::following) {
            activate(target);
        }
    }

    advance_generation();
    return {};
}

FollowEditsResult FollowEditsModel::apply_navigation(
    const FollowNavigation& navigation) {
    const auto client = find_client(state_.clients, navigation.client);
    if (client == state_.clients.end()) {
        return {FollowEditsError::unknown_client};
    }

    if (navigation.classification == NavigationClass::user) {
        state_.mode = FollowMode::paused;
    }
    if (navigation.pane) {
        state_.active_pane = *navigation.pane;
    }
    if (navigation.offset) {
        client->offset = *navigation.offset;
    }
    advance_generation();
    return {};
}

FollowEditsResult FollowEditsModel::pause() {
    state_.mode = FollowMode::paused;
    advance_generation();
    return {};
}

FollowEditsResult FollowEditsModel::resume(const DiffViewState& current_diff) {
    if (current_diff.revision < latest_source_revision_) {
        return {FollowEditsError::stale_revision};
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
            resolved = target_for(*current, queued->source_revision);
            break;
        }
    }

    state_.mode = FollowMode::following;
    state_.queued_targets.clear();
    if (resolved) {
        activate(*resolved);
    }
    advance_generation();
    return {};
}

FollowEditsViewState FollowEditsModel::view_state() const {
    return state_;
}

FollowEditsFooterProjection FollowEditsModel::footer_projection() const {
    if (state_.mode == FollowMode::paused) {
        return {"paused", config_.resume_binding, "follow_edits.resume"};
    }
    return {"following", std::nullopt, std::nullopt};
}

FollowTarget FollowEditsModel::target_for(const DiffFileView& file,
                                          Revision source_revision) const {
    const auto opened = diff_open_file(file);
    return {file.id, opened.path, file.deleted, file.hunks.back().target_start,
            source_revision};
}

void FollowEditsModel::activate(const FollowTarget& target) {
    state_.active_target = target;
    for (auto& client : state_.clients) {
        client.offset = offset_for(target.newest_hunk_line, client.dimensions);
    }
}

void FollowEditsModel::advance_generation() noexcept {
    if (state_.generation != std::numeric_limits<std::uint64_t>::max()) {
        ++state_.generation;
    }
}

}  // namespace ssg
