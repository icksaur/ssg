#include "ssg/diff.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ssg {
namespace {

bool validWorkspacePath(const std::filesystem::path& path) {
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

std::optional<ComputedDiff> computeDiff(std::string_view baseline,
                                         std::string_view target,
                                         const DiffConfig& config) {
    const auto oldLines = splitDiffLines(baseline);
    const auto newLines = splitDiffLines(target);
    if (oldLines.size() > config.maximum_line_count ||
        newLines.size() > config.maximum_line_count) {
        return std::nullopt;
    }

    const auto rows = oldLines.size() + 1;
    const auto columns = newLines.size() + 1;
    if (rows > std::numeric_limits<std::size_t>::max() / columns ||
        rows * columns > config.maximum_matrix_cells) {
        return std::nullopt;
    }

    std::vector<std::size_t> lcs(rows * columns);
    const auto at = [&](std::size_t oldIndex, std::size_t newIndex) -> std::size_t& {
        return lcs[oldIndex * columns + newIndex];
    };
    for (std::size_t oldIndex = oldLines.size(); oldIndex-- > 0;) {
        for (std::size_t newIndex = newLines.size(); newIndex-- > 0;) {
            at(oldIndex, newIndex) =
                oldLines[oldIndex] == newLines[newIndex]
                    ? at(oldIndex + 1, newIndex + 1) + 1
                    : std::max(at(oldIndex + 1, newIndex),
                               at(oldIndex, newIndex + 1));
        }
    }

    ComputedDiff result;
    std::size_t oldIndex = 0;
    std::size_t newIndex = 0;
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

    while (oldIndex < oldLines.size() || newIndex < newLines.size()) {
        if (oldIndex < oldLines.size() && newIndex < newLines.size() &&
            oldLines[oldIndex] == newLines[newIndex]) {
            flush();
            ++oldIndex;
            ++newIndex;
            continue;
        }
        if (!pending) {
            pending = DiffHunk{.baseline_start = oldIndex,
                               .target_start = newIndex};
        }
        if (oldIndex < oldLines.size() &&
            (newIndex == newLines.size() ||
             at(oldIndex + 1, newIndex) >= at(oldIndex, newIndex + 1))) {
            pending->baseline_lines.push_back(oldLines[oldIndex++]);
        } else {
            pending->target_lines.push_back(newLines[newIndex++]);
        }
    }
    flush();
    return result;
}

template <typename Entries>
auto findEntry(Entries& entries, const DiffFileId& id) {
    return std::find_if(entries.begin(), entries.end(),
                        [&](const auto& entry) { return entry.view.id == id; });
}

bool containsId(const std::vector<DiffFileId>& ids, const DiffFileId& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

} // namespace

DiffFileId::DiffFileId(std::string value) : value_(std::move(value)) {
    if (value_.empty()) {
        throw std::invalid_argument("diff file identity must not be empty");
    }
}

std::vector<std::string> splitDiffLines(std::string_view content) {
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

DiffMutationResult DiffModel::updateGitFile(GitDiffFile file,
                                               Revision revision) {
    if (revision <= revision_) {
        return {DiffError::StaleRevision};
    }
    if (!validWorkspacePath(file.path) ||
        (file.previous_path && !validWorkspacePath(*file.previous_path))) {
        return {DiffError::InvalidPath};
    }
    if (file.index_identity.empty()) {
        return {DiffError::BaselineIdentityRequired};
    }
    const auto existing = findEntry(entries_, file.id);
    if (existing != entries_.end() && existing->source != Source::Git) {
        return {DiffError::DuplicateFile};
    }

    const std::string baseline = file.index_content.value_or("");
    const std::string target = file.working_content.value_or("");
    auto computed = computeDiff(baseline, target, config_);
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

DiffMutationResult DiffModel::seedNonGit(std::vector<SeededDiffFile> files,
                                           Revision revision) {
    if (revision <= revision_) {
        return {DiffError::StaleRevision};
    }

    std::vector<Entry> seeded;
    seeded.reserve(files.size());
    for (auto& file : files) {
        if (!validWorkspacePath(file.path)) {
            return {DiffError::InvalidPath};
        }
        if (findEntry(entries_, file.id) != entries_.end() ||
            findEntry(seeded, file.id) != seeded.end()) {
            return {DiffError::DuplicateFile};
        }
        if (splitDiffLines(file.content).size() > config_.maximum_line_count) {
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

DiffMutationResult DiffModel::applyNonGitEvent(NonGitDiffEvent event,
                                                  Revision revision) {
    if (revision <= revision_) {
        return {DiffError::StaleRevision};
    }
    if (!validWorkspacePath(event.path) ||
        (event.previous_path && !validWorkspacePath(*event.previous_path))) {
        return {DiffError::InvalidPath};
    }
    const bool removed = event.kind == NonGitDiffEventKind::Remove;
    if (removed && event.content) {
        return {DiffError::ContentForbidden};
    }
    if (!removed && !event.content) {
        return {DiffError::ContentRequired};
    }

    auto existing = findEntry(entries_, event.id);
    if (existing != entries_.end() && existing->source != Source::NonGit) {
        return {DiffError::DuplicateFile};
    }
    if (existing == entries_.end()) {
        if (event.kind != NonGitDiffEventKind::Create) {
            return {DiffError::UnknownFile};
        }
        const std::string target = *event.content;
        auto computed = computeDiff("", target, config_);
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
        computeDiff(existing->acknowledged_content, target, config_);
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

DiffViewState DiffModel::viewState() const {
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
    const auto found = findEntry(entries_, id);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    return std::cref(found->view);
}

DiffCommandSet diffCommandSet() {
    return {};
}

std::optional<std::size_t> nextDiffHunk(
    const DiffFileView& file, std::optional<std::size_t> currentTargetLine) {
    if (file.hunks.empty()) {
        return std::nullopt;
    }
    if (!currentTargetLine) {
        return 0;
    }
    const auto found = std::find_if(
        file.hunks.begin(), file.hunks.end(), [&](const auto& hunk) {
            return hunk.target_start > *currentTargetLine;
        });
    return found == file.hunks.end()
               ? std::optional<std::size_t>{0}
               : std::optional<std::size_t>{
                     static_cast<std::size_t>(found - file.hunks.begin())};
}

std::optional<std::size_t> previousDiffHunk(
    const DiffFileView& file, std::optional<std::size_t> currentTargetLine) {
    if (file.hunks.empty()) {
        return std::nullopt;
    }
    if (!currentTargetLine) {
        return file.hunks.size() - 1;
    }
    for (std::size_t index = file.hunks.size(); index-- > 0;) {
        if (file.hunks[index].target_start < *currentTargetLine) {
            return index;
        }
    }
    return file.hunks.size() - 1;
}

DiffOpenTarget diffOpenFile(const DiffFileView& file) {
    return {file.id,
            file.deleted && file.previous_path ? *file.previous_path : file.path,
            file.deleted};
}

DiffDelta deriveDiffDelta(const DiffViewState& base,
                            const DiffViewState& target) {
    DiffDelta delta{.base_revision = base.revision,
                    .revision = target.revision};
    for (const auto& targetFile : target.files) {
        const auto baseFile = std::find_if(
            base.files.begin(), base.files.end(),
            [&](const auto& candidate) { return candidate.id == targetFile.id; });
        if (baseFile == base.files.end() || *baseFile != targetFile) {
            delta.upserted.push_back(targetFile);
        }
    }
    for (const auto& baseFile : base.files) {
        const auto targetFile = std::find_if(
            target.files.begin(), target.files.end(),
            [&](const auto& candidate) { return candidate.id == baseFile.id; });
        if (targetFile == target.files.end()) {
            delta.removed.push_back(baseFile.id);
        }
    }
    return delta;
}

DiffReplayResult replayDiffDelta(const DiffViewState& base,
                                   const DiffDelta& delta) {
    if (base.revision != delta.base_revision) {
        return {std::nullopt, DiffReplayError::StaleRevision};
    }
    DiffViewState result = base;
    for (const auto& id : delta.removed) {
        if (containsId(delta.removed, id) &&
            std::count(delta.removed.begin(), delta.removed.end(), id) != 1) {
            return {std::nullopt, DiffReplayError::MalformedDelta};
        }
        result.files.erase(
            std::remove_if(result.files.begin(), result.files.end(),
                           [&](const auto& file) { return file.id == id; }),
            result.files.end());
    }
    std::vector<DiffFileId> upsertedIds;
    for (const auto& file : delta.upserted) {
        if (containsId(upsertedIds, file.id) ||
            containsId(delta.removed, file.id)) {
            return {std::nullopt, DiffReplayError::MalformedDelta};
        }
        upsertedIds.push_back(file.id);
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
