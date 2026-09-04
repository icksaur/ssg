#pragma once

namespace ssg::app {

struct FdReadiness {
    bool input = false;
    bool signal = false;
    bool gitDiff = false;
    bool initScript = false;
};

[[nodiscard]] FdReadiness waitReadiness(int timeoutMs, int signalFd, int gitDiffFd,
                                        int initScriptFd = -1);

}  // namespace ssg::app
