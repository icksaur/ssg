#include <ssg/GitDiffIngress.h>

namespace ssg {

GitDiffIngress::GitDiffIngress(const std::filesystem::path& root,
                               bool enableGitDiffWorker,
                               bool enableFilesystemWatcher)
    : worker{root, enableGitDiffWorker, enableFilesystemWatcher} {}

GitDiffWorkerDrain GitDiffIngress::drain() {
    return worker.drain();
}

const PlatformWake* GitDiffIngress::wake() const noexcept {
    return worker.wake();
}

} // namespace ssg
