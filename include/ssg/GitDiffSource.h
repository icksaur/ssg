#pragma once

#include "ssg/DiffModel.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
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
    std::string baselineIdentity;
    std::vector<GitDiffScanFile> files;
    bool complete = true;

    friend bool operator==(const GitDiffScan&, const GitDiffScan&) = default;
};

struct GitWorkingTreeScan {
    std::string baselineIdentity;
    std::vector<std::filesystem::path> requestedPaths;
    std::vector<GitDiffScanFile> files;
    bool complete = true;

    friend bool operator==(const GitWorkingTreeScan&,
                           const GitWorkingTreeScan&) = default;
};

class GitRepository {
public:
    virtual ~GitRepository() = default;

    [[nodiscard]] virtual GitDiffScan scanDiff(const GitDiffConfig& config) = 0;
    [[nodiscard]] virtual GitWorkingTreeScan scanPaths(
        const std::vector<std::filesystem::path>& paths,
        const GitDiffConfig& config) = 0;
};

}  // namespace ssg
