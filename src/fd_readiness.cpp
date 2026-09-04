#include <ssg/fd_readiness.h>

#include <algorithm>

#include <sys/select.h>
#include <unistd.h>

namespace ssg::app {

FdReadiness waitReadiness(int timeoutMs, int signalFd, int gitDiffFd,
                          int initScriptFd) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    if (signalFd >= 0) FD_SET(signalFd, &set);
    int maxFd = std::max(STDIN_FILENO, signalFd);
    if (gitDiffFd >= 0) {
        FD_SET(gitDiffFd, &set);
        maxFd = std::max(maxFd, gitDiffFd);
    }
    if (initScriptFd >= 0) {
        FD_SET(initScriptFd, &set);
        maxFd = std::max(maxFd, initScriptFd);
    }
    timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int const ready =
        ::select(maxFd + 1, &set, nullptr, nullptr,
                 timeoutMs < 0 ? nullptr : &timeout);
    if (ready <= 0) return {};
    return {
        FD_ISSET(STDIN_FILENO, &set) != 0,
        signalFd >= 0 ? FD_ISSET(signalFd, &set) != 0 : false,
        gitDiffFd >= 0 ? FD_ISSET(gitDiffFd, &set) != 0 : false,
        initScriptFd >= 0 ? FD_ISSET(initScriptFd, &set) != 0 : false};
}

}  // namespace ssg::app
