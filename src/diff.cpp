#include "ssg/diff.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ssg {
namespace {

bool valid_workspace_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

struct ComputedDiff {
    std::vector<DiffHunk> hunks;
    std::vector<DiffLineChange> changes;
};

std::optional<ComputedDiff> compute_diff(std::string_view baseline,
                                         std::string_view target,
                                         const DiffConfig& config) {
    const auto old_lines = split_diff_lines(baseline);
    const auto new_lines = split_diff_lines(target);
    if (old_lines.size() > config.maximum_line_count ||
        new_lines.size() > config.maximum_line_count) {
        return std::nullopt;
    }

    const auto rows = old_lines.size() + 1;
    const auto columns = new_lines.size() + 1;
    if (rows > std::numeric_limits<std::size_t>::max() / columns ||
        rows * columns > config.maximum_matrix_cells) {
        return std::nullopt;
    }

    std::vector<std::size_t> lcs(rows * columns);
    const auto at = [&](std::size_t old_index, std::size_t new_index) -> std::size_t& {
        return lcs[old_index * columns + new_index];
    };
    for (std::size_t old_index = old_lines.size(); old_index-- > 0;) {
        for (std::size_t new_index = new_lines.size(); new_index-- > 0;) {
            at(old_index, new_index) =
                old_lines[old_index] == new_lines[new_index]
                    ? at(old_index + 1, new_index + 1) + 1
                    : std::max(at(old_index + 1, new_index),
                               at(old_index, new_index + 1));
        }
    }

    ComputedDiff result;
    std::size_t old_index = 0;
    std::size_t new_index = 0;
    std::optional<DiffHunk> pending;
    const auto flush = [&] {
        if (!pending) {
            return;
        }
        const auto paired =
            std::min(pending->baseline_lines.size(), pending->target_lines.size());
        for (std::size_t index = 0; index < paired; ++index) {
            result.changes.push_back(
                {DiffLineKind::Modified, pending->baseline_start + index,
                 pending->target_start + index});
        }
        for (std::size_t index = paired; index < pending->baseline_lines.size();
             ++index) {
            result.changes.push_back(
                {DiffLineKind::Removed, pending->baseline_start + index,
                 std::nullopt});
        }
        for (std::size_t index = paired; index < pending->target_lines.size();
             ++index) {
            result.changes.push_back(
                {DiffLineKind::Added, std::nullopt,
                 pending->target_start + index});
        }
        result.hunks.push_back(std::move(*pending));
        pending.reset();
    };

    while (old_index < old_lines.size() || new_index < new_lines.size()) {
        if (old_index < old_lines.size() && new_index < new_lines.size() &&
            old_lines[old_index] == new_lines[new_index]) {
            flush();
            ++old_index;
            ++new_index;
            continue;
        }
        if (!pending) {
            pending = DiffHunk{.baseline_start = old_index,
                               .target_start = new_index};
        }
        if (old_index < old_lines.size() &&
            (new_index == new_lines.size() ||
             at(old_index + 1, new_index) >= at(old_index, new_index + 1))) {
            pending->baseline_lines.push_back(old_lines[old_index++]);
        } else {
            pending->target_lines.push_back(new_lines[new_index++]);
        }
    }
    flush();
    return result;
}

template <typename Entries>
auto find_entry(Entries& entries, const DiffFileId& id) {
    return std::find_if(entries.begin(), entries.end(),
                        [&](const auto& entry) { return entry.view.id == id; });
}

bool contains_id(const std::vector<DiffFileId>& ids, const DiffFileId& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

} // namespace

DiffFileId::DiffFileId(std::string value) : value_(std::move(value)) {
    if (value_.empty()) {
        throw std::invalid_argument("diff file identity must not be empty");
    }
}

std::vector<std::string> split_diff_lines(std::string_view content) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < content.size()) {
        const auto newline = content.find('\n', start);
        const auto end = newline == std::string_view::npos ? content.size()
                                                           : newline + 1;
        lines.emplace_back(content.substr(start, end - start));
        start = end;
    }
    return lines;
}

DiffModel::DiffModel(DiffConfig config) : config_(config) {
    if (config_.maximum_line_count == 0 ||
        config_.maximum_matrix_cells == 0) {
        throw std::invalid_argument("diff work limits must be greater than zero");
    }
}

DiffMutationResult DiffModel::update_git_file(GitDiffFile file,
                                               Revision revision) {
    if (revision <= revision_) {
        return {DiffError::StaleRevision};
    }
    if (!valid_workspace_path(file.path) ||
        (file.previous_path && !valid_workspace_path(*file.previous_path))) {
        return {DiffError::InvalidPath};
    }
    if (file.index_identity.empty()) {
        return {DiffError::BaselineIdentityRequired};
    }
    const auto existing = find_entry(entries_, file.id);
    if (existing != entries_.end() && existing->source != Source::Git) {
        return {DiffError::DuplicateFile};
    }

    const std::string baseline = file.index_content.value_or("");
    const std::string target = file.working_content.value_or("");
    auto computed = compute_diff(baseline, target, config_);
    if (!computed) {
        return {DiffError::WorkLimitExceeded};
    }

    DiffFileView view{.id = file.id,
                      .path = std::move(file.path),
                      .previous_path = std::move(file.previous_path),
                      .deleted = !file.working_content.has_value(),
                      .baseline_identity = std::move(file.index_identity),
                      .current_content = target,
                      .hunks = std::move(computed->hunks),
                      .changed_lines = std::move(computed->changes)};
    if (existing == entries_.end()) {
        entries_.push_back({std::move(view), Source::Git, {}});
    } else {
        existing->view = std::move(view);
        existing->source = Source::Git;
        existing->acknowledged_content.clear();
    }
    revision_ = revision;
    return {};
}

DiffMutationResult DiffModel::seed_non_git(std::vector<SeededDiffFile> files,
                                           Revision revision) {
    if (revision <= revision_) {
        return {DiffError::StaleRevision};
    }

    std::vector<Entry> seeded;
    seeded.reserve(files.size());
    for (auto& file : files) {
        if (!valid_workspace_path(file.path)) {
            return {DiffError::InvalidPath};
        }
        if (find_entry(entries_, file.id) != entries_.end() ||
            find_entry(seeded, file.id) != seeded.end()) {
            return {DiffError::DuplicateFile};
        }
        if (split_diff_lines(file.content).size() > config_.maximum_line_count) {
            return {DiffError::WorkLimitExceeded};
        }
        seeded.push_back(
            {DiffFileView{.id = file.id,
                          .path = std::move(file.path),
                          .current_content = file.content},
             Source::NonGit, std::move(file.content)});
    }

    entries_.insert(entries_.end(),
                    std::make_move_iterator(seeded.begin()),
                    std::make_move_iterator(seeded.end()));
    revision_ = revision;
    return {};
}

DiffMutationResult DiffModel::apply_non_git_event(NonGitDiffEvent event,
                                                  Revision revision) {
    if (revision <= revision_) {
        return {DiffError::StaleRevision};
    }
    if (!valid_workspace_path(event.path) ||
        (event.previous_path && !valid_workspace_path(*event.previous_path))) {
        return {DiffError::InvalidPath};
    }
    const bool removed = event.kind == NonGitDiffEventKind::Remove;
    if (removed && event.content) {
        return {DiffError::ContentForbidden};
    }
    if (!removed && !event.content) {
        return {DiffError::ContentRequired};
    }

    auto existing = find_entry(entries_, event.id);
    if (existing != entries_.end() && existing->source != Source::NonGit) {
        return {DiffError::DuplicateFile};
    }
    if (existing == entries_.end()) {
        if (event.kind != NonGitDiffEventKind::Create) {
            return {DiffError::UnknownFile};
        }
        const std::string target = *event.content;
        auto computed = compute_diff("", target, config_);
        if (!computed) {
            return {DiffError::WorkLimitExceeded};
        }
        entries_.push_back(
            {DiffFileView{.id = event.id,
                          .path = std::move(event.path),
                          .previous_path = std::move(event.previous_path),
                          .current_content = target,
                          .hunks = std::move(computed->hunks),
                          .changed_lines = std::move(computed->changes)},
             Source::NonGit, target});
        revision_ = revision;
        return {};
    }

    const std::string target = event.content.value_or("");
    auto computed =
        compute_diff(existing->acknowledged_content, target, config_);
    if (!computed) {
        return {DiffError::WorkLimitExceeded};
    }
    existing->view.path = std::move(event.path);
    existing->view.previous_path = std::move(event.previous_path);
    existing->view.deleted = removed;
    existing->view.baseline_identity.clear();
    existing->view.current_content = target;
    existing->view.hunks = std::move(computed->hunks);
    existing->view.changed_lines = std::move(computed->changes);
    existing->acknowledged_content = target;
    revision_ = revision;
    return {};
}

DiffViewState DiffModel::view_state() const {
    DiffViewState result{.revision = revision_};
    result.files.reserve(entries_.size());
    for (const auto& entry : entries_) {
        result.files.push_back(entry.view);
    }
    std::sort(result.files.begin(), result.files.end(),
              [](const auto& left, const auto& right) {
                  return left.id < right.id;
              });
    return result;
}

std::optional<std::reference_wrapper<const DiffFileView>>
DiffModel::file(const DiffFileId& id) const {
    const auto found = find_entry(entries_, id);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    return std::cref(found->view);
}

DiffCommandSet diff_command_set() {
    return {};
}

std::optional<std::size_t> next_diff_hunk(
    const DiffFileView& file, std::optional<std::size_t> current_target_line) {
    if (file.hunks.empty()) {
        return std::nullopt;
    }
    if (!current_target_line) {
        return 0;
    }
    const auto found = std::find_if(
        file.hunks.begin(), file.hunks.end(), [&](const auto& hunk) {
            return hunk.target_start > *current_target_line;
        });
    return found == file.hunks.end()
               ? std::optional<std::size_t>{0}
               : std::optional<std::size_t>{
                     static_cast<std::size_t>(found - file.hunks.begin())};
}

std::optional<std::size_t> previous_diff_hunk(
    const DiffFileView& file, std::optional<std::size_t> current_target_line) {
    if (file.hunks.empty()) {
        return std::nullopt;
    }
    if (!current_target_line) {
        return file.hunks.size() - 1;
    }
    for (std::size_t index = file.hunks.size(); index-- > 0;) {
        if (file.hunks[index].target_start < *current_target_line) {
            return index;
        }
    }
    return file.hunks.size() - 1;
}

DiffOpenTarget diff_open_file(const DiffFileView& file) {
    return {file.id,
            file.deleted && file.previous_path ? *file.previous_path : file.path,
            file.deleted};
}

DiffDelta derive_diff_delta(const DiffViewState& base,
                            const DiffViewState& target) {
    DiffDelta delta{.base_revision = base.revision,
                    .revision = target.revision};
    for (const auto& target_file : target.files) {
        const auto base_file = std::find_if(
            base.files.begin(), base.files.end(),
            [&](const auto& candidate) { return candidate.id == target_file.id; });
        if (base_file == base.files.end() || *base_file != target_file) {
            delta.upserted.push_back(target_file);
        }
    }
    for (const auto& base_file : base.files) {
        const auto target_file = std::find_if(
            target.files.begin(), target.files.end(),
            [&](const auto& candidate) { return candidate.id == base_file.id; });
        if (target_file == target.files.end()) {
            delta.removed.push_back(base_file.id);
        }
    }
    return delta;
}

DiffReplayResult replay_diff_delta(const DiffViewState& base,
                                   const DiffDelta& delta) {
    if (base.revision != delta.base_revision) {
        return {std::nullopt, DiffReplayError::StaleRevision};
    }
    DiffViewState result = base;
    for (const auto& id : delta.removed) {
        if (contains_id(delta.removed, id) &&
            std::count(delta.removed.begin(), delta.removed.end(), id) != 1) {
            return {std::nullopt, DiffReplayError::MalformedDelta};
        }
        result.files.erase(
            std::remove_if(result.files.begin(), result.files.end(),
                           [&](const auto& file) { return file.id == id; }),
            result.files.end());
    }
    std::vector<DiffFileId> upserted_ids;
    for (const auto& file : delta.upserted) {
        if (contains_id(upserted_ids, file.id) ||
            contains_id(delta.removed, file.id)) {
            return {std::nullopt, DiffReplayError::MalformedDelta};
        }
        upserted_ids.push_back(file.id);
        const auto existing = std::find_if(
            result.files.begin(), result.files.end(),
            [&](const auto& candidate) { return candidate.id == file.id; });
        if (existing == result.files.end()) {
            result.files.push_back(file);
        } else {
            *existing = file;
        }
    }
    std::sort(result.files.begin(), result.files.end(),
              [](const auto& left, const auto& right) {
                  return left.id < right.id;
              });
    result.revision = delta.revision;
    return {std::move(result), DiffReplayError::None};
}

} // namespace ssg
