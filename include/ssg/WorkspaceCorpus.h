#pragma once

#include <ssg/GitDiffSource.h>
#include <ssg/platform_files.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ssg {

struct WorkspaceCorpusBuffer {
    std::optional<std::string> path;
    std::function<std::optional<std::string>()> read;
};

struct WorkspaceCorpusOptions {
    bool respectGitignore = true;
    std::size_t maximumFiles = 20'000;
    std::vector<std::filesystem::path> excludedDirectories;
};

enum class WorkspaceCorpusOutcome {
    Complete,
    Truncated,
    Failed,
};

struct WorkspaceCorpusStatus {
    WorkspaceCorpusOutcome outcome = WorkspaceCorpusOutcome::Complete;
    std::string message;
};

using WorkspaceCorpusReader =
    std::function<FileReadResult(const std::filesystem::path&)>;

struct WorkspaceCorpusFile {
    std::string path;
    std::string text;
};

// Owns one deterministic workspace file set. Paths are resident; file contents
// are retrieved one at a time, with open buffers taking precedence over disk.
class WorkspaceCorpus {
public:
    WorkspaceCorpus(std::filesystem::path workspaceRoot,
                    std::vector<WorkspaceCorpusBuffer> openBuffers,
                    const GitIgnoreMatcher& ignore,
                    WorkspaceCorpusReader reader,
                    WorkspaceCorpusOptions options = {});

    [[nodiscard]] const std::vector<std::string>& paths() const noexcept;
    [[nodiscard]] const WorkspaceCorpusStatus& status() const noexcept;
    [[nodiscard]] std::optional<WorkspaceCorpusFile> read(
        std::size_t index) const;

private:
    std::filesystem::path root_;
    std::vector<std::string> paths_;
    std::map<std::string,
             std::function<std::optional<std::string>()>> openBuffers_;
    WorkspaceCorpusReader reader_;
    WorkspaceCorpusStatus status_;
};

}  // namespace ssg
