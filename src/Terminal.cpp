#include <ssg/Terminal.h>

#include <stdexcept>
#include <utility>
#include <vector>

namespace ssg {

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
    explicit Impl(std::unique_ptr<NativeTerminal> configuredNative)
        : native{std::move(configuredNative)},
          modes{[this](std::string_view bytes) { native->write(bytes); }} {}

    std::unique_ptr<NativeTerminal> native;
    TerminalModes modes;
    std::vector<TerminalModes::Guard> entered;
    bool active = false;
    bool keyboardProtocolEntered = false;
};

TerminalSession::TerminalSession()
    : TerminalSession{makePlatformTerminal()} {}

TerminalSession::TerminalSession(std::unique_ptr<NativeTerminal> native)
    : impl_{std::make_unique<Impl>(std::move(native))} {
    if (!impl_->native) {
        throw std::invalid_argument{"terminal backend is required"};
    }
    if (!impl_->native->activate()) return;
    impl_->active = true;
    try {
        impl_->entered.push_back(impl_->modes.enter(kAlternateScreen));
        impl_->entered.push_back(impl_->modes.enter(kCursorStyleBar));
        impl_->entered.push_back(impl_->modes.enter(kMouseButtons));
        impl_->entered.push_back(impl_->modes.enter(kMouseMotion));
        impl_->entered.push_back(impl_->modes.enter(kMouseSgrCoordinates));
        impl_->entered.push_back(impl_->modes.enter(kBracketedPaste));
    } catch (...) {
        restore();
        throw;
    }
}

TerminalSession::~TerminalSession() { restore(); }

void TerminalSession::restore() noexcept {
    if (!impl_->active) return;
    impl_->active = false;
    impl_->entered.clear();
    impl_->native->restore();
}

bool TerminalSession::active() const noexcept { return impl_->active; }

void TerminalSession::enableKeyboardProtocol() {
    if (!impl_->active || impl_->keyboardProtocolEntered) return;
    impl_->entered.push_back(impl_->modes.enter(kKeyboardProtocol));
    impl_->keyboardProtocolEntered = true;
}

} // namespace ssg
