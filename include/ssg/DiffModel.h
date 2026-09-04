#pragma once

#include <ssg/types.h>
#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

class DiffFileId {
public:
    explicit DiffFileId(std::string value);

    [[nodiscard]] const std::string& value() const noexcept { return value_; }
    auto operator<=>(const DiffFileId&) const = default;

private:
    std::string value_;
};

enum class DiffLineKind {
    Added = 0,
    Removed = 1,
    Modified = 2,
};
enum class DiffFileStatus {
    Added = 0,
    Modified = 1,
    Deleted = 2,
    Renamed = 3,
};

struct DiffWordRange {
    std::size_t byteStart = 0;
    std::size_t byteLength = 0;

    friend bool operator==(const DiffWordRange&, const DiffWordRange&) = default;
};

// A single-line Modified pair's baseline and target text merged into ONE
// ordered sequence of segments (git-diff --word-diff style: old and new
// words appear inline on one line, not as two separate lines). Unchanged and
// Added segments are REAL: concatenated in order (skipping Removed and
// Separator) they reconstruct the target line's text byte-for-byte, so a
// consumer can map a byte within one of these segments back to a real
// document offset by simple accumulation. Removed carries baseline text no
// longer present in the target; Separator is a synthetic single space
// spliced between a Removed/Added pair with no shared whitespace, purely so
// the two colored words don't visually run together -- both Removed and
// Separator are GHOST: they contribute display-only text with no
// corresponding real document byte. Viewport turns this sequence into ghost
// spans (see RealRow::mergedSegments) for hit-testing/caret/selection;
// Renderer paints it without recomputing the segmentation.
struct InlineWordSegment {
    enum class Kind { Unchanged, Removed, Added, Separator };
    Kind kind = Kind::Unchanged;
    std::string text;

    friend bool operator==(const InlineWordSegment&, const InlineWordSegment&) = default;
};

struct DiffLineChange {
    DiffLineKind kind = DiffLineKind::Modified;
    std::optional<std::size_t> baselineLine;
    std::optional<std::size_t> targetLine;
    std::vector<DiffWordRange> targetAddedWordRanges;
    std::vector<DiffWordRange> baselineRemovedWordRanges;
    std::vector<DiffWordRange> targetModifiedWordRanges;
    // Populated only for a Modified change backed by a clean single-line
    // 1:1 hunk (see DiffModel.cpp's flush()); empty otherwise.
    std::vector<InlineWordSegment> inlineWordSegments;

    friend bool operator==(const DiffLineChange&, const DiffLineChange&) = default;
};

struct DiffHunk {
    std::size_t baselineStart = 0;
    std::size_t targetStart = 0;
    std::vector<std::string> baselineLines;
    std::vector<std::string> targetLines;

    friend bool operator==(const DiffHunk&, const DiffHunk&) = default;
};

struct DiffFileView {
    DiffFileId id;
    std::filesystem::path path;
    std::optional<std::filesystem::path> previousPath;
    bool deleted = false;
    DiffFileStatus status = DiffFileStatus::Modified;
    std::string baselineIdentity;
    std::string currentContent;
    std::vector<DiffHunk> hunks;
    std::vector<DiffLineChange> changedLines;

    friend bool operator==(const DiffFileView&, const DiffFileView&) = default;
};

struct DiffViewState {
    std::uint64_t revision{0};
    std::vector<DiffFileView> files;

    [[nodiscard]] std::optional<std::reference_wrapper<const DiffFileView>>
    fileForIdentity(const std::optional<std::string>& identity) const;

    friend bool operator==(const DiffViewState&, const DiffViewState&) = default;
};

struct DiffConfig {
    std::size_t maximumLineCount = 200'000;
    std::size_t maximumMatrixCells = 4'000'000;
    std::size_t maximumWordMatrixCells = 4'000'000;
};

enum class DiffError {
    None,
    StaleRevision,
    UnknownFile,
    DuplicateFile,
    InvalidPath,
    BaselineIdentityRequired,
    ContentRequired,
    ContentForbidden,
    WorkLimitExceeded,
};

struct DiffMutationResult {
    DiffError error = DiffError::None;
    [[nodiscard]] bool accepted() const noexcept { return error == DiffError::None; }
};

struct GitDiffFile {
    DiffFileId id;
    std::filesystem::path path;
    std::optional<std::filesystem::path> previousPath;
    std::optional<std::string> baselineContent;
    std::optional<std::string> workingContent;

    [[nodiscard]] DiffFileStatus status() const noexcept;
    friend bool operator==(const GitDiffFile&, const GitDiffFile&) = default;
};

struct SeededDiffFile {
    DiffFileId id;
    std::filesystem::path path;
    std::string content;
};

enum class NonGitDiffEventKind { Create, Modify, Rename, Remove };

struct NonGitDiffEvent {
    NonGitDiffEventKind kind = NonGitDiffEventKind::Modify;
    DiffFileId id;
    std::filesystem::path path;
    std::optional<std::filesystem::path> previousPath;
    std::string baselineContent;
    std::optional<std::string> targetContent;
};

class DiffModel {
public:
    explicit DiffModel(DiffConfig config = {});

    [[nodiscard]] DiffMutationResult updateGitFile(
        GitDiffFile file, std::string baselineIdentity, std::uint64_t revision);
    [[nodiscard]] DiffMutationResult removeFile(const DiffFileId& id,
                                                std::uint64_t revision);
    [[nodiscard]] DiffMutationResult seedNonGit(
        std::vector<SeededDiffFile> files, std::uint64_t revision);
    [[nodiscard]] DiffMutationResult applyNonGitEvent(
        NonGitDiffEvent event, std::uint64_t revision);

    [[nodiscard]] DiffViewState viewState() const;
    [[nodiscard]] std::optional<std::reference_wrapper<const DiffFileView>>
    file(const DiffFileId& id) const;

    // Whether an entry exists for `id` and originates from a git scan, as
    // opposed to a non-git diff (a draft-vs-disk view, or an external
    // modification view). A git rescan owns and reconciles only its own
    // entries, so it must leave non-git entries untouched rather than evicting
    // them as "no longer changed".
    [[nodiscard]] bool isGitFile(const DiffFileId& id) const noexcept;

private:
    enum class Source { Git, NonGit };

    struct Entry {
        DiffFileView view;
        Source source = Source::Git;
    };

    DiffConfig config_;
    std::uint64_t revision_{0};
    std::vector<Entry> entries_;
};

[[nodiscard]] std::vector<std::string> splitDiffLines(std::string_view content);

[[nodiscard]] std::optional<std::size_t> nextDiffHunk(
    const DiffFileView& file, std::optional<std::size_t> currentTargetLine);
[[nodiscard]] std::optional<std::size_t> previousDiffHunk(
    const DiffFileView& file, std::optional<std::size_t> currentTargetLine);

struct DiffOpenTarget {
    DiffFileId id;
    std::filesystem::path path;
    bool deleted = false;

    friend bool operator==(const DiffOpenTarget&, const DiffOpenTarget&) = default;
};

[[nodiscard]] DiffOpenTarget diffOpenFile(const DiffFileView& file);

} // namespace ssg
