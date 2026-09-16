#include <ssg/PlatformRuntime.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdexcept>
#include <system_error>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ssg {
namespace {

std::array<int, 2> makePipe() {
    std::array<int, 2> descriptors{-1, -1};
    if (::pipe2(descriptors.data(), O_NONBLOCK | O_CLOEXEC) != 0) {
        throw std::system_error{errno, std::generic_category(),
                                "failed to create platform wake"};
    }
    return descriptors;
}

sigset_t processControlSignals() {
    sigset_t signals;
    if (::sigemptyset(&signals) != 0 ||
        ::sigaddset(&signals, SIGWINCH) != 0 ||
        ::sigaddset(&signals, SIGTERM) != 0 ||
        ::sigaddset(&signals, SIGHUP) != 0 ||
        ::sigaddset(&signals, SIGINT) != 0) {
        throw std::system_error{errno, std::generic_category(),
                                "failed to configure process-control signals"};
    }
    return signals;
}

TerminationRequest terminationRequest(std::uint32_t signal) {
    if (signal == SIGHUP) return {TerminationKind::Hangup};
    if (signal == SIGINT) return {TerminationKind::Interrupt};
    return {TerminationKind::Terminate};
}

int terminationSignal(TerminationKind kind) {
    if (kind == TerminationKind::Hangup) return SIGHUP;
    if (kind == TerminationKind::Interrupt) return SIGINT;
    return SIGTERM;
}

int pollTimeout(std::optional<std::chrono::milliseconds> timeout) {
    if (!timeout) return -1;
    return static_cast<int>(
        std::clamp<std::int64_t>(timeout->count(), 0, INT_MAX));
}

} // namespace

struct PlatformWake::Impl {
    Impl() : descriptors{makePipe()} {}
    ~Impl() {
        (void)::close(descriptors[0]);
        (void)::close(descriptors[1]);
    }

    std::array<int, 2> descriptors;
};

PlatformWake::PlatformWake() : impl_{std::make_unique<Impl>()} {}
PlatformWake::~PlatformWake() = default;
PlatformWake::PlatformWake(PlatformWake&&) noexcept = default;
PlatformWake& PlatformWake::operator=(PlatformWake&&) noexcept = default;

void PlatformWake::notify() noexcept {
    const unsigned char tag = 1;
    ssize_t result;
    do {
        result = ::write(impl_->descriptors[1], &tag, 1);
    } while (result < 0 && errno == EINTR);
    (void)result;
}

void PlatformWake::consume() noexcept {
    std::array<unsigned char, 64> tags{};
    while (::read(impl_->descriptors[0], tags.data(), tags.size()) > 0) {
    }
}

struct PlatformEventLoop::Impl {
    explicit Impl(PlatformEventLoopOptions configuredOptions)
        : options{configuredOptions} {
        if (!options.monitorProcessControl) return;

        signals = processControlSignals();
        const int maskResult =
            ::pthread_sigmask(SIG_BLOCK, &signals, &previousSignals);
        if (maskResult != 0) {
            throw std::system_error{maskResult, std::generic_category(),
                                    "failed to block process-control signals"};
        }
        signalMaskInstalled = true;
        signalDescriptor =
            ::signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
        if (signalDescriptor < 0) {
            const int error = errno;
            restoreSignalMask();
            throw std::system_error{error, std::generic_category(),
                                    "failed to create process-control input"};
        }
    }

    ~Impl() {
        if (signalDescriptor >= 0) (void)::close(signalDescriptor);
        restoreSignalMask();
    }

    void restoreSignalMask() noexcept {
        if (!signalMaskInstalled) return;
        signalMaskInstalled = false;
        (void)::pthread_sigmask(SIG_SETMASK, &previousSignals, nullptr);
    }

    PlatformEventLoopOptions options;
    sigset_t signals{};
    sigset_t previousSignals{};
    int signalDescriptor = -1;
    bool signalMaskInstalled = false;
};

PlatformEventLoop::PlatformEventLoop(PlatformEventLoopOptions options)
    : impl_{std::make_unique<Impl>(options)} {}

PlatformEventLoop::~PlatformEventLoop() = default;

PlatformReadiness PlatformEventLoop::wait(
    std::optional<std::chrono::milliseconds> timeout,
    std::span<const PlatformWake* const> wakes) {
    std::vector<pollfd> descriptors;
    std::optional<std::size_t> inputIndex;
    std::optional<std::size_t> signalIndex;
    std::vector<std::optional<std::size_t>> wakeIndices(wakes.size());

    const auto add = [&](int descriptor) {
        descriptors.push_back({descriptor, POLLIN, 0});
        return descriptors.size() - 1;
    };
    if (impl_->options.monitorInput) inputIndex = add(STDIN_FILENO);
    if (impl_->signalDescriptor >= 0) {
        signalIndex = add(impl_->signalDescriptor);
    }
    for (std::size_t index = 0; index < wakes.size(); ++index) {
        if (wakes[index] != nullptr) {
            wakeIndices[index] = add(wakes[index]->impl_->descriptors[0]);
        }
    }

    const int ready =
        ::poll(descriptors.data(), descriptors.size(), pollTimeout(timeout));
    if (ready < 0) {
        if (errno == EINTR) return {};
        throw std::system_error{errno, std::generic_category(),
                                "platform wait failed"};
    }
    if (ready == 0) return {};

    PlatformReadiness result;
    if (inputIndex) {
        const auto events = descriptors[*inputIndex].revents;
        result.input = (events & (POLLIN | POLLHUP)) != 0;
        if ((events & (POLLERR | POLLNVAL)) != 0) {
            throw std::runtime_error{"standard input wait failed"};
        }
    }
    for (std::size_t index = 0; index < wakes.size(); ++index) {
        if (!wakeIndices[index]) continue;
        const auto events = descriptors[*wakeIndices[index]].revents;
        if ((events & POLLIN) != 0) result.wakes.push_back(index);
        if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            throw std::runtime_error{"platform wake wait failed"};
        }
    }

    if (signalIndex) {
        const auto events = descriptors[*signalIndex].revents;
        if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            throw std::runtime_error{"process-control wait failed"};
        }
        if ((events & POLLIN) != 0) {
            signalfd_siginfo information{};
            for (;;) {
                const auto count =
                    ::read(impl_->signalDescriptor, &information,
                           sizeof(information));
                if (count < 0 && errno == EINTR) continue;
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }
                if (count < 0) {
                    throw std::system_error{
                        errno, std::generic_category(),
                        "failed to read process-control input"};
                }
                if (count == 0) break;
                if (count != static_cast<ssize_t>(sizeof(information))) {
                    throw std::runtime_error{
                        "incomplete process-control input"};
                }
                if (information.ssi_signo == SIGWINCH) {
                    result.resize = true;
                } else {
                    result.termination =
                        terminationRequest(information.ssi_signo);
                }
            }
        }
    }
    return result;
}

std::size_t PlatformEventLoop::readInput(std::span<char> destination) {
    if (!impl_->options.monitorInput) {
        throw std::logic_error{"standard input monitoring is disabled"};
    }
    if (destination.empty()) {
        throw std::invalid_argument{"standard input destination is empty"};
    }
    for (;;) {
        const auto count =
            ::read(STDIN_FILENO, destination.data(), destination.size());
        if (count >= 0) return static_cast<std::size_t>(count);
        if (errno == EINTR) continue;
        throw std::system_error{errno, std::generic_category(),
                                "failed to read standard input"};
    }
}

std::uint64_t processId() noexcept {
    return static_cast<std::uint64_t>(::getpid());
}

std::chrono::nanoseconds monotonicTime() {
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        throw std::system_error{errno, std::generic_category(),
                                "failed to read monotonic clock"};
    }
    return std::chrono::seconds{now.tv_sec} +
           std::chrono::nanoseconds{now.tv_nsec};
}

[[noreturn]] void terminateProcess(TerminationRequest request) {
    const int signal = terminationSignal(request.kind);
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    (void)::sigemptyset(&action.sa_mask);
    (void)::sigaction(signal, &action, nullptr);

    sigset_t unblocked;
    (void)::sigemptyset(&unblocked);
    (void)::sigaddset(&unblocked, signal);
    (void)::pthread_sigmask(SIG_UNBLOCK, &unblocked, nullptr);
    (void)::kill(::getpid(), signal);
    std::_Exit(128 + signal);
}

} // namespace ssg
