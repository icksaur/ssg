#pragma once

#include "ssg/snapshot.h"
#include "ssg/types.h"

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

enum class DiffLineKind { Added, Removed, Modified };

struct DiffWordRange {
    std::size_t byteStart = 0;
    std::size_t byteLength = 0;

    friend bool operator==(const DiffWordRange&, const DiffWordRange&) = default;
};

struct DiffLineChange {
    DiffLineKind kind = DiffLineKind::Modified;
    std::optional<std::size_t> baselineLine;
    std::optional<std::size_t> targetLine;
    std::vector<DiffWordRange> targetAddedWordRanges;
    std::vector<DiffWordRange> baselineRemovedWordRanges;
    std::vector<DiffWordRange> targetModifiedWordRanges;

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
    std::string baselineIdentity;
    std::string currentContent;
    std::vector<DiffHunk> hunks;
    std::vector<DiffLineChange> changedLines;

    friend bool operator==(const DiffFileView&, const DiffFileView&) = default;
};

struct DiffViewState {
    Revision revision{0};
    std::vector<DiffFileView> files;

    [[nodiscard]] std::optional<std::reference_wrapper<const DiffFileView>>
    fileForDocument(const DocumentViewState& document) const;

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
    std::optional<std::string> indexContent;
    std::optional<std::string> workingContent;
    std::string indexIdentity;
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

    [[nodiscard]] DiffMutationResult updateGitFile(GitDiffFile file,
                                                      Revision revision);
    [[nodiscard]] DiffMutationResult seedNonGit(
        std::vector<SeededDiffFile> files, Revision revision);
    [[nodiscard]] DiffMutationResult applyNonGitEvent(
        NonGitDiffEvent event, Revision revision);

    [[nodiscard]] DiffViewState viewState() const;
    [[nodiscard]] std::optional<std::reference_wrapper<const DiffFileView>>
    file(const DiffFileId& id) const;

private:
    enum class Source { Git, NonGit };

    struct Entry {
        DiffFileView view;
        Source source = Source::Git;
    };

    DiffConfig config_;
    Revision revision_{0};
    std::vector<Entry> entries_;
};

[[nodiscard]] std::vector<std::string> splitDiffLines(std::string_view content);

struct DiffCommandDescriptor {
    std::string_view id;
    friend bool operator==(const DiffCommandDescriptor&,
                           const DiffCommandDescriptor&) = default;
};

class DiffCommandSet {
public:
    [[nodiscard]] const std::array<DiffCommandDescriptor, 3>& descriptors()
        const noexcept {
        return descriptors_;
    }

private:
    const std::array<DiffCommandDescriptor, 3> descriptors_{{
        {"diff.next_hunk"},
        {"diff.previous_hunk"},
        {"diff.open_file"},
    }};
};

[[nodiscard]] DiffCommandSet diffCommandSet();
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

struct DiffDelta {
    Revision baseRevision{0};
    Revision revision{0};
    std::vector<DiffFileView> upserted;
    std::vector<DiffFileId> removed;

    friend bool operator==(const DiffDelta&, const DiffDelta&) = default;
};

enum class DiffReplayError { None, StaleRevision, MalformedDelta };

struct DiffReplayResult {
    std::optional<DiffViewState> state;
    DiffReplayError error = DiffReplayError::None;
    [[nodiscard]] bool accepted() const noexcept { return state.has_value(); }
};

class DiffDeltaCodec {
public:
    [[nodiscard]] DiffDelta derive(const DiffViewState& base,
                                   const DiffViewState& target);
    [[nodiscard]] DiffReplayResult replay(const DiffViewState& base,
                                          const DiffDelta& delta);
};

} // namespace ssg
