#pragma once

#include <ssg/DiffModel.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ssg {

// How the git-diff worker decides when to refresh. Poll re-scans on a fixed
// cadence; Event refreshes on filesystem watch events (with a long-interval
// backstop). The distinction is the worker's; it lives here so the mode resolver
// below is unit-testable.
enum class GitDiffMode : std::uint8_t { Poll, Event };

// Resolve the git-diff refresh mode. An explicit env value ("poll" or "event")
// wins; otherwise the default follows watcher availability -- Event when a watcher
// will drive refreshes (idle-cheap), Poll otherwise. `envValue` is the raw
// SSG_GIT_DIFF_MODE string, or nullptr when unset. Pure, so it is unit-testable.
[[nodiscard]] GitDiffMode resolveGitDiffMode(const char* envValue,
                                             bool watcherAvailable) noexcept;

struct GitDiffConfig {
    std::size_t maxFiles = 10'000;
    std::size_t maxBytesPerFile = 4 * 1024 * 1024;
};

struct GitDiffScan {
    std::uint64_t revision{0};
    std::string baselineIdentity;
    std::optional<std::string> currentBranch;
    std::vector<GitDiffFile> files;
    bool complete = true;

    friend bool operator==(const GitDiffScan&, const GitDiffScan&) = default;
};

struct GitWorkingTreeScan {
    std::uint64_t revision{0};
    std::string baselineIdentity;
    std::vector<std::filesystem::path> requestedPaths;
    std::vector<GitDiffFile> files;
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
    // CONTRACT
    // GitRepository::metadataDirectories: only the repository adapter may
    // resolve Git metadata roots. Callers must not infer a `.git` path from
    // their workspace because linked worktrees and nested workspaces can put
    // metadata elsewhere.
    [[nodiscard]] virtual std::vector<std::filesystem::path>
    metadataDirectories() = 0;
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

    // CONTRACT
    // GitIgnoreMatcher::ignores: rebasing the workspace-relative path onto the
    //   repository work directory is the matcher's job; callers must never
    //   pre-rebase and must not pass an absolute path. The workspace root this
    //   matcher was built for is not necessarily the repository root.
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

    // CONTRACT
    // GitDiffSource::takeBranchOnlyScanIfChanged: the current branch is published
    //   independently of scan application. A refresh rejected because repository
    //   enumeration is incomplete still records the branch. Returns a revision-0
    //   branch-only scan exactly once per branch change not already carried by an
    //   applied full scan; nullopt when the branch is unchanged.
    [[nodiscard]] std::optional<GitDiffScan> takeBranchOnlyScanIfChanged();

private:
    [[nodiscard]] GitDiffRefreshResult applyFullScan(const GitDiffScan& scan);
    [[nodiscard]] GitDiffRefreshResult applyPathScan(
        GitRepository& repository, const GitWorkingTreeScan& scan);

    DiffModel* diffModel_ = nullptr;
    GitDiffConfig config_{};
    std::uint64_t nextRevision_{1};
    std::uint64_t publishedRevision_{1};
    std::map<DiffFileId, GitDiffFile> currentFiles_;
    std::string baselineIdentity_;
    std::optional<std::string> currentBranch_;
    std::optional<std::string> publishedBranch_;
    std::optional<GitDiffScan> latestAppliedScan_;
};

}  // namespace ssg
