#include <ssg/Terminal.h>

#include <cerrno>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <memory>

namespace ssg {
namespace {

class LinuxTerminal final : public NativeTerminal {
  public:
    ~LinuxTerminal() override { restore(); }

    [[nodiscard]] bool activate() override {
        if (::tcgetattr(STDIN_FILENO, &original_) != 0) return false;
        termios raw = original_;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        raw.c_oflag &= ~(OPOST);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;
        active_ = true;
        return true;
    }

    void restore() noexcept override {
        if (!active_) return;
        active_ = false;
        (void)::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
    }

    void write(std::string_view bytes) noexcept override {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto written =
                ::write(STDOUT_FILENO, bytes.data() + offset,
                        bytes.size() - offset);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
            } else if (written < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
    }

  private:
    termios original_{};
    bool active_ = false;
};

} // namespace

std::unique_ptr<NativeTerminal> makePlatformTerminal() {
    return std::make_unique<LinuxTerminal>();
}

void writeAll(std::string_view bytes) {
    LinuxTerminal terminal;
    terminal.write(bytes);
}

ViewportDimensions terminalSize() {
    winsize size{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 &&
        size.ws_row > 0) {
        return {size.ws_col, size.ws_row};
    }
    return {80, 24};
}

} // namespace ssg
