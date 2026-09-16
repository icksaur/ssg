#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ssg {

// Product operating-system calls and types belong only to the selected
// implementation below src/platform/linux or src/platform/windows.
class PlatformWake {
public:
    PlatformWake();
    ~PlatformWake();
    PlatformWake(PlatformWake&&) noexcept;
    PlatformWake& operator=(PlatformWake&&) noexcept;
    PlatformWake(const PlatformWake&) = delete;
    PlatformWake& operator=(const PlatformWake&) = delete;

    // Producers queue authoritative data before notify. Consumers consume
    // before draining it. Notifications coalesce but remain ready until consume.
    void notify() noexcept;
    void consume() noexcept;

private:
    friend class PlatformEventLoop;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class TerminationKind : std::uint8_t {
    Hangup,
    Terminate,
    Interrupt,
    Break,
    Close,
    Logoff,
    Shutdown,
};

struct TerminationRequest {
    TerminationKind kind;
};

struct PlatformReadiness {
    bool input = false;
    bool resize = false;
    std::optional<TerminationRequest> termination;
    std::vector<std::size_t> wakes;
};

struct PlatformEventLoopOptions {
    bool monitorInput = true;
    bool monitorProcessControl = true;
};

class PlatformEventLoop {
public:
    explicit PlatformEventLoop(PlatformEventLoopOptions options = {});
    ~PlatformEventLoop();
    PlatformEventLoop(const PlatformEventLoop&) = delete;
    PlatformEventLoop& operator=(const PlatformEventLoop&) = delete;

    // Registered wakes are borrowed only for this call and must outlive it.
    // A missing timeout blocks until readiness without periodic polling.
    [[nodiscard]] PlatformReadiness wait(
        std::optional<std::chrono::milliseconds> timeout,
        std::span<const PlatformWake* const> wakes);
    [[nodiscard]] std::size_t readInput(std::span<char> destination);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::uint64_t processId() noexcept;
[[nodiscard]] std::chrono::nanoseconds monotonicTime();
[[noreturn]] void terminateProcess(TerminationRequest request);

} // namespace ssg
