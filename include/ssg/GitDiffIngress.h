#pragma once

#include <ssg/DiffModel.h>
#include <ssg/FilesystemWatcher.h>
#include <ssg/FollowEditsModel.h>
#include <ssg/GitDiffWorker.h>
#include <ssg/Workspace.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace ssg {

struct DiffIngressResult;
struct Editor;

// Owns git and watcher ingress state and applies worker results to one Editor.
struct GitDiffIngress {
    GitDiffIngress(Editor& editor, const std::filesystem::path& root,
                   bool enableGitDiffWorker, bool enableFilesystemWatcher);

    [[nodiscard]] bool drainGitDiffWorker();
    void refreshLiveDiffDocuments(const DiffViewState& view);
    [[nodiscard]] DiffIngressResult applyGitDiffScanLocked(GitDiffScan scan);
    [[nodiscard]] bool revealCurrentDiffTarget(
        const FollowTarget& target, NavigationClass classification);
    Editor& editor;
    std::uint64_t lastGitScanRevision{0};
    std::optional<std::string> currentGitBranch;
    GitDiffWorker worker;
};

} // namespace ssg
