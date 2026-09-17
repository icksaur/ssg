#include "test_helpers.h"

#include <ssg/Terminal.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

bool sameTerminalState(const termios& left, const termios& right) {
    return left.c_iflag == right.c_iflag && left.c_oflag == right.c_oflag &&
           left.c_cflag == right.c_cflag && left.c_lflag == right.c_lflag &&
           std::memcmp(left.c_cc, right.c_cc, sizeof(left.c_cc)) == 0;
}

TEST(linuxTerminalActivatesRawStateAndRestoresExactly) {
    const pid_t child = ::fork();
    if (child == 0) {
        const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
            std::_Exit(1);
        }
        const char* slaveName = ::ptsname(master);
        if (slaveName == nullptr) std::_Exit(2);
        const int slave = ::open(slaveName, O_RDWR | O_NOCTTY);
        if (slave < 0 || ::dup2(slave, STDIN_FILENO) < 0) std::_Exit(3);

        termios original{};
        if (::tcgetattr(STDIN_FILENO, &original) != 0) std::_Exit(4);
        {
            ssg::TerminalSession session;
            if (!session.active()) std::_Exit(5);
            termios active{};
            if (::tcgetattr(STDIN_FILENO, &active) != 0) std::_Exit(6);
            if ((active.c_lflag & (ICANON | ECHO | ISIG | IEXTEN)) != 0 ||
                (active.c_iflag & (IXON | ICRNL | BRKINT | INPCK | ISTRIP)) !=
                    0 ||
                (active.c_oflag & OPOST) != 0 || active.c_cc[VMIN] != 1 ||
                active.c_cc[VTIME] != 0) {
                std::_Exit(7);
            }
            session.restore();
            session.restore();
        }
        termios restored{};
        if (::tcgetattr(STDIN_FILENO, &restored) != 0) std::_Exit(8);
        std::_Exit(sameTerminalState(original, restored) ? 0 : 9);
    }
    ASSERT_TRUE(child > 0);
    if (child <= 0) return;
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
}

} // namespace

SSG_TEST_SUITE(test_linux_terminal) {
    RUN(linuxTerminalActivatesRawStateAndRestoresExactly);
    return failed == 0 ? 0 : 1;
}
