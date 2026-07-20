#pragma once

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

struct DiffLineChange {
    DiffLineKind kind = DiffLineKind::Modified;
    std::optional<std::size_t> baseline_line;
    std::optional<std::size_t> target_line;

    friend bool operator==(const DiffLineChange&, const DiffLineChange&) = default;
};

struct DiffHunk {
    std::size_t baseline_start = 0;
    std::size_t target_start = 0;
    std::vector<std::string> baseline_lines;
    std::vector<std::string> target_lines;

    friend bool operator==(const DiffHunk&, const DiffHunk&) = default;
};

struct DiffFileView {
    DiffFileId id;
    std::filesystem::path path;
    std::optional<std::filesystem::path> previous_path;
    bool deleted = false;
    std::string baseline_identity;
    std::string current_content;
    std::vector<DiffHunk> hunks;
    std::vector<DiffLineChange> changed_lines;

    friend bool operator==(const DiffFileView&, const DiffFileView&) = default;
};

struct DiffViewState {
    Revision revision{0};
    std::vector<DiffFileView> files;

    friend bool operator==(const DiffViewState&, const DiffViewState&) = default;
};

struct DiffConfig {
    std::size_t maximum_line_count = 200'000;
    std::size_t maximum_matrix_cells = 4'000'000;
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
    std::optional<std::filesystem::path> previous_path;
    std::optional<std::string> index_content;
    std::optional<std::string> working_content;
    std::string index_identity;
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
    std::optional<std::filesystem::path> previous_path;
    std::optional<std::string> content;
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
        std::string acknowledged_content;
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
    const DiffFileView& file, std::optional<std::size_t> current_target_line);
[[nodiscard]] std::optional<std::size_t> previousDiffHunk(
    const DiffFileView& file, std::optional<std::size_t> current_target_line);

struct DiffOpenTarget {
    DiffFileId id;
    std::filesystem::path path;
    bool deleted = false;

    friend bool operator==(const DiffOpenTarget&, const DiffOpenTarget&) = default;
};

[[nodiscard]] DiffOpenTarget diffOpenFile(const DiffFileView& file);

struct DiffDelta {
    Revision base_revision{0};
    Revision revision{0};
    std::vector<DiffFileView> upserted;
    std::vector<DiffFileId> removed;

    friend bool operator==(const DiffDelta&, const DiffDelta&) = default;
};

[[nodiscard]] DiffDelta deriveDiffDelta(const DiffViewState& base,
                                          const DiffViewState& target);

enum class DiffReplayError { None, StaleRevision, MalformedDelta };

struct DiffReplayResult {
    std::optional<DiffViewState> state;
    DiffReplayError error = DiffReplayError::None;
    [[nodiscard]] bool accepted() const noexcept { return state.has_value(); }
};

[[nodiscard]] DiffReplayResult replayDiffDelta(const DiffViewState& base,
                                                 const DiffDelta& delta);

} // namespace ssg
