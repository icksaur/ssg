#include <ssg/PlatformRuntime.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace ssg {
namespace {

[[noreturn]] void throwLastError(const char* message) {
    throw std::system_error{static_cast<int>(::GetLastError()),
                            std::system_category(), message};
}

} // namespace

struct PlatformWake::Impl {
    Impl() : event{::CreateEventW(nullptr, TRUE, FALSE, nullptr)} {
        if (event == nullptr) throwLastError("failed to create platform wake");
    }
    ~Impl() { (void)::CloseHandle(event); }

    HANDLE event;
};

PlatformWake::PlatformWake() : impl_{std::make_unique<Impl>()} {}
PlatformWake::~PlatformWake() = default;
PlatformWake::PlatformWake(PlatformWake&&) noexcept = default;
PlatformWake& PlatformWake::operator=(PlatformWake&&) noexcept = default;

void PlatformWake::notify() noexcept { (void)::SetEvent(impl_->event); }
void PlatformWake::consume() noexcept { (void)::ResetEvent(impl_->event); }

struct PlatformEventLoop::Impl {
    explicit Impl(PlatformEventLoopOptions configuredOptions)
        : options{configuredOptions} {}
    PlatformEventLoopOptions options;
};

PlatformEventLoop::PlatformEventLoop(PlatformEventLoopOptions options)
    : impl_{std::make_unique<Impl>(options)} {}
PlatformEventLoop::~PlatformEventLoop() = default;

PlatformReadiness PlatformEventLoop::wait(
    std::optional<std::chrono::milliseconds> timeout,
    std::span<const PlatformWake* const> wakes) {
    std::vector<HANDLE> handles;
    if (impl_->options.monitorInput) {
        const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
        if (input == nullptr || input == INVALID_HANDLE_VALUE) {
            throwLastError("failed to access console input");
        }
        handles.push_back(input);
    }
    for (const auto* wake : wakes) {
        if (wake != nullptr) handles.push_back(wake->impl_->event);
    }
    if (handles.empty()) {
        throw std::logic_error{"platform wait requires an input or wake handle"};
    }

    const DWORD milliseconds =
        timeout ? static_cast<DWORD>(std::max<std::int64_t>(0, timeout->count()))
                : INFINITE;
    const DWORD result = ::WaitForMultipleObjects(
        static_cast<DWORD>(handles.size()), handles.data(), FALSE, milliseconds);
    if (result == WAIT_TIMEOUT) return {};
    if (result == WAIT_FAILED) throwLastError("platform wait failed");

    PlatformReadiness readiness;
    if (impl_->options.monitorInput) {
        readiness.input = ::WaitForSingleObject(handles[0], 0) == WAIT_OBJECT_0;
    }
    for (std::size_t index = 0; index < wakes.size(); ++index) {
        if (wakes[index] != nullptr &&
            ::WaitForSingleObject(wakes[index]->impl_->event, 0) ==
                WAIT_OBJECT_0) {
            readiness.wakes.push_back(index);
        }
    }
    return readiness;
}

std::size_t PlatformEventLoop::readInput(std::span<char>) {
    throw std::logic_error{"Windows console input is not implemented"};
}

std::uint64_t processId() noexcept {
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
}

std::chrono::nanoseconds monotonicTime() {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    if (::QueryPerformanceFrequency(&frequency) == 0 ||
        ::QueryPerformanceCounter(&counter) == 0) {
        throwLastError("failed to read monotonic clock");
    }
    const auto seconds = counter.QuadPart / frequency.QuadPart;
    const auto remainder = counter.QuadPart % frequency.QuadPart;
    return std::chrono::seconds{seconds} +
           std::chrono::nanoseconds{
               remainder * 1'000'000'000LL / frequency.QuadPart};
}

[[noreturn]] void terminateProcess(TerminationRequest) {
    ::ExitProcess(1);
}

} // namespace ssg
