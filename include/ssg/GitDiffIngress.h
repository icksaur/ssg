#pragma once

#include <ssg/GitDiffWorker.h>

#include <filesystem>

namespace ssg {

// Owns and drains worker ingress; never retains its editor or adopts model state.
class GitDiffIngress {
public:
    GitDiffIngress(const std::filesystem::path& root,
                   bool enableGitDiffWorker, bool enableFilesystemWatcher);

    [[nodiscard]] GitDiffWorkerDrain drain();
    [[nodiscard]] const PlatformWake* wake() const noexcept;

private:
    GitDiffWorker worker;
};

} // namespace ssg
