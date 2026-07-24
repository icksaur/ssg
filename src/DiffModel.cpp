#include "ssg/DiffModel.h"

#include <algorithm>
#include <limits>
#include <span>
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

struct WordToken {
    std::size_t byteStart;
    std::size_t byteLength;
};

struct WordDiff {
    std::vector<DiffWordRange> targetAdded;
    std::vector<DiffWordRange> baselineRemoved;
    std::vector<DiffWordRange> targetModified;
};

bool isAsciiWordByte(char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '_';
}

std::vector<WordToken> wordTokens(std::string_view line) {
    std::vector<WordToken> result;
    for (std::size_t start = 0; start < line.size();) {
        const bool word = isAsciiWordByte(line[start]);
        std::size_t end = start + 1;
        while (end < line.size() && isAsciiWordByte(line[end]) == word) {
            ++end;
        }
        result.push_back({start, end - start});
        start = end;
    }
    return result;
}

void appendTokenRanges(std::vector<DiffWordRange>& ranges,
                       std::span<const WordToken> tokens) {
    for (const auto& token : tokens) {
        if (!ranges.empty() &&
            ranges.back().byteStart + ranges.back().byteLength ==
                token.byteStart) {
            ranges.back().byteLength += token.byteLength;
        } else {
            ranges.push_back({token.byteStart, token.byteLength});
        }
    }
}

std::optional<WordDiff> computeWordDiff(std::string_view baseline,
                                        std::string_view target,
                                        std::size_t& remainingMatrixCells) {
    // Maximal ASCII alnum/underscore and non-word runs keep review marks
    // readable; character-level differences obscure the surrounding token.
    const auto oldTokens = wordTokens(baseline);
    const auto newTokens = wordTokens(target);
    const auto rows = oldTokens.size() + 1;
    const auto columns = newTokens.size() + 1;
    if (rows > std::numeric_limits<std::size_t>::max() / columns ||
        rows * columns > remainingMatrixCells) {
        return std::nullopt;
    }
    remainingMatrixCells -= rows * columns;

    const auto tokenText = [](std::string_view line, const WordToken& token) {
        return line.substr(token.byteStart, token.byteLength);
    };
    std::vector<std::size_t> lcs(rows * columns);
    const auto at = [&](std::size_t oldIndex,
                        std::size_t newIndex) -> std::size_t& {
        return lcs[oldIndex * columns + newIndex];
    };
    for (std::size_t oldIndex = oldTokens.size(); oldIndex-- > 0;) {
        for (std::size_t newIndex = newTokens.size(); newIndex-- > 0;) {
            at(oldIndex, newIndex) =
                tokenText(baseline, oldTokens[oldIndex]) ==
                        tokenText(target, newTokens[newIndex])
                    ? at(oldIndex + 1, newIndex + 1) + 1
                    : std::max(at(oldIndex + 1, newIndex),
                               at(oldIndex, newIndex + 1));
        }
    }

    WordDiff result;
    std::size_t oldIndex = 0;
    std::size_t newIndex = 0;
    while (oldIndex < oldTokens.size() || newIndex < newTokens.size()) {
        if (oldIndex < oldTokens.size() && newIndex < newTokens.size() &&
            tokenText(baseline, oldTokens[oldIndex]) ==
                tokenText(target, newTokens[newIndex])) {
            ++oldIndex;
            ++newIndex;
            continue;
        }

        const auto oldStart = oldIndex;
        const auto newStart = newIndex;
        while (oldIndex < oldTokens.size() || newIndex < newTokens.size()) {
            if (oldIndex < oldTokens.size() &&
                newIndex < newTokens.size() &&
                tokenText(baseline, oldTokens[oldIndex]) ==
                    tokenText(target, newTokens[newIndex])) {
                break;
            }
            if (oldIndex < oldTokens.size() &&
                (newIndex == newTokens.size() ||
                 at(oldIndex + 1, newIndex) >=
                     at(oldIndex, newIndex + 1))) {
                ++oldIndex;
            } else {
                ++newIndex;
            }
        }

        appendTokenRanges(
            result.baselineRemoved,
            std::span<const WordToken>{oldTokens}.subspan(oldStart,
                                                          oldIndex - oldStart));
        auto& targetRanges =
            oldIndex == oldStart ? result.targetAdded : result.targetModified;
        appendTokenRanges(
            targetRanges,
            std::span<const WordToken>{newTokens}.subspan(newStart,
                                                          newIndex - newStart));
    }
    return result;
}

std::optional<ComputedDiff> computeDiff(std::string_view baseline,
                                         std::string_view target,
                                         const DiffConfig& config) {
    const auto oldLines = splitDiffLines(baseline);
    const auto newLines = splitDiffLines(target);
    if (oldLines.size() > config.maximumLineCount ||
        newLines.size() > config.maximumLineCount) {
        return std::nullopt;
    }

    const auto rows = oldLines.size() + 1;
    const auto columns = newLines.size() + 1;
    if (rows > std::numeric_limits<std::size_t>::max() / columns ||
        rows * columns > config.maximumMatrixCells) {
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
    std::size_t remainingWordMatrixCells = config.maximumWordMatrixCells;
    bool wordLimitExceeded = false;
    std::optional<DiffHunk> pending;
    const auto flush = [&] {
        if (!pending) {
            return;
        }
        if (wordLimitExceeded) {
            pending.reset();
            return;
        }
        const auto paired =
            std::min(pending->baselineLines.size(), pending->targetLines.size());
        for (std::size_t index = 0; index < paired; ++index) {
            auto wordDiff =
                computeWordDiff(pending->baselineLines[index],
                               pending->targetLines[index],
                               remainingWordMatrixCells);
            if (!wordDiff) {
                wordLimitExceeded = true;
                pending.reset();
                return;
            }
            result.changes.push_back(
                {.kind = DiffLineKind::Modified,
                 .baselineLine = pending->baselineStart + index,
                 .targetLine = pending->targetStart + index,
                 .targetAddedWordRanges = std::move(wordDiff->targetAdded),
                 .baselineRemovedWordRanges =
                     std::move(wordDiff->baselineRemoved),
                 .targetModifiedWordRanges =
                     std::move(wordDiff->targetModified)});
        }
        for (std::size_t index = paired; index < pending->baselineLines.size();
             ++index) {
            result.changes.push_back(
                {DiffLineKind::Removed, pending->baselineStart + index,
                 std::nullopt});
        }
        for (std::size_t index = paired; index < pending->targetLines.size();
             ++index) {
            result.changes.push_back(
                {DiffLineKind::Added, std::nullopt,
                 pending->targetStart + index});
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
            pending = DiffHunk{.baselineStart = oldIndex,
                               .targetStart = newIndex};
        }
        if (oldIndex < oldLines.size() &&
            (newIndex == newLines.size() ||
             at(oldIndex + 1, newIndex) >= at(oldIndex, newIndex + 1))) {
            pending->baselineLines.push_back(oldLines[oldIndex++]);
        } else {
            pending->targetLines.push_back(newLines[newIndex++]);
        }
    }
    flush();
    if (wordLimitExceeded) {
        return std::nullopt;
    }
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

DiffFileStatus gitFileStatus(const GitDiffFile& file) {
    if (!file.workingContent.has_value()) {
        return DiffFileStatus::Deleted;
    }
    if (!file.baselineContent.has_value()) {
        return DiffFileStatus::Added;
    }
    if (file.previousPath.has_value()) {
        return DiffFileStatus::Renamed;
    }
    return DiffFileStatus::Modified;
}

DiffFileStatus nonGitFileStatus(NonGitDiffEventKind kind,
                                bool deleted,
                                const std::optional<std::filesystem::path>& previousPath) {
    if (deleted) {
        return DiffFileStatus::Deleted;
    }
    switch (kind) {
        case NonGitDiffEventKind::Create:
            return DiffFileStatus::Added;
        case NonGitDiffEventKind::Rename:
            return DiffFileStatus::Renamed;
        case NonGitDiffEventKind::Modify:
        case NonGitDiffEventKind::Remove:
            break;
    }
    (void)previousPath;
    return DiffFileStatus::Modified;
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

std::optional<std::reference_wrapper<const DiffFileView>>
DiffViewState::fileForDocument(const DocumentViewState& document) const {
    if (!document.diffFileIdentity) {
        return std::nullopt;
    }
    auto const found = std::find_if(
        files.begin(), files.end(), [&](const DiffFileView& file) {
            return file.id.value() == *document.diffFileIdentity;
        });
    if (found == files.end()) {
        return std::nullopt;
    }
    return std::cref(*found);
}

DiffModel::DiffModel(DiffConfig config) : config_(config) {
    if (config_.maximumLineCount == 0 ||
        config_.maximumMatrixCells == 0 ||
        config_.maximumWordMatrixCells == 0) {
        throw std::invalid_argument("diff work limits must be greater than zero");
    }
}

DiffMutationResult DiffModel::updateGitFile(GitDiffFile file,
                                               Revision revision) {
    if (revision <= revision_) {
        return {DiffError::StaleRevision};
    }
    if (!validWorkspacePath(file.path) ||
        (file.previousPath && !validWorkspacePath(*file.previousPath))) {
        return {DiffError::InvalidPath};
    }
    if (file.baselineIdentity.empty()) {
        return {DiffError::BaselineIdentityRequired};
    }
    const auto existing = findEntry(entries_, file.id);
    if (existing != entries_.end() && existing->source != Source::Git) {
        return {DiffError::DuplicateFile};
    }

    const auto status = gitFileStatus(file);
    const std::string baseline = file.baselineContent.value_or("");
    const std::string target = file.workingContent.value_or("");
    auto computed = computeDiff(baseline, target, config_);
    if (!computed) {
        return {DiffError::WorkLimitExceeded};
    }

    DiffFileView view{.id = file.id,
                      .path = std::move(file.path),
                      .previousPath = std::move(file.previousPath),
                      .deleted = !file.workingContent.has_value(),
                      .status = status,
                      .baselineIdentity = std::move(file.baselineIdentity),
                      .currentContent = target,
                      .hunks = std::move(computed->hunks),
                      .changedLines = std::move(computed->changes)};
    if (existing == entries_.end()) {
        entries_.push_back({std::move(view), Source::Git});
    } else {
        existing->view = std::move(view);
        existing->source = Source::Git;
    }
    revision_ = revision;
    return {};
}

DiffMutationResult DiffModel::removeFile(const DiffFileId& id,
                                         Revision revision) {
    if (revision <= revision_) {
        return {DiffError::StaleRevision};
    }
    const auto existing = findEntry(entries_, id);
    if (existing == entries_.end()) {
        return {DiffError::UnknownFile};
    }
    entries_.erase(existing);
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
        if (splitDiffLines(file.content).size() > config_.maximumLineCount) {
            return {DiffError::WorkLimitExceeded};
        }
        seeded.push_back(
            {DiffFileView{.id = file.id,
                          .path = std::move(file.path),
                          .status = DiffFileStatus::Added,
                          .currentContent = file.content},
             Source::NonGit});
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
        (event.previousPath && !validWorkspacePath(*event.previousPath))) {
        return {DiffError::InvalidPath};
    }
    const bool removed = event.kind == NonGitDiffEventKind::Remove;
    if (removed && event.targetContent) {
        return {DiffError::ContentForbidden};
    }
    if (!removed && !event.targetContent) {
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
        const std::string target = *event.targetContent;
        const auto status = nonGitFileStatus(event.kind, removed, event.previousPath);
        auto computed = computeDiff(event.baselineContent, target, config_);
        if (!computed) {
            return {DiffError::WorkLimitExceeded};
        }
        entries_.push_back(
            {DiffFileView{.id = event.id,
                          .path = std::move(event.path),
                          .previousPath = std::move(event.previousPath),
                          .status = status,
                          .currentContent = target,
                          .hunks = std::move(computed->hunks),
                          .changedLines = std::move(computed->changes)},
             Source::NonGit});
        revision_ = revision;
        return {};
    }

    const std::string target = event.targetContent.value_or("");
    auto computed = computeDiff(event.baselineContent, target, config_);
    if (!computed) {
        return {DiffError::WorkLimitExceeded};
    }
    existing->view.path = std::move(event.path);
    existing->view.previousPath = std::move(event.previousPath);
    existing->view.deleted = removed;
    existing->view.status =
        nonGitFileStatus(event.kind, removed, existing->view.previousPath);
    existing->view.baselineIdentity.clear();
    existing->view.currentContent = target;
    existing->view.hunks = std::move(computed->hunks);
    existing->view.changedLines = std::move(computed->changes);
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
            return hunk.targetStart > *currentTargetLine;
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
        if (file.hunks[index].targetStart < *currentTargetLine) {
            return index;
        }
    }
    return file.hunks.size() - 1;
}

DiffOpenTarget diffOpenFile(const DiffFileView& file) {
    return {file.id,
            file.deleted && file.previousPath ? *file.previousPath : file.path,
            file.deleted};
}

DiffDelta DiffDeltaCodec::derive(const DiffViewState& base,
                            const DiffViewState& target) {
    DiffDelta delta{.baseRevision = base.revision,
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

DiffReplayResult DiffDeltaCodec::replay(const DiffViewState& base,
                                   const DiffDelta& delta) {
    if (base.revision != delta.baseRevision) {
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
