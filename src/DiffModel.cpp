#include "ssg/DiffModel.h"

#include <algorithm>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
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
    // Same information as the three ranges above, but as one ORDERED,
    // interleaved sequence (see InlineWordSegment) for the merged
    // single-line inline word diff -- Added covers both true insertions and
    // in-place changes (both read as "new content" inline).
    std::vector<InlineWordSegment> segments;
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
    // Tracks a run of consecutive common tokens so they merge into ONE
    // Unchanged segment (mirrors appendTokenRanges' adjacency merge for the
    // existing range vectors) instead of one segment per token.
    std::optional<std::size_t> commonRunStart;
    const auto flushCommonRun = [&] {
        if (!commonRunStart) return;
        const auto& first = newTokens[*commonRunStart];
        const auto& last = newTokens[newIndex - 1];
        result.segments.push_back(
            {InlineWordSegment::Kind::Unchanged,
             std::string{target.substr(
                 first.byteStart,
                 last.byteStart + last.byteLength - first.byteStart)}});
        commonRunStart.reset();
    };
    while (oldIndex < oldTokens.size() || newIndex < newTokens.size()) {
        if (oldIndex < oldTokens.size() && newIndex < newTokens.size() &&
            tokenText(baseline, oldTokens[oldIndex]) ==
                tokenText(target, newTokens[newIndex])) {
            if (!commonRunStart) commonRunStart = newIndex;
            ++oldIndex;
            ++newIndex;
            continue;
        }
        flushCommonRun();

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
        if (oldIndex > oldStart) {
            const auto& first = oldTokens[oldStart];
            const auto& last = oldTokens[oldIndex - 1];
            result.segments.push_back(
                {InlineWordSegment::Kind::Removed,
                 std::string{baseline.substr(
                     first.byteStart,
                     last.byteStart + last.byteLength - first.byteStart)}});
        }
        if (newIndex > newStart) {
            // A Removed segment immediately followed by an Added segment
            // (a substitution with no shared whitespace between them, e.g.
            // "original"/"modified" swapping in place) would otherwise
            // read as one run-together word ("originalmodified") in the
            // merged single-line spike -- splice a plain space so the two
            // colored words stay visually separated, matching how git's
            // own --word-diff delimits a substitution.
            if (oldIndex > oldStart &&
                !result.segments.empty() &&
                result.segments.back().kind == InlineWordSegment::Kind::Removed) {
                result.segments.push_back(
                    {InlineWordSegment::Kind::Separator, " "});
            }
            const auto& first = newTokens[newStart];
            const auto& last = newTokens[newIndex - 1];
            result.segments.push_back(
                {InlineWordSegment::Kind::Added,
                 std::string{target.substr(
                     first.byteStart,
                     last.byteStart + last.byteLength - first.byteStart)}});
        }
    }
    flushCommonRun();
    // baseline/target are RAW split lines (they retain their trailing '\n',
    // consistent with how DiffHunk stores line text elsewhere -- see
    // lineText() in Viewport.cpp, which strips it for the SAME reason when
    // building a phantom row's display text). That trailing newline byte
    // always lands in the LAST segment here; left in place it becomes an
    // embedded control character in the merged single-line render text,
    // which GraphemeLayout replaces with a literal U+FFFD glyph. Only the
    // final segment can ever contain it (it's the final byte of both input
    // strings), so trim it there rather than touching the tokenization/
    // byte-range logic above, which existing callers depend on unchanged.
    if (!result.segments.empty()) {
        auto& text = result.segments.back().text;
        if (!text.empty() && text.back() == '\n') text.pop_back();
        if (!text.empty() && text.back() == '\r') text.pop_back();
    }
    return result;
}

// Dice coefficient over word tokens (case-sensitive), ignoring separator
// tokens (whitespace/punctuation) so "line two" vs "line  two" don't get
// penalized for spacing. Empty-vs-empty or empty-vs-nonempty are never
// similar: a blank line pairing with real content is never the intended
// pairing, and blank-vs-blank never reaches here (exact string equality
// already matches it before any hunk is opened).
double lineSimilarity(std::string_view baseline, std::string_view target) {
    const auto wordsOf = [](std::string_view line) {
        std::vector<std::string_view> words;
        for (const auto& token : wordTokens(line)) {
            if (token.byteLength > 0 && isAsciiWordByte(line[token.byteStart])) {
                words.push_back(line.substr(token.byteStart, token.byteLength));
            }
        }
        return words;
    };
    const auto baselineWords = wordsOf(baseline);
    const auto targetWords = wordsOf(target);
    if (baselineWords.empty() || targetWords.empty()) {
        return 0.0;
    }
    std::multiset<std::string_view> remainingTargetWords(targetWords.begin(),
                                                          targetWords.end());
    std::size_t common = 0;
    for (const auto& word : baselineWords) {
        auto found = remainingTargetWords.find(word);
        if (found != remainingTargetWords.end()) {
            ++common;
            remainingTargetWords.erase(found);
        }
    }
    return (2.0 * static_cast<double>(common)) /
           static_cast<double>(baselineWords.size() + targetWords.size());
}

// A replaced block's baseline/target lines are word-level "Modified" pairs
// only when they share enough content to plausibly be the same line edited
// (this project's git-diff-following/word-highlight UX depends on genuinely
// related lines being paired, not merely co-located at the same position --
// positional pairing alone hid unrelated removed lines inside spurious
// word diffs against whatever target line happened to sit at the same
// index). Threshold picked empirically: 0.5
// (half the tokens shared) was ambiguous for lines that share only common
// filler words ("gamma", "line") with no real semantic overlap; 0.6
// requires a clear majority of shared content while still catching a
// single-word edit in an otherwise-identical line.
constexpr double kLineSimilarityThreshold = 0.6;

bool linesAreSimilarEnoughToPair(std::string_view baseline,
                                  std::string_view target) {
    return lineSimilarity(baseline, target) >= kLineSimilarityThreshold;
}

// Above this many (baselineLines x targetLines) cells, skip the fine-grained
// alignment below and treat the whole block as pure Removed+Added -- a
// pathological hunk with no exact-matching lines at all (e.g. a fully
// rewritten huge file) would otherwise cost O(n*m) time and memory for a
// per-line alignment nobody asked for; falling back to a plain block
// replacement is still correct, just less precise.
constexpr std::size_t kMaxLineAlignmentCells = 200'000;

struct LineAlignmentStep {
    enum class Kind { Remove, Add, Match };
    Kind kind;
    std::size_t baselineIndex = 0;
    std::size_t targetIndex = 0;
};

// Aligns a hunk's baseline/target line lists by CONTENT similarity, not
// position: a baseline line pairs with a target line only when they are
// plausibly the same line edited (linesAreSimilarEnoughToPair), regardless
// of whether they sit at the same index. This is the same LCS shape as the
// file-level diff above, just with a fuzzy line-similarity predicate instead
// of exact equality, so a replaced block like "3 old lines rewritten into 4
// new ones" correctly identifies the ONE OR TWO lines among them that are
// really edits of each other and leaves the rest as genuine removals/
// additions -- rather than naively pairing by position and hiding removed
// content behind an unrelated line's word diff.
std::vector<LineAlignmentStep> alignHunkLines(
    std::span<const std::string> baselineLines,
    std::span<const std::string> targetLines) {
    const auto nb = baselineLines.size();
    const auto nt = targetLines.size();
    std::vector<LineAlignmentStep> steps;
    // A single line replaced by a single line is unambiguous -- there is no
    // OTHER line either side could have paired with, so always treat it as
    // a word-level edit regardless of how much of the line's content
    // changed (e.g. "beta" -> "BETA" shares no case-sensitive word tokens,
    // but is still obviously the same line edited). The similarity gate
    // below only matters for choosing WHICH lines pair up when there is
    // more than one candidate on either side.
    if (nb == 1 && nt == 1) {
        return {LineAlignmentStep{LineAlignmentStep::Kind::Match, 0, 0}};
    }
    if (nb == 0 || nt == 0 ||
        nb > std::numeric_limits<std::size_t>::max() / std::max<std::size_t>(nt, 1) ||
        nb * nt > kMaxLineAlignmentCells) {
        steps.reserve(nb + nt);
        for (std::size_t index = 0; index < nb; ++index) {
            steps.push_back({LineAlignmentStep::Kind::Remove, index, 0});
        }
        for (std::size_t index = 0; index < nt; ++index) {
            steps.push_back({LineAlignmentStep::Kind::Add, 0, index});
        }
        return steps;
    }

    const auto rows = nb + 1;
    const auto columns = nt + 1;
    std::vector<std::size_t> lcs(rows * columns);
    const auto at = [&](std::size_t baselineIndex,
                        std::size_t targetIndex) -> std::size_t& {
        return lcs[baselineIndex * columns + targetIndex];
    };
    for (std::size_t baselineIndex = nb; baselineIndex-- > 0;) {
        for (std::size_t targetIndex = nt; targetIndex-- > 0;) {
            at(baselineIndex, targetIndex) =
                linesAreSimilarEnoughToPair(baselineLines[baselineIndex],
                                            targetLines[targetIndex])
                    ? at(baselineIndex + 1, targetIndex + 1) + 1
                    : std::max(at(baselineIndex + 1, targetIndex),
                               at(baselineIndex, targetIndex + 1));
        }
    }

    std::size_t baselineIndex = 0;
    std::size_t targetIndex = 0;
    steps.reserve(nb + nt);
    while (baselineIndex < nb || targetIndex < nt) {
        if (baselineIndex < nb && targetIndex < nt &&
            linesAreSimilarEnoughToPair(baselineLines[baselineIndex],
                                        targetLines[targetIndex]) &&
            at(baselineIndex, targetIndex) ==
                at(baselineIndex + 1, targetIndex + 1) + 1) {
            steps.push_back(
                {LineAlignmentStep::Kind::Match, baselineIndex, targetIndex});
            ++baselineIndex;
            ++targetIndex;
            continue;
        }
        if (baselineIndex < nb &&
            (targetIndex == nt ||
             at(baselineIndex + 1, targetIndex) >=
                 at(baselineIndex, targetIndex + 1))) {
            steps.push_back(
                {LineAlignmentStep::Kind::Remove, baselineIndex, 0});
            ++baselineIndex;
        } else {
            steps.push_back({LineAlignmentStep::Kind::Add, 0, targetIndex});
            ++targetIndex;
        }
    }
    return steps;
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
        // Align this block's baseline/target lines by CONTENT similarity
        // (see alignHunkLines), not by position: a block replacing 3 old
        // lines with 4 new ones may still contain one or two lines that are
        // genuinely edits of each other (share most of their words) among
        // otherwise-unrelated removals/additions. Emit one finer DiffHunk
        // per contiguous same-kind run of alignment steps so the phantom-row
        // projection (Viewport.cpp's removedBlocks) places each removed run
        // exactly where it falls in the aligned sequence, not lumped at one
        // end of the block.
        const auto steps = alignHunkLines(pending->baselineLines,
                                          pending->targetLines);
        std::size_t stepIndex = 0;
        while (stepIndex < steps.size()) {
            const auto& step = steps[stepIndex];
            if (step.kind == LineAlignmentStep::Kind::Match) {
                auto wordDiff = computeWordDiff(
                    pending->baselineLines[step.baselineIndex],
                    pending->targetLines[step.targetIndex],
                    remainingWordMatrixCells);
                if (!wordDiff) {
                    wordLimitExceeded = true;
                    pending.reset();
                    return;
                }
                result.hunks.push_back(
                    {.baselineStart = pending->baselineStart + step.baselineIndex,
                     .targetStart = pending->targetStart + step.targetIndex,
                     .baselineLines = {pending->baselineLines[step.baselineIndex]},
                     .targetLines = {pending->targetLines[step.targetIndex]}});
                result.changes.push_back(
                    {.kind = DiffLineKind::Modified,
                     .baselineLine = pending->baselineStart + step.baselineIndex,
                     .targetLine = pending->targetStart + step.targetIndex,
                     .targetAddedWordRanges = std::move(wordDiff->targetAdded),
                     .baselineRemovedWordRanges =
                         std::move(wordDiff->baselineRemoved),
                     .targetModifiedWordRanges =
                         std::move(wordDiff->targetModified),
                     .inlineWordSegments = std::move(wordDiff->segments)});
                ++stepIndex;
                continue;
            }
            const auto runKind = step.kind;
            const auto runStart = stepIndex;
            while (stepIndex < steps.size() && steps[stepIndex].kind == runKind) {
                ++stepIndex;
            }
            // The insertion anchor for this run is wherever the walk sits on
            // the OTHER side once the run ends: a Removed run doesn't
            // advance targetIndex, so its phantom rows belong immediately
            // before whichever target line comes next (or at the block's
            // end if none); symmetrically for an Added run's baselineStart.
            if (runKind == LineAlignmentStep::Kind::Remove) {
                const auto targetAnchor =
                    stepIndex < steps.size()
                        ? pending->targetStart + steps[stepIndex].targetIndex
                        : pending->targetStart + pending->targetLines.size();
                DiffHunk removedHunk{.baselineStart = pending->baselineStart +
                                                       steps[runStart].baselineIndex,
                                     .targetStart = targetAnchor};
                for (std::size_t index = runStart; index < stepIndex; ++index) {
                    removedHunk.baselineLines.push_back(
                        pending->baselineLines[steps[index].baselineIndex]);
                    result.changes.push_back(
                        {DiffLineKind::Removed,
                         pending->baselineStart + steps[index].baselineIndex,
                         std::nullopt});
                }
                result.hunks.push_back(std::move(removedHunk));
            } else {
                const auto baselineAnchor =
                    stepIndex < steps.size()
                        ? pending->baselineStart + steps[stepIndex].baselineIndex
                        : pending->baselineStart + pending->baselineLines.size();
                DiffHunk addedHunk{.baselineStart = baselineAnchor,
                                  .targetStart = pending->targetStart +
                                                 steps[runStart].targetIndex};
                for (std::size_t index = runStart; index < stepIndex; ++index) {
                    addedHunk.targetLines.push_back(
                        pending->targetLines[steps[index].targetIndex]);
                    result.changes.push_back(
                        {DiffLineKind::Added, std::nullopt,
                         pending->targetStart + steps[index].targetIndex});
                }
                result.hunks.push_back(std::move(addedHunk));
            }
        }
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

DiffFileStatus GitDiffFile::status() const noexcept {
    if (!workingContent.has_value()) return DiffFileStatus::Deleted;
    if (!baselineContent.has_value()) return DiffFileStatus::Added;
    if (previousPath.has_value()) return DiffFileStatus::Renamed;
    return DiffFileStatus::Modified;
}

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

DiffMutationResult DiffModel::updateGitFile(
    GitDiffFile file, std::string baselineIdentity, Revision revision) {
    if (revision <= revision_) {
        return {DiffError::StaleRevision};
    }
    if (!validWorkspacePath(file.path) ||
        (file.previousPath && !validWorkspacePath(*file.previousPath))) {
        return {DiffError::InvalidPath};
    }
    if (baselineIdentity.empty()) {
        return {DiffError::BaselineIdentityRequired};
    }
    const auto existing = findEntry(entries_, file.id);
    if (existing != entries_.end() && existing->source != Source::Git) {
        return {DiffError::DuplicateFile};
    }

    const auto status = file.status();
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
                      .baselineIdentity = std::move(baselineIdentity),
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

bool DiffModel::isGitFile(const DiffFileId& id) const noexcept {
    const auto found = findEntry(entries_, id);
    return found != entries_.end() && found->source == Source::Git;
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

} // namespace ssg
