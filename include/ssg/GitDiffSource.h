#pragma once

#include "ssg/DiffModel.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ssg {

enum class GitBaselineKind : std::uint8_t { Head };

struct GitDiffConfig {
    GitBaselineKind baseline = GitBaselineKind::Head;
    std::size_t maxFiles = 10'000;
    std::size_t maxBytesPerFile = 4 * 1024 * 1024;
};

struct GitDiffScanFile {
    DiffFileId id;
    std::filesystem::path path;
    std::optional<std::filesystem::path> previousPath;
    std::optional<std::string> baselineContent;
    std::optional<std::string> workingContent;

    friend bool operator==(const GitDiffScanFile&, const GitDiffScanFile&) =
        default;
};

struct GitDiffScan {
    Revision revision{0};
    std::string baselineIdentity;
    std::optional<std::string> currentBranch;
    std::vector<GitDiffScanFile> files;
    bool complete = true;

    friend bool operator==(const GitDiffScan&, const GitDiffScan&) = default;
};

struct GitWorkingTreeScan {
    Revision revision{0};
    std::string baselineIdentity;
    std::vector<std::filesystem::path> requestedPaths;
    std::vector<GitDiffScanFile> files;
    bool complete = true;

    friend bool operator==(const GitWorkingTreeScan&,
                           const GitWorkingTreeScan&) = default;
};

struct GitDiffRefreshResult {
    bool applied = false;
    bool requestedRescan = false;
    bool accepted = true;

    [[nodiscard]] bool shouldRetry() const noexcept { return requestedRescan; }
};

class GitRepository {
public:
    virtual ~GitRepository() = default;

    [[nodiscard]] virtual bool isUsable() const = 0;
    [[nodiscard]] virtual GitDiffScan scanDiff(const GitDiffConfig& config) = 0;
    [[nodiscard]] virtual GitWorkingTreeScan scanPaths(
        const std::vector<std::filesystem::path>& paths,
        const GitDiffConfig& config) = 0;
    [[nodiscard]] virtual std::optional<std::string> currentBranch() = 0;
};

[[nodiscard]] std::unique_ptr<GitRepository> makePlatformGitRepository(
    const std::filesystem::path& canonicalRoot);

// Answers "does gitignore exclude this path?" for every step of a directory
// walk.  Deliberately not a method on GitRepository: that class opens and closes
// a repository handle inside each call, which is right for one scan per refresh
// but not for the thousands of queries a walk makes, so this holds its handle
// open for its lifetime instead.
class GitIgnoreMatcher {
public:
    virtual ~GitIgnoreMatcher() = default;

    // False when there is no usable repository, in which case `ignores` is
    // always false and every file is listed.  Not an error: a workspace need
    // not be a git repository.
    [[nodiscard]] virtual bool usable() const = 0;

    // `workspaceRelative` is relative to the workspace root this matcher was
    // built for, which is NOT necessarily the repository root.  Rebasing it onto
    // the repository work directory is the matcher's job; callers must never
    // pre-rebase, and must not pass absolute paths.
    [[nodiscard]] virtual bool ignores(
        const std::filesystem::path& workspaceRelative) const = 0;
};

[[nodiscard]] std::unique_ptr<GitIgnoreMatcher> makePlatformGitIgnoreMatcher(
    const std::filesystem::path& workspaceRoot);

class GitDiffSource {
public:
    explicit GitDiffSource(DiffModel& diffModel, GitDiffConfig config = {});

    [[nodiscard]] GitDiffRefreshResult refresh(GitRepository& repository);
    [[nodiscard]] GitDiffRefreshResult refreshPaths(
        GitRepository& repository,
        const std::vector<std::filesystem::path>& paths);
    [[nodiscard]] std::optional<GitDiffScan> latestAppliedScan() const;

private:
    [[nodiscard]] GitDiffRefreshResult applyFullScan(const GitDiffScan& scan);
    [[nodiscard]] GitDiffRefreshResult applyPathScan(
        GitRepository& repository, const GitWorkingTreeScan& scan);

    DiffModel* diffModel_ = nullptr;
    GitDiffConfig config_{};
    Revision nextRevision_{1};
    Revision publishedRevision_{1};
    std::map<DiffFileId, GitDiffScanFile> currentFiles_;
    std::string baselineIdentity_;
    std::optional<std::string> currentBranch_;
    std::optional<GitDiffScan> latestAppliedScan_;
};

}  // namespace ssg
