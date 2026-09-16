#include <ssg/PlatformRuntime.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <sys/select.h>
#include <unistd.h>
#include <utility>

namespace ssg {
namespace {

volatile std::sig_atomic_t signalWriteDescriptor = -1;

void setNonBlocking(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags == -1 ||
        ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
        throw std::system_error{errno, std::generic_category(),
                                "failed to configure platform wake"};
    }
}

std::array<int, 2> makePipe() {
    std::array<int, 2> descriptors{-1, -1};
    if (::pipe(descriptors.data()) != 0) {
        throw std::system_error{errno, std::generic_category(),
                                "failed to create platform wake"};
    }
    try {
        setNonBlocking(descriptors[0]);
        setNonBlocking(descriptors[1]);
    } catch (...) {
        (void)::close(descriptors[0]);
        (void)::close(descriptors[1]);
        throw;
    }
    return descriptors;
}

extern "C" void signalHandler(int signal) {
    const int descriptor = signalWriteDescriptor;
    if (descriptor < 0) return;
    const unsigned char tag = static_cast<unsigned char>(signal);
    const auto ignored = ::write(descriptor, &tag, 1);
    (void)ignored;
}

struct InstalledSignal {
    int number;
    struct sigaction previous {};
};

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
    const auto ignored = ::write(impl_->descriptors[1], &tag, 1);
    (void)ignored;
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
        if (signalWriteDescriptor != -1) {
            throw std::logic_error{"only one process-control event loop may exist"};
        }
        signalDescriptors = makePipe();
        signalWriteDescriptor = signalDescriptors[1];
        try {
            for (auto& installed : signals) {
                struct sigaction action {};
                action.sa_handler = signalHandler;
                sigemptyset(&action.sa_mask);
                action.sa_flags = SA_RESTART;
                if (::sigaction(installed.number, &action,
                                &installed.previous) != 0) {
                    throw std::system_error{
                        errno, std::generic_category(),
                        "failed to install process-control handler"};
                }
                ++installedCount;
            }
        } catch (...) {
            restoreSignals();
            (void)::close(signalDescriptors[0]);
            (void)::close(signalDescriptors[1]);
            signalDescriptors = {-1, -1};
            throw;
        }
    }

    ~Impl() {
        restoreSignals();
        if (signalDescriptors[0] != -1) {
            (void)::close(signalDescriptors[0]);
            (void)::close(signalDescriptors[1]);
        }
    }

    void restoreSignals() noexcept {
        signalWriteDescriptor = -1;
        while (installedCount > 0) {
            --installedCount;
            auto& installed = signals[installedCount];
            (void)::sigaction(installed.number, &installed.previous, nullptr);
        }
    }

    PlatformEventLoopOptions options;
    std::array<int, 2> signalDescriptors{-1, -1};
    std::array<InstalledSignal, 3> signals{{
        {SIGWINCH, {}},
        {SIGTERM, {}},
        {SIGHUP, {}},
    }};
    std::size_t installedCount = 0;
};

PlatformEventLoop::PlatformEventLoop(PlatformEventLoopOptions options)
    : impl_{std::make_unique<Impl>(options)} {}

PlatformEventLoop::~PlatformEventLoop() = default;

PlatformReadiness PlatformEventLoop::wait(
    std::optional<std::chrono::milliseconds> timeout,
    std::span<const PlatformWake* const> wakes) {
    fd_set descriptors;
    FD_ZERO(&descriptors);
    int maximum = -1;
    const auto add = [&](int descriptor) {
        if (descriptor < 0) return;
        FD_SET(descriptor, &descriptors);
        maximum = std::max(maximum, descriptor);
    };

    if (impl_->options.monitorInput) add(STDIN_FILENO);
    add(impl_->signalDescriptors[0]);
    for (const auto* wake : wakes) {
        if (wake != nullptr) add(wake->impl_->descriptors[0]);
    }

    timeval value{};
    timeval* timeoutPointer = nullptr;
    if (timeout) {
        const auto bounded = std::max(*timeout, std::chrono::milliseconds{0});
        value.tv_sec = static_cast<decltype(value.tv_sec)>(bounded.count() / 1000);
        value.tv_usec =
            static_cast<decltype(value.tv_usec)>((bounded.count() % 1000) * 1000);
        timeoutPointer = &value;
    }

    const int ready =
        ::select(maximum + 1, &descriptors, nullptr, nullptr, timeoutPointer);
    if (ready <= 0) return {};

    PlatformReadiness result;
    result.input = impl_->options.monitorInput &&
                   FD_ISSET(STDIN_FILENO, &descriptors) != 0;
    for (std::size_t index = 0; index < wakes.size(); ++index) {
        if (wakes[index] != nullptr &&
            FD_ISSET(wakes[index]->impl_->descriptors[0], &descriptors) != 0) {
            result.wakes.push_back(index);
        }
    }

    if (impl_->signalDescriptors[0] >= 0 &&
        FD_ISSET(impl_->signalDescriptors[0], &descriptors) != 0) {
        std::array<unsigned char, 64> tags{};
        for (;;) {
            const auto count = ::read(impl_->signalDescriptors[0], tags.data(),
                                      tags.size());
            if (count <= 0) break;
            for (std::size_t index = 0;
                 index < static_cast<std::size_t>(count); ++index) {
                if (tags[index] == SIGWINCH) {
                    result.resize = true;
                } else if (tags[index] == SIGTERM) {
                    result.termination =
                        TerminationRequest{TerminationKind::Terminate};
                } else if (tags[index] == SIGHUP) {
                    result.termination =
                        TerminationRequest{TerminationKind::Hangup};
                }
            }
        }
    }
    return result;
}

[[noreturn]] void terminateProcess(TerminationRequest request) {
    const int signal = request.kind == TerminationKind::Hangup ? SIGHUP : SIGTERM;
    (void)::signal(signal, SIG_DFL);
    (void)::raise(signal);
    std::terminate();
}

} // namespace ssg
