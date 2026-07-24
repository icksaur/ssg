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
