#include <ssg/Terminal.h>

#include <csignal>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <utility>
#include <vector>

namespace ssg {

void writeAll(std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto written =
            ::write(STDOUT_FILENO, bytes.data() + offset, bytes.size() - offset);
        if (written <= 0) break;
        offset += static_cast<std::size_t>(written);
    }
}

ssg::ViewportDimensions terminalSize() {
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 &&
        size.ws_row > 0) {
        return {size.ws_col, size.ws_row};
    }
    return {80, 24};
}

SignalEvents classifySignalTags(std::string_view drained) {
    SignalEvents events;
    for (unsigned char byte : drained) {
        int const signo = static_cast<int>(byte);
        if (signo == SIGWINCH) {
            events.resize = true;
        } else if (signo == SIGTERM || signo == SIGHUP) {
            events.terminate = signo;
        }
    }
    return events;
}


struct TerminalModes::Impl {
    explicit Impl(Writer configuredWriter) : writer{std::move(configuredWriter)} {}
    Writer writer;
    std::vector<TerminalMode> entered;
};

TerminalModes::TerminalModes(Writer writer)
    : impl_{std::make_unique<Impl>(std::move(writer))} {}

TerminalModes::~TerminalModes() { leaveThrough(0); }

TerminalModes::Guard TerminalModes::enter(TerminalMode mode) {
    impl_->entered.push_back(mode);
    impl_->writer(mode.enter);
    return Guard{*this, impl_->entered.size()};
}

// Leaves every mode entered at or above `depth`, deepest first.
void TerminalModes::leaveThrough(std::size_t depth) noexcept {
    while (impl_->entered.size() > depth) {
        auto const mode = impl_->entered.back();
        impl_->entered.pop_back();
        impl_->writer(mode.leave);
    }
}

TerminalModes::Guard::~Guard() {
    if (owner_ != nullptr) owner_->leaveThrough(depth_ - 1);
}

TerminalModes::Guard::Guard(Guard&& other) noexcept
    : owner_{other.owner_}, depth_{other.depth_} {
    other.owner_ = nullptr;
}

TerminalModes::Guard& TerminalModes::Guard::operator=(Guard&& other) noexcept {
    if (this != &other) {
        if (owner_ != nullptr) owner_->leaveThrough(depth_ - 1);
        owner_ = other.owner_;
        depth_ = other.depth_;
        other.owner_ = nullptr;
    }
    return *this;
}

struct TerminalSession::Impl {
    Impl() : modes{[](std::string_view bytes) { writeAll(bytes); }} {}

    TerminalModes modes;
    std::vector<TerminalModes::Guard> entered;
    termios original{};
    bool active = false;
    bool keyboardProtocolEntered = false;
};

TerminalSession::TerminalSession()
    : impl_{std::make_unique<Impl>()} {
    if (tcgetattr(STDIN_FILENO, &impl_->original) != 0) return;
    termios raw = impl_->original;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return;
    impl_->active = true;
    impl_->entered.push_back(impl_->modes.enter(kAlternateScreen));
    impl_->entered.push_back(impl_->modes.enter(kCursorStyleBar));
    impl_->entered.push_back(impl_->modes.enter(kMouseButtons));
    impl_->entered.push_back(impl_->modes.enter(kMouseMotion));
    impl_->entered.push_back(impl_->modes.enter(kMouseSgrCoordinates));
    impl_->entered.push_back(impl_->modes.enter(kBracketedPaste));
}

TerminalSession::~TerminalSession() { restore(); }

void TerminalSession::restore() noexcept {
    if (!impl_->active) return;
    impl_->active = false;
    impl_->entered.clear();
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &impl_->original);
}

bool TerminalSession::active() const noexcept { return impl_->active; }

void TerminalSession::enableKeyboardProtocol() {
    if (!impl_->active || impl_->keyboardProtocolEntered) return;
    impl_->entered.push_back(impl_->modes.enter(kKeyboardProtocol));
    impl_->keyboardProtocolEntered = true;
}

} // namespace ssg
